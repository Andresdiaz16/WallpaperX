// SPDX-License-Identifier: GPL-2.0-or-later

// Step 13 Stages E1, E2 and E3: the daemon draws a `video` wallpaper's first picture, keeps a decode
// session for it rather than building one inside every render pass -- one per file and speed, shared
// by the screens showing it -- and plays it.
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

#include "test_image_dir.h"
#include "daemon/crash_record.h"
#include "daemon/dbus_service.h"
#include "daemon/render_worker.h"
#include "daemon/wallpaper_manager.h"

#include "vulkan/frame_export.h"
#include "vulkan/offscreen.h"

#include "support/cover_expectation.h"
#include "support/decoder_message_id.h"
#include "support/exported_frame_map.h"
#include "support/loaded_range.h"
#include "support/picture_counter.h"
#include "support/test_image.h"
#include "support/validation_grade.h"
#include "tests/client/call_gate.h"
#include "tests/client/daemon_client.h"
#include "tests/client/scratch_dir.h"
#include "tests/manager_output.h"
#include "tests/served_pixels.h"
#include "tests/self_exec.h"
#include "tests/test_phase.h"
#include "tests/test_report.h"

#if WALLPAPERX_HAVE_FRAME_IMPORT
#include "frame_import.h"
#include "tests/frame_import_request.h"
#include "tests/gl_import_harness.h"
#endif

#include <sdbus-c++/sdbus-c++.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
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
#include <random>
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
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

using wallpaperx::DbusService;
using wallpaperx::RenderWorker;
using wallpaperx::WallpaperManager;
using wallpaperx::testing::CallGate;
using wallpaperx::testing::check;
using wallpaperx::testing::exportDelivered;
using wallpaperx::testing::FrameReadyLog;
using wallpaperx::testing::notVerified;
using wallpaperx::testing::phaseSelected;

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
CallGate g_imageGate;            // an image created at the armed extent ("40x30"): a still's decode, by its size

// Queue submissions to be failed as VK_ERROR_DEVICE_LOST -- which the daemon sorts as the GPU's
// own fault, where the shortage g_submitFailGate injects is sorted as a resource running out. The
// difference is the whole of "a GPU fault never retries", so the two cannot share a gate.
std::atomic<int> g_submitDeviceLost{0};

// Armed at N >= 0: N queue submissions go through, the next fails as VK_ERROR_OUT_OF_DEVICE_MEMORY,
// and it disarms itself (-1). What fails a clock round's second draw and not its first.
std::atomic<int> g_submitFailAfter{-1};

// Image creations to be failed as VK_ERROR_OUT_OF_DEVICE_MEMORY. Only the daemon's own: ffmpeg
// reaches Vulkan through a function table it fetched from the loader, which no definition here can
// interpose, so an armed count is spent on the picture converter's image inside the decode that
// opens a session -- which is the shortage the retry-once path exists for.
std::atomic<int> g_failImageCreates{0};

// Images created at this extent (width << 32 | height) since the last reset. A still of a size nothing
// else has is decoded into exactly one image of it, so this counts decodes.
std::atomic<uint64_t> g_countedExtent{0};
std::atomic<int> g_extentCreates{0};

// Packets sent to the wallpaper's decoder since the last reset. What makes "the decode happened
// before the images were rebuilt" an observation rather than an inference.
std::atomic<int> g_decodeCalls{0};
std::atomic<int> g_codecFrees{0};
// The same packets, counted only when sent on g_countedThread (a render worker's thread): what a
// render pass decodes itself, apart from a session's decode thread filling its queue meanwhile.
std::atomic<pid_t> g_countedThread{0};
std::atomic<int> g_countedThreadDecodes{0};
// Pictures the wallpaper's decoder delivered since the last reset, on any thread.
std::atomic<int> g_pictureDeliveries{0};

// While armed, when each pass of the one playing file began at the decoder: a packet whose timestamp is below the
// one sent before it. The decoder runs a bounded queue ahead of the screen, so pass to pass the spacing is the
// pass's wall time.
std::atomic<bool> g_passWatch{false};
std::mutex g_passMutex;
int64_t g_passLastPts = INT64_MIN;
std::vector<std::chrono::steady_clock::time_point> g_passStarts;

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
    // open on every thread of this process pays. `byScan` sets the processor-decoding scan's opens
    // apart (see t_scanThread): it reads each file once beside the decoder, and none of the counts
    // or the live set here is about it.
    bool opened(int fd, const char* path, bool byScan) {
        if (!m_armed.load(std::memory_order_relaxed) || path == nullptr || fd < 0) return false;
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto watched = m_watched.find(path);
        if (watched == m_watched.end()) return false;
        if (byScan) {
            m_scanLive[fd] = *watched;
            ++m_scanOpens;
            return true;
        }
        m_live[fd] = *watched;
        ++m_opens[*watched];
        return true;
    }

    // The path `fd` was opened on, or empty for a descriptor this ledger never saw or the scan opened.
    std::string closing(int fd) {
        if (!m_armed.load(std::memory_order_relaxed) || fd < 0) return {};
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_scanLive.erase(fd) != 0) return {};
        const auto it = m_live.find(fd);
        if (it == m_live.end()) return {};
        std::string path = it->second;
        m_live.erase(it);
        ++m_closes[path];
        return path;
    }

    // Opens of a watched file by the scan's thread since this process started; never reset.
    int scanOpens() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_scanOpens;
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
    std::map<int, std::string> m_scanLive;
    int m_scanOpens = 0;
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
    // which the decoder reports as the daemon running short. After `limit` failures every picture
    // goes through again, without waiting for disarm().
    void arm(int fileFaults = std::numeric_limits<int>::max(), int limit = std::numeric_limits<int>::max()) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_fileFaultsLeft = fileFaults;
        m_failuresLeft = limit;
        m_armed.store(true);
    }
    void disarm() { m_armed.store(false); }

    // The error to fail this picture with, or 0 to let it through.
    int failureFor(const AVCodecContext* context) {
        if (!m_armed.load()) return 0;
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_delivered.find(context);
        if (it == m_delivered.end() || it->second == 0 || m_failuresLeft == 0) return 0;
        --m_failuresLeft;
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
    int m_failuresLeft = 0;
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

// While armed: at every seek the decoder makes, and at the first few packets it is handed after each,
// whether the crash record for one monitor was on disk naming one file. What says the catch-up of a
// resumed video -- the seek back and the decode forward -- runs under no crash record. Counted from the
// thread that makes the call, which is the render worker's.
class RecordWatch {
public:
    void arm(const std::filesystem::path& stateDir, int monitorId, const std::string& path) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stateDir = stateDir;
        m_monitorId = monitorId;
        m_path = path;
        m_seeks = m_seeksCovered = m_packets = m_packetsCovered = 0;
        m_packetsLeft = 0;
        m_armed.store(true);
    }
    void disarm() { m_armed.store(false); }

    void seek() { note(true); }
    void packet() { note(false); }

    int seeks() const { return read(m_seeks); }
    int seeksCovered() const { return read(m_seeksCovered); }
    int packets() const { return read(m_packets); }
    int packetsCovered() const { return read(m_packetsCovered); }

private:
    // Packets are noted only just after a seek: those are the catch-up's, not the clock's play after it.
    static constexpr int kPacketsAfterSeek = 3;

    void note(bool isSeek) {
        if (!m_armed.load(std::memory_order_relaxed)) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!isSeek && m_packetsLeft == 0) return;
        const std::optional<wallpaperx::CrashRecord> record = wallpaperx::readCrashRecord(m_stateDir, m_monitorId);
        const bool covered = record && record->path == m_path;
        if (isSeek) {
            ++m_seeks;
            if (covered) ++m_seeksCovered;
            m_packetsLeft = kPacketsAfterSeek;
        } else {
            --m_packetsLeft;
            ++m_packets;
            if (covered) ++m_packetsCovered;
        }
    }
    int read(const int& value) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return value;
    }

    std::atomic<bool> m_armed{false};
    mutable std::mutex m_mutex;
    std::filesystem::path m_stateDir;
    int m_monitorId = 0;
    std::string m_path;
    int m_seeks = 0;
    int m_seeksCovered = 0;
    int m_packets = 0;
    int m_packetsCovered = 0;
    int m_packetsLeft = 0;
};

RecordWatch g_recordWatch;

// The last watched wallpaper file opened on this thread; see g_contextFiles below.
thread_local std::string t_lastWallpaperOpened;

// While armed: counts the seeks aimed at or past `fromSeconds` -- a resume's seek to where a video stood --
// and fails the first read after each of them with EIO while `count` lasts. With `alsoNextOpen`, each failed
// read also fails the next open() of the same file, so the open from the first picture that follows fails too.
// With `afterSend`, the read failed is the first after a packet was sent to a decoder following the seek: on
// the device the open's reset-point scan reads after the seek too, and this lets it pass, so the failure lands
// in the catch-up decode on either driver. With `holdNextDecode`, each failed read arms g_decodeGate to hold
// the next packet sent: the reopen's first decode, while what the failure left is readable. `error` is the
// errno the read fails with: EIO is the file's fault, ENOMEM the daemon running short.
class FarReadFailure {
public:
    void arm(double fromSeconds, int count, bool alsoNextOpen, bool afterSend = false, bool holdNextDecode = false,
             int error = EIO) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_error = error;
        m_from = fromSeconds;
        m_left = count;
        m_alsoNextOpen = alsoNextOpen;
        m_afterSend = afterSend;
        m_holdNextDecode = holdNextDecode;
        m_sentSinceSeek = false;
        m_farSeeks = 0;
        m_injected = 0;
        m_pending = nullptr;
        m_armed.store(true);
    }
    void disarm() { m_armed.store(false); }

    void seek(const AVFormatContext* format, int stream, int64_t timestamp, int flags) {
        if (!m_armed.load(std::memory_order_relaxed) || format == nullptr || stream < 0 ||
            (flags & AVSEEK_FLAG_BYTE) != 0) {
            return;
        }
        const double seconds = static_cast<double>(timestamp) * av_q2d(format->streams[stream]->time_base);
        std::lock_guard<std::mutex> lock(m_mutex);
        if (seconds < m_from) return;
        ++m_farSeeks;
        if (m_left > 0) {
            m_pending = format;
            m_sentSinceSeek = false;
        }
    }
    void sent() {
        if (!m_armed.load(std::memory_order_relaxed)) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_pending != nullptr) m_sentSinceSeek = true;
    }
    // The error to fail this read with, or 0.
    int readFailure(const AVFormatContext* format) {
        if (!m_armed.load(std::memory_order_relaxed)) return 0;
        std::string path;
        int error = EIO;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_pending == nullptr || m_pending != format || (m_afterSend && !m_sentSinceSeek)) return 0;
            m_pending = nullptr;
            --m_left;
            ++m_injected;
            if (m_alsoNextOpen) path = t_lastWallpaperOpened;
            if (m_holdNextDecode) g_decodeGate.arm(CallGate::Mode::Hold);
            error = m_error;
        }
        if (!path.empty()) g_openFailure.arm(path, EACCES, 1);
        return AVERROR(error);
    }
    int farSeeks() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_farSeeks;
    }
    int injected() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_injected;
    }

private:
    std::atomic<bool> m_armed{false};
    mutable std::mutex m_mutex;
    double m_from = 0;
    int m_error = EIO;
    int m_left = 0;
    bool m_alsoNextOpen = false;
    bool m_afterSend = false;
    bool m_holdNextDecode = false;
    bool m_sentSinceSeek = false;
    int m_farSeeks = 0;
    int m_injected = 0;
    const AVFormatContext* m_pending = nullptr;
};

FarReadFailure g_farRead;

// A mount table read in place of /proc/self/mountinfo while set: how a phase puts a wallpaper on a
// network filesystem without mounting one (S13.194, S13.195). Empty is the machine's own.
std::mutex g_mountTableMutex;
std::string g_mountTable;
std::atomic<bool> g_mountTableSet{false};

// Which wallpaper file each decoder context reads, so a death can be staged on one video's work
// and not another's. Learned at the context's first packet from the last wallpaper file opened on
// that thread: a decoder opens its file and then decodes from it on the same thread, with no other
// wallpaper opened in between.
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
// death there would be graded as the staged one. Seek: a seek in that file -- armed after its session
// was released, the resume's seek back. CatchUp: any packet sent for that file -- armed the same way,
// the first is the resume's catch-up, before the picture it resumes on.
enum class CrashOn { Nothing, Decode, Free, Seek, CatchUp };
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

// Where a staged Decode death writes which thread it died on, before faulting; empty in the parent.
std::string g_deathNotePath;
// Set once a decoder packet is sent off both the render worker's thread and the main one: the
// device runs a decode thread. Lavapipe's does not, and decodes on the render thread.
std::atomic<bool> g_decodeThreadSeen{false};

// A Decode death is accepted on any thread but the child's main one: with the decode thread on, the
// clock's packets are sent from the session's own thread, not the render worker's. Teardown frees,
// the reason for the thread rule, are Free's concern and run on the main thread.
bool crashDueFor(CrashOn on, const AVCodecContext* context) {
    if (g_crashOn.load() != on) return false;
    if (on == CrashOn::Decode ? ::gettid() == ::getpid() : ::gettid() != g_crashThread.load()) return false;
    std::string target;
    {
        std::lock_guard<std::mutex> lock(g_crashPathMutex);
        target = g_crashPath;
    }
    return !target.empty() && fileOfContext(context) == target;
}

// A seek on the render thread reads the file the decoder opened last on it.
bool crashDueAtSeek() {
    if (g_crashOn.load() != CrashOn::Seek || ::gettid() != g_crashThread.load()) return false;
    std::lock_guard<std::mutex> lock(g_crashPathMutex);
    return !g_crashPath.empty() && t_lastWallpaperOpened == g_crashPath;
}

// While set, each packet sent to a wallpaper's decoder is delayed by this many milliseconds: a catch-up
// then spans many clock rounds, long enough for a phase to act while it is still under way.
std::atomic<int> g_sendDelayMs{0};

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
    if (info != nullptr) {
        char extent[32] = "";
        std::snprintf(extent, sizeof(extent), "%ux%u", info->extent.width, info->extent.height);
        if (g_imageGate.enter(extent)) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    for (int armed = g_failImageCreates.load(); armed > 0;) {
        if (g_failImageCreates.compare_exchange_weak(armed, armed - 1)) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    if (info != nullptr && g_countedExtent.load() == ((uint64_t{info->extent.width} << 32) | info->extent.height)) {
        g_extentCreates.fetch_add(1);
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
    for (int left = g_submitFailAfter.load(); left >= 0;) {
        if (g_submitFailAfter.compare_exchange_weak(left, left - 1)) {
            if (left == 0) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
            break;
        }
    }
    return real(queue, count, infos, fence);
}

#if defined(__SANITIZE_THREAD__)
// ThreadSanitizer's in-process symboliser opens and closes files while it holds the sanitizer's report
// lock. The ledger and the gates below take mutexes, which re-enter the sanitizer and deadlock it, so
// a call from the sanitizer's own runtime goes straight through. Nothing in it opens a wallpaper.
static const wallpaperx::testing::LoadedRange g_sanitizerRuntime{"libtsan.so"};
static bool calledFromSanitizer(const void* returnAddress) { return g_sanitizerRuntime.contains(returnAddress); }
#else
static constexpr bool calledFromSanitizer(const void*) { return false; }
#endif

// The processor-decoding scan's thread (backend/daemon/processor_check.cpp's lowerPriority()) is the one
// thread that sets itself to nice 19, before it opens anything. Marked from that call, the scan's
// read of each file is kept out of the ledger's counts and the close gate, and staged open failures
// are left for the decoder they are aimed at.
thread_local bool t_scanThread = false;

extern "C" int setpriority(__priority_which_t which, id_t who, int prio) noexcept {
    using Fn = int (*)(__priority_which_t, id_t, int);
    static const Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "setpriority"));
    if (which == PRIO_PROCESS && prio == 19 && (who == 0 || who == static_cast<id_t>(::gettid()))) t_scanThread = true;
    return real == nullptr ? -1 : real(which, who, prio);
}

// Holds the scan thread inside its open() of one armed path until released, so a phase can queue
// work behind a scan that provably has not finished. Bounded: an unreleased hold gives up after
// 20 s and says so through expired().
class ScanOpenGate {
public:
    void arm(const std::string& path) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_path = path;
        m_held = m_released = m_expired = false;
        m_armed.store(true, std::memory_order_release);
    }
    bool waitHeld(std::chrono::milliseconds within) {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_wake.wait_for(lock, within, [this] { return m_held; });
    }
    void release() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_released = true;
            m_armed.store(false, std::memory_order_release);
        }
        m_wake.notify_all();
    }
    bool expired() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_expired;
    }
    void pass(const char* path) {
        if (!m_armed.load(std::memory_order_acquire) || path == nullptr) return;
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_released || m_path != path) return;
        m_held = true;
        m_wake.notify_all();
        if (!m_wake.wait_for(lock, std::chrono::seconds(20), [this] { return m_released; })) m_expired = true;
    }

private:
    std::atomic<bool> m_armed{false};
    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::string m_path;
    bool m_held = false;
    bool m_released = false;
    bool m_expired = false;
};
ScanOpenGate g_scanOpenGate;

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
    if (calledFromSanitizer(__builtin_return_address(0))) return real(path, flags, mode);
    if (const int error = t_scanThread ? 0 : g_openFailure.failureFor(path); error != 0) {
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
    if (t_scanThread) g_scanOpenGate.pass(path);
    const int fd = real(path, flags, mode);
    if (fd >= 0 && g_files.opened(fd, path, t_scanThread)) t_lastWallpaperOpened = path;
    return fd;
}

extern "C" int close(int fd) {
    using Fn = int (*)(int);
    static const Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "close"));
    if (real == nullptr) return -1;
    if (calledFromSanitizer(__builtin_return_address(0))) return real(fd);
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
        if (g_crashThread.load() != 0 && ::gettid() != g_crashThread.load() && ::gettid() != ::getpid()) {
            g_decodeThreadSeen.store(true);
        }
        if (crashDueFor(CrashOn::Decode, context) && g_playbackFailure.hasDelivered(context)) {
            if (!g_deathNotePath.empty()) {
                char note[128];
                const int n = std::snprintf(note, sizeof(note), "%d %d %d %d\n", static_cast<int>(::gettid()),
                                            static_cast<int>(g_crashThread.load()), static_cast<int>(::getpid()),
                                            g_decodeThreadSeen.load() ? 1 : 0);
                const int fd = ::open(g_deathNotePath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
                if (fd >= 0) {
                    if (n > 0 && ::write(fd, note, static_cast<size_t>(n)) != n) ::unlink(g_deathNotePath.c_str());
                    ::close(fd);
                }
            }
            faultNow();
        }
        if (crashDueFor(CrashOn::CatchUp, context)) faultNow();
        if (packet != nullptr) g_recordWatch.packet();
        if (packet != nullptr) g_farRead.sent();
        if (packet != nullptr && packet->pts != AV_NOPTS_VALUE && g_passWatch.load(std::memory_order_relaxed)) {
            std::lock_guard<std::mutex> lock(g_passMutex);
            if (packet->pts < g_passLastPts) g_passStarts.push_back(std::chrono::steady_clock::now());
            g_passLastPts = packet->pts;
        }
        g_decodeCalls.fetch_add(1, std::memory_order_relaxed);
        if (::gettid() == g_countedThread.load()) g_countedThreadDecodes.fetch_add(1, std::memory_order_relaxed);
        if (const int delay = g_sendDelayMs.load(std::memory_order_relaxed); delay > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }
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
    if (result == 0 && isDecodersContext(context)) {
        g_playbackFailure.delivered(context);
        g_pictureDeliveries.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
}

// Noted before the real seek: the seek itself is inside the span a record must cover.
extern "C" int av_seek_frame(AVFormatContext* format, int stream, int64_t timestamp, int flags) {
    using Fn = int (*)(AVFormatContext*, int, int64_t, int);
    static const Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "av_seek_frame"));
    if (real == nullptr) return -1;
    if (crashDueAtSeek()) faultNow();
    g_recordWatch.seek();
    g_farRead.seek(format, stream, timestamp, flags);
    return real(format, stream, timestamp, flags);
}

extern "C" int av_read_frame(AVFormatContext* format, AVPacket* packet) {
    using Fn = int (*)(AVFormatContext*, AVPacket*);
    static const Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "av_read_frame"));
    if (real == nullptr) return -1;
    if (const int failure = g_farRead.readFailure(format); failure != 0) return failure;
    return real(format, packet);
}

namespace {

using Clock = std::chrono::steady_clock;
using wallpaperx::testing::getFrame;
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

std::string testAssetDir() { return wallpaperx::testing::testImageDir(); }
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
// 240 pictures a second with regular timestamps: twice what the cap lets through, at speed 1.
std::string rate240ClipPath() { return std::string(WALLPAPERX_VIDEO_ASSET_DIR) + "/wallpaper_rate240.mp4"; }

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

// The worker's thread id, asked on the worker; nothing if it does not answer.
std::optional<pid_t> workerThreadId(RenderWorker& worker) {
    auto answer = std::make_shared<std::promise<pid_t>>();
    std::future<pid_t> future = answer->get_future();
    worker.postTask([answer](RenderWorker::TaskState) { answer->set_value(::gettid()); });
    if (future.wait_for(std::chrono::seconds(60)) != std::future_status::ready) return std::nullopt;
    return future.get();
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

// The bus-served overload and readServedBgra() are shared in tests/served_pixels.h; this one reads the
// manager's own hand-over, which only this file's in-process phases see.
using wallpaperx::testing::FrameLayout;
using wallpaperx::testing::layoutOf;
using wallpaperx::testing::readServedBgra;

FrameLayout layoutOf(const wallpaperx::FrameHandle& handle) {
    const wallpaperx::ExportedFrame& frame = handle.frame;
    FrameLayout layout{frame.fd,
                       frame.width,
                       frame.height,
                       frame.memorySize,
                       frame.type == wallpaperx::ExportedFrame::HandleType::DmaBuf ? 1 : 0,
                       frame.modifier};
    for (const wallpaperx::ExportedFramePlane& plane : frame.planes) {
        layout.planeOffsets.push_back(plane.offset);
        layout.rowPitches.push_back(plane.rowPitch);
    }
    layout.drmFormat = frame.drmFormat;
    layout.deviceUuid.assign(frame.deviceUuid.begin(), frame.deviceUuid.end());
    if (handle.acquireFd >= 0) layout.syncFds.push_back(handle.acquireFd);
    return layout;
}

PixelVerdict gradeCoverCrop(const FrameLayout& frame) {
    PixelVerdict verdict;
    const std::vector<unsigned char> image = readServedBgra(frame, verdict.reason);
    if (image.empty()) return verdict;
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

        const unsigned char* pixels = image.data() + static_cast<size_t>(row) * frame.width * 4;
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
    //       GetFrame serves it, and GetStatus reports the monitor as renderable with a frame and
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

    const wallpaperx::testing::Frame frame = getFrame(proxy, kMonitorClip);
    check(frame.fd.get() >= 0, "GetFrame serves the video monitor's frame");
    check(frame.generation >= 1, "the served frame names a generation");

    // GOAL: S13.115's first half -- the picture is drawn at the monitor's target size, not at the
    //       clip's. False for a daemon that sizes the frame from the decoded picture, which would
    //       hand the plugin a 320x240 frame for a 64x24 screen.
    // WHY-THIS-SHAPE: the frame's own published width and height, which is what an importer builds
    //       its texture from; the target size is deliberately a different aspect AND both axes
    //       differ from the clip's, so a swap or a passthrough cannot satisfy it.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, the render pass's `ensureTargets(monitorId, size)`
    //       given the size with its axes swapped when the pass draws a video -> "FAIL: the video frame is
    //       served at the monitor's target size, not the clip's", alone (the pixel check below reads the
    //       frame at its own size and stays green). Via control_batch.sh, --phase '^bus$', 2026-10-14,
    //       NVIDIA. Reverted.
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
    //       The pixels are read by readServedBgra() (tests/served_pixels.h): a LINEAR dma_buf is mapped, an opaque fd
    //       (NVIDIA) is imported by the plugin's own GL importer on the exporting GPU, waiting on the
    //       reply's sync_fds. Mapping alone would leave the target GPU's path ungraded.
    // CONTROL: renderer/renderers/cover_crop.h's placedRegion() made to return the whole source onto the
    //       whole target for Cover -- a stretch -> "FAIL: the video picture is cover-cropped to the monitor,
    //       like a still image", on video_wallpaper and video_wallpaper_lavapipe. Via control_batch.sh, phase
    //       bus, 2026-10-18. Reverted.
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
    // CONTROL: a video request made to re-render every other monitor (requestWallpaper() in
    //       backend/daemon/wallpaper_manager.cpp bumping their requestSeq, DbusService::setWallpaper in
    //       backend/daemon/dbus_service.cpp posting them) -> "FAIL: a still wallpaper on another monitor is
    //       unchanged by the video render" and the two lines after it. Via control_batch.sh, --phase
    //       '^bus$', 2026-10-14, NVIDIA. Reverted.
    const StatusReply stillAfter = getStatusOrExplain(proxy);
    const nlohmann::json* stillEntry = wallpaperStatus(stillAfter, std::to_string(kMonitorStill));
    const bool untouched = stillEntry != nullptr && stillEntry->value("has_frame", false) &&
                           stillEntry->value("generation", uint64_t{0}) == stillGeneration &&
                           !stillEntry->contains("last_error");
    check(untouched, "a still wallpaper on another monitor is unchanged by the video render");
    const wallpaperx::testing::Frame stillFrame = getFrame(proxy, kMonitorStill);
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

// Resets the file ledger's counts, first asserting it recorded `path` being opened. A count of zero
// read after the reset is evidence only while the hook records opens at all, and under
// WALLPAPERX_TEST_PHASE no other phase need have shown that, so every phase that grades a zero proves
// it for itself. Call only where the phase has itself just caused that open.
// GOAL: each phase that asserts "no file was opened" first sees its own open recorded. False for a
//       ledger gone blind (the hook bypassed), where every zero-open check would pass.
// WHY-THIS-SHAPE: a harness fault, not check(): the phase has just drawn the video, so the daemon
//       certainly opened it, and a ledger that did not see it is this file's instrument failing.
// CONTROL: `g_files.opened(fd, path)` taken out of this file's open() hook, via control_batch.sh,
//       2026-09-29, each phase run alone -> "FAIL (harness): the resize phase: the file ledger recorded
//       no open of ...", the same line for the clock, one-picture, speed and GPU-fault phases, and "1
//       fault(s) in this test file, not in the daemon" -- except one-picture, whose open-file checks read
//       the same blind ledger, fail as checks and end the run first. Restored.
void resetCountsAfterOpenOf(const std::string& path, const char* phase) {
    if (g_files.opens(path) < 1) {
        wallpaperx::testing::harnessFailure(std::string(phase) + ": the file ledger recorded no open of " + path +
                                            " although this phase just drew it, so the open() hook no longer sees "
                                            "the daemon's opens and every zero-open count after this is unmeasured");
    }
    g_files.resetCounts();
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
    shape.reason = exportDelivered(manager, monitorId).reason;
    return shape;
}

void runUndecodableChecks(const WallpaperManager::Config& config, const std::string& junk) {
    const char* kGoal = "S13.116 (an undecodable video fails like an undecodable image)";
    PhaseGrade grade("the undecodable phase");
    WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};

    // GOAL: S13.116 -- a file that is not decodable as a video fails exactly as an undecodable
    //       image does: last_error is set, no frame, GetFrame answers RenderFailed, and the GPU
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
    //       decode does not mark the GPU failed". Observed 2026-09-21. Reverted.
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
                    wallpaperx::FrameExport e = exportDelivered(manager, monitorId);
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
    //       DECODES ARE COUNTED ON THE RENDER THREAD ONLY: the parked session's decode thread keeps
    //       refilling its two-picture queue (kDepth) inside this window, which is the session's own work
    //       and not the pass's. A pass that rebuilt the session still shows, as an open.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, dropStaleVideoSessionLocked()'s keep-it
    //       `return` disabled (`&& false`), so every pass reopens its file -> "FAIL: a resize of a
    //       monitor with a resident session opens and decodes nothing", printing "1 open(s), 1 packet(s)
    //       on the render thread by the rebuild; 1 open(s), 1 by the commit (7 on any thread)".
    //       control_batch.sh, --phase '^resize$', 2026-10-19 (T5c), NVIDIA. Reverted.
    // COVERAGE-ASSERT: the gate is asserted to have fired, so "nothing was decoded" is never
    //       concluded from a rebuild that never happened; the render thread's id is asserted known.
    const std::optional<pid_t> renderThread = workerThreadId(worker);
    check(renderThread.has_value(), "the render worker says which thread it is");
    if (!renderThread) return;
    g_countedThread.store(*renderThread);
    g_decodeCalls.store(0);
    g_countedThreadDecodes.store(0);
    resetCountsAfterOpenOf(clip, "the resize phase");
    g_framebufferGate.arm(CallGate::Mode::Hold, extentOf(kResizedWidth, kResizedHeight));
    resize("DP-ORDER", kResizedWidth, kResizedHeight);
    const bool rebuilding = g_framebufferGate.waitFired(kGateLimit);
    check(rebuilding, "a new size makes the render thread rebuild the video monitor's images");
    const int opensAtRebuild = g_files.totalOpens();
    const int decodesAtRebuild = g_countedThreadDecodes.load();
    g_framebufferGate.release();
    const bool resizedCommit = signals.wait(kMonitorOrder, 2);
    check(resizedCommit, "and the redraw commits once the rebuild is let go");
    if (rebuilding && resizedCommit) {
        const bool nothing = opensAtRebuild == 0 && decodesAtRebuild == 0 && g_files.totalOpens() == 0 &&
                             g_countedThreadDecodes.load() == 0;
        check(nothing, "a resize of a monitor with a resident session opens and decodes nothing");
        if (!nothing) {
            std::fprintf(stderr,
                         "  %d open(s), %d packet(s) on the render thread by the rebuild; %d open(s), %d by the commit "
                         "(%d on any thread)\n",
                         opensAtRebuild, decodesAtRebuild, g_files.totalOpens(), g_countedThreadDecodes.load(),
                         g_decodeCalls.load());
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
    // CONTROL: in backend/daemon/wallpaper_manager.cpp's renderPassLocked(), an
    //       `if (ensureTargets(monitorId, size)) rebuilt = true;` put just above
    //       `clip = &videoSessionLocked(...)` -> "FAIL: a new video wallpaper is decoded before the
    //       monitor's images are rebuilt". Via control_batch.sh, --phase '^resize$', 2026-10-18,
    //       NVIDIA. Reverted.
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
    //       goal forbids). Observed 2026-09-18. Reverted.
    if (!firstFrame(kMonitorRace, "DP-RACE")) {
        check(false, "the monitor the overtaken video pass is about is drawn first");
        return;
    }
    g_decodeCalls.store(0);
    g_countedThreadDecodes.store(0);
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
        //       same window and no counter could say which of the two did it. Packets counted on the
        //       render thread only, as above: the session's decode thread refills its queue meanwhile.
        // CONTROL: in backend/daemon/wallpaper_manager.cpp, `m_videoSessions.erase(monitorId);` added
        //       before the superseded `return RenderPass{{}, true};` in the commit block -> "FAIL: the
        //       retry after an overtaken rebuild opens and decodes nothing", printing "1 open(s), 1
        //       packet(s) on the render thread across the overtaken pass and its retry (7 on any thread)".
        //       The session is keyed by id now, so the break is `m_videoSessions.erase(ref.id)` of
        //       videoSessionOfLocked(monitorId). control_batch.sh, --phase '^resize$', 2026-10-19 (T5c),
        //       NVIDIA. Reverted.
        const bool retried = signals.wait(kMonitorRace, 2);
        check(retried, "the retry after the overtaken rebuild commits");
        if (retried) {
            const bool nothing = g_files.totalOpens() == 0 && g_countedThreadDecodes.load() == 0;
            check(nothing, "the retry after an overtaken rebuild opens and decodes nothing");
            if (!nothing) {
                std::fprintf(stderr,
                             "  %d open(s), %d packet(s) on the render thread across the overtaken pass and its retry "
                             "(%d on any thread)\n",
                             g_files.totalOpens(), g_countedThreadDecodes.load(), g_decodeCalls.load());
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
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, videoSessionLocked()'s attempt left without
    //       its ApplyingRecord (`applying.emplace(...)` made a no-op) -> "FAIL: a crash record is on
    //       disk while the video is being decoded", and the eviction phase's positive reading ("and a
    //       record naming the new file IS on disk ..."), nothing else. Observed 2026-09-21 on the
    //       NVIDIA entry; this phase re-run via control_batch.sh, --phase '^crashRecordSpan$',
    //       2026-10-18, NVIDIA. Reverted.
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
// Stage E2: decode sessions kept between passes (one per file and speed; these checks give each monitor
// its own copy of the file, so here a session is also one monitor's)
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

// The monitor's current frame against the cover crop, read as readServedBgra() reads it.
void gradeServedPicture(WallpaperManager& manager, int monitorId, const char* goal, const char* what) {
    wallpaperx::FrameExport exported;
    try {
        exported = exportDelivered(manager, monitorId);
    } catch (const std::exception& e) {
        reportUnless(false, what, std::string("the export threw: ") + e.what());
        return;
    }
    if (!exported.handle) {
        check(false, (std::string(what) + " -- the frame is not even served").c_str());
        return;
    }
    const PixelVerdict verdict = gradeCoverCrop(layoutOf(*exported.handle));
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
    if (verdict.wrong == 0) {
        std::printf("video_wallpaper_test: %s -- %d monitor rows checked against the cover crop\n", goal,
                    verdict.compared);
    }
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
    // GOAL: the counts here are the decoder's alone: the processor-decoding scan's own read of each file
    //       (t_scanThread) is neither counted as a session's open nor taken for a resident session.
    // WHY-THIS-SHAPE: told apart by the thread, not by waiting for the scan: it runs at idle priority on its
    //       own schedule, so a wait would be a bound on the scheduler, and a scan that never opened would pass.
    // CONTROL: control_batch.sh 2026-10-24 (T18a), --phase '^session$', reverted: lowerPriority()'s setpriority
    //       deleted (backend/daemon/processor_check.cpp) -> this line red, "committed, 2 open(s), 1 close(s), 3
    //       packet(s)", and the ninth-video lines. Test-side, by hand, restored: every watched open filed as the
    //       scan's -> this line and eight more red, "0 open(s)".
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
    //       render() made `static`, so a session keeps the crop of the first size it drew -> "FAIL: after
    //       a size report the monitor shows the video, cover-cropped at the new size", printing "8 of 36
    //       sampled rows show the wrong band; first: monitor row 0 samples clip row 41.7, which is red
    //       (255,0,0), but reads (19,206,2)", on video_wallpaper (NVIDIA, opaque fd read through the GL
    //       importer) and video_wallpaper_lavapipe alike. (a) observed 2026-09-16; (b) via
    //       control_batch.sh, --phase '^session$', 2026-10-08. Reverted.
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
        wallpaperx::FrameExport probe = exportDelivered(manager, v0);
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
                        "this host exports opaque file descriptors, which carry no layout (no DRM modifier) for a "
                        "report to exclude, so no report re-posts a monitor that has a frame -- the clause is "
                        "inapplicable to an opaque-fd export, not ungraded on it");
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
        wallpaperx::FrameExport held = exportDelivered(manager, v0, kConsumer);
        check(held.handle.has_value(), "a consumer is handed the video monitor's frame while a render is owed");
        if (held.handle) {
            const uint64_t bufferId = held.handle->frame.bufferId;
            if (held.handle->frame.fd >= 0) close(held.handle->frame.fd);
            if (held.handle->acquireFd >= 0) close(held.handle->acquireFd);
            const std::vector<int> retry =
                manager.releaseBuffer(kConsumer, v0, bufferId, wallpaperx::testing::handoverOf(*held.handle));
            check(contains(retry, v0), "releasing that image posts the video monitor");
            const PassCost released = renderCounted(manager, v0);
            reportUnless(released.outcome.rendered && released.outcome.generation == generation + 1,
                         "the release's pass of a video monitor commits the next frame", describe(released));
            reportUnless(released.opens == 0 && released.decodes == 0 && released.closes == 0,
                         "the release's pass of a video monitor opens, decodes and closes nothing", describe(released));
        }
    }

    // GOAL: S13.124 -- giving a monitor alone on a file the same video path again opens a new session
    //       and closes the old one, leaving one. (A screen re-set to a file another screen plays joins
    //       that session instead: S15.820.)
    // CONTROL: `++state.wallpaperSeq;` deleted from requestWallpaper() in
    //       backend/daemon/wallpaper_manager.cpp, after its save is queued -> "FAIL: the same video path
    //       given again opens a new session and closes the old one", printing "committed, 0 open(s), 0
    //       close(s), 0 packet(s)", with the ninth-video ceiling checks after it red too. Via
    //       control_batch.sh, --phase session, 2026-09-29, NVIDIA. Reverted.
    manager.requestWallpaper("video", clips[0], v0);
    const PassCost again = renderCounted(manager, v0);
    check(again.outcome.rendered, "the same video given again commits");
    reportUnless(again.opens == 1 && again.closes == 1 && again.decodes > 0,
                 "the same video path given again opens a new session and closes the old one", describe(again));
    reportUnless(g_files.live().count(clips[0]) == 1, "and one session holds that file",
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
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, evictIdleVideoSessionLocked()'s skip of a
    //       session one of whose screens shows its current picture (`if (showing) continue;`) made
    //       a no-op -> "FAIL: a ninth video wallpaper asked for while all eight play opens nothing
    //       and commits nothing" and five more in this phase. Via control_batch.sh, --phase session,
    //       2026-10-18, NVIDIA. Raising
    //       kMaxVideoSessions cannot be used as the control: the static_assert above stops the
    //       build, which is the point of tying the monitor count to the constant. Reverted.
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
        // CONTROL: in classifyFailure() (backend/daemon/wallpaper_manager.cpp), the ResourceExhausted arm
        //       returning `RenderFailure{e.what(), false, false, false}` -- the file's fault -> "FAIL: the
        //       refusal is recorded as the daemon's fault, not the file's", 2026-10-18 (T2), control_batch.sh,
        //       NVIDIA. Clearing `daemonFault` alone cannot redden it: `exhausted` makes fault() Shortage,
        //       which GetStatus also reports as the daemon's. Changing the thrown text in videoSessionLocked()
        //       to drop the count reddens the naming clause instead (2026-09-18). Reverted.
        const std::optional<wallpaperx::MonitorStatus> status = monitorStatus(manager, refused);
        const std::string error = status ? status->lastError : std::string();
        reportUnless(error.find("at most " + std::to_string(WallpaperManager::kMaxVideoSessions) + " videos") !=
                         std::string::npos,
                     "the refused monitor's error names the session limit that caused it", "last_error: " + error);
        reportUnless(status.has_value() && status->lastErrorIsDaemonFault,
                     "the refusal is recorded as the daemon's fault, not the file's",
                     status ? "daemon fault: no" : "the monitor has no status");
        reportUnless(exportDelivered(manager, refused).reason == wallpaperx::NoFrameReason::RenderFailed,
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
        // GOAL: S13.126 -- the ninth monitor, once given a session, shows its video cover-cropped,
        //       not merely "committed".
        // CONTROL: renderer/renderers/cover_crop.h's placedRegion() returning the whole source for Cover ->
        //       "FAIL: the ninth monitor's frame shows its video, cover-cropped", on video_wallpaper and
        //       video_wallpaper_lavapipe. Via control_batch.sh, phase session, 2026-10-18. Reverted.
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
    // CONTROL: control_batch.sh 2026-10-18, phase failedPass, NVIDIA, reverted, backend/daemon/wallpaper_manager.cpp:
    //       leaveVideoSessionLocked() closing the last member's session inside an `ApplyingRecord` naming the
    //       file -> "FAIL: no crash record is on disk while a session is torn down for a fault that is not
    //       the file's" and "FAIL: a start after dying in that teardown applies the video ...". This failure
    //       (an image shortage) leaves the session through leaveVideoSessionLocked(), not
    //       closeFailedVideoSessionLocked(): the same record added there reddens only the S13.129 pair below.
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
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, renderPassLocked()'s first `if (failure)
    //       tearDownFailed();` -- before the commit block, whose `dropFailedClip()` frees the session --
    //       deleted -> "FAIL: a failed, overtaken pass leaves no session: the newer pass opens its own".
    //       Re-run 2026-10-18 (T2) via control_batch.sh on NVIDIA and Lavapipe. Reverted.
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
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, renderPassLocked()'s `tearDownFailed` lambda's
    //       `if (failure->fault() != Fault::File) applying.reset();` deleted, so the pass's record stays on
    //       disk through the teardown -> "FAIL: no record naming the video is on disk while a session whose
    //       draw failed for the daemon's reason is torn down". Re-run 2026-10-18 (T2) via control_batch.sh,
    //       NVIDIA. Reverted.
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
    //       decode are running". Each observed 2026-09-21 on the NVIDIA entry, (a) together with the
    //       applying-record control in runPlaybackBlameChecks. (a) re-run via control_batch.sh, --phase
    //       '^evictionRecord$', 2026-09-30, now that an eviction frees through releaseVideoSessionLocked():
    //       the same line, printing the record naming video_parked_0.mp4. Reverted. (a) and (b) re-run
    //       2026-10-18 (T2) via control_batch.sh, NVIDIA: (a) as an ApplyingRecord naming the first member
    //       around the pipeline's reset in closeVideoSessionLocked(). Reverted.
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
// Recorded into before a FrameReadyLog, never after: a wait on that log returns the moment it
// records, and a read of this one straight after must already hold the picture waited for.
// CONTROL: test-side, the clock phase, 2026-09-29, a 50 ms sleep between the two records: in the old
//       order, via control_batch.sh -> "FAIL: enough successive pictures were compared to grade their
//       order"; in this order, by hand -> green, "20 pictures took 2.000 s".
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

// When `generation` was first announced in `events`, or nothing when it never was. The one rule every
// timing check here uses for "when a picture was shown".
std::optional<Clock::time_point> firstSeen(const std::vector<PictureLog::Event>& events, uint64_t generation) {
    for (const PictureLog::Event& e : events) {
        if (e.generation == generation) return e.at;
    }
    return std::nullopt;
}

std::optional<Clock::time_point> firstSeen(const PictureLog& pictures, int monitorId, uint64_t generation) {
    return firstSeen(pictures.forMonitor(monitorId), generation);
}

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
// owns it, and exporting a frame is a GPU call. It is also what the daemon does -- getFrame posts
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
            const wallpaperx::FrameExport e = exportDelivered(manager, monitorId, client);
            out.reason = e.reason;
            out.firstRefusal = e.firstRefusal;
            if (e.handle) {
                out.served = true;
                out.bufferId = e.handle->frame.bufferId;
                out.generation = e.handle->generation;
                if (e.handle->frame.fd >= 0) close(e.handle->frame.fd);
                if (e.handle->acquireFd >= 0) close(e.handle->acquireFd);
                if (!keep) wallpaperx::testing::releaseHandle(manager, client, monitorId, *e.handle);
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
            const wallpaperx::FrameExport e = exportDelivered(m_manager, m_monitorId, m_client);
            if (!e.handle) {
                m_refused.fetch_add(1);
                return;
            }
            if (e.handle->frame.fd >= 0) close(e.handle->frame.fd);
            if (e.handle->acquireFd >= 0) close(e.handle->acquireFd);
            wallpaperx::testing::releaseHandle(m_manager, m_client, m_monitorId, *e.handle);
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
        pictures.record(monitorId, generation);
        signals.record(monitorId, generation);
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
    resetCountsAfterOpenOf(clip, "the clock phase");

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
    //       commitFrameLocked() -- reddens the ordering check instead. Observed 2026-09-20. Reverted.
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
        //       kMinPictureInterval always, via control_batch.sh, 2026-09-29, this phase alone -> "FAIL: the
        //       pictures are paced by the file's own rate", printing "20 pictures took 0.166692 s", where
        //       the file's own rate makes them 2.000 s. Reverted. Re-run at the default frame rate 60,
        //       control_batch.sh 2026-10-20 (O1), --phase '^clock$', NVIDIA -> the same line, 0.166670 s
        //       (green 2.000 s): kMinPictureInterval stays 1/120 s whatever the setting, so the rate is
        //       not what this check depends on.
        const std::optional<Clock::time_point> from = firstSeen(shown, kRateFrom);
        const std::optional<Clock::time_point> to = firstSeen(shown, kGradedPictures);
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
        //       which is why the reopen count is asserted beside it. Observed 2026-09-18; re-run via
        //       control_batch.sh, --phase '^clock$', 2026-10-18, NVIDIA. Reverted.
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
    //       monitor; observed 2026-09-20. Re-run against the bound below via control_batch.sh, --phase
    //       '^onePicture$', 2026-10-19 (T5c), NVIDIA: "7 picture(s) delivered, 14 packet(s)" and "7
    //       further announcement(s)". Reverted.
    //       BOUNDED BY THE DECODE QUEUE, NOT ZERO: the session's decode thread keeps up to kDepth (2,
    //       renderer/video/decode_ahead.cpp) pictures ready, and with this file each is the one picture
    //       again after a restart. After the park nothing takes from that queue, so the thread can
    //       deliver at most kDepth more pictures whatever the interleaving, and then waits; a clock that
    //       kept asking takes a picture every round and the thread delivers one per round after it.
    //       Packets are not bounded this way (a restart's flush sends more than one per picture).
    constexpr int kQueueDepth = 2;
    g_decodeCalls.store(0);
    g_pictureDeliveries.store(0);
    resetCountsAfterOpenOf(onePicture, "the one-picture phase");
    const size_t announced = signals.countFor(kMonitorParked);
    std::this_thread::sleep_for(config.idleSessionRelease / 2);
    reportUnless(g_pictureDeliveries.load() <= kQueueDepth, "a video file holding one picture is never decoded again",
                 std::to_string(g_pictureDeliveries.load()) + " picture(s) delivered, " +
                     std::to_string(g_decodeCalls.load()) + " packet(s), in a quiet window; at most " +
                     std::to_string(kQueueDepth) + " refill the decode queue");
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
    //       reduced to `if (!session.idleSince) session.idleSince = now; return std::nullopt;` -- the
    //       delay choice and the releaseVideoSessionLocked() call gone -- so nothing is ever freed ->
    //       "FAIL: a parked one-picture video's decoder and file are released once it has been idle",
    //       printing wallpaper_one_picture.mp4 still open after the wait. Re-run via control_batch.sh,
    //       --phase '^onePicture$', 2026-10-18, NVIDIA, as the stopped branch's
    //       releaseVideoSessionLocked() call made a no-op. Reverted.
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
        pictures.record(monitorId, generation);
        signals.record(monitorId, generation);
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
    //       made to `return std::nullopt;` in place of choosing the delay and calling
    //       releaseVideoSessionLocked() -- so nothing is ever freed -> "FAIL: a video stopped for a few
    //       seconds has its file and decoder released", printing wallpaper_bands.mp4 still open, and the
    //       reopen line below ("0 open(s) of the clip since the hand-over"). Making it release
    //       immediately (`const SteadyTime releaseAt = *session.idleSince;`) reddens "the stopped video
    //       still holds its file open just after stopping", printing "open files: none" -- and a dozen
    //       other checks in this file with it, since a session freed the moment it stops breaks every
    //       reuse and resize property Stage E2 established. Both re-run via control_batch.sh, --phase
    //       '^offSwitch$', 2026-09-30; the first again 2026-10-18, NVIDIA, as the stopped branch's
    //       releaseVideoSessionLocked() call made a no-op, with the same two lines red. Reverted.
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
    // CONTROL: backend/daemon/wallpaper_manager.cpp, the unwatched arm of the release delay in
    //       advanceSessionLocked() computed from kIdleSessionRelease instead of
    //       m_config.idleSessionRelease, via control_batch.sh, --phase '^offSwitch$', 2026-09-30 ->
    //       "FAIL: the stopped video is released after the configured idle interval, not sooner or
    //       later", released 5.811 s after its first picture where 2.750 s was due. Reverted.
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
    //       whose session was released opens its file afresh to do it: nothing of the old session
    //       survives. Which picture the fresh open shows -- the one on screen when it stopped -- is
    //       S15.151's hand-over half, graded by frame number in runResumeChecks. False for a daemon that leaves a
    //       stopped video stopped -- a desktop that comes back to a frozen wallpaper for ever -- and
    //       false for one that needs a render to be posted before it will reopen, since nothing
    //       posts one: the monitor's request is already serviced.
    // WHY-THIS-SHAPE: nothing but the hand-over is done. No render is posted and no size reported,
    //       so the pictures that follow can only have come from the daemon deciding for itself that
    //       the monitor is watched again. The reopen is counted from a reset taken before the
    //       hand-over, so a file opened during the release above is not credited to it.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, the `it->second.uncollectedSince.reset();` line
    //       deleted from recordDeliveredLocked() -> "FAIL: a hand-over to a named consumer starts a
    //       stopped video again", after the 30-second signal limit; re-run via control_batch.sh,
    //       --phase '^offSwitch$', 2026-10-05. Deleting the reopen
    //       loop body in reopenVideosLocked() -- `if (!owed) continue;` changed to `continue;` -- reddens
    //       the same line for the other half, since the monitor becomes watched with no session to play
    //       from; observed 2026-09-20. The reopen count's own control is the never-release break above,
    //       which leaves nothing to reopen: "0 open(s) of the clip since the hand-over". Reverted.
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
                     "and a video whose session was released opens its file again to do it",
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
// A consumer that takes the newest picture and gives it straight back, as the KDE plugin does, without
// reading it. A refused fetch is an ordinary answer.
// GOAL: S15.182 -- the daemon takes back every buffer it has just served to a consumer that is still on
//       the bus; a refusal is the daemon's failure, never folded into "the video stalled".
// WHY-THIS-SHAPE: graded in the destructor, so every path out of the phase -- a notVerified return
//       included -- grades it; one try around both calls would swallow the release refusal as if it
//       were a refused fetch, which is how the phases' copies of this loop hid it.
// CONTROL: via control_batch.sh, 2026-10-01, reverted: DbusService::releaseBuffer
//       (backend/daemon/dbus_service.cpp) throwing InvalidRequest before the manager call -> "FAIL: the
//       daemon takes back every buffer it served to the pause phase's consumer", and the visibility and
//       speed phases' lines, each phase run alone, printing "the daemon refused to take back buffer 1 (2,
//       6) it had just served: org.freedesktop.DBus.Error.InvalidArgs"; the stalls that follow are red too.
//       The pause phase's line re-run 2026-10-05.
class GiveBack {
public:
    GiveBack(sdbus::IProxy& proxy, std::string what) : m_proxy(proxy), m_what(std::move(what)) {}
    GiveBack(const GiveBack&) = delete;
    GiveBack& operator=(const GiveBack&) = delete;
    ~GiveBack() {
        check(m_releaseFault.empty(), m_what.c_str());
        if (!m_releaseFault.empty()) std::fprintf(stderr, "  %s\n", m_releaseFault.c_str());
    }

    // Whether a picture was served.
    bool take(int32_t monitorId) {
        std::optional<wallpaperx::testing::Frame> frame;
        try {
            frame.emplace(getFrame(m_proxy, monitorId));
        } catch (const sdbus::Error&) {
            return false;
        }
        try {
            wallpaperx::testing::releaseFrame(m_proxy, monitorId, *frame);
        } catch (const sdbus::Error& e) {
            if (m_releaseFault.empty()) {
                m_releaseFault = "the daemon refused to take back buffer " + std::to_string(frame->bufferId) +
                                 " it had just served: " + e.getName() + ": " + e.getMessage();
            }
        }
        return true;
    }

private:
    sdbus::IProxy& m_proxy;
    std::string m_what;
    std::string m_releaseFault;
};

void runVisibilityChecks(sdbus::IProxy& proxy, const PictureLog& pictures, const std::string& clip,
                         const WallpaperManager::Config& config) {
    using wallpaperx::testing::setMonitorOutput;
    using wallpaperx::testing::setOutputVisible;
    const char* kGoals = "S13.154-S13.157 (a collecting consumer keeps a video playing, a covered screen's video stops "
                         "the screen coming back starts it again)";
    const std::string screen = "DP-VIS";
    GiveBack giveBack(proxy, "the daemon takes back every buffer it served to the visibility phase's consumer");

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
    const auto collectUntilQuiet = [&giveBack, &pictures](std::chrono::milliseconds quiet, std::chrono::seconds limit) {
        Watched out;
        out.newest = latestGeneration(pictures, kMonitorVisible);
        Clock::time_point lastChange = Clock::now();
        const Clock::time_point deadline = Clock::now() + limit;
        while (Clock::now() < deadline) {
            const uint64_t now = latestGeneration(pictures, kMonitorVisible);
            if (now != out.newest) {
                out.newest = now;
                lastChange = Clock::now();
                if (giveBack.take(kMonitorVisible)) ++out.collected;
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
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, recordDeliveredLocked() made to set
    //       `uncollectedSince = now` instead of resetting it -- the obvious wrong rule, "how long
    //       since the last hand-over" -> the video still plays here, because this file's pictures
    //       are a tenth of a second apart; it is runSlowPictureChecks that reddens on that
    //       mutation (observed 2026-09-20). Deleting its `it->second.uncollectedSince.reset();`
    //       instead -> "FAIL: a video whose pictures are being collected keeps playing", via
    //       control_batch.sh, --phase '^visibility$', 2026-10-05. Reverted.
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
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, recordDeliveredLocked() given a loop that
    //       also erases this monitor's screen from m_outputsNotDrawn -- "the hand-over is evidence
    //       the screen is being drawn", which is the shape this check exists to forbid -> "FAIL: a
    //       covered screen's video is not restarted by a consumer that keeps fetching". Observed via
    //       control_batch.sh, --phase '^visibility$', 2026-10-05. Reverted.
    //       Deleting watchedLocked()'s `if (!screenDrawnLocked(monitorId)) return false;` is NOT a
    //       usable control here: the video then never stops, so the phase abandons at the check
    //       above and this line is never reached.
    int servedWhileCovered = 0;
    const Clock::time_point coveredUntil = Clock::now() + kQuietWindow;
    while (Clock::now() < coveredUntil) {
        if (giveBack.take(kMonitorVisible)) ++servedWhileCovered;
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

// --------------------------------------------------------------------------- //
// Step 15 chunk 3b: `speed` on a playing video, over the bus -- the method, the manager and the
// worker being woken are the chain under test, so a manager-level phase would stand in for the last.
// --------------------------------------------------------------------------- //

constexpr int32_t kMonitorSpeed = 0;
constexpr int32_t kMonitorSpeedStill = 1;
constexpr int32_t kMonitorSpeedBarrier = 2;

void runSpeedChecks(sdbus::IProxy& proxy, const PictureLog& pictures, const std::string& clip, const std::string& still,
                    PhaseGrade& grade) {
    using wallpaperx::testing::errorNameFrom;
    using wallpaperx::testing::setWallpaperProperties;
    using wallpaperx::testing::setWallpaperWithProperties;
    const char* kGoals = "S15.99-S15.101 (speed on a playing video: its rate, the picture already waiting, no restart)";
    const auto speed = [](double value) { return wallpaperx::testing::WireProperties{{"speed", sdbus::Variant(value)}}; };

    // Takes each newly announced picture and gives it straight back, as the KDE plugin does, until
    // `until` holds or `limit` passes; false when it never held. A video nobody collects stops after
    // unwatchedAfter, which would be graded as a slow one -- so what was collected is remembered
    // across calls, and the picture that ended one call is taken by the next.
    uint64_t collected = 0;
    GiveBack giveBack(proxy, "the daemon takes back every buffer it served to the speed phase's consumer");
    const auto collectUntil = [&](const std::function<bool()>& until, Clock::duration limit) {
        const Clock::time_point deadline = Clock::now() + limit;
        for (;;) {
            if (const uint64_t now = latestGeneration(pictures, kMonitorSpeed); now > collected) {
                collected = now;
                giveBack.take(kMonitorSpeed);
            }
            if (until()) return true;
            if (Clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    };
    const auto newest = [&pictures](int32_t monitorId) { return latestGeneration(pictures, monitorId); };

    // What says this host draws at all is the still, set through the plain method: the video below is
    // set through the path under test, so its silence must fail, never read as a host limit.
    if (!wallpaperx::testing::trySetWallpaper(proxy, "static", still, kMonitorSpeedStill) ||
        !settlesWithin(std::chrono::seconds(30), [&] { return newest(kMonitorSpeedStill) > 0; })) {
        grade.expectNoDevice();
        notVerified(kGoals, "the still this phase compares against never rendered on this host");
        return;
    }
    const std::string setError =
        errorNameFrom([&] { setWallpaperWithProperties(proxy, "video", clip, kMonitorSpeed, speed(2.0)); });
    check(setError.empty(), "a video can be set together with a speed");
    if (!setError.empty()) {
        std::fprintf(stderr, "  SetWallpaperWithProperties answered %s\n", setError.c_str());
        return;
    }
    // GOAL: S15.100 -- a video set together with a speed draws, on a host that has just drawn a still.
    // CONTROL: the `m_worker.post(monitorId);` that DbusService::setWallpaper
    //       (backend/daemon/dbus_service.cpp) ends with made `if (properties.empty())`, so only a call
    //       with properties is never posted -- via control_run.sh, 2026-09-29 -> "FAIL: a video set
    //       together with a speed commits a picture", the still having drawn. Reverted.
    if (!settlesWithin(std::chrono::seconds(30), [&] { return newest(kMonitorSpeed) > 0; })) {
        check(false, "a video set together with a speed commits a picture");
        return;
    }
    resetCountsAfterOpenOf(clip, "the speed phase");

    // GOAL: S15.100 -- a video set with speed 2 shows its pictures twice as fast as its file's own
    //       rate. False for a daemon that stores speed and never hands it to the clock.
    // WHY-THIS-SHAPE: the time a fixed NUMBER of pictures takes, never pictures counted in a fixed
    //       time. Speed 1 makes these 20 take 2.0 s and speed 2 1.0 s: the upper bound, 1.5 s, splits
    //       the two, and the lower, 0.5 s, is half the speed-2 time, which a clock free-running at the
    //       daemon's 1/120 s floor on one picture's interval (0.17 s) cannot meet; the frame-rate setting
    //       does not move that floor. The times are FrameReady receipts, so a client-side
    //       stall that delays generation 5's signal shortens the span; a burst of receipts under 1 ms
    //       apart (the daemon spaces these 50 ms, and even the floor 8.3 ms) means that happened, and the
    //       lower bound is then not graded. A stall only lengthens the span, so the upper bound always is.
    // CONTROL: `speed = playbackSpeed(it->second.properties);` in advanceSessionLocked
    //       (backend/daemon/wallpaper_manager.cpp) replaced by `speed = 1.0;`, via control_run.sh,
    //       2026-09-29 -> "FAIL: a video set with speed 2 shows its pictures twice as fast as its file's
    //       own rate", with the speed-0.01 check below. Reverted. Re-run via control_batch.sh, --phase
    //       '^speed$', 2026-10-14 -> the same line, with the speed-0.5 check below. That line is now
    //       `speed = session.speed;`; re-run 2026-10-18 via control_batch.sh, --phase '^speed$', NVIDIA, as
    //       `speed = 1.0;` after the round's speed-change block -> the same line and the speed-0.5 one.
    // CONTROL: gapBetween() (backend/daemon/wallpaper_manager.cpp) made to return kMinPictureInterval
    //       always, a clock free-running at the floor -> the same line, by the lower bound: "20 pictures
    //       took 0.166630 s". Reverted. Re-run at the default frame rate 60, control_batch.sh 2026-10-20 (O1),
    //       --phase '^speed$', NVIDIA -> the same line, 0.166632 s (green 1.000 s).
    // CONTROL (harness): FrameReady for generation 5 held 800 ms in this phase's proxy callback -- a
    //       client stall -> "NOT VERIFIED -- S15.100's lower bound ...", the span read 0.200 s; the same
    //       stall with the burst test disabled -> "FAIL: a video set with speed 2 shows its pictures
    //       twice as fast", a correct daemon accused. Both via control_run.sh 2026-09-29, reverted.
    constexpr uint64_t kFrom = 5;
    constexpr uint64_t kTo = 25;
    const bool reached = collectUntil([&] { return newest(kMonitorSpeed) >= kTo; }, std::chrono::seconds(30));
    check(reached, "a video set with speed 2 keeps committing pictures for a consumer that collects them");
    if (!reached) {
        notVerified(kGoals, "the video did not reach generation " + std::to_string(kTo) +
                                ", so there is no run of pictures to time -- a stalled clock is not a slow one");
        return;
    }
    {
        const std::vector<PictureLog::Event> shown = pictures.forMonitor(kMonitorSpeed);
        const std::optional<Clock::time_point> from = firstSeen(shown, kFrom);
        const std::optional<Clock::time_point> to = firstSeen(shown, kTo);
        const double atOne = static_cast<double>(kTo - kFrom) / kClipPictures;
        const double atTwo = atOne / 2;
        const double seconds = from && to ? std::chrono::duration<double>(*to - *from).count() : -1.0;
        std::optional<uint64_t> burstAt;
        for (uint64_t g = kFrom; g < kTo && !burstAt; ++g) {
            const std::optional<Clock::time_point> a = firstSeen(shown, g);
            const std::optional<Clock::time_point> b = firstSeen(shown, g + 1);
            if (a && b && *b - *a < std::chrono::milliseconds(1)) burstAt = g;
        }
        std::printf("video_wallpaper_test: at speed 2, %llu pictures took %.3f s (%.3f s at the file's own rate)\n",
                    static_cast<unsigned long long>(kTo - kFrom), seconds, atOne);
        std::fflush(stdout);
        const std::string detail = std::to_string(kTo - kFrom) + " pictures took " + std::to_string(seconds) +
                                   " s, where speed 2 makes them " + std::to_string(atTwo) +
                                   " s and the file's own rate " + std::to_string(atOne) + " s";
        if (burstAt) {
            reportUnless(from && to && seconds <= atOne * 0.75,
                         "a video set with speed 2 shows its pictures twice as fast as its file's own rate", detail);
            notVerified("S15.100's lower bound", "FrameReady receipts for generations " + std::to_string(*burstAt) +
                                                     " and " + std::to_string(*burstAt + 1) +
                                                     " arrived under 1 ms apart, so the client stalled and receipt "
                                                     "times cannot show a clock running too fast");
        } else {
            reportUnless(from && to && seconds <= atOne * 0.75 && seconds >= atTwo / 2,
                         "a video set with speed 2 shows its pictures twice as fast as its file's own rate", detail);
        }
    }

    // GOAL: S15.100's other direction, on a video already playing -- SetWallpaperProperties setting
    //       speed 0.5 slows it at once: in the next second it shows about five pictures, where speed 2
    //       shows twenty and speed 1 ten.
    // WHY-THIS-SHAPE: an upper bound only, since a slow machine only lowers the count; seven is five plus
    //       one under way plus one of slack, still three short of speed 1. Collecting throughout, so "few
    //       pictures" cannot be the off switch stopping an unwatched video.
    // CONTROL: the same `speed = 1.0;` mutation as above, via control_batch.sh, --phase '^speed$',
    //       2026-10-14, NVIDIA and Lavapipe -> "FAIL: a playing video set to speed 0.5 shows at most seven
    //       pictures in the next second", with the speed-2 line above. Reverted.
    const std::string slowError = errorNameFrom([&] { setWallpaperProperties(proxy, kMonitorSpeed, speed(0.5)); });
    check(slowError.empty(), "a playing video's speed can be changed with SetWallpaperProperties");
    const uint64_t slowFrom = newest(kMonitorSpeed);
    collectUntil([] { return false; }, std::chrono::seconds(1));
    const uint64_t slowShown = newest(kMonitorSpeed) - slowFrom;
    reportUnless(slowError.empty() && slowShown <= 7,
                 "a playing video set to speed 0.5 shows at most seven pictures in the next second",
                 std::to_string(slowShown) + " picture(s) in that second");

    // GOAL: S15.101 -- the picture left waiting when the speed changes is timed again at the new speed at
    //       once. At 0.5 a picture just shown has its successor due 200 ms out; raised to 2 at that moment,
    //       the successor is due within 50 ms. False for a clock that fixes a picture's time when it is
    //       decoded (about 200 ms), and for a daemon that does not wake the render thread, asleep until
    //       that old time.
    // WHY-THIS-SHAPE: the speed is raised right after a picture's FrameReady, so the old time is a whole
    //       interval away; five rounds and their MEDIAN graded against 125 ms, so one late receipt or one
    //       slow render neither accuses a correct daemon nor clears a wrong one. Each round first puts the
    //       video back to 0.5 and lets two pictures pass, so the wait raised is one timed at 0.5. Waited up
    //       to five seconds a round and every time printed, so a picture that comes at its old time reads
    //       as late, not as never.
    // CONTROL: via control_batch.sh, --phase '^speed$', 2026-10-14, NVIDIA and Lavapipe, each alone,
    //       reverted: `m_worker.wake();` deleted from DbusService::setWallpaperProperties
    //       (backend/daemon/dbus_service.cpp), and VideoSession::Waiting::retime
    //       (backend/daemon/wallpaper_manager.cpp) reduced to `return;` -> "FAIL: the picture left waiting at
    //       speed 0.5 is shown within 125 ms once the speed goes to 2", medians 198.8-199.2 ms (clean: 49.7).
    // COVERAGE-ASSERT: five rounds must each see the next picture, or the phase fails rather than grading
    //       fewer.
    {
        std::vector<double> waits;
        bool lost = false;
        for (int round = 0; round < 5 && !lost; ++round) {
            const std::string downError =
                errorNameFrom([&] { setWallpaperProperties(proxy, kMonitorSpeed, speed(0.5)); });
            const uint64_t before = newest(kMonitorSpeed);
            if (!downError.empty() ||
                !collectUntil([&] { return newest(kMonitorSpeed) >= before + 2; }, std::chrono::seconds(5))) {
                lost = true;
                std::fprintf(stderr, "  round %d: speed 0.5 answered '%s'; the video stopped at generation %llu\n", round,
                             downError.c_str(), static_cast<unsigned long long>(newest(kMonitorSpeed)));
                break;
            }
            const uint64_t shownAt = newest(kMonitorSpeed);
            const Clock::time_point raised = Clock::now();
            const std::string upError = errorNameFrom([&] { setWallpaperProperties(proxy, kMonitorSpeed, speed(2.0)); });
            const bool next = collectUntil([&] { return newest(kMonitorSpeed) > shownAt; }, std::chrono::seconds(5));
            const std::optional<Clock::time_point> came = firstSeen(pictures, kMonitorSpeed, shownAt + 1);
            if (!upError.empty() || !next || !came) {
                lost = true;
                std::fprintf(stderr, "  round %d: speed 2 answered '%s'; no picture after generation %llu\n", round,
                             upError.c_str(), static_cast<unsigned long long>(shownAt));
                break;
            }
            waits.push_back(std::chrono::duration<double, std::milli>(*came - raised).count());
        }
        check(!lost && waits.size() == 5, "five rounds of raising the speed from 0.5 to 2 each showed the next picture");
        if (!lost && waits.size() == 5) {
            std::vector<double> sorted = waits;
            std::ranges::sort(sorted);
            std::printf("video_wallpaper_test: speed 0.5 raised to 2 just after a picture; the next came %.1f, %.1f, "
                        "%.1f, %.1f and %.1f ms later (median %.1f; re-timed it is due within 50, unchanged 200)\n",
                        waits[0], waits[1], waits[2], waits[3], waits[4], sorted[2]);
            std::fflush(stdout);
            reportUnless(sorted[2] <= 125.0,
                         "the picture left waiting at speed 0.5 is shown within 125 ms once the speed goes to 2",
                         "the median wait was " + std::to_string(sorted[2]) + " ms");
        }
    }

    // GOAL: S15.99's video half -- a properties change keeps the playing video: no speed change above
    //       opened its file again, so it played on from the picture it had reached.
    // WHY-THIS-SHAPE: read from open(2), counted since the first picture; a restart shows the same
    //       generations climbing whether or not the file was opened again.
    // CONTROL: WallpaperManager::setWallpaperProperties (backend/daemon/wallpaper_manager.cpp) given
    //       `++it->second.requestSeq; ++it->second.wallpaperSeq;` and DbusService::setWallpaperProperties
    //       (backend/daemon/dbus_service.cpp) `m_worker.post(monitorId)` for its wake -- what
    //       SetWallpaper does -- the counters bumped after the save is queued, via control_batch.sh,
    //       --phase speed, 2026-09-29 -> "FAIL: no speed change reopened the playing video's file", "2
    //       open(s)", and the still check below red with it. Reverted.
    reportUnless(g_files.totalOpens() == 0, "no speed change reopened the playing video's file",
                 std::to_string(g_files.totalOpens()) + " open(s) since its first picture");

    // GOAL: S15.99's still half -- a properties change on a still draws nothing: its generation is
    //       where it was once a later render has been announced.
    // WHY-THIS-SHAPE: an absent picture is evidence only after a barrier: the render thread takes
    //       posted monitors in order, so a render SetWallpaperProperties had posted comes before the
    //       barrier monitor's, whose FrameReady is waited for.
    // CONTROL: the same re-set mutation as the check above -> "FAIL: a properties change on a still
    //       draws nothing", "the still is at generation 2, drawn at 1", in the same run. Reverted.
    const uint64_t stillAt = newest(kMonitorSpeedStill);
    const std::string stillError =
        errorNameFrom([&] { setWallpaperProperties(proxy, kMonitorSpeedStill, speed(2.0)); });
    check(stillError.empty(), "a speed can be set on a still");
    const bool barrier = wallpaperx::testing::trySetWallpaper(proxy, "static", still, kMonitorSpeedBarrier) &&
                         settlesWithin(std::chrono::seconds(30), [&] { return newest(kMonitorSpeedBarrier) > 0; });
    check(barrier, "the barrier render after the still's properties change is announced");
    if (barrier) {
        reportUnless(newest(kMonitorSpeedStill) == stillAt, "a properties change on a still draws nothing",
                     "the still is at generation " + std::to_string(newest(kMonitorSpeedStill)) + ", drawn at " +
                         std::to_string(stillAt));
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
    const auto takeAndKeepOn = [&](int monitorId, const std::string& consumer) {
        wallpaperx::FrameExport e = exportDelivered(manager, monitorId, consumer);
        if (e.handle) {
            open.push_back(e.handle->frame.fd);
            open.push_back(e.handle->acquireFd);
        }
        return e;
    };
    const auto takeAndKeep = [&](const std::string& consumer) { return takeAndKeepOn(kMonitorEvidence, consumer); };

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
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, recordDeliveredLocked()'s
    //       `if (client.empty()) return false;` deleted -> "FAIL: a hand-over with no bus name behind
    //       it does not count as collection", via control_batch.sh, --phase '^watchEvidence$', re-run
    //       2026-10-06. Reverted.
    {
        wallpaperx::FrameExport anonymous = exportDelivered(manager, kMonitorEvidence);
        check(anonymous.handle.has_value(), "an unnamed poll is served the monitor's picture");
        if (anonymous.handle) {
            if (anonymous.handle->frame.fd >= 0) close(anonymous.handle->frame.fd);
            if (anonymous.handle->acquireFd >= 0) close(anonymous.handle->acquireFd);
            check(!watched(kMonitorEvidence), "a hand-over with no bus name behind it does not count as collection");
            // Nothing to give back: an unnamed hand-over records no claim and carries no number.
        }
    }
    {
        wallpaperx::FrameExport named = exportDelivered(manager, kMonitorEvidence, ":1.900");
        check(named.handle.has_value(), "and a named consumer is served the same picture");
        if (named.handle) {
            if (named.handle->frame.fd >= 0) close(named.handle->frame.fd);
            if (named.handle->acquireFd >= 0) close(named.handle->acquireFd);
            check(watched(kMonitorEvidence), "which does count: a served hand-over to a named consumer is collection");
            wallpaperx::testing::releaseHandle(manager, ":1.900", kMonitorEvidence, *named.handle);
        }
    }

    // GOAL: S15.321 -- an export is not collection until the caller says the reply went out: before
    //       handoverDelivered() the stale monitor stays unwatched, after it the monitor is watched. This
    //       is what lets the bus record nothing for a reply it failed to send.
    // WHY-THIS-SHAPE: asked of a monitor proven stale first, so either half would flip at once if the
    //       evidence were recorded at the wrong moment; the hand-over itself is asserted served, so the
    //       first half cannot pass on an export that failed.
    // CONTROL: (a) `(void)recordDeliveredLocked(monitorId, client);` added before the final `return out;`
    //       of WallpaperManager::exportCurrentFrame (backend/daemon/wallpaper_manager.cpp), recording the
    //       delivery inside the export -> "FAIL: an export the caller has not yet reported delivered is not
    //       collection"; (b) WallpaperManager::handoverDelivered made to return false without recording ->
    //       "FAIL: and reporting it delivered makes the monitor watched", the S13.150 named line beside it.
    //       Via control_batch.sh, --phase '^watchEvidence$', 2026-10-06. Reverted.
    {
        const bool stale = rearmStale();
        if (!stale) {
            notVerified("S15.321", "the monitor never went stale again, so a recorded delivery would change nothing");
        } else {
            std::vector<int> toRender;
            wallpaperx::FrameExport reported =
                manager.exportCurrentFrame(kMonitorEvidence, ":1.902", false, {}, toRender);
            check(reported.handle.has_value(), "an export not yet reported delivered is served the monitor's picture");
            if (reported.handle) {
                if (reported.handle->frame.fd >= 0) close(reported.handle->frame.fd);
                if (reported.handle->acquireFd >= 0) close(reported.handle->acquireFd);
                check(!watched(kMonitorEvidence),
                      "an export the caller has not yet reported delivered is not collection");
                manager.handoverDelivered(kMonitorEvidence, ":1.902");
                check(watched(kMonitorEvidence), "and reporting it delivered makes the monitor watched");
                wallpaperx::testing::releaseHandle(manager, ":1.902", kMonitorEvidence, *reported.handle);
            }
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
        //       monitor's images counts as collecting". Both re-run 2026-10-06 on the NVIDIA entry via
        //       control_batch.sh, --phase '^watchEvidence$'. Reverted.
        const bool unwatchedAgain = rearmStale();
        check(unwatchedAgain, "the monitor goes unwatched again with its images out and nothing collecting");
        if (unwatchedAgain) {
            const wallpaperx::FrameExport emptyHanded = exportDelivered(manager, kMonitorEvidence, starvedEmptyHanded);
            check(emptyHanded.reason == wallpaperx::NoFrameReason::NoFreeImage && !emptyHanded.handle,
                  "a consumer holding nothing is refused, which is the state this rule is about");
            check(watched(kMonitorEvidence),
                  "a consumer refused while holding none of this monitor's images counts as collecting");

            const bool unwatchedOnceMore = rearmStale();
            check(unwatchedOnceMore, "and the monitor goes unwatched again after it");
            if (unwatchedOnceMore) {
                const wallpaperx::FrameExport byHolder = exportDelivered(manager, kMonitorEvidence, holder);
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

    // GOAL: S15.346 -- a consumer refused for holding too many descriptors is neither collecting the
    //       monitor it asked of nor refused a hand-over there: a stale video it asks of stays unwatched,
    //       and that monitor's refused-hand-over count does not move. The refusal is the consumer's
    //       own doing and says nothing about the monitor's ring.
    // WHY-THIS-SHAPE: the consumer reaches the cap on the still monitor, holding nothing on the video
    //       one -- the case the NoFreeImage branch would count as collecting -- and asks the video
    //       monitor only once it is proven stale, so a recorded collection would flip `watched` at once.
    // CONTROL: in the `if (!admission.admitted)` branch of WallpaperManager::exportCurrentFrame
    //       (backend/daemon/wallpaper_manager.cpp), (a) `it->second.uncollectedSince.reset();` added ->
    //       "FAIL: a refusal for descriptors is not collection: the stale video stays unwatched"; (b)
    //       `++it->second.refusedHandovers;` added -> "FAIL: and it is not a refused hand-over on that
    //       monitor (3 then 4)". Via control_batch.sh, --phase '^watchEvidence$', 2026-10-06. Reverted.
    // COVERAGE-ASSERT: the consumer must be refused for descriptors on the still monitor before it asks
    //       the video one, or the goal is reported not verified.
    {
        const std::string capped = ":1.930";
        bool atCap = false;
        for (int i = 0; i < 2 * static_cast<int>(WallpaperManager::kMaxDescriptorsPerClient) && !atCap; ++i) {
            const wallpaperx::FrameExport e = takeAndKeepOn(kMonitorRebound, capped);
            atCap = e.reason == wallpaperx::NoFrameReason::TooManyDescriptors && !e.handle;
            if (!e.handle && !atCap) break;
        }
        const bool stale = atCap && rearmStale();
        if (!atCap || !stale) {
            notVerified("S15.346", atCap ? "the video monitor never went stale again"
                                         : "the consumer was never refused for descriptors on the still monitor");
        } else {
            const std::optional<wallpaperx::MonitorStatus> before = monitorStatus(manager, kMonitorEvidence);
            const wallpaperx::FrameExport refused = exportDelivered(manager, kMonitorEvidence, capped);
            const std::optional<wallpaperx::MonitorStatus> after = monitorStatus(manager, kMonitorEvidence);
            check(refused.reason == wallpaperx::NoFrameReason::TooManyDescriptors && !refused.handle,
                  "a consumer at its descriptor cap is refused on another monitor too");
            check(before && after && !after->watched,
                  "a refusal for descriptors is not collection: the stale video stays unwatched");
            check(before && after && after->refusedHandovers == before->refusedHandovers,
                  ("and it is not a refused hand-over on that monitor (" +
                   std::to_string(before ? before->refusedHandovers : 0) + " then " +
                   std::to_string(after ? after->refusedHandovers : 0) + ")")
                      .c_str());
        }
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
        pictures.record(monitorId, generation);
        signals.record(monitorId, generation);
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
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, recordDeliveredLocked() made to set
    //       `it->second.uncollectedSince = std::chrono::steady_clock::now();` instead of resetting
    //       it -- which is the rule measured from the last hand-over -> "FAIL: a video whose pictures
    //       are farther apart than the unwatched interval keeps playing for a consumer that collects
    //       them", via control_batch.sh, --phase '^slowPicture$', 2026-10-05. Reverted.
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
            out.reason = exportDelivered(manager, monitorId).reason;
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
    //       The monitor records it, GetFrame answers RenderFailed rather than "still rendering,
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
    //       finished normally. Re-run 2026-10-24 via control_batch.sh. Reverted.
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

// The frame-rate setting a rate phase runs at, through the manager's own setter (60 is the default, so
// setting it changes nothing). False, failed, when refused.
bool runAtFrameRate(WallpaperManager& manager, int rate) {
    try {
        manager.setFrameRate(rate);
    } catch (const std::exception& e) {
        wallpaperx::testing::check(false, ("the frame rate " + std::to_string(rate) + " is accepted").c_str());
        std::fprintf(stderr, "  refused: %s\n", e.what());
        return false;
    }
    return true;
}

// S13.175 and S13.176 -- the two bounds the daemon puts on the gap between one picture and the
// next, each driven by a file whose own timing sits outside it. Its own phase, with one monitor and
// one collecting consumer, because both halves are measurements of how often that monitor commits
// and anything else drawing on the same GPU thread is noise in them. Run at the frame-rate setting
// 120 (S15.895) and at the default 60 (S15.896).
void runIntervalClampChecks(const WallpaperManager::Config& config, const std::string& fast,
                            const std::string& gap, int frameRate) {
    const char* kGoals = "S13.175-S13.176 (a file asking for more pictures a second than a screen can show is "
                         "played at the daemon's own ceiling, and a gap longer than five seconds is not waited out)";
    PhaseGrade grade("the interval-clamp phase");

    // GOAL: S13.175 -- a file declaring a thousand pictures a second is committed AT MOST the frame-rate
    //       setting's times a second (the thinning half is graded per setting below). False for
    //       the code before this fix, whose floor was four milliseconds -- 250 a second -- which
    //       the QA pass measured as 688 renders a second across four screens, the daemon at 43% of
    //       a core and every other client's frame fetch going from 0.8 ms to 12 ms. Also false for
    //       a fix that capped the rate by dropping pictures, which would play a two-second clip in
    //       a quarter of a second and show a quarter of its content.
    // WHY-THIS-SHAPE: two oracles, because either alone is satisfied by a wrong daemon. The rate
    //       alone is satisfied by one that slows the file to the setting. Packets per picture alone is
    //       satisfied by one that thins right and draws too often. The second oracle is the number of
    //       PACKETS the decoder was given against the number of pictures committed, which no status
    //       field can say. The rate is measured across a fixed number of pictures rather than counting
    //       pictures in a fixed time, and the bound is twice the setting at 120, so a loaded machine
    //       cannot cross it.
    // CONTROL: three things in backend/daemon/wallpaper_manager.cpp hold the ceiling: the frame-rate grid
    //       (targetRateLocked()), the 120 cap kPictureRateCap (overflowsCap() thins to its grid) and the floor
    //       kMinPictureInterval derived from it. control_batch.sh, --phase intervalClamp, G3e 2026-10-23, NVIDIA,
    //       reverted: kMinPictureInterval 4 ms with targetRateLocked() returning 250 stays green (120.7 a second:
    //       the cap thins it); kPictureRateCap 250 with targetRateLocked() returning 250 -> "FAIL: a file declaring
    //       a thousand pictures a second is committed no faster than the daemon's own ceiling ...", 250.0 a second.
    //       The same with the floor kept at 1/120 s commits 120.0 a second, so the floor alone still holds this
    //       line; only the packets-per-picture check below goes red ("498 packet(s) decoded for 125 picture(s)").
    // COVERAGE-ASSERT: the packet count is asserted to be non-zero, so a run in which the decode
    //       hook saw nothing fails instead of reading as the rate having been established.
    {
        FrameReadyLog signals;
        PictureLog pictures;
        WallpaperManager manager(config);
        CaptureGradeOnExit captureGrade{grade, manager};
        if (!runAtFrameRate(manager, frameRate)) return;
        Collector collector(manager, kMonitorClamp, ":1.770");
        RenderWorker worker(manager, [&signals, &pictures, &collector](int monitorId, uint64_t generation) {
            pictures.record(monitorId, generation);
            signals.record(monitorId, generation);
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
            const double floorRate = std::min(1.0 / kMinPictureIntervalSeconds, static_cast<double>(frameRate));
            std::printf("video_wallpaper_test: at frame rate %d, the %g fps fixture committed %llu pictures in %.3f s "
                        "(%.1f a second; the daemon allows %.1f), sending %d packet(s) to the decoder\n",
                        frameRate, kFastClipDeclaredFps, static_cast<unsigned long long>(kTo - kFrom), seconds,
                        perSecond, floorRate, packets);
            std::fflush(stdout);
            reportUnless(packets > 0, "the decoder was actually asked for the pictures that were shown",
                         std::to_string(packets) + " packet(s)");
            if (frameRate >= 120) {
                reportUnless(perSecond <= 2 * floorRate,
                             "a file declaring a thousand pictures a second is committed no faster than the daemon's "
                             "own ceiling on how often a wallpaper may be drawn",
                             std::to_string(perSecond) + " a second, where the floor on the gap between pictures "
                                                         "allows " + std::to_string(floorRate));
                // GOAL: S13.175 as restated 2026-10-20 -- at 120 the file plays at its own speed, thinned, not
                //       slowed: each committed picture stands for 1/120 s of the file, about 1000/120 = 8.3 packets
                //       sent per picture shown. False for a daemon that slows the file (1 per picture, every
                //       picture played) or thins past the setting (16.7 per picture at a 60 grid).
                // WHY-THIS-SHAPE: packets per committed picture, as the 60 form below; the old "every picture"
                //       bound (packets <= 1.5 x pictures) is the superseded wording and is red by design now.
                //       25% either side keeps both wrong answers out (1 and 16.7 against 8.3).
                // CONTROL: control_batch.sh 2026-10-20 (F2), NVIDIA, reverted, --phase '^intervalClamp$', both in
                //       renderer/video/picture_skip.h: ShowGrid::shows() answering true at once -> this line, 1995
                //       packets for 125 pictures at 62.5 a second; ShowGrid::shows()' step doubled -> this line,
                //       123 packets for 125 pictures (every picture, slowed). Green: 1031 packets.
                const double perPicture = static_cast<double>(packets) / static_cast<double>(kTo);
                const double expected = kFastClipDeclaredFps / frameRate;
                reportUnless(packets > 0 && perPicture >= 0.75 * expected && perPicture <= 1.25 * expected,
                             "at frame rate 120, it plays at its own speed, thinned to one picture each 1/120 s of "
                             "the file, neither slowed down nor thinned further",
                             std::to_string(packets) + " packet(s) decoded for " + std::to_string(kTo) +
                                 " picture(s) committed");
            } else {
                // GOAL: S15.896 -- at the default 60, the thousand-a-second file is committed at most 60 a second,
                //       and by thinning, not slowing: each committed picture stands for 1/60 s of the file, so
                //       about 1000/60 packets are sent per picture shown. False for a daemon that slows the file
                //       (1 per picture), ignores the setting (120 a second, 8.3 per picture) or thins too far.
                // WHY-THIS-SHAPE: the same two oracles as at 120, the second restated -- at 60 thinning is the
                //       point. The rate bound is 1.5x, not 120's 2x: 2x of 60 is 120, which a daemon ignoring the
                //       setting would meet exactly. Packets per picture within 25% of the file's rate over 60.
                // CONTROL: control_batch.sh 2026-10-20 (F1), NVIDIA, reverted: targetRateLocked() returning 120
                //       (backend/daemon/wallpaper_manager.cpp) -> both lines red; ShowGrid::shows()' step doubled
                //       (renderer/video/picture_skip.h) -> the packets line alone. Green: 62.5 a second, 1995 packets.
                reportUnless(perSecond <= 1.5 * floorRate,
                             "at frame rate 60, a file declaring a thousand pictures a second is committed no faster "
                             "than 60 a second",
                             std::to_string(perSecond) + " a second, where the setting allows " +
                                 std::to_string(floorRate));
                const double perPicture = static_cast<double>(packets) / static_cast<double>(kTo);
                const double expected = kFastClipDeclaredFps / frameRate;
                reportUnless(packets > 0 && perPicture >= 0.75 * expected && perPicture <= 1.25 * expected,
                             "at frame rate 60, it is thinned to one picture each 1/60 s of the file, neither slowed "
                             "down nor thinned further",
                             std::to_string(packets) + " packet(s) decoded for " + std::to_string(kTo) +
                                 " picture(s) committed");
            }
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
    // CONTROL: gapBetween() (backend/daemon/wallpaper_manager.cpp) made to return kMinPictureInterval
    //       always -> "FAIL: and the wait is still the daemon's ceiling rather than no wait at all",
    //       "0.008503 s". Both via control_batch.sh, --phase intervalClamp, 2026-09-29. Reverted.
    //       Both re-run 2026-10-20 (F1) on --phase intervalClamp60, the same block at 60: held.
    {
        FrameReadyLog signals;
        PictureLog pictures;
        WallpaperManager manager(config);
        CaptureGradeOnExit captureGrade{grade, manager};
        if (!runAtFrameRate(manager, frameRate)) return;
        Collector collector(manager, kMonitorClamp, ":1.771");
        RenderWorker worker(manager, [&signals, &pictures, &collector](int monitorId, uint64_t generation) {
            pictures.record(monitorId, generation);
            signals.record(monitorId, generation);
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
        const std::optional<Clock::time_point> first = firstSeen(shown, 1);
        const std::optional<Clock::time_point> second = firstSeen(shown, 2);
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

// The picture rate over each run of `window` consecutive generations starting in [from, to), timed by
// first announcement; runs with an end never announced are left out. Sorted, slowest first.
std::vector<double> windowRates(const std::vector<PictureLog::Event>& shown, uint64_t from, uint64_t to,
                                uint64_t window) {
    std::map<uint64_t, Clock::time_point> first;
    for (const PictureLog::Event& e : shown) first.emplace(e.generation, e.at);
    std::vector<double> rates;
    for (uint64_t g = from; g < to; ++g) {
        const auto a = first.find(g);
        const auto b = first.find(g + window);
        if (a == first.end() || b == first.end()) continue;
        const double seconds = std::chrono::duration<double>(b->second - a->second).count();
        if (seconds > 0) rates.push_back(static_cast<double>(window) / seconds);
    }
    std::sort(rates.begin(), rates.end());
    return rates;
}

double medianOf(const std::vector<double>& sorted) { return sorted.empty() ? 0.0 : sorted[sorted.size() / 2]; }

// S13.183 -- the 120 a second cap is held between the times successive pictures are DUE, not on
// each gap in the file on its own.
// Run at the frame-rate setting 120 (S15.895) and at the default 60 (S15.896): at 60 the same file plays
// at 60 a second, every other picture passed over.
void runCapBudgetChecks(const WallpaperManager::Config& config, const std::string& rate120, int frameRate) {
    const char* kGoal = "S13.183 (a 120 a second file with millisecond timestamps plays at the frame rate, not slower)";
    PhaseGrade grade("the cap-budget phase");
    FrameReadyLog signals;
    PictureLog pictures;
    WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};
    if (!runAtFrameRate(manager, frameRate)) return;
    Collector collector(manager, kMonitorClamp, ":1.772");
    RenderWorker worker(manager, [&signals, &pictures, &collector](int monitorId, uint64_t generation) {
        pictures.record(monitorId, generation);
        signals.record(monitorId, generation);
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
    //       `std::max(gap, kMinPictureInterval)` -- the floor held per gap -> "FAIL: a 120
    //       a second file with millisecond timestamps plays at 120 a second, not slower", median
    //       116.88 (fastest window 116.92) against 119.99 with the fix, via control_batch.sh, --phase
    //       capBudget, 2026-09-29. Reverted.
    // CONTROL: kMinPictureInterval in backend/daemon/wallpaper_manager.cpp made 1/10 s, so the file plays
    //       at 10 a second and misses the 30 s wait, via control_batch.sh, --phase capBudget, 2026-09-29 ->
    //       "FAIL: a 120 a second file with millisecond timestamps plays at 120 a second, not slower",
    //       median 10.00 over the 175 windows that arrived, not NOT VERIFIED. Reverted.
    //       F1 2026-10-20, frame rate 120, NVIDIA: both re-run, held; targetRateLocked() returning 60
    //       (backend/daemon/wallpaper_manager.cpp) -> the same line, median 60.
    // COVERAGE-ASSERT: the number of windows compared is asserted, and a wait that ran out grades the
    //       windows that did arrive: only a video that drew less than one window after the first
    //       picture is not graded, since a slow video is the defect and a stopped one is not a rate.
    // GOAL: S15.896 -- at 60 the same file plays at 60 a second, median one-second window at least 59.5 (the
    //       119/120 margin of 120's bound), every other picture passed over. False for a grid that passes over
    //       a picture it should show, or a floor that slows the shown pictures.
    // CONTROL: control_batch.sh 2026-10-20 (F1), NVIDIA, reverted: targetRateLocked() returning 120
    //       (backend/daemon/wallpaper_manager.cpp) -> "... drawn no faster than 60 a second"; ShowGrid::shows()'
    //       step doubled (renderer/video/picture_skip.h) -> "... plays at 60 a second, not slower".
    const uint64_t kWindow = static_cast<uint64_t>(frameRate);
    // Three seconds of pictures: two seconds of one-second windows, which is plenty for a median.
    const uint64_t kPictures = 3 * kWindow;
    const bool reached = signals.wait(kMonitorClamp, kPictures + 5);
    const uint64_t arrived = latestGeneration(pictures, kMonitorClamp);
    if (!reached && arrived < 6 + kWindow) {
        notVerified(kGoal, "the 120 a second video drew " + std::to_string(arrived) +
                               " picture(s), less than one window after the first, so no rate can be measured -- a "
                               "stopped clock must not be graded as a slow one");
        return;
    }
    const uint64_t windowsEnd = reached ? kPictures + 6 - kWindow : arrived + 1 - kWindow;
    const std::vector<double> rates = windowRates(pictures.forMonitor(kMonitorClamp), 6, windowsEnd, kWindow);
    const double median = medianOf(rates);
    std::printf("video_wallpaper_test: at frame rate %d, the 120 a second file with millisecond timestamps: median "
                "one-second window %.2f a second over %zu windows (slowest %.2f, fastest %.2f)\n",
                frameRate, median, rates.size(), rates.empty() ? 0.0 : rates.front(),
                rates.empty() ? 0.0 : rates.back());
    std::fflush(stdout);
    reportUnless(rates.size() >= 100, "enough one-second windows were compared for a median to mean something",
                 std::to_string(rates.size()) + " window(s)");
    const double atLeast = frameRate * 119.0 / 120.0;
    reportUnless(median >= atLeast,
                 frameRate >= 120 ? "a 120 a second file with millisecond timestamps plays at 120 a second, not slower"
                                  : "at frame rate 60, a 120 a second file with millisecond timestamps plays at 60 a "
                                    "second, not slower",
                 std::to_string(median) + " a second in the median one-second window, where " + std::to_string(atLeast) +
                     " is the bound");
    if (frameRate < 120) {
        reportUnless(median <= frameRate + 1.0,
                     "at frame rate 60, a 120 a second file is drawn no faster than 60 a second",
                     std::to_string(median) + " a second in the median one-second window");
    }
}

std::string rate120CodecClipPath(const std::string& codec) {
    return std::string(WALLPAPERX_VIDEO_ASSET_DIR) + "/wallpaper_rate120_" + codec + ".mp4";
}

// How evenly one monitor drew from generation `from` to `to`: its one-second windows and the gaps
// between successive generations, each sorted. Generations not both announced are left out.
struct DrawShape {
    std::vector<double> windows;
    std::vector<double> gaps;
    double median() const { return medianOf(windows); }
    double gapAt(double quantile) const {
        return gaps.empty() ? 0.0 : gaps[std::min(gaps.size() - 1, static_cast<size_t>(quantile * gaps.size()))];
    }
    double fractionAbove(double seconds) const {
        if (gaps.empty()) return 1.0;
        return static_cast<double>(std::count_if(gaps.begin(), gaps.end(), [&](double g) { return g > seconds; })) /
               static_cast<double>(gaps.size());
    }
    std::string describe(double interval) const {
        // A histogram in eighths of the interval, up to three intervals.
        std::array<int, 25> bins{};
        for (const double g : gaps) bins[std::min<size_t>(24, static_cast<size_t>(g / interval * 8.0))]++;
        std::string out = "median window " + std::to_string(median()) + "/s over " + std::to_string(windows.size()) +
                          " windows; gaps " + std::to_string(gaps.size()) + ", quartiles " +
                          std::to_string(gapAt(0.25) * 1e3) + "/" + std::to_string(gapAt(0.5) * 1e3) + "/" +
                          std::to_string(gapAt(0.75) * 1e3) + " ms, longest " +
                          std::to_string(gaps.empty() ? 0.0 : gaps.back() * 1e3) + " ms; histogram (eighths of " +
                          std::to_string(interval * 1e3) + " ms):";
        for (size_t i = 0; i < bins.size(); ++i) {
            if (bins[i] != 0) out += " " + std::to_string(i) + ":" + std::to_string(bins[i]);
        }
        return out;
    }
};

DrawShape drawShapeOf(const std::vector<PictureLog::Event>& shown, uint64_t from, uint64_t to, uint64_t window) {
    DrawShape shape;
    shape.windows = windowRates(shown, from, to > window ? to - window : from, window);
    std::map<uint64_t, Clock::time_point> first;
    for (const PictureLog::Event& e : shown) first.emplace(e.generation, e.at);
    for (uint64_t g = from; g < to; ++g) {
        const auto a = first.find(g);
        const auto b = first.find(g + 1);
        if (a != first.end() && b != first.end()) {
            shape.gaps.push_back(std::chrono::duration<double>(b->second - a->second).count());
        }
    }
    std::sort(shape.gaps.begin(), shape.gaps.end());
    return shape;
}

// The checks S15.897-S15.899 put on one monitor's drawing at `rate` a second, from a 120 a second file:
// the rate, its evenness, and no picture held for two of its intervals. `what` names the case. `slack` is
// how far the gap quartiles may sit from the interval: a quarter of it, or one picture of the file where
// the screen's interval is no whole number of the file's pictures (50 a second from 120: 16.7 and 25 ms).
void gradeEvenDrawing(const DrawShape& shape, double rate, const std::string& what, size_t minGaps,
                      std::optional<double> slackSeconds = std::nullopt) {
    const double interval = 1.0 / rate;
    const double slack = slackSeconds.value_or(0.25 * interval);
    std::printf("video_wallpaper_test: %s: %s\n", what.c_str(), shape.describe(interval).c_str());
    std::fflush(stdout);
    reportUnless(shape.windows.size() >= minGaps / 2 && shape.gaps.size() >= minGaps,
                 (what + ": enough windows and gaps were measured to grade it").c_str(),
                 std::to_string(shape.windows.size()) + " window(s), " + std::to_string(shape.gaps.size()) + " gap(s)");
    reportUnless(shape.median() >= rate * 119.0 / 120.0 && shape.median() <= rate + 1.0,
                 (what + ": draws its rate, no slower and no faster").c_str(),
                 std::to_string(shape.median()) + " a second in the median one-second window");
    reportUnless(shape.gapAt(0.25) >= interval - slack && shape.gapAt(0.75) <= interval + slack,
                 (what + ": its pictures are evenly spaced, one interval apart").c_str(),
                 "gap quartiles " + std::to_string(shape.gapAt(0.25) * 1e3) + " and " +
                     std::to_string(shape.gapAt(0.75) * 1e3) + " ms against " + std::to_string(interval * 1e3) + " +- " +
                     std::to_string(slack * 1e3));
    reportUnless(shape.fractionAbove(1.5 * interval) <= 0.03,
                 (what + ": no picture is held for two intervals while the file has one for the second").c_str(),
                 std::to_string(shape.fractionAbove(1.5 * interval) * 100.0) + "% of gaps above 1.5 intervals");
}

// S15.897 and S15.898 -- a 120 a second file on one screen at the frame-rate setting `frameRate`, in
// H.264, H.265 and AV1. Its own phase per setting, one monitor and one consumer, RUN_SERIAL.
// GOAL: S15.897 -- at 60 the 120 a second file draws 60 a second, evenly; at 120 it draws 120. False for a
//       daemon that slows, ignores the setting, or picks pictures off an uneven grid. S15.898 -- on H.265
//       (x265 defaults) at 60 no picture is doubled: false if a gap is two intervals while the file has a
//       picture for it (the ~52 a second pattern from leaving out pictures a screen shows).
// WHY-THIS-SHAPE: commit times are wall-clock, so every bound is on a robust statistic, never on one gap:
//       the median one-second window (a stall moves only the windows holding it), gap quartiles within 25% of
//       the interval (an alternating 1-and-2 picture pick moves the quartiles, a few late rounds do not), and
//       at most 3% of gaps above 1.5 intervals (the doubling defect is ~13% of gaps; a late round on an idle,
//       serial run is rare). "None late" for every single gap is printed (longest gap), not asserted: a
//       correct daemon on a busy host can be late once, and that would be asserting on the scheduler.
// CONTROL: control_batch.sh 2026-10-20 (F2), NVIDIA, reverted: targetRateLocked() returning 120
//       (backend/daemon/wallpaper_manager.cpp) -> thinRate60's rate and evenness lines, all three codecs;
//       returning 60 -> thinRate120's three lines, all codecs; showSpacing() times 1.25 (same file) -> all three
//       lines at 60, all codecs; DecodeLoad::skip() held at Unreferenced (backend/daemon/decode_load.h)
//       -> the H.265 lines alone, median 24.3 a second, 75 gaps of 50-67 ms. Re-run at decode_load.h,
//       control_batch.sh 2026-10-20 (F5): held, the three hevc lines at 60.
// COVERAGE-ASSERT: the numbers of windows and gaps compared are asserted, and a wait that ran out is
//       NOT VERIFIED, never graded as a slow file.
void runThinRateChecks(const WallpaperManager::Config& config, const std::string& codec, int frameRate,
                       std::optional<std::string> clip = std::nullopt) {
    const std::string path = clip.value_or(rate120CodecClipPath(codec));
    const std::string goal = "S15.897-S15.898 (" + codec + " at " + std::to_string(frameRate) + ")";
    if (!std::filesystem::exists(path)) {
        notVerified(goal.c_str(), "the fixture " + path + " was not generated");
        return;
    }
    PhaseGrade grade("the thinned-rate phase");
    FrameReadyLog signals;
    PictureLog pictures;
    WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};
    if (!runAtFrameRate(manager, frameRate)) return;
    Collector collector(manager, kMonitorClamp, ":1.780");
    RenderWorker worker(manager, [&signals, &pictures, &collector](int monitorId, uint64_t generation) {
        pictures.record(monitorId, generation);
        signals.record(monitorId, generation);
        collector.announced(monitorId, generation);
    });
    collector.attach(worker);
    WorkerUnblock unblock{worker};

    wallpaperx::testing::nameScreen(manager, kMonitorClamp, screenOf(kMonitorClamp));
    manager.requestWallpaper("video", path, kMonitorClamp);
    worker.post(kMonitorClamp);
    if (!signals.wait(kMonitorClamp, 1)) {
        grade.expectNoDevice();
        notVerified(goal.c_str(), "the video produced no first picture on this host");
        return;
    }
    // Three seconds of drawing, inside the file's four, from generation 6 on.
    const uint64_t to = 6 + 3 * static_cast<uint64_t>(frameRate);
    if (!signals.wait(kMonitorClamp, to)) {
        notVerified(goal.c_str(), "the video drew " + std::to_string(latestGeneration(pictures, kMonitorClamp)) +
                                      " picture(s), not " + std::to_string(to) + ", so no rate can be measured -- a "
                                      "stopped clock must not be graded as a slow one");
        return;
    }
    const DrawShape shape = drawShapeOf(pictures.forMonitor(kMonitorClamp), 6, to, static_cast<uint64_t>(frameRate));
    gradeEvenDrawing(shape, frameRate, "the 120 a second " + codec + " file at frame rate " + std::to_string(frameRate),
                     2 * static_cast<size_t>(frameRate));
    reportUnless(collector.threw().empty(), "the consumer's hand-overs did not throw", collector.threw());
}

// S15.902 -- with no decode thread, the pictures a round passes over unshown are bounded by the round's
// catch-up slice. Its own phase, one monitor on the thousand-a-second file at 60, every packet held 2 ms.
// GOAL: S15.902 -- a round that passes over unshown pictures on the render thread stops at the catch-up slice
//       (4 ms) and lets other work have the thread, so a task posted meanwhile waits about a slice and a
//       decode or two, not a whole run of ~16 unshown pictures (32 ms at 2 ms each). False if the run is
//       unbounded.
// WHY-THIS-SHAPE: the slice is not visible from outside, but what it is for is: the median wait of a task
//       posted to the worker. 100 samples, median against 12 ms (slice + two held decodes + margin); one
//       wait is never graded alone, since a late wake-up is the scheduler's. Graded only where the render
//       thread sends most packets: with a decode thread the passing-over is that thread's, and the phase
//       says so instead of passing (the NVIDIA entry; the Lavapipe twin decodes in software, no thread).
// CONTROL: control_batch.sh 2026-10-20 (F2), Lavapipe twin, reverted: the slice test in
//       backend/daemon/wallpaper_manager.cpp's pass-over loop disabled (`dropped && false`) -> this line, median
//       wait 65.8 ms against 1.07 green.
// COVERAGE-ASSERT: the number of waits measured and the render thread's share of the packets are asserted.
void runThinSliceChecks(const WallpaperManager::Config& config, const std::string& fast) {
    const char* kGoal = "S15.902 (with no decode thread, passing over unshown pictures is bounded by the slice)";
    PhaseGrade grade("the thinning-slice phase");
    FrameReadyLog signals;
    WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};
    struct Undelay {
        ~Undelay() { g_sendDelayMs.store(0); }
    } undelay;
    Collector collector(manager, kMonitorClamp, ":1.783");
    RenderWorker worker(manager, [&signals, &collector](int monitorId, uint64_t generation) {
        signals.record(monitorId, generation);
        collector.announced(monitorId, generation);
    });
    collector.attach(worker);
    WorkerUnblock unblock{worker};
    wallpaperx::testing::nameScreen(manager, kMonitorClamp, screenOf(kMonitorClamp));
    manager.requestWallpaper("video", fast, kMonitorClamp);
    worker.post(kMonitorClamp);
    if (!signals.wait(kMonitorClamp, 1)) {
        grade.expectNoDevice();
        notVerified(kGoal, "the fast video produced no first picture on this host");
        return;
    }
    const std::optional<pid_t> renderThread = workerThreadId(worker);
    check(renderThread.has_value(), "the render worker says which thread it is");
    if (!renderThread) return;
    g_countedThread.store(*renderThread);
    g_decodeCalls.store(0);
    g_countedThreadDecodes.store(0);
    g_sendDelayMs.store(2);

    std::vector<double> waits;
    for (int i = 0; i < 100; ++i) {
        auto ran = std::make_shared<std::promise<Clock::time_point>>();
        std::future<Clock::time_point> future = ran->get_future();
        const Clock::time_point posted = Clock::now();
        worker.postTask([ran](RenderWorker::TaskState) { ran->set_value(Clock::now()); });
        if (future.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
            check(false, "a task posted to the render worker while it passes over pictures runs within 5 s");
            return;
        }
        waits.push_back(std::chrono::duration<double, std::milli>(future.get() - posted).count());
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    g_sendDelayMs.store(0);
    const int all = g_decodeCalls.load();
    const int onRender = g_countedThreadDecodes.load();
    std::sort(waits.begin(), waits.end());
    std::printf("video_wallpaper_test: thinning with packets held 2 ms: %d of %d packet(s) on the render thread; "
                "task waits median %.2f ms, 90th %.2f, longest %.2f\n",
                onRender, all, waits[waits.size() / 2], waits[waits.size() * 9 / 10], waits.back());
    std::fflush(stdout);
    if (all < 100 || onRender * 2 < all) {
        notVerified(kGoal, std::to_string(onRender) + " of " + std::to_string(all) +
                               " packets were sent on the render thread: a decode thread passes over the pictures "
                               "on this host, so the render thread's slice is not what bounds them");
        return;
    }
    reportUnless(waits.size() == 100, "every task wait was measured", std::to_string(waits.size()));
    reportUnless(waits[waits.size() / 2] <= 12.0,
                 "with no decode thread, a round passing over unshown pictures stops at its catch-up slice, so "
                 "work posted meanwhile waits no longer than the slice and a decode",
                 std::to_string(waits[waits.size() / 2]) + " ms median wait");
}

// S15.899 -- two screens sharing one decoder on the 120 a second H.264 file. Its own phase, RUN_SERIAL.
void runThinSharedChecks(const WallpaperManager::Config& config) {
    const std::string path = rate120CodecClipPath("h264");
    const char* kGoal = "S15.899 (two screens sharing one decoder each draw their own rate)";
    if (!std::filesystem::exists(path)) {
        notVerified(kGoal, "the fixture " + path + " was not generated");
        return;
    }
    constexpr int kFast = 0;
    constexpr int kSlow = 1;
    PhaseGrade grade("the shared thinned-rate phase");
    FrameReadyLog signals;
    PictureLog pictures;
    WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};
    Collector fastCollector(manager, kFast, ":1.781");
    Collector slowCollector(manager, kSlow, ":1.782");
    RenderWorker worker(manager, [&](int monitorId, uint64_t generation) {
        pictures.record(monitorId, generation);
        signals.record(monitorId, generation);
        fastCollector.announced(monitorId, generation);
        slowCollector.announced(monitorId, generation);
    });
    fastCollector.attach(worker);
    slowCollector.attach(worker);
    WorkerUnblock unblock{worker};

    g_files.watch(path);
    g_files.resetCounts();
    for (const int id : {kFast, kSlow}) wallpaperx::testing::nameScreen(manager, id, screenOf(id));
    manager.setOutputRefreshRate(screenOf(kSlow), 50.0);
    for (const int id : {kFast, kSlow}) manager.requestWallpaper("video", path, id);
    for (const int id : {kFast, kSlow}) worker.post(id);
    if (!signals.wait(kFast, 1) || !signals.wait(kSlow, 1)) {
        grade.expectNoDevice();
        notVerified(kGoal, "the video produced no first picture on both screens on this host");
        return;
    }

    // GOAL: S15.899, first half -- beside a 60 screen, a screen whose refresh caps it at 50 draws 50 a second
    //       evenly (no gap above 1.5 of its 20 ms), while both draw from one decoder. False for thinning on the
    //       fastest screen's grid alone, whose 60-grid pictures a 50 screen can only take unevenly.
    // WHY-THIS-SHAPE: as runThinRateChecks; and the file is open exactly once while both play (the ledger's
    //       opens less closes), so the two screens are proven to share one session, not graded apart.
    // CONTROL: control_batch.sh 2026-10-20 (F2), NVIDIA, reverted, backend/daemon/wallpaper_manager.cpp: the
    //       per-screen shows() taking all spacings -> both screens' rate and evenness lines (80 a second each);
    //       ShowGrids::shows() (renderer/video/picture_skip.h) consulting only its first spacing -> the 50
    //       screen's three lines, 29.7 a second, 50 gaps of 67 ms; the join in videoSessionLocked() and
    //       mergeSharedSessionsLocked() both disabled -> "the two screens ... share one decoder" (the join alone
    //       stays green: the merge folds the sessions).
    const uint64_t slowTo = 6 + 3 * 50;
    if (!signals.wait(kSlow, slowTo) || !signals.wait(kFast, 6 + 3 * 60)) {
        notVerified(kGoal, "the two screens did not draw three seconds of pictures, so no rate can be measured");
        return;
    }
    const int open = g_files.opens(path) - g_files.closes(path);
    reportUnless(open == 1, "the two screens playing the same file share one decoder",
                 std::to_string(open) + " open(s) of the file at once (" + std::to_string(g_files.opens(path)) +
                     " opened, " + std::to_string(g_files.closes(path)) + " closed)");
    gradeEvenDrawing(drawShapeOf(pictures.forMonitor(kSlow), 6, slowTo, 50), 50,
                     "beside a 60 screen, the screen refreshing at 50", 100, 1.0 / 120.0);
    gradeEvenDrawing(drawShapeOf(pictures.forMonitor(kFast), 6, 6 + 3 * 60, 60), 60,
                     "beside a 50 screen, the 60 screen", 120);

    // GOAL: S15.899, second half -- with the other screen at 120, the 60 screen draws exactly every other
    //       picture of the shared stream. False for one that draws every picture, or skips unevenly.
    // WHY-THIS-SHAPE: counts, not times: both screens draw in the same round, so between two successive draws
    //       of the 60 screen the 120 screen draws exactly twice, under any interleaving that holds the rule.
    //       Only the run after both have drawn 30 more pictures since the change is counted.
    // CONTROL: the same per-screen shows() break -> this line, "1:120" (one draw of the 120 screen per interval).
    manager.setOutputRefreshRate(screenOf(kSlow), 0.0);
    manager.setMonitorFrameRate(kFast, 120);
    for (const int id : {kFast, kSlow}) worker.post(id);
    const uint64_t slowFrom = latestGeneration(pictures, kSlow) + 30;
    const uint64_t slowEnd = slowFrom + 120;
    if (!signals.waitAtLeast(kSlow, slowEnd) || !signals.waitAtLeast(kFast, latestGeneration(pictures, kFast) + 2)) {
        notVerified(kGoal, "the 60 screen did not draw two more seconds beside the 120 one");
        return;
    }
    std::map<uint64_t, Clock::time_point> slowAt;
    for (const PictureLog::Event& e : pictures.forMonitor(kSlow)) slowAt.emplace(e.generation, e.at);
    std::vector<Clock::time_point> fastTimes;
    for (const PictureLog::Event& e : pictures.forMonitor(kFast)) fastTimes.push_back(e.at);
    std::map<long, int> perInterval;
    int intervals = 0;
    int exactlyTwo = 0;
    for (uint64_t g = slowFrom; g < slowEnd; ++g) {
        const auto a = slowAt.find(g);
        const auto b = slowAt.find(g + 1);
        if (a == slowAt.end() || b == slowAt.end()) continue;
        const long between = std::count_if(fastTimes.begin(), fastTimes.end(),
                                           [&](Clock::time_point t) { return t > a->second && t <= b->second; });
        ++perInterval[between];
        ++intervals;
        if (between == 2) ++exactlyTwo;
    }
    std::string histogram;
    for (const auto& [count, times] : perInterval) histogram += " " + std::to_string(count) + ":" + std::to_string(times);
    std::printf("video_wallpaper_test: beside a 120 screen, draws of the 120 screen between two of the 60 screen's:%s\n",
                histogram.c_str());
    std::fflush(stdout);
    reportUnless(intervals >= 100, "enough of the 60 screen's intervals were counted", std::to_string(intervals));
    reportUnless(intervals > 0 && exactlyTwo * 100 >= intervals * 97,
                 "beside a 120 screen, the 60 screen draws exactly every other picture of the shared stream",
                 std::to_string(exactlyTwo) + " of " + std::to_string(intervals) + " intervals held exactly two;" +
                     histogram);
    reportUnless(fastCollector.threw().empty() && slowCollector.threw().empty(), "the consumers' hand-overs did not throw",
                 fastCollector.threw() + slowCollector.threw());
}

// Draws by `monitorId` announced in (from, to], a second.
double drawRateIn(const PictureLog& pictures, int monitorId, Clock::time_point from, Clock::time_point to) {
    const std::vector<PictureLog::Event> events = pictures.forMonitor(monitorId);
    const long n = std::count_if(events.begin(), events.end(), [&](const PictureLog::Event& e) { return e.at > from && e.at <= to; });
    return static_cast<double>(n) / std::chrono::duration<double>(to - from).count();
}

// The gaps between successive draws by `monitorId` in (from, to], sorted, in seconds.
std::vector<double> drawGapsIn(const PictureLog& pictures, int monitorId, Clock::time_point from, Clock::time_point to) {
    std::vector<Clock::time_point> at;
    for (const PictureLog::Event& e : pictures.forMonitor(monitorId)) {
        if (e.at > from && e.at <= to) at.push_back(e.at);
    }
    std::sort(at.begin(), at.end());
    std::vector<double> gaps;
    for (size_t i = 1; i < at.size(); ++i) gaps.push_back(std::chrono::duration<double>(at[i] - at[i - 1]).count());
    std::sort(gaps.begin(), gaps.end());
    return gaps;
}

// The render worker's time on a CPU so far, from /proc's schedstat for that thread; nothing if unreadable.
std::optional<double> threadCpuSeconds(pid_t tid) {
    std::ifstream in("/proc/self/task/" + std::to_string(tid) + "/schedstat");
    long long onCpu = 0;
    if (!(in >> onCpu)) return std::nullopt;
    return static_cast<double>(onCpu) / 1e9;
}

// S15.921 / S15.923 (a screen joining) -- a file above 120 a second shared by a 120 screen and one its refresh
// caps at `slowRate` (50 or 75): the shared session is over the cap. Each pair in a session of its own: a refresh
// changed while an over-the-cap session plays slows it today (G2a), which is reported, not graded here. Its own
// phase, RUN_SERIAL.
void runCapSharedChecks(const WallpaperManager::Config& config, double slowRate) {
    const std::string path = rate240ClipPath();
    const char* kGoal = "S15.921 (a shared file over the cap plays at its own speed, each screen at its rate)";
    if (!std::filesystem::exists(path)) {
        notVerified(kGoal, "the fixture " + path + " was not generated");
        return;
    }
    constexpr int kFast = 0;
    constexpr int kSlow = 1;
    constexpr double kPassSeconds = 3.0;  // wallpaper_rate240.mp4: 720 pictures at 240 a second
    PhaseGrade grade("the shared over-the-cap phase");
    FrameReadyLog signals;
    PictureLog pictures;
    WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};
    struct Unwatch {
        ~Unwatch() { g_passWatch.store(false); }
    } unwatch;
    Collector fastCollector(manager, kFast, ":1.784");
    Collector slowCollector(manager, kSlow, ":1.785");
    RenderWorker worker(manager, [&](int monitorId, uint64_t generation) {
        pictures.record(monitorId, generation);
        signals.record(monitorId, generation);
        fastCollector.announced(monitorId, generation);
        slowCollector.announced(monitorId, generation);
    });
    fastCollector.attach(worker);
    slowCollector.attach(worker);
    WorkerUnblock unblock{worker};

    g_files.watch(path);
    g_files.resetCounts();
    for (const int id : {kFast, kSlow}) wallpaperx::testing::nameScreen(manager, id, screenOf(id));
    manager.setMonitorFrameRate(kFast, 120);
    manager.setMonitorFrameRate(kSlow, 120);
    manager.setOutputRefreshRate(screenOf(kSlow), slowRate);
    // The 120 screen alone first, so the session's first cap verdict is taken under the cap and the join must
    // recompute it.
    manager.requestWallpaper("video", path, kFast);
    worker.post(kFast);
    if (!signals.wait(kFast, 30)) {
        grade.expectNoDevice();
        notVerified(kGoal, "the video produced no 30 pictures on the first screen on this host");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_passMutex);
        g_passStarts.clear();
        g_passLastPts = INT64_MIN;
    }
    g_passWatch.store(true);
    manager.requestWallpaper("video", path, kSlow);
    worker.post(kSlow);
    worker.post(kFast);
    if (!signals.wait(kSlow, 1)) {
        notVerified(kGoal, "the second screen drew no picture of the shared file");
        return;
    }
    const int open = g_files.opens(path) - g_files.closes(path);
    reportUnless(open == 1, "the two screens over the cap share one decoder",
                 std::to_string(open) + " open(s) of the file at once");

    // Two passes after a settling one, timed pass start to pass start at the decoder. False (and red) when the
    // passes never come: a session that stalls is not one that plays at its own speed.
    const auto measure = [&](const std::string& what) {
        size_t base = 0;
        {
            std::lock_guard<std::mutex> lock(g_passMutex);
            base = g_passStarts.size();
        }
        const Clock::time_point deadline = Clock::now() + std::chrono::seconds(20);
        std::vector<Clock::time_point> starts;
        while (Clock::now() < deadline) {
            {
                std::lock_guard<std::mutex> lock(g_passMutex);
                starts.assign(g_passStarts.begin() + static_cast<std::ptrdiff_t>(base), g_passStarts.end());
            }
            if (starts.size() >= 3) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (starts.size() < 3) {
            check(false, (what + ": the shared file plays at its own speed -- it began only " +
                          std::to_string(starts.size()) + " of 3 passes in 20 s").c_str());
            return;
        }
        const Clock::time_point from = starts[1];
        const Clock::time_point to = starts[2];
        const double wall = std::chrono::duration<double>(to - from).count();
        const double ratio = kPassSeconds / wall;
        const double fast = drawRateIn(pictures, kFast, from, to);
        const double slow = drawRateIn(pictures, kSlow, from, to);
        std::printf("video_wallpaper_test: %s: one pass of %.1f s took %.3f s (file/wall %.3f); draws a second: "
                    "120 screen %.1f, other %.1f\n", what.c_str(), kPassSeconds, wall, ratio, fast, slow);
        std::fflush(stdout);
        // GOAL: S15.921 -- over the cap the shared file's own time keeps pace with the wall clock (file/wall 1.0) and
        //       each screen draws its own rate; S15.923 -- the cap verdict taken with the 120 screen alone is
        //       recomputed when the other screen joins, and when its refresh changes.
        // WHY-THIS-SHAPE: file time read as whole passes at the decoder, not drawn pictures, so no picture content is
        //       needed; one 3 s pass, after a settling one, makes the queue's lead cancel. The 120 screen plays first
        //       so the first verdict is under the cap. Before the cap rule this ran 0.67-0.86.
        // CONTROL: control_batch.sh 2026-10-21 (G2a), NVIDIA, reverted, backend/daemon/wallpaper_manager.cpp:
        //       overflowsCap() returning false -> "120 beside 50: the shared file plays at its own speed"; the verdict
        //       never recomputed after the first -> the same line.
        reportUnless(ratio >= 0.95 && ratio <= 1.03, (what + ": the shared file plays at its own speed").c_str(),
                     "file/wall " + std::to_string(ratio));
        reportUnless(fast >= 120 * 0.95 && fast <= 120 * 1.03, (what + ": the 120 screen draws 120 a second").c_str(),
                     std::to_string(fast));
        reportUnless(slow >= slowRate * 0.95 && slow <= slowRate * 1.03,
                     (what + ": the other screen draws its own rate").c_str(), std::to_string(slow));
    };
    measure("120 beside " + std::to_string(static_cast<int>(slowRate)));
    reportUnless(fastCollector.threw().empty() && slowCollector.threw().empty(), "the consumers' hand-overs did not throw",
                 fastCollector.threw() + slowCollector.threw());
}

// S15.922 / S15.923 (a screen leaving) -- two screens sharing the thousand-a-second file at pairs of rates over and
// under the cap with no screen at 120. Its own phase, RUN_SERIAL.
void runCapNoTopChecks(const WallpaperManager::Config& config, const std::string& fast) {
    const char* kGoal = "S15.922 (over the cap with no 120 screen, both screens draw their rates)";
    const std::string other = rate120CodecClipPath("h264");
    if (!std::filesystem::exists(fast) || !std::filesystem::exists(other) || !std::filesystem::exists(rate240ClipPath())) {
        notVerified(kGoal, "the fixtures " + fast + ", " + other + " and " + rate240ClipPath() + " were not all generated");
        return;
    }
    constexpr int kA = 0;
    constexpr int kB = 1;
    // One session per case: screen B plays alone, screen A joins it (and, for `leaves`, goes to another file).
    // Every rate is set before the join, so no case depends on a refresh changed under a playing session.
    struct Case {
        const char* what;
        int frameA, frameB;
        double refreshA, refreshB;
        bool leaves;
        bool rate240 = false;
    };
    const std::string rate240 = rate240ClipPath();
    const auto run = [&](const Case& c, const std::function<void(PictureLog&, Clock::time_point, Clock::time_point, double)>& grade) {
        PhaseGrade phaseGrade("the over-the-cap phase without a 120 screen");
        FrameReadyLog signals;
        PictureLog pictures;
        WallpaperManager manager(config);
        CaptureGradeOnExit captureGrade{phaseGrade, manager};
        Collector aCollector(manager, kA, ":1.786");
        Collector bCollector(manager, kB, ":1.787");
        RenderWorker worker(manager, [&](int monitorId, uint64_t generation) {
            pictures.record(monitorId, generation);
            signals.record(monitorId, generation);
            aCollector.announced(monitorId, generation);
            bCollector.announced(monitorId, generation);
        });
        aCollector.attach(worker);
        bCollector.attach(worker);
        WorkerUnblock unblock{worker};
        for (const int id : {kA, kB}) wallpaperx::testing::nameScreen(manager, id, screenOf(id));
        manager.setMonitorFrameRate(kA, c.frameA);
        manager.setMonitorFrameRate(kB, c.frameB);
        manager.setOutputRefreshRate(screenOf(kA), c.refreshA);
        manager.setOutputRefreshRate(screenOf(kB), c.refreshB);
        const std::string& file = c.rate240 ? rate240 : fast;
        manager.requestWallpaper("video", file, kB);
        worker.post(kB);
        if (!signals.wait(kB, 1)) {
            phaseGrade.expectNoDevice();
            notVerified(kGoal, "the video produced no first picture on this host");
            return false;
        }
        const std::optional<pid_t> renderThread = workerThreadId(worker);
        check(renderThread.has_value(), "the render worker says which thread it is");
        if (!renderThread) return false;
        manager.requestWallpaper("video", file, kA);
        worker.post(kA);
        if (!signals.wait(kA, 1)) {
            check(false, (std::string(c.what) + ": the joining screen drew a picture of the shared file").c_str());
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        if (c.leaves) {
            manager.requestWallpaper("video", other, kA);
            worker.post(kA);
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
        const Clock::time_point from = Clock::now();
        const std::optional<double> cpuFrom = threadCpuSeconds(*renderThread);
        std::this_thread::sleep_for(std::chrono::milliseconds(3000));
        const Clock::time_point to = Clock::now();
        const std::optional<double> cpuTo = threadCpuSeconds(*renderThread);
        const double cpu = cpuFrom && cpuTo ? (*cpuTo - *cpuFrom) / std::chrono::duration<double>(to - from).count() : -1.0;
        grade(pictures, from, to, cpu);
        reportUnless(aCollector.threw().empty() && bCollector.threw().empty(), "the consumers' hand-overs did not throw",
                     aCollector.threw() + bCollector.threw());
        return true;
    };
    // The 50 screen's draw gaps in the window: quartiles, printed, and the first `minimum` count asserted.
    const auto quartiles = [&](PictureLog& pictures, Clock::time_point from, Clock::time_point to, const std::string& what)
        -> std::optional<std::pair<double, double>> {
        const std::vector<double> gaps = drawGapsIn(pictures, kB, from, to);
        std::printf("video_wallpaper_test: %s: the 50 screen's gaps, quartiles %.2f / %.2f ms over %zu\n", what.c_str(),
                    gaps.empty() ? 0.0 : gaps[gaps.size() / 4] * 1e3, gaps.empty() ? 0.0 : gaps[gaps.size() * 3 / 4] * 1e3,
                    gaps.size());
        std::fflush(stdout);
        if (gaps.size() < 100) {
            check(false, (what + ": the 50 screen drew 100 gaps to grade -- it drew " + std::to_string(gaps.size())).c_str());
            return std::nullopt;
        }
        return std::pair{gaps[gaps.size() / 4], gaps[gaps.size() * 3 / 4]};
    };

    // GOAL: S15.923 -- the cap verdict is recomputed when a screen leaves: a 120 screen joins the 50 one (over the
    //       cap), then leaves, and the 50 screen alone draws its own even 20 ms grid again. False for a stale "over".
    // WHY-THIS-SHAPE: only a verdict that changes is visible, so the leave goes from over to under; on the
    //       thousand-a-second file the 50 grid lands on pictures, where the cap's 120 grid gives 16.7/25 ms steps
    //       (the 120 file gives both rules the same steps).
    // CONTROL: control_batch.sh 2026-10-21 (G2a), NVIDIA, reverted, backend/daemon/wallpaper_manager.cpp: the
    //       verdict recomputed only when the count of spacings grows -> "after a 120 screen left: the 50 screen
    //       draws every 20 ms".
    run({"after a 120 screen left", 120, 0, 0.0, 50.0, true}, [&](PictureLog& pictures, Clock::time_point from, Clock::time_point to, double) {
        if (const auto q = quartiles(pictures, from, to, "after a 120 screen left")) {
            reportUnless(q->first >= 0.0175 && q->second <= 0.0225, "after a 120 screen left: the 50 screen draws every 20 ms",
                         std::to_string(q->first * 1e3) + " / " + std::to_string(q->second * 1e3) + " ms quartiles");
        }
    });

    // GOAL: S15.922 -- under the cap (60 beside 50) the 50 screen's draws are as before the rule; over it with no
    //       120 screen (100 beside 75, 75 beside 60) both keep drawing their own rates.
    // WHY-THIS-SHAPE: under the cap the floor under the due times moves some of the 50 screen's pictures on this
    //       file (one gap in five comes out 16.7 ms), where the cap's grid gives three in five: so the share of gaps
    //       under 17.5 ms is graded, not evenness. The over pairs play wallpaper_rate240.mp4, whose 3 s pass keeps the
    //       extra draw each slower screen makes at a pass's first picture (E16, not fixed) out of the rate. Rates
    //       over a fixed 3 s wall window, so a stall reads as a low rate, never as a pass. The render thread's CPU
    //       share is printed, not asserted: the one break known to hit this path stalls (0 draws, 0% CPU), so no
    //       control has shown a spin, and an assertion never seen red is not shipped.
    // CONTROL: control_batch.sh 2026-10-21 (G2a), NVIDIA, reverted, backend/daemon/wallpaper_manager.cpp:
    //       overflowsCap() returning true -> "60 beside 50: the 50 screen draws on its own 20 ms grid"; the "no
    //       screen draws it, pass it over" clause removed -> "100 beside 75: the faster screen draws its own rate"
    //       and the other three rate lines, 0.0 draws a second on both screens.
    run({"60 beside 50", 0, 0, 0.0, 50.0, false}, [&](PictureLog& pictures, Clock::time_point from, Clock::time_point to, double) {
        if (quartiles(pictures, from, to, "60 beside 50")) {
            const std::vector<double> gaps = drawGapsIn(pictures, kB, from, to);
            const double shortShare = static_cast<double>(std::count_if(gaps.begin(), gaps.end(), [](double g) { return g < 0.0175; })) /
                                      static_cast<double>(gaps.size());
            reportUnless(shortShare <= 0.4, "60 beside 50: the 50 screen draws on its own 20 ms grid",
                         std::to_string(shortShare * 100) + "% of its gaps under 17.5 ms");
        }
    });
    struct Pair {
        Case c;
        double rateA, rateB;
    };
    for (const Pair& p : {Pair{{"100 beside 75", 120, 120, 100.0, 75.0, false, true}, 100, 75},
                          Pair{{"75 beside 60", 120, 0, 75.0, 0.0, false, true}, 75, 60}}) {
        run(p.c, [&](PictureLog& pictures, Clock::time_point from, Clock::time_point to, double cpu) {
            const double a = drawRateIn(pictures, kA, from, to);
            const double b = drawRateIn(pictures, kB, from, to);
            std::printf("video_wallpaper_test: %s: draws a second %.1f and %.1f; render thread on a CPU %.0f%% of the time\n",
                        p.c.what, a, b, cpu * 100);
            std::fflush(stdout);
            const std::string what = p.c.what;
            reportUnless(a >= p.rateA * 0.95 && a <= p.rateA * 1.03, (what + ": the faster screen draws its own rate").c_str(),
                         std::to_string(a));
            reportUnless(b >= p.rateB * 0.95 && b <= p.rateB * 1.03, (what + ": the slower screen draws its own rate").c_str(),
                         std::to_string(b));
        });
    }
}

constexpr int kMonitorHighRate = 0;

// Holds the render worker up for 6 ms at a time, from a task of this test's, at pseudo-random spacings
// of 15 to 35 ms: a picture due during a hold is drawn up to 6 ms late -- less than the capped 8.33 ms
// interval, and later than an idle machine ever makes it. Random spacing, because a fixed one that is a
// multiple of the picture interval locks onto the schedule: after one restart every due time lands
// at the end of a hold and nothing is late again. Each hold's start and end are recorded as the worker
// measured them, so a hold the host stretched can be seen. Stops and joins on destruction.
class WorkerStalls {
public:
    struct Hold {
        Clock::time_point start;
        Clock::time_point end;
    };

    explicit WorkerStalls(RenderWorker& worker)
        : m_thread([&worker, shared = m_shared](std::stop_token stop) {
              std::minstd_rand random(20260929);
              std::uniform_int_distribution<int> spacing(15, 35);
              while (!stop.stop_requested()) {
                  // Shared: a hold still queued when this object goes runs, or is refused, after it.
                  worker.postTask([shared](RenderWorker::TaskState state) {
                      if (state != RenderWorker::TaskState::Run) return;
                      const Clock::time_point start = Clock::now();
                      std::this_thread::sleep_for(std::chrono::milliseconds(6));
                      const Clock::time_point end = Clock::now();
                      std::lock_guard<std::mutex> lock(shared->mutex);
                      shared->holds.push_back(Hold{start, end});
                  });
                  std::this_thread::sleep_for(std::chrono::milliseconds(spacing(random)));
              }
          }) {}

    std::vector<Hold> holds() const {
        std::lock_guard<std::mutex> lock(m_shared->mutex);
        return m_shared->holds;
    }

private:
    struct Shared {
        std::mutex mutex;
        std::vector<Hold> holds;
    };
    std::shared_ptr<Shared> m_shared = std::make_shared<Shared>();
    std::jthread m_thread;
};

// The render worker's own run-queue delay -- how long the host kept it runnable but not running, the
// second field of /proc/thread-self/schedstat -- read as each picture is announced, which happens on
// the worker. The growth between two announcements is lateness the host added to the second picture,
// whatever the daemon decided.
class WorkerDelayLog {
public:
    // On the worker thread, from the frame callback.
    void record(uint64_t generation) {
        std::ifstream in("/proc/thread-self/schedstat");
        long long onCpu = 0;
        long long runDelay = 0;
        const bool read = static_cast<bool>(in >> onCpu >> runDelay);
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!read) {
            m_unreadable = true;
            return;
        }
        m_byGeneration.emplace(generation, runDelay);
    }

    // Cumulative delay when `generation` was first announced; nothing when unknown or unreadable.
    std::optional<long long> at(uint64_t generation) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_byGeneration.find(generation);
        if (m_unreadable || it == m_byGeneration.end()) return std::nullopt;
        return it->second;
    }

    bool readable() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return !m_unreadable && !m_byGeneration.empty();
    }

private:
    mutable std::mutex m_mutex;
    std::map<uint64_t, long long> m_byGeneration;
    bool m_unreadable = false;
};

// One graded stretch under WorkerStalls: every one-second window that arrived, and the ones in which the
// host provably pushed no picture past the capped interval.
struct StalledStretch {
    std::vector<double> rates;
    std::vector<double> cleanRates;
    std::size_t holds = 0;
    double longestHoldMs = 0;
    std::size_t hostHeldPictures = 0;
    std::size_t delayedPictures = 0;
    std::size_t longHolds = 0;
    bool delayReadable = false;
    bool reachedEnd = false;
};

// What the host may add to one picture before a correct daemon could have restarted on it. A round
// judges lateness at the time it starts, so a picture waiting behind a hold is late by at most that
// hold, plus the worker's run-queue delay since the picture before: 7.5 ms together, under the capped
// 8.33. Not measured: a draw or decode slow enough to make the next picture late on its own, which on
// an idle machine takes well under a millisecond.
// GOAL: a stretch in which the host, not the daemon, made pictures late past the allowance is reported
//       as not graded, never as a FAIL of a correct daemon.
// WHY-THIS-SHAPE: windows are dropped per picture, not the whole run on one long hold: one oversleep
//       would then void every loaded run, and the hold alone misses the worker waiting to run at all.
// CONTROL: test-side, each by hand in a scratch copy, 2026-09-29, highRate phase: WorkerStalls' hold
//       made 10 ms -> NOT VERIFIED, "leaving 0 of 240 windows ... clear", where the median over all
//       windows read 109.5-111.3 (both drivers) and would have failed a correct daemon; its spacing made
//       300-500 ms -> NOT VERIFIED "ran only 7 holds"; the schedstat path misspelt -> NOT VERIFIED
//       "does not expose /proc/thread-self/schedstat". Environment: six busy loops on the two cores
//       the test was pinned to, four runs -> NOT VERIFIED each time, the unfiltered median 92.6-114.7.
constexpr std::chrono::microseconds kHoldLimit{6500};
constexpr std::chrono::microseconds kWorkerDelayLimit{1000};

// Windows of kWindow pictures from `from`, once the video has played kWindows + kWindow more under
// stalls. A wait that runs out still returns the windows that arrived, so a video that plays slowly is
// graded as slow; nothing only when not one window arrived, which the caller reports as not graded.
std::optional<StalledStretch> stalledWindowRates(RenderWorker& worker, FrameReadyLog& signals,
                                                 const PictureLog& pictures, const WorkerDelayLog& workerDelay) {
    constexpr uint64_t kWindow = 120;
    constexpr uint64_t kWindows = 240;
    StalledStretch stretch;
    std::vector<WorkerStalls::Hold> holds;
    uint64_t from = 0;
    {
        const WorkerStalls stalls(worker);
        // Into the stalls before the first graded picture.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        from = latestGeneration(pictures, kMonitorHighRate) + 10;
        stretch.reachedEnd = signals.wait(kMonitorHighRate, from + kWindows + kWindow);
        holds = stalls.holds();
    }
    const uint64_t arrived = latestGeneration(pictures, kMonitorHighRate);
    if (!stretch.reachedEnd && arrived < from + kWindow) return std::nullopt;
    const uint64_t to = stretch.reachedEnd ? from + kWindows : arrived + 1 - kWindow;
    stretch.delayReadable = workerDelay.readable();

    const std::vector<PictureLog::Event> shown = pictures.forMonitor(kMonitorHighRate);
    std::map<uint64_t, Clock::time_point> first;
    for (const PictureLog::Event& e : shown) first.emplace(e.generation, e.at);
    stretch.rates = windowRates(shown, from, to, kWindow);

    // Per picture, whether the host could have made it late past the allowance: the worker's run-queue
    // delay since the picture before, and the length of any hold that ended between the two.
    std::map<uint64_t, bool> hostHeld;
    for (uint64_t g = from; g <= to + kWindow - 1; ++g) {
        const auto shownAt = first.find(g);
        const auto before = first.find(g - 1);
        if (shownAt == first.end() || before == first.end()) {
            hostHeld[g] = true;
            continue;
        }
        // Holds are counted whether or not the delay can be read, so the caller's hold count and its
        // unreadable-delay verdict each report their own cause.
        bool held = false;
        const std::optional<long long> delayNow = workerDelay.at(g);
        const std::optional<long long> delayBefore = workerDelay.at(g - 1);
        if (!delayNow || !delayBefore) {
            held = true;
        } else if (std::chrono::nanoseconds(*delayNow - *delayBefore) > kWorkerDelayLimit) {
            held = true;
            ++stretch.delayedPictures;
        }
        for (const WorkerStalls::Hold& hold : holds) {
            if (hold.end <= before->second || hold.end > shownAt->second) continue;
            ++stretch.holds;
            const auto length = hold.end - hold.start;
            stretch.longestHoldMs =
                std::max(stretch.longestHoldMs, std::chrono::duration<double, std::milli>(length).count());
            if (length > kHoldLimit) {
                held = true;
                ++stretch.longHolds;
            }
        }
        hostHeld[g] = held;
        if (held) ++stretch.hostHeldPictures;
    }
    for (uint64_t g = from; g < to; ++g) {
        const auto a = first.find(g);
        const auto b = first.find(g + kWindow);
        if (a == first.end() || b == first.end() || b->second <= a->second) continue;
        // The first picture's lateness only moves the window's start, so a restart inside it is what counts.
        bool clean = true;
        for (uint64_t j = g + 1; j <= g + kWindow && clean; ++j) clean = !hostHeld[j];
        if (!clean) continue;
        stretch.cleanRates.push_back(static_cast<double>(kWindow) /
                                     std::chrono::duration<double>(b->second - a->second).count());
    }
    std::sort(stretch.cleanRates.begin(), stretch.cleanRates.end());
    return stretch;
}

// S15.109-S15.110 -- the schedule when the file asks for more than the 120-a-second cap: a 240 a second
// file at speed 1 and then at speed 2, each with the render thread held up for less than one capped
// interval. A file above the cap is thinned, not slowed, so it banks no lag to bound.
// Run at the frame-rate setting 120 (S15.895) and at the default 60 (S15.896), where the cap is 60 a second.
void runHighRateChecks(const WallpaperManager::Config& config, const std::string& rate240, int frameRate) {
    const std::string rate = std::to_string(frameRate);
    const char* kGoals = "S15.109-S15.110 (a video asking for more than 120 pictures a second: its rate and a late "
                         "picture at speed 2)";
    PhaseGrade grade("the high-rate phase");
    FrameReadyLog signals;
    PictureLog pictures;
    WorkerDelayLog workerDelay;
    WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};
    if (!runAtFrameRate(manager, frameRate)) return;
    Collector collector(manager, kMonitorHighRate, ":1.773");
    RenderWorker worker(manager, [&signals, &pictures, &collector, &workerDelay](int monitorId, uint64_t generation) {
        if (monitorId == kMonitorHighRate) workerDelay.record(generation);
        pictures.record(monitorId, generation);
        signals.record(monitorId, generation);
        collector.announced(monitorId, generation);
    });
    collector.attach(worker);
    WorkerUnblock unblock{worker};
    const auto setSpeed = [&](double speed) {
        try {
            manager.setWallpaperProperties(kMonitorHighRate, {{"speed", speed}});
        } catch (const std::exception& e) {
            check(false, ("the phase's speed " + std::to_string(speed) + " is accepted").c_str());
            std::fprintf(stderr, "  refused: %s\n", e.what());
            return false;
        }
        worker.wake();
        return true;
    };

    wallpaperx::testing::nameScreen(manager, kMonitorHighRate, screenOf(kMonitorHighRate));
    manager.requestWallpaper("video", rate240, kMonitorHighRate);
    worker.post(kMonitorHighRate);
    if (!signals.wait(kMonitorHighRate, 1)) {
        grade.expectNoDevice();
        notVerified(kGoals, "the 240 a second video produced no first picture on this host");
        return;
    }
    // One graded stretch under stalls: false, with the reason given, when the video drew less than one
    // window. Missing windows are the daemon's and fail; too few holds, or a host that delayed every
    // window, is this harness's premise not met and is reported as not graded.
    const auto gradeUnderStalls = [&](const std::string& label, const std::string& goal, double atLeast) {
        const std::optional<StalledStretch> stretch = stalledWindowRates(worker, signals, pictures, workerDelay);
        if (!stretch) {
            notVerified(kGoals, label + " drew less than one window of pictures, and a stopped clock must not be "
                                        "graded as a slow one");
            return false;
        }
        const std::vector<double>& all = stretch->rates;
        const std::vector<double>& clean = stretch->cleanRates;
        std::printf("video_wallpaper_test: %s, with %zu holds of 6 ms (longest %.2f ms): median one-second window "
                    "%.2f a second over %zu windows (slowest %.2f, fastest %.2f); %zu picture(s) the host may have "
                    "made late past the cap (%zu behind a hold over 6.5 ms, %zu with the worker delayed over 1 ms), "
                    "leaving %zu windows clear of them, median %.2f (slowest %.2f)\n",
                    label.c_str(), stretch->holds, stretch->longestHoldMs, medianOf(all), all.size(),
                    all.empty() ? 0.0 : all.front(), all.empty() ? 0.0 : all.back(), stretch->hostHeldPictures,
                    stretch->longHolds, stretch->delayedPictures, clean.size(), medianOf(clean),
                    clean.empty() ? 0.0 : clean.front());
        std::fflush(stdout);
        reportUnless(all.size() >= 200, ("every picture of " + label + " was announced, so 200 windows compared").c_str(),
                     std::to_string(all.size()) + " window(s)" +
                         (stretch->reachedEnd ? "" : ", and the video missed the 30 s wait for its last picture"));
        reportUnless(medianOf(all) <= frameRate + 1.0, (label + " plays no faster than the " + rate + " a second cap").c_str(),
                     std::to_string(medianOf(all)) + " a second in the median one-second window");
        if (stretch->holds < 60) {
            notVerified(kGoals, label + " ran only " + std::to_string(stretch->holds) +
                                    " holds of the render thread, so pictures late by less than the capped interval "
                                    "were not produced");
            return true;
        }
        if (!stretch->delayReadable) {
            notVerified(kGoals, "this host does not expose /proc/thread-self/schedstat, so a picture the host made "
                                "late past the capped interval cannot be told from the defect in " + label);
            return true;
        }
        if (clean.size() < 40) {
            notVerified(kGoals, "the host may have made " + std::to_string(stretch->hostHeldPictures) +
                                    " picture(s) late past the capped interval, leaving " + std::to_string(clean.size()) +
                                    " of " + std::to_string(all.size()) + " windows of " + label +
                                    " clear of them, so its median cannot be told from the defect's");
            return true;
        }
        reportUnless(medianOf(clean) >= atLeast, goal.c_str(),
                     std::to_string(medianOf(clean)) + " a second in the median one-second window the host did not "
                                                       "hold up");
        return true;
    };

    // GOAL: S15.109 -- a video asking for more than 120 pictures a second plays at 120, and no faster: a
    //       240 a second file at speed 1 holds a median one-second window of at least 119.5 while pictures
    //       are drawn up to 6 ms late, and at most 121. False for a rate cap that lets the file run faster.
    // WHY-THIS-SHAPE: under WorkerStalls, so pictures are drawn late; the median is taken only over
    //       windows in which the host provably pushed no picture past the 8.33 ms allowance (see
    //       kHoldLimit), since a busy host alone moves an unfiltered median well below the cap. The median
    //       window, for runCapBudgetChecks()' reason. Missing windows and a slow video fail: they are the
    //       daemon's.
    // CONTROL: control_batch.sh 2026-10-20 (F1), NVIDIA, --phase highRate, reverted, in
    //       backend/daemon/wallpaper_manager.cpp: targetRateLocked() returning 240 with
    //       VideoSession::Waiting::planned() given no floor under the due time -> "the 240 a second file at
    //       speed 1 plays no faster than the 120 a second cap"; targetRateLocked() returning 60 -> the speed-1
    //       rate line.
    // GOAL: S15.896 -- at 60, the same file at speed 1 and 2 holds a median of at least 59.5 and at most 61 under
    //       the same holds. CONTROL: control_batch.sh 2026-10-20 (F1), NVIDIA, reverted, --phase highRate60:
    //       targetRateLocked() returning 120 (backend/daemon/wallpaper_manager.cpp) -> both "no faster than the 60
    //       a second cap" lines; ShowGrid::shows()' step doubled (renderer/video/picture_skip.h) -> both rate lines;
    //       showSpacing() squaring the speed -> the speed-2 line alone.
    if (!gradeUnderStalls("the 240 a second file at speed 1",
                          "a 240 a second video at speed 1 plays at " + rate + " a second, pictures drawn up to 6 ms "
                          "late included",
                          frameRate - 0.5)) {
        return;
    }

    // GOAL: S15.110 -- at speed 2, a picture drawn later than the file's own interval at that speed
    //       but within the capped one (8.33 ms) is on time: the schedule keeps its plan, and the rate
    //       holds. False for an allowance of the interval divided by the speed with no floor (4.17 ms
    //       here), which restarts the schedule on those pictures and keeps each one's delay.
    // WHY-THIS-SHAPE: WorkerStalls again -- lateness between 4.17 and 8.33 ms is what separates the two
    //       allowances, and an idle machine never produces it -- graded over the same clear windows as
    //       S15.109. Held at 6 ms, not 5: a picture due in a hold is late by 0-6 ms, so 30% of held
    //       pictures land in the separating band at 6 against 17% at 5, and the 2.33 ms the 6 ms hold
    //       leaves under the allowance no longer has to absorb the host, which the window filter measures.
    // CONTROL: intervalAtSpeed()'s `std::max(..., kMinPictureInterval)` in
    //       backend/daemon/wallpaper_manager.cpp reduced to `atSpeed(ownInterval(picture), speed)`, via
    //       control_batch.sh, --phase highRate, 2026-09-29 -> "FAIL: at speed 2, pictures held up by less
    //       than one capped interval keep the video at 120 a second", median 113.35-113.71 (Lavapipe 113.40) with
    //       every window clear, the speed-1 line green. Reverted. Re-run F1 2026-10-20 at frame rate 120: held.
    if (!setSpeed(2.0)) return;
    if (!gradeUnderStalls("the 240 a second file at speed 2",
                          "at speed 2, pictures held up by less than one capped interval keep the video at " + rate +
                              " a second",
                          frameRate - 0.5)) {
        return;
    }
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
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, evictIdleVideoSessionLocked()'s per-screen
    //       test `currentSeqLocked(...) && screenDrawnLocked(...)` changed to
    //       `playingSeqLocked(...).has_value()` -- the clock's own test -> "FAIL: a ninth video is
    //       refused while every screen is showing a current picture", because a stale-but-drawn
    //       session is evicted instead. Re-run via control_batch.sh, --phase '^evictionWatch$',
    //       2026-10-18, NVIDIA. Reverted.
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
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, the `&& screenDrawnLocked(m.monitorId)` term
    //       deleted from evictIdleVideoSessionLocked() -> "FAIL: a screen that says it is not being
    //       drawn gives its session up for the ninth video", because nothing is then evictable and
    //       the ninth is refused again. Re-run via control_batch.sh, --phase '^evictionWatch$',
    //       2026-10-18, NVIDIA. Reverted.
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
    // `routine` receives handoverDelivered()'s answer: whether the bus would leave the hand-over unlogged.
    const auto take = [&](const std::string& consumer, bool* routine = nullptr) {
        wallpaperx::FrameExport e = exportDelivered(manager, kMonitorHandover, consumer, false, {}, routine);
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
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, recordDeliveredLocked()'s
    //       `return !reportConsumer(...)` made `return true || !reportConsumer(...)`, so every
    //       hand-over is routine and nobody is recorded -> "FAIL: the first hand-over to a consumer is
    //       not routine, so it is logged", via control_batch.sh, --phase '^handoverLog$', re-run
    //       2026-10-06. Reverted.
    bool firstARoutine = false;
    bool againARoutine = false;
    bool firstBRoutine = false;
    const wallpaperx::FrameExport firstA = take(":1.301", &firstARoutine);
    const wallpaperx::FrameExport againA = take(":1.301", &againARoutine);
    const wallpaperx::FrameExport firstB = take(":1.302", &firstBRoutine);
    if (!firstA.handle || !againA.handle || !firstB.handle) {
        notVerified(kGoals, "the three hand-overs this check needs were not all served");
        return;
    }
    check(!firstARoutine, "the first hand-over to a consumer is not routine, so it is logged");
    check(againARoutine, "the next hand-over to the same consumer is routine, so it is not logged again");
    check(!firstBRoutine, "and a second consumer's first hand-over is its own line, not silenced by the first");

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
    //       is not reported again". Replacing it with `false` reddens the other two clauses. Re-run
    //       2026-10-06 via control_batch.sh, --phase '^handoverLog$', the first line. Reverted.
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
    const wallpaperx::FrameExport refusedAgain = exportDelivered(manager, kMonitorHandover, starved);
    check(refusedAgain.reason == wallpaperx::NoFrameReason::NoFreeImage && !refusedAgain.firstRefusal,
          "the same consumer refused again is not reported again");
    const wallpaperx::FrameExport refusedOther = exportDelivered(manager, kMonitorHandover, ":1.499");
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
            wallpaperx::FrameExport e = exportDelivered(manager, kMonitorHandoverCycle, consumer);
            if (e.handle) {
                open.push_back(e.handle->frame.fd);
                open.push_back(e.handle->acquireFd);
            }
            return e;
        };
        bool atCap = false;
        bool firstRefusalReported = false;
        uint64_t lastHeld = 0;
        uint64_t lastNumber = 0;
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
            lastNumber = wallpaperx::testing::handoverOf(*e.handle);
            if (!republish()) break;
        }
        check(atCap && lastHeld != 0, "a consumer can be taken to the point where it is refused while holding images");
        if (atCap && lastHeld != 0) {
            check(firstRefusalReported, "its first refusal is reported");
            manager.releaseBuffer(cycling, kMonitorHandoverCycle, lastHeld, lastNumber);
            const bool republished = republish();
            check(republished, "another picture is published for it to be served again");
            const wallpaperx::FrameExport servedAgain =
                republished ? exportDelivered(manager, kMonitorHandoverCycle, cycling) : wallpaperx::FrameExport{};
            check(servedAgain.handle.has_value(), "and it is served again after giving an image back");
            if (servedAgain.handle) {
                open.push_back(servedAgain.handle->frame.fd);
                open.push_back(servedAgain.handle->acquireFd);
                const bool ready = republish();
                check(ready, "and one more picture is published, so it can be refused once more");
                if (ready) {
                    const wallpaperx::FrameExport refusedAfterService =
                        exportDelivered(manager, kMonitorHandoverCycle, cycling);
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
                    bool routine = false;
                    const wallpaperx::FrameExport e =
                        exportDelivered(manager, kMonitorHandoverMany, consumer, false, {}, &routine);
                    if (!e.handle) continue;
                    ++served;
                    if (!routine) {
                        ++reported;
                        ++reportsPer[consumer];
                    }
                    // Given straight back, so the ring never runs out and every consumer keeps
                    // getting a turn: what is being measured is the reporting, not the holding.
                    wallpaperx::testing::releaseHandle(manager, consumer, kMonitorHandoverMany, *e.handle);
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
                bool routine = false;
                const wallpaperx::FrameExport e =
                    exportDelivered(manager, kMonitorHandoverMany, consumer, false, {}, &routine);
                if (!e.handle) return std::nullopt;
                wallpaperx::testing::releaseHandle(manager, consumer, kMonitorHandoverMany, *e.handle);
                close(e.handle->frame.fd);
                close(e.handle->acquireFd);
                return !routine;
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
            //       newcomer was silent". Re-run 2026-10-24 via control_batch.sh. Reverted.
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
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, retryOnShortageLocked()'s
    //       `if (!classifyFailure().exhausted) throw;` made `throw;`, so no shortage is ever retried ->
    //       "FAIL: an open that runs out of memory frees an idle session and commits on the retry", and the
    //       checks after it. Via control_batch.sh 2026-10-18, phase memoryRetry. Reverted.
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

    // GOAL: S13.139's bound -- each kind of relief is tried ONCE: with no kept still to drop, a second
    //       shortage in a row is reported, not answered by freeing session after session. False for a loop
    //       that keeps evicting while anything is idle, which turns one bad allocation into every other
    //       screen going dark.
    // WHY-THIS-SHAPE: the count graded is OPENS of the new file -- two, the attempt and its one
    //       retry -- rather than closes, which also carry each failed attempt tearing its own
    //       half-built pipeline down and so say nothing about how many sessions were freed. How many
    //       were freed is asserted separately, by name. THREE idle sessions are set up above for this
    //       one check: with two, a daemon that retries once and a daemon that retries for ever both
    //       run out of sessions on the same attempt and produce the same counts, so the check could
    //       not have failed at all.
    // CONTROL: in retryOnShortageLocked() (backend/daemon/wallpaper_manager.cpp), the idle-video step's
    //       `next = Next::Nothing;` made `next = Next::IdleVideo;`, so sessions are freed while any is idle
    //       -> "FAIL: a second shortage in a row is reported rather than answered by freeing another
    //       session", and the check after it. Via control_batch.sh 2026-10-18, phase memoryRetry. Reverted.
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

    // A kept still beside the idle session on monitor 3, so both kinds of relief are on offer.
    constexpr int kStill = 6, kLadder = 7, kBound = 8;
    const std::string still =
        wallpaperx::testing::writeGradientPng(testAssetDir() + "/memory_retry_still.png", 40, 30);
    const auto keepStill = [&] {
        manager.requestWallpaper("static", still, kStill);
        return manager.renderPending(kStill).rendered;
    };
    for (const int id : {kStill, kLadder, kBound}) wallpaperx::testing::nameScreen(manager, id, screenOf(id));
    // A shortage the checks above left unspent would fail the still instead.
    g_failImageCreates.store(0);
    if (!keepStill()) {
        check(false, "a still is drawn and kept beside the idle session (the premise of the relief-order checks)");
        return;
    }

    // GOAL: S15.780 -- a shortage that dropping the kept stills does not relieve then closes an idle video
    //       session and tries again: two shortages in a row still open the video. False for a daemon that
    //       stops after the stills, or tries only one kind of relief.
    // WHY-THIS-SHAPE: two shortages, a kept still and one idle session; three opens of the new file is the
    //       attempt and its two retries, and which session closed is asserted by name.
    // CONTROL: control_batch.sh 2026-10-18, phase memoryRetry, reverted: in retryOnShortageLocked()
    //       (backend/daemon/wallpaper_manager.cpp) the still step's `next = relief == ... ? Next::IdleVideo :
    //       Next::Nothing;` made `next = Next::Nothing;` -> this line, and S13.139's lines above (no still kept there).
    // COVERAGE-ASSERT: both shortages are asserted spent.
    manager.requestWallpaper("video", clips[6], kLadder);
    g_failImageCreates.store(2);
    const PassCost ladder = renderCounted(manager, kLadder);
    reportUnless(g_failImageCreates.load() == 0 && ladder.outcome.rendered && ladder.opens == 3 &&
                     g_files.closes(clips[3]) == 1,
                 "a shortage the kept stills do not relieve closes the idle session and commits on the second retry",
                 describe(ladder) + ", idle session closed " + std::to_string(g_files.closes(clips[3])) + " time(s), " +
                     std::to_string(g_failImageCreates.load()) + " shortage(s) unspent");

    // GOAL: S15.780's bound -- at most two retries, one per kind of relief: a third shortage is reported even
    //       with a second idle session to close. False for a ladder that keeps closing sessions.
    // WHY-THIS-SHAPE: TWO idle sessions are left for it, so a daemon that closed a second one would commit on
    //       its fourth attempt and the check could tell; with one, both would fail alike.
    // CONTROL: control_batch.sh 2026-10-18, phase memoryRetry, reverted: retryOnShortageLocked()'s idle-video
    //       step `next = Next::Nothing;` made `next = Next::IdleVideo;` (backend/daemon/wallpaper_manager.cpp)
    //       -> this line, the one above, and S13.139's bound lines.
    std::vector<int> idle;
    for (const int id : {kMemoryMonitors[0], kMemoryMonitors[4]}) {
        manager.setOutputSize(screenOf(id), kResizedWidth, kResizedHeight, idle);
    }
    if (!keepStill() || idle.size() != 2) {
        check(false, "a still kept again and two sessions left idle (the premise of the two-retry bound)");
        return;
    }
    manager.requestWallpaper("video", clips[8], kBound);
    g_failImageCreates.store(3);
    const PassCost bound = renderCounted(manager, kBound);
    const int idleClosed = g_files.closes(clips[0]) + g_files.closes(clips[4]);
    reportUnless(g_failImageCreates.load() == 0 && !bound.outcome.rendered && bound.opens == 3 && idleClosed == 1,
                 "a third shortage in a row is reported: one kept-still retry and one idle-session retry, no more",
                 describe(bound) + ", " + std::to_string(idleClosed) + " idle session(s) closed, " +
                     std::to_string(g_failImageCreates.load()) + " shortage(s) unspent");
}

// --------------------------------------------------------------------------- //
// Positioning on the daemon: which property changes redraw, and the still's decoded image kept between
// passes. Manager-level, no worker, so every render is one the phase asked for.
// --------------------------------------------------------------------------- //
constexpr uint32_t kKeptStillWidth = 40;
constexpr uint32_t kKeptStillHeight = 30;

struct StillPass {
    bool rendered = false;
    int decodes = 0;
    uint64_t generation = 0;
};

StillPass drawCounted(WallpaperManager& manager, int monitorId) {
    g_extentCreates.store(0);
    StillPass pass;
    pass.rendered = manager.renderPending(monitorId).rendered;
    pass.decodes = g_extentCreates.load();
    if (const auto status = monitorStatus(manager, monitorId)) pass.generation = status->generation;
    return pass;
}

std::string describe(const StillPass& pass) {
    return std::string(pass.rendered ? "committed" : "did not commit") + ", " + std::to_string(pass.decodes) +
           " decode(s), generation " + std::to_string(pass.generation);
}

bool owesRender(const WallpaperManager& manager, int monitorId) {
    const auto status = monitorStatus(manager, monitorId);
    return !status || status->renderOutstanding;
}

std::optional<bool> leftEdgeBlack(WallpaperManager& manager, int monitorId, std::string& reason);

void runKeptStillChecks(const WallpaperManager::Config& base, const std::vector<std::string>& clips) {
    const char* kGoals = "S15.743-S15.749 (the still's decoded image kept between passes)";
    using wallpaperx::PositionMode;
    WallpaperManager::Config config = base;
    config.stillKeptFor = std::chrono::milliseconds(400);
    PhaseGrade grade("the kept-still phase");
    WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};
    struct Disarm {
        ~Disarm() {
            g_failImageCreates.store(0);
            g_countedExtent.store(0);
        }
    } disarm;
    g_countedExtent.store((uint64_t{kKeptStillWidth} << 32) | kKeptStillHeight);
    const std::string dir = testAssetDir();
    const std::string a = wallpaperx::testing::writeGradientPng(dir + "/kept_still_a.png", kKeptStillWidth, kKeptStillHeight);
    const std::string c = wallpaperx::testing::writeGradientPng(dir + "/kept_still_c.png", kKeptStillWidth, kKeptStillHeight);
    const std::string c2 = wallpaperx::testing::writeGradientPng(dir + "/kept_still_c2.png", kKeptStillWidth, kKeptStillHeight);
    const std::string shader = writeUndecodable("kept_still_shader.frag");
    constexpr int kA = 0, kVideo = 1, kC = 2;
    for (const int id : {kA, kVideo, kC}) wallpaperx::testing::nameScreen(manager, id, screenOf(id));

    manager.requestWallpaper("static", a, kA);
    const StillPass first = drawCounted(manager, kA);
    if (!first.rendered) {
        grade.expectNoDevice();
        notVerified(kGoals, "the first still render on this host produced no frame");
        return;
    }
    // COVERAGE-ASSERT: the counter sees the still's own image, so a zero below is a pass that decoded nothing.
    check(first.decodes == 1, "the first draw of a still is seen decoding it once");

    // GOAL: S15.743 -- a layout change on a still is owed one render and draws it from the kept image: one new
    //       frame, no decode, nothing owed after.
    // CONTROL: control_batch.sh 2026-10-18, phase keptStill, each alone, reverted: in renderPassLocked
    //       (backend/daemon/wallpaper_manager.cpp) the m_keptStills lookup made `find(-1)` -> this line, the
    //       free-mode and resize lines (re-run 2026-10-18, NVIDIA); in setWallpaperProperties `++state.requestSeq;
    //       return true;` made `return false;` -> this line and every later line that redraws after a change.
    const bool fitOwed = manager.setWallpaperProperties(kA, {{"position", PositionMode::Fit}});
    const StillPass fit = drawCounted(manager, kA);
    reportUnless(fitOwed && fit.rendered && fit.generation == first.generation + 1 && fit.decodes == 0 && !owesRender(manager, kA),
                 "a layout change on an unchanged still redraws it once without decoding the file", describe(fit));

    // GOAL: S15.744 -- a speed-only change, and a zoom or offset change outside free mode, redraw nothing.
    // CONTROL: control_batch.sh 2026-10-18, phase keptStill, reverted: `const bool relaid = placement(...) !=
    //       placement(properties);` made `= true` (backend/daemon/wallpaper_manager.cpp) -> this line alone.
    const bool speedOwed = manager.setWallpaperProperties(kA, {{"position", PositionMode::Fit}, {"speed", 2.0}});
    const bool speedOutstanding = owesRender(manager, kA);
    const bool zoomOwed = manager.setWallpaperProperties(
        kA, {{"position", PositionMode::Fit}, {"speed", 2.0}, {"position.zoom", 3.0}, {"position.x", 1.0}});
    check(!speedOwed && !speedOutstanding && !zoomOwed && !owesRender(manager, kA),
          "a speed-only change and a zoom/offset change outside free mode redraw nothing");

    const bool freeOwed = manager.setWallpaperProperties(kA, {{"position", PositionMode::Free}, {"position.zoom", 3.0}});
    const StillPass free = drawCounted(manager, kA);
    const bool zoomInFreeOwed =
        manager.setWallpaperProperties(kA, {{"position", PositionMode::Free}, {"position.zoom", 2.0}, {"position.x", 0.5}});
    const StillPass freeAgain = drawCounted(manager, kA);
    reportUnless(freeOwed && zoomInFreeOwed && free.rendered && freeAgain.rendered && free.decodes == 0 && freeAgain.decodes == 0,
                 "in free mode, a zoom or offset change redraws without decoding", describe(free) + "; " + describe(freeAgain));

    // GOAL: S15.743 -- a resize of an unchanged still draws from the kept image.
    std::vector<int> owed;
    manager.setOutputSize(screenOf(kA), kResizedWidth, kResizedHeight, owed);
    const StillPass resized = drawCounted(manager, kA);
    reportUnless(contains(owed, kA) && resized.rendered && resized.decodes == 0,
                 "a resize of an unchanged still redraws without decoding the file", describe(resized));

    // GOAL: S15.745 -- a file touched, rewritten with the same bytes, or replaced behind the daemon is decoded
    //       again at the next redraw; one deleted fails that redraw.
    // WHY-THIS-SHAPE: the same bytes and size are written back, so only the identity (times, inode) can tell.
    // CONTROL: control_batch.sh 2026-10-18, phase keptStill, reverted: StillImage::load's `kept->m_file ==
    //       identity` made `kept` (renderer/renderers/static_renderer.cpp) -> the touched/rewritten/replaced lines.
    PositionMode laidOut = PositionMode::Free;
    const auto layOut = [&](PositionMode mode) {
        laidOut = mode;
        return manager.setWallpaperProperties(kA, {{"position", mode}});
    };
    const auto relayout = [&] { return layOut(laidOut == PositionMode::Centre ? PositionMode::Fit : PositionMode::Centre); };
    const auto bytesOf = [](const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    const std::string original = bytesOf(a);
    const std::vector<std::pair<const char*, std::function<void()>>> changes{
        {"touched", [&] { std::filesystem::last_write_time(a, std::filesystem::last_write_time(a) + std::chrono::seconds(5)); }},
        {"rewritten with the same bytes", [&] { std::ofstream(a, std::ios::binary | std::ios::trunc) << original; }},
        {"replaced by a rename", [&] {
             std::ofstream(a + ".new", std::ios::binary) << original;
             std::filesystem::rename(a + ".new", a);
         }},
    };
    for (const auto& [what, change] : changes) {
        change();
        const bool changeOwed = relayout();
        const StillPass pass = drawCounted(manager, kA);
        reportUnless(changeOwed && pass.rendered && pass.decodes == 1,
                     (std::string("a still ") + what + " behind the daemon is decoded again at the next redraw").c_str(),
                     describe(pass));
    }

    layOut(PositionMode::Stretch);
    g_submitFailGate.arm(CallGate::Mode::Fail);
    const StillPass failed = drawCounted(manager, kA);
    g_submitFailGate.release();
    check(!failed.rendered, "a draw failed at its submission commits nothing (the premise of S15.746 and S15.801)");

    // GOAL: S15.801 -- a layout change on a monitor whose last render failed posts no redraw, and the new
    //       layout is still saved: the next redraw by another cause draws with it (Fit leaves the left edge
    //       black, Stretch before it did not).
    // CONTROL: control_batch.sh 2026-10-18, phase keptStill, NVIDIA and Lavapipe, each alone, reverted, in
    //       setWallpaperProperties (backend/daemon/wallpaper_manager.cpp): `|| !state.lastError.empty()` deleted
    //       -> "...posts no redraw" alone; `if (!state.lastError.empty()) return false;` added above the save ->
    //       "...draws the layout at the next redraw" alone, "the left edge is not black".
    const bool failedOwed = layOut(PositionMode::Fit);
    check(!failedOwed && !owesRender(manager, kA), "a layout change on a monitor whose last render failed posts no redraw");

    // GOAL: S15.746 -- a pass that fails keeps nothing: the redraw after it decodes the unchanged file again.
    // WHY-THIS-SHAPE: the redraw is a resize; a new request would make a new wallpaperSeq, whose stale-image
    //       drop decodes afresh whether the failure kept anything or not.
    // CONTROL: control_batch.sh 2026-10-18 (T2), phase keptStill, NVIDIA and Lavapipe, reverted: the erase is
    //       one line inside renderPassLocked()'s `tearDownFailed` (backend/daemon/wallpaper_manager.cpp), called
    //       before and after the commit block; the call AFTER it deleted -> this line (a failed submission is
    //       inside the commit block, which the earlier call misses). S15.802 is the earlier call's.
    owed.clear();
    manager.setOutputSize(screenOf(kA), kResizedAgainWidth, kResizedAgainHeight, owed);
    const StillPass afterFailure = drawCounted(manager, kA);
    reportUnless(contains(owed, kA) && afterFailure.rendered && afterFailure.decodes == 1,
                 "after a failed draw the next redraw decodes the file again", describe(failed) + "; " + describe(afterFailure));
    std::string edgeReason;
    const std::optional<bool> fitAfterFailure = leftEdgeBlack(manager, kA, edgeReason);
    reportUnless(fitAfterFailure.value_or(false), "a layout set while the last render had failed draws the layout at the next redraw",
                 fitAfterFailure ? "the left edge is not black" : "unreadable: " + edgeReason);

    // GOAL: S15.747 -- setting the same path again decodes it afresh.
    // CONTROL: control_batch.sh 2026-10-18, phase keptStill, reverted: dropStaleKeptStillLocked's
    //       `&& it->second.wallpaperSeq == wallpaperSeq` deleted (backend/daemon/wallpaper_manager.cpp) -> this line.
    manager.requestWallpaper("static", a, kA);
    const StillPass again = drawCounted(manager, kA);
    reportUnless(again.rendered && again.decodes == 1, "setting the same path again decodes the file afresh", describe(again));

    // GOAL: S15.748 -- the kept image is freed stillKeptFor after its last draw by the clock round, which
    //       asks to be woken for it until then.
    // WHY-THIS-SHAPE: advanceVideo is handed the time, so the expiry is reached without sleeping on it.
    // CONTROL: control_batch.sh 2026-10-18, phase keptStill, reverted: the releaseExpiredStillsLocked call
    //       deleted from advanceVideo (backend/daemon/wallpaper_manager.cpp) -> this line.
    const auto drawnAt = std::chrono::steady_clock::now();
    const wallpaperx::ClockRound pending = manager.advanceVideo(drawnAt);
    manager.advanceVideo(drawnAt + config.stillKeptFor + std::chrono::milliseconds(200));
    relayout();
    const StillPass expired = drawCounted(manager, kA);
    reportUnless(pending.nextDue.has_value() && *pending.nextDue <= drawnAt + config.stillKeptFor && expired.rendered &&
                     expired.decodes == 1,
                 "the kept image is freed stillKeptFor after its last draw, with a wake asked for until then",
                 describe(expired) + (pending.nextDue ? "" : "; no wake asked for"));

    // An idle video session: drawn, then owing a render after a resize, as in the memory-retry phase.
    manager.requestWallpaper("video", clips[0], kVideo);
    const PassCost video = renderCounted(manager, kVideo);
    owed.clear();
    manager.setOutputSize(screenOf(kVideo), kResizedWidth, kResizedHeight, owed);
    manager.requestWallpaper("static", c, kC);
    const bool setUp = video.outcome.rendered && contains(owed, kVideo) && drawCounted(manager, kC).rendered;
    check(setUp, "an idle video session and a second still are set up for the shortage cases");
    if (!setUp) return;

    // GOAL: S15.749 -- running out of GPU memory frees the kept still images before an idle video session.
    // CONTROL: control_batch.sh 2026-10-18, phase keptStill, reverted: in retryOnShortageLocked()
    //       (backend/daemon/wallpaper_manager.cpp) `Next next = Next::KeptStills;` made `Next::IdleVideo` ->
    //       this line and the replaced-still line below.
    // COVERAGE-ASSERT: the injected shortage is asserted spent.
    g_files.resetCounts();
    manager.requestWallpaper("static", c2, kC);
    g_failImageCreates.store(1);
    const StillPass shortC = drawCounted(manager, kC);
    check(g_failImageCreates.load() == 0, "the injected shortage was spent on the still's image");
    relayout();
    const StillPass aAfterShortage = drawCounted(manager, kA);
    reportUnless(shortC.rendered && g_files.closes(clips[0]) == 0 && aAfterShortage.decodes == 1,
                 "a shortage frees the kept stills, not the idle video session, and the retry commits",
                 describe(shortC) + "; video closed " + std::to_string(g_files.closes(clips[0])) + " time(s); the other "
                     "still then " + describe(aAfterShortage));

    // GOAL: S15.747 -- another wallpaper taking the screen frees its kept image: with none kept anywhere, the
    //       next shortage has only the idle video session to free. Also S15.782: a relief step that frees
    //       nothing does not spend the retry.
    // CONTROL: control_batch.sh 2026-10-18, phase keptStill, each alone, reverted (backend/daemon/
    //       wallpaper_manager.cpp): the dropStaleKeptStillLocked call deleted from renderPassLocked -> this
    //       line and the same-path line; in retryOnShortageLocked() `freed = dropKeptStillsLocked();` made
    //       `dropKeptStillsLocked(); freed = true;` -> this line.
    manager.requestWallpaper("shader", shader, kA);
    manager.renderPending(kA);
    g_files.resetCounts();
    manager.requestWallpaper("static", c, kC);
    g_failImageCreates.store(1);
    const StillPass shortAgain = drawCounted(manager, kC);
    reportUnless(g_failImageCreates.load() == 0 && shortAgain.rendered && g_files.closes(clips[0]) >= 1,
                 "a still replaced by another wallpaper keeps nothing, so a shortage frees the idle video session",
                 describe(shortAgain) + "; video closed " + std::to_string(g_files.closes(clips[0])) + " time(s)");
}

// Whether the left edge of the monitor's served frame, half way down, is black. Empty when unreadable.
std::optional<bool> leftEdgeBlack(WallpaperManager& manager, int monitorId, std::string& reason) {
    wallpaperx::FrameExport exported = exportDelivered(manager, monitorId);
    if (!exported.handle) {
        reason = "no frame was served";
        return std::nullopt;
    }
    const FrameLayout layout = layoutOf(*exported.handle);
    const std::vector<unsigned char> image = readServedBgra(layout, reason);
    if (exported.handle->frame.fd >= 0) close(exported.handle->frame.fd);
    if (exported.handle->acquireFd >= 0) close(exported.handle->acquireFd);
    if (image.size() < size_t{layout.width} * layout.height * 4) return std::nullopt;
    const unsigned char* px = &image[(size_t{layout.height / 2} * layout.width) * 4];
    return px[0] < 8 && px[1] < 8 && px[2] < 8;
}

void runPositionVideoChecks(const WallpaperManager::Config& base, const std::string& clip, const std::string& onePicture) {
    const char* kGoals = "S15.750-S15.751 (a video takes a layout change)";
    using wallpaperx::PositionMode;
    PhaseGrade grade("the video position phase");
    WallpaperManager::Config config = base;
    config.unwatchedAfter = std::chrono::hours(1);  // plays with no consumer for the whole phase
    WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};
    constexpr int kPlaying = 0, kStopped = 1;
    for (const int id : {kPlaying, kStopped}) wallpaperx::testing::nameScreen(manager, id, screenOf(id));
    manager.requestWallpaper("video", clip, kPlaying);
    if (!renderCounted(manager, kPlaying).outcome.rendered) {
        grade.expectNoDevice();
        notVerified(kGoals, "the first video render on this host produced no frame");
        return;
    }
    std::string reason;
    const std::optional<bool> coverBlack = leftEdgeBlack(manager, kPlaying, reason);
    if (!coverBlack.has_value() || *coverBlack) {
        notVerified(kGoals, coverBlack ? "the cover frame's left edge is already black" : "the frame was unreadable: " + reason);
        return;
    }

    // GOAL: S15.750 -- a playing video takes a layout change at its next picture: no render owed, no file
    //       reopened, no decoder torn down, and the next picture shown is laid out the new way (a 4:3 clip
    //       fitted to a 64x24 screen leaves the left edge black).
    // WHY-THIS-SHAPE: the wait has a deadline whose failure is graded, so a stalled clock cannot pass as shown.
    // CONTROL: control_batch.sh 2026-10-18, phase positionVideo, each alone, reverted: in advanceSessionLocked
    //       (backend/daemon/wallpaper_manager.cpp) the clock's `placement(it->second.properties)` made
    //       `wallpaperx::Placement{}` -> "...is laid out the new way"; in setWallpaperProperties the Playing
    //       early return deleted -> "...owes no render" and "showed a picture within 5 s" (an owed render stops it).
    g_files.resetCounts();
    g_codecFrees.store(0);
    const bool owed = manager.setWallpaperProperties(kPlaying, {{"position", PositionMode::Fit}});
    check(!owed && !owesRender(manager, kPlaying), "a layout change on a playing video owes no render");
    bool shown = false;
    for (const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
         !shown && std::chrono::steady_clock::now() < deadline;) {
        for (const wallpaperx::ClockRound::Shown& s : manager.advanceVideo(std::chrono::steady_clock::now()).shown) {
            shown = shown || s.monitorId == kPlaying;
        }
        if (!shown) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(shown, "the playing video showed a picture within 5 s of the change");
    if (shown) {
        const std::optional<bool> fitBlack = leftEdgeBlack(manager, kPlaying, reason);
        reportUnless(fitBlack.value_or(false), "the playing video's next picture is laid out the new way",
                     fitBlack ? "the left edge is not black" : "unreadable: " + reason);
        reportUnless(g_files.opens(clip) == 0 && g_codecFrees.load() == 0, "and the video was not restarted for it",
                     std::to_string(g_files.opens(clip)) + " open(s), " + std::to_string(g_codecFrees.load()) + " decoder(s) freed");
    }

    // GOAL: S15.751 -- a video that is not playing (a one-picture file, parked on its picture) is redrawn once
    //       for a layout change, in the new layout.
    // CONTROL: control_batch.sh 2026-10-18, phase positionVideo, reverted: setWallpaperProperties's
    //       `++state.requestSeq; return true;` made `return false;` (backend/daemon/wallpaper_manager.cpp) -> this line.
    manager.requestWallpaper("video", onePicture, kStopped);
    if (!renderCounted(manager, kStopped).outcome.rendered) {
        check(false, "the parked video monitor is drawn");
        return;
    }
    for (int round = 0; round < 3; ++round) manager.advanceVideo(std::chrono::steady_clock::now());
    const bool stoppedOwed = manager.setWallpaperProperties(kStopped, {{"position", PositionMode::Fit}});
    const uint64_t before = monitorStatus(manager, kStopped) ? monitorStatus(manager, kStopped)->generation : 0;
    const bool redrawn = manager.renderPending(kStopped).rendered;
    const uint64_t after = monitorStatus(manager, kStopped) ? monitorStatus(manager, kStopped)->generation : 0;
    const std::optional<bool> stoppedBlack = leftEdgeBlack(manager, kStopped, reason);
    reportUnless(stoppedOwed && redrawn && after == before + 1 && !owesRender(manager, kStopped) && stoppedBlack.value_or(false),
                 "a video that is not playing is redrawn once in the new layout",
                 std::string(stoppedOwed ? "owed" : "not owed") + ", generation " + std::to_string(before) + " -> " +
                     std::to_string(after) + (stoppedBlack.value_or(false) ? "" : ", left edge not black"));
}

void runRelaidReleaseChecks(const WallpaperManager::Config& base, const std::string& clip, const std::string& onePicture) {
    using wallpaperx::PositionMode;
    const char* kGoals = "S15.783 (a layout change on a video whose decoder was released is drawn)";
    PhaseGrade grade("the relaid-release phase");
    WallpaperManager::Config config = base;
    config.idleSessionRelease = std::chrono::milliseconds(200);
    config.pausedSessionRelease = std::chrono::milliseconds(400);
    config.unwatchedAfter = std::chrono::milliseconds(300);
    WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};
    // Its own copy, so the paused monitor's reopened session closing again is not counted as this one's.
    const std::string unwatchedClip = testAssetDir() + "/relaid_unwatched.mp4";
    std::filesystem::copy_file(clip, unwatchedClip, std::filesystem::copy_options::overwrite_existing);
    g_files.watch(unwatchedClip);
    struct Variant {
        int id;
        const char* what;
        std::string path;
        bool pause;
        bool plays;
    };
    const Variant variants[] = {{0, "a video paused past pausedSessionRelease", clip, true, true},
                                {1, "an unwatched video", unwatchedClip, false, true},
                                {2, "a parked one-picture video", onePicture, false, false}};
    for (const Variant& v : variants) {
        wallpaperx::testing::nameScreen(manager, v.id, screenOf(v.id));
        manager.requestWallpaper("video", v.path, v.id);
        if (!renderCounted(manager, v.id).outcome.rendered) {
            if (v.id == 0) {
                grade.expectNoDevice();
                notVerified(kGoals, "the first video render on this host produced no frame");
                return;
            }
            check(false, (std::string(v.what) + " is drawn first").c_str());
            continue;
        }
        std::string reason;
        const std::optional<bool> coverBlack = leftEdgeBlack(manager, v.id, reason);
        if (!coverBlack.has_value() || *coverBlack) {
            notVerified(kGoals, std::string(v.what) + ": the cover frame's left edge is not readable, or already black");
            continue;
        }
        // A picture shown by the clock spends the credit a new video is watched on.
        bool shown = !v.plays;
        for (const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
             !shown && std::chrono::steady_clock::now() < deadline;) {
            for (const wallpaperx::ClockRound::Shown& s : manager.advanceVideo(std::chrono::steady_clock::now()).shown) {
                shown = shown || s.monitorId == v.id;
            }
            if (!shown) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!shown) {
            check(false, (std::string(v.what) + " shows a picture within 5 s (the premise of S15.783)").c_str());
            continue;
        }
        if (v.pause) manager.setMonitorPause(v.id, "always_paused");
        // Past unwatchedAfter on the real clock, which is the one watchedLocked() reads.
        std::this_thread::sleep_for(config.unwatchedAfter + std::chrono::milliseconds(100));
        g_files.resetCounts();
        bool released = false;
        auto at = std::chrono::steady_clock::now();
        for (int round = 0; round < 20 && !released; ++round, at += std::chrono::milliseconds(250)) {
            manager.advanceVideo(at);
            released = g_files.closes(v.path) >= 1;
        }
        if (!released) {
            check(false, (std::string(v.what) + " has its decoder released (the premise of S15.783)").c_str());
            continue;
        }

        // GOAL: S15.783 -- a layout change on a video whose decoder was released is drawn: the reopened session
        //       catches up to the picture on screen and draws it in the new layout. False for a daemon that takes
        //       the picture on screen as already shown, leaving the old layout up until the video next plays.
        // WHY-THIS-SHAPE: three ways a session is released, each its own monitor; the redraw is graded by the
        //       generation and the frame's pixels (4:3 fitted to 64x24 leaves the left edge black), not by
        //       "rendered" alone, which a pass that kept the old frame could also report.
        // CONTROL: control_batch.sh 2026-10-18, phase relaidRelease, reverted: in renderPassLocked
        //       (backend/daemon/wallpaper_manager.cpp) `&& it->second.shownPlacement == placement(...)` deleted from
        //       keepsFrame -> this line, for each of the three.
        const uint64_t before = monitorStatus(manager, v.id) ? monitorStatus(manager, v.id)->generation : 0;
        const bool owed = manager.setWallpaperProperties(v.id, {{"position", PositionMode::Fit}});
        const PassCost redraw = renderCounted(manager, v.id);
        const uint64_t after = monitorStatus(manager, v.id) ? monitorStatus(manager, v.id)->generation : 0;
        const std::optional<bool> fitBlack = leftEdgeBlack(manager, v.id, reason);
        reportUnless(owed && redraw.outcome.rendered && after == before + 1 && !owesRender(manager, v.id) &&
                         fitBlack.value_or(false),
                     (std::string(v.what) + ", its decoder released, is redrawn in the new layout").c_str(),
                     std::string(owed ? "owed" : "not owed") + ", " + describe(redraw) + ", generation " +
                         std::to_string(before) + " -> " + std::to_string(after) +
                         (fitBlack.value_or(false) ? "" : ", left edge not black"));
    }
}

// A still pass held inside its decode while the screen's settings change under it, on a render worker
// because the hold must be on the thread the Device is pinned to.
void runLayoutDragChecks(const WallpaperManager::Config& config) {
    using wallpaperx::PositionMode;
    const char* kGoals = "S15.784-S15.786 (a still pass overtaken by layout changes alone commits)";
    constexpr int kDrag = 0;
    PhaseGrade grade("the layout-drag phase");
    FrameReadyLog signals;
    WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};
    RenderWorker worker(manager, [&signals](int id, uint64_t g) { signals.record(id, g); });
    struct Disarm {
        RenderWorker& worker;
        ~Disarm() {
            g_imageGate.release();
            g_framebufferGate.release();
            g_framebufferLostGate.release();
            g_failImageCreates.store(0);
            g_countedExtent.store(0);
            worker.stop();
        }
    } disarm{worker};
    const std::string dir = testAssetDir();
    const std::string a = wallpaperx::testing::writeGradientPng(dir + "/layout_drag_a.png", 40, 30);
    const std::string b = wallpaperx::testing::writeGradientPng(dir + "/layout_drag_b.png", 40, 30);
    wallpaperx::testing::nameScreen(manager, kDrag, screenOf(kDrag));
    const auto generation = [&] {
        const auto status = monitorStatus(manager, kDrag);
        return status ? status->generation : 0;
    };
    manager.requestWallpaper("static", a, kDrag);
    worker.post(kDrag);
    if (!drainWorker(worker) || generation() == 0) {
        grade.expectNoDevice();
        notVerified(kGoals, "the first still render on this host produced no frame");
        return;
    }
    // A pass that decodes `path` afresh, held inside the creation of the still's own image.
    const auto holdPass = [&](const std::string& path) {
        g_imageGate.arm(CallGate::Mode::Hold, "40x30");
        manager.requestWallpaper("static", path, kDrag);
        worker.post(kDrag);
        const bool held = g_imageGate.waitFired(std::chrono::seconds(30));
        check(held, "a redraw is held inside its decode (the premise of the layout-drag checks)");
        return held;
    };
    const auto edgeBlack = [&]() -> std::optional<bool> {
        auto answer = std::make_shared<std::promise<std::optional<bool>>>();
        std::future<std::optional<bool>> future = answer->get_future();
        worker.postTask([answer, &manager](RenderWorker::TaskState state) {
            std::optional<bool> black;
            std::string reason;
            if (state == RenderWorker::TaskState::Run) {
                try {
                    black = leftEdgeBlack(manager, kDrag, reason);
                } catch (const std::exception&) {
                }
            }
            answer->set_value(black);
        });
        if (future.wait_for(std::chrono::seconds(60)) != std::future_status::ready) return std::optional<bool>{};
        return future.get();
    };
    const auto settle = [&] {
        worker.post(kDrag);
        return drainWorker(worker) && !owesRender(manager, kDrag);
    };

    // GOAL: S15.784 -- a still pass overtaken only by a layout change commits, laid out the newest way, and
    //       leaves nothing owed. False for a daemon that discards it, which during a slider drag discards
    //       every pass and shows nothing until the drag stops.
    // WHY-THIS-SHAPE: the pass is held inside its own decode while the change lands, and no render is posted
    //       for the change, so a frame after the release can only be the held pass's. Fit on a 64x24 screen
    //       blacks the left edge, which the held pass's cover start does not.
    // CONTROL: control_batch.sh 2026-10-18, phase layoutDrag, reverted: in renderPassLocked
    //       (backend/daemon/wallpaper_manager.cpp) `|| (error.empty() && !keepsFrame && relaidOnlySince(...))`
    //       deleted from `current` -> this line alone.
    {
        const uint64_t before = generation();
        if (!holdPass(a)) return;
        const bool owed = manager.setWallpaperProperties(kDrag, {{"position", PositionMode::Fit}});
        g_imageGate.release();
        const bool drained = drainWorker(worker);
        const uint64_t after = generation();
        const bool stillOwed = owesRender(manager, kDrag);
        const std::optional<bool> black = edgeBlack();
        reportUnless(owed && drained && after == before + 1 && !stillOwed && black.value_or(false),
                     "a still pass overtaken only by a layout change commits, laid out the newest way",
                     std::string(owed ? "owed" : "not owed") + ", generation " + std::to_string(before) + " -> " +
                         std::to_string(after) + (stillOwed ? ", a render owed" : "") +
                         (black.value_or(false) ? "" : ", left edge not black"));
    }

    // GOAL: S15.785 -- any other change discards the pass, before or after a layout change: a new wallpaper
    //       set while it is held means its picture is never committed, and the newer request stays owed.
    // WHY-THIS-SHAPE: both orders, because the daemon tracks the latest unbroken run of layout changes: a
    //       new wallpaper after the run ends it, and one before means the run does not reach back to the pass.
    // CONTROL: control_batch.sh 2026-10-18, phase layoutDrag, each alone, reverted, in relaidOnlySince()
    //       (backend/daemon/wallpaper_manager.cpp): `state.relaid.to == state.requestSeq &&` deleted -> the
    //       "layout change and then" line; `state.relaid.from <= seq &&` deleted -> the "and then a layout
    //       change" line.
    const auto discardedBy = [&](const char* what, const std::function<bool()>& change) {
        if (!settle()) {
            check(false, "the screen settles between the layout-drag cases");
            return;
        }
        const uint64_t before = generation();
        if (!holdPass(a)) return;
        const bool owed = change();
        g_imageGate.release();
        const bool drained = drainWorker(worker);
        const uint64_t after = generation();
        const bool stillOwed = owesRender(manager, kDrag);
        reportUnless(owed && drained && after == before && stillOwed, what,
                     std::string(owed ? "owed" : "not owed") + ", generation " + std::to_string(before) + " -> " +
                         std::to_string(after) + (stillOwed ? "" : ", nothing owed"));
    };
    discardedBy("a still pass overtaken by a layout change and then a new wallpaper is discarded", [&] {
        const bool owed = manager.setWallpaperProperties(kDrag, {{"position", PositionMode::Free}, {"position.zoom", 2.0}});
        manager.requestWallpaper("static", b, kDrag);
        return owed;
    });
    discardedBy("a still pass overtaken by a new wallpaper and then a layout change is discarded", [&] {
        manager.requestWallpaper("static", b, kDrag);
        return manager.setWallpaperProperties(kDrag, {{"position", PositionMode::Free}, {"position.zoom", 2.5}});
    });

    // GOAL: S15.786 -- a pass that failed is discarded when only a layout change overtook it: it records no
    //       failure in the newer request's name, and that request stays owed. False for a daemon that lets a
    //       failed pass through as current, which marks the layout change serviced and never draws it.
    // WHY-THIS-SHAPE: the shortage is injected into the held creation itself, with no kept still or idle
    //       session to free, so no retry can turn it into a success.
    // CONTROL: control_batch.sh 2026-10-18, phase layoutDrag, reverted: `error.empty() &&` deleted from
    //       renderPassLocked's `current` (backend/daemon/wallpaper_manager.cpp) -> this line, "nothing owed".
    // COVERAGE-ASSERT: the injected shortage is asserted spent, so the pass graded is one that failed.
    discardedBy("a failed still pass overtaken by a layout change is discarded, and the change stays owed", [&] {
        const bool owed = manager.setWallpaperProperties(kDrag, {{"position", PositionMode::Free}, {"position.zoom", 3.0}});
        g_failImageCreates.store(1);
        return owed;
    });
    check(g_failImageCreates.load() == 0, "the shortage injected into the failed pass was spent on it");

    // GOAL: S15.802 -- a still pass that failed after keeping its decoded image, and was overtaken, keeps
    //       nothing: the newer pass decodes the unchanged file again. False for a daemon that erases the
    //       kept image only after the commit block, which an overtaken pass returns from inside.
    // WHY-THIS-SHAPE: held inside the image rebuild of a resize (after the kept image is back in the map),
    //       overtaken there by a layout change, then failed as a lost device: a shortage would have its relief
    //       drop every kept still anyway. The decode count is of images at the still's own 40x30 size.
    // CONTROL: control_batch.sh 2026-10-18 (T2), phase layoutDrag, NVIDIA and Lavapipe, reverted: in
    //       renderPassLocked (backend/daemon/wallpaper_manager.cpp) the `if (failure) tearDownFailed();` BEFORE
    //       the commit block deleted (the erase is one line in that lambda; S15.746 is the later call's) -> this
    //       line. The same break reddens S13.128 in failedPass, whose session that call also frees.
    // COVERAGE-ASSERT: the hold and the injected failure are asserted to have fired.
    if (!settle()) {
        check(false, "the screen settles before the overtaken-failure case");
        return;
    }
    g_countedExtent.store((uint64_t{40} << 32) | 30);
    g_framebufferGate.arm(CallGate::Mode::Hold, extentOf(kResizedWidth, kResizedHeight));
    g_framebufferLostGate.arm(CallGate::Mode::Fail, extentOf(kResizedWidth, kResizedHeight));
    std::vector<int> resized;
    manager.setOutputSize(screenOf(kDrag), kResizedWidth, kResizedHeight, resized);
    worker.post(kDrag);
    const bool heldInRebuild = g_framebufferGate.waitFired(std::chrono::seconds(30));
    check(heldInRebuild && contains(resized, kDrag), "a still's resize pass is held inside its image rebuild (the premise)");
    if (!heldInRebuild) return;
    const bool relaidOwed = manager.setWallpaperProperties(kDrag, {{"position", PositionMode::Fit}});
    const uint64_t before = generation();
    g_extentCreates.store(0);
    g_framebufferGate.release();
    worker.post(kDrag);
    const bool drained = drainWorker(worker);
    const bool failedLost = g_framebufferLostGate.waitFired(std::chrono::seconds(0));
    check(relaidOwed && drained && failedLost, "the held pass is overtaken by a layout change and fails (the premise)");
    const int decodes = g_extentCreates.load();
    reportUnless(generation() == before + 1 && decodes == 1,
                 "a failed still pass that was overtaken keeps nothing: the newer pass decodes the file again",
                 "generation " + std::to_string(before) + " -> " + std::to_string(generation()) + ", " +
                     std::to_string(decodes) + " decode(s)");
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
    //       reported not verified. Observed 2026-09-21 on the NVIDIA entry. Reverted.
    // GOAL: S13.192 -- a video that fails for good shows nothing and its error: its last picture is
    //       not held. The same settled state -- no frame, the error standing, a frame request
    //       answering RenderFailed -- is what is read below.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, both `state.hasFrame = false;` in
    //       recordFailureLocked() and the `releaseTarget(member.monitorId);` of advanceSessionLocked()'s
    //       failure path deleted, so a failed video keeps its last picture -> "FAIL: a video whose playback
    //       keeps failing stops on its own ...". Re-run via control_batch.sh, --phase '^playbackRetry$',
    //       2026-10-18, NVIDIA, reverted (S13.142's and the shortage phase's lines were not re-run).
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
    reportUnless(exportDelivered(manager, kMonitorRetry).reason == wallpaperx::NoFrameReason::RenderFailed,
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
    // WHY-THIS-SHAPE: the injector fails exactly two pictures and then lets every one through by
    //       itself, so two of the three reopens are spent before the video plays again whatever the
    //       scheduler does. Disarming it from this thread instead leaves the number of reopens spent to
    //       scheduling, so the budget could run out or the partial-spend check fail on a correct
    //       daemon. Two spent, not one, so a budget left unrestored shows as one reopen where three
    //       were owed.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, `playbackBudgetLocked(it->second) =
    //       {it->second.wallpaperSeq};` deleted from advanceSessionLocked()'s commit, via
    //       control_batch.sh 2026-10-05 on video_wallpaper and video_wallpaper_lavapipe -> exactly "FAIL:
    //       a picture shown since the last failure restores the whole retry budget", printing "1
    //       reopen(s) after the picture, where 3 were owed". Reverted.
    // COVERAGE-ASSERT: the two reopens before the picture, and the two failures the injector made,
    //       are both asserted. CONTROL (test-side, via control_batch.sh 2026-10-05): the arm's limit made
    //       kSpentFirst + 1 -> "FAIL: part of the budget -- two of three reopens -- ...", printing "3
    //       reopen(s) spent, 3 failure(s) injected". Reverted.
    if (!play(clips[1])) {
        check(false, "a new video wallpaper on that monitor draws again after the budget was spent");
        return;
    }
    constexpr int kSpentFirst = static_cast<int>(WallpaperManager::kPlaybackRetries) - 1;
    g_files.resetCounts();
    const int injectedFirst = g_playbackFailure.injected();
    g_playbackFailure.arm(std::numeric_limits<int>::max(), kSpentFirst);
    const bool failedFirst = settlesWithin(std::chrono::seconds(30),
                                           [&clips] { return g_files.opens(clips[1]) >= kSpentFirst; });
    check(failedFirst, "the new wallpaper's playback fails and is reopened, so the budget is partly spent");
    if (!failedFirst) {
        g_playbackFailure.disarm();
        return;
    }

    // Let it play again: a committed clock picture is what restores the budget. Every picture of the
    // last reopen goes through, so five more generations mean it is playing.
    const std::optional<wallpaperx::MonitorStatus> playing = monitorStatus(manager, kMonitorRetry);
    const uint64_t resumedBy = (playing ? playing->generation : 0) + 5;
    const bool resumed = signals.waitAtLeast(kMonitorRetry, resumedBy);
    g_playbackFailure.disarm();
    if (!resumed) {
        check(false, "the reopened wallpaper plays again once the injected failures are spent");
        return;
    }
    const int spentFirst = g_files.opens(clips[1]);
    const int injectedSpent = g_playbackFailure.injected() - injectedFirst;
    reportUnless(spentFirst == kSpentFirst && injectedSpent == kSpentFirst,
                 "part of the budget -- two of three reopens -- was spent before the video played again",
                 std::to_string(spentFirst) + " reopen(s) spent, " + std::to_string(injectedSpent) +
                     " failure(s) injected");

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
    //       Re-run via control_batch.sh, --phase '^gpuFault$', 2026-10-18, NVIDIA: the same three lines. Reverted.
    // COVERAGE-ASSERT: the injected fault is asserted to have been spent before anything is read, so
    //       "no reopen" cannot be read off an injection that never fired.
    resetCountsAfterOpenOf(clips[0], "the GPU-fault phase");
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
//   resume-seek   in the seek back of <path>'s resume: played 40 pictures, hidden until its session
//                 is released, then shown again
//   resume-catch-up  the same, in the first packet the resume's catch-up sends
//   shared-catch-up  <path> on <monitor> and <extra monitor>, both paused until released; the first
//                 reopens catching up, and the death is in the second's render pass decoding that catch-up
constexpr const char* kBlameChildFlag = "--blame-child";
constexpr uint64_t kBlameResumeAfterPictures = 40;
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
    const bool resume = scenario == "resume-seek" || scenario == "resume-catch-up";
    if (scenario == "idle") {
        config.unwatchedAfter = std::chrono::milliseconds(300);
        config.idleSessionRelease = std::chrono::milliseconds(300);
    }
    if (resume) config.idleSessionRelease = std::chrono::milliseconds(300);
    if (scenario == "shared-catch-up") config.pausedSessionRelease = std::chrono::milliseconds(300);
    g_files.watch(path);
    if (!extra.empty()) g_files.watch(extra);
    if (scenario == "decode") g_deathNotePath = (std::filesystem::path(argv[3]).parent_path() / "death-thread").string();
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
        if (scenario == "shared-catch-up") {
            // Driven on this thread with the worker idle, so no clock round runs between the two passes.
            g_crashThread.store(::gettid());
            std::vector<int> owed;
            wallpaperx::testing::nameScreen(manager, monitorId, "DP-BLAME");
            wallpaperx::testing::nameScreen(manager, extraMonitor, "DP-SECOND");
            for (const int id : restored) manager.renderPending(id);
            if (g_files.live().size() != 1) std::_Exit(kBlameChildSetupFailed);
            uint64_t shown = 0;
            for (const Clock::time_point until = Clock::now() + std::chrono::seconds(20); shown < kBlameResumeAfterPictures;) {
                if (Clock::now() > until) std::_Exit(kBlameChildSetupFailed);
                for (const auto& s : manager.advanceVideo(std::chrono::steady_clock::now()).shown) shown += s.monitorId == monitorId;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            manager.setMonitorPause(monitorId, "always_paused");
            manager.setMonitorPause(extraMonitor, "always_paused");
            const auto now = std::chrono::steady_clock::now();
            manager.advanceVideo(now);
            manager.advanceVideo(now + std::chrono::seconds(2));
            if (!g_files.live().empty()) std::_Exit(kBlameChildSetupFailed);
            // The first screen reopens at its picture, keeping its frame, so its session is left catching up.
            manager.setMonitorPause(monitorId, "play");
            const wallpaperx::ClockRound round = manager.advanceVideo(std::chrono::steady_clock::now());
            if (std::find(round.retry.begin(), round.retry.end(), monitorId) == round.retry.end()) {
                std::_Exit(kBlameChildSetupFailed);
            }
            manager.renderPending(monitorId);
            if (g_files.live().size() != 1) std::_Exit(kBlameChildSetupFailed);
            // The second joins it at a new size, so its own pass decodes the catch-up.
            manager.setMonitorPause(extraMonitor, "play");
            manager.setOutputSize("DP-SECOND", kResizedWidth, kResizedHeight, owed);
            armCrash(CrashOn::CatchUp, path);
            manager.renderPending(extraMonitor);
            std::_Exit(kBlameChildNeverDied);
        }
        for (const int id : restored) worker.post(id);
        if (scenario == "decode") {
            armCrash(CrashOn::Decode, path);
        } else if (resume) {
            for (const int id : wallpaperx::testing::nameScreen(manager, monitorId, "DP-BLAME")) worker.post(id);
            if (!signals.waitAtLeast(monitorId, kBlameResumeAfterPictures)) std::_Exit(kBlameChildSetupFailed);
            g_files.resetCounts();
            manager.setOutputVisible("DP-BLAME", false);
            worker.post(monitorId);
            const Clock::time_point releasedBy = Clock::now() + std::chrono::seconds(20);
            while (g_files.closes(path) < 1) {
                if (Clock::now() > releasedBy) std::_Exit(kBlameChildSetupFailed);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            armCrash(scenario == "resume-seek" ? CrashOn::Seek : CrashOn::CatchUp, path);
            manager.setOutputVisible("DP-BLAME", true);
            worker.post(monitorId);
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
                // GOAL: both staged shortages land on the request's and the render's opens, never on the scan's.
                // CONTROL: test-side, by hand, 2026-10-24 (T18a), restored: the open hook's `t_scanThread ? 0 :`
                //       removed -> the scan logs "SoftwareScan: the daemon has run out of file descriptors" and
                //       the phase reports "NOT VERIFIED -- S13.190 ... the 'evict' child never reached ...".
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
    //       With a decode thread the packet is sent on it, so a Decode death is staged on any thread
    //       but the child's main one (see crashDueFor), and the check below says which thread died.
    // CONTROL: backend/daemon/wallpaper_manager.cpp, via control_batch.sh, --phase '^playbackBlame$',
    //       2026-10-19 (T5c), NVIDIA, reverted: `const PlayingBlame decoding(*line);` deleted from the
    //       decode thread's hooks.around -> "FAIL: a death in the clock's decode of one video names that
    //       video", "FAIL: after two deaths in its playback, the next start refuses that video, saying
    //       so" and "FAIL: and the refused file is never opened". `const PlayingBlame working(*line);`
    //       deleted from advanceSessionLocked() instead stays green on NVIDIA: the death is on the
    //       decode thread, which that guard never covers.
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

        // GOAL: S13.184's decode-thread clause -- the death graded above happened on the session's own
        //       decode thread, not the render worker's or the child's main thread. False if the staging
        //       fired somewhere else, which would leave the decode thread's blame ungraded.
        // WHY-THIS-SHAPE: the child writes the dying thread's id, the render worker's, its own pid and
        //       whether any packet was sent off those two threads (a decode thread exists) to a file just
        //       before faulting; the only other thread that sends this file's packets is the session's
        //       decode thread. A device with no decode thread (Lavapipe) decodes on the render thread, so
        //       there only "not the main thread" is graded, and that is printed.
        // CONTROL: test-side only, by hand, 2026-10-19 (T5c), NVIDIA, restored by hash: the note made to
        //       give the render worker's id as the dying thread -> this line, "died on thread N, render
        //       worker N, ..., a decode thread running". It controls the comparison, not a production
        //       break: no small change to production moves this death onto the render thread.
        {
            std::ifstream note(scratch.path() / "decode" / "death-thread");
            long dying = 0, render = 0, mainThread = 0;
            int threaded = 0;
            const bool read = static_cast<bool>(note >> dying >> render >> mainThread >> threaded);
            const std::string where = read ? "died on thread " + std::to_string(dying) + ", render worker " +
                                                 std::to_string(render) + ", main " + std::to_string(mainThread) +
                                                 (threaded != 0 ? ", a decode thread running" : ", no decode thread")
                                           : std::string("the child left no note of the dying thread");
            reportUnless(read && dying != mainThread && (threaded == 0 || dying != render),
                         "the death in the clock's decode was on the session's decode thread", where);
            if (read && threaded == 0) {
                std::printf("video_wallpaper_test: this device runs no decode thread, so the death was graded as off "
                            "the main thread only (%s)\n", where.c_str());
            }
        }

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
        // The ledger's own evidence here comes after the reset: the bystander's file, which this start
        // opened to play it, must have been seen, or the zero below reads a blind hook.
        reportUnless(drainWorker(worker) && g_files.opens(bystander) >= 1 && g_files.opens(blamed) == 0,
                     "and the refused file is never opened, while the ledger saw the other video's opened",
                     std::to_string(g_files.opens(blamed)) + " open(s) of the refused file, " +
                         std::to_string(g_files.opens(bystander)) + " of the other");
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
    //       pipeline under a PlayingBlame naming `*session.blame->line.load()`, as closeFailedVideoSessionLocked()
    //       does for the file's own failure -> "FAIL: a death freeing a video's decoder when its
    //       monitor is given another video names nothing" and the same for a still, the idle release
    //       and an eviction, each printing the record naming video_session_0.mp4. The applying-record
    //       half: the same function made to free under an `ApplyingRecord` naming the file -> the four
    //       "-- and leaves no applying record either" lines, beside S13.119's and S13.130's. Both
    //       observed 2026-09-21 on the NVIDIA entry, and both re-run via control_batch.sh, --phase
    //       '^playbackBlame$', 2026-09-30, NVIDIA, now that the idle release and an eviction reach
    //       closeVideoSessionLocked() through releaseVideoSessionLocked(): the same four lines each.
    //       Reverted.
    //       The naming half re-run 2026-10-19 (T7), RTX, against the shared blame line: held, the same lines.
    //       The applying-record half re-run 2026-10-19 (T7b), RTX: held, the "-- and leaves no applying record
    //       either" lines.
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
    //       video"; (b) the condition of `if (failure.fault() == Fault::File) freeing.emplace(...)` dropped, so the
    //       guard is taken for every failure -> "FAIL: a death freeing a video's decoder after the device was
    //       lost names nothing". Each alone; (a) observed 2026-09-21, (b) re-run 2026-10-18 (T2) via
    //       control_batch.sh, NVIDIA. Reverted.
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
// CONTROL: control_batch.sh 2026-10-18 (T3), NVIDIA, each hide length alone, reverted, all in
//       backend/daemon/wallpaper_manager.cpp. The three `cleanPlay.lastShown.reset()` drops this drive
//       crosses -- setOutputVisible()'s, videoSessionLocked()'s new-session one, advanceSessionLocked()'s
//       stopped branch -- deleted AND creditCleanPlayLocked()'s `onTime` replaced by the rule before the
//       fix, `now - *play.lastShown <= kMaxPictureInterval` -> "FAIL: a 3 s hide is not counted as play",
//       the 6 s run GREEN: the defect the short hide exists for. The same drops with advanceSessionLocked's
//       `onTime` forced true -> "FAIL: a 6 s hide is not counted as play"; the 3 s run stayed GREEN, not
//       understood. The drops alone, or `onTime` forced true alone, leave both green: the reopened session's
//       first picture is drawn from a catch-up, never a Waiting clock, so it is never on time and the gap
//       before it is never credited.
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
    //       has its count cleared", printing "the count is still 1, 138 collected, 0 refused", alone.
    //       Via control_batch.sh, --phase cleanPlayHide6s, 2026-09-29, NVIDIA. Reverted.
    const bool cleared = settlesWithin(std::chrono::seconds(12), [&] { return countNow() == 0; });
    reportUnless(cleared, "a video that plays normally for cleanPlayAfter has its count cleared",
                 "the count is still " + std::to_string(countNow()) + ", " + collector.describe());
}

// Waits for kMonitorClean's first two pictures. No first picture is this host not drawing; a first
// with no second is the clock, which is the daemon's.
// CONTROL: the clock's wait check in advanceSessionLocked (backend/daemon/wallpaper_manager.cpp) returning
//       `now + std::chrono::seconds(100)` for a waiting picture, via control_batch.sh, --phase
//       '^(slowStallCredit|midWaitRaise|dueSlowdown)$', 2026-10-14, NVIDIA -> this line red in all three,
//       where a wait for the second picture alone had reported NOT VERIFIED "on this host". Restored.
bool firstTwoPictures(FrameReadyLog& signals, PhaseGrade& grade, const char* goal) {
    if (!signals.wait(kMonitorClean, 1)) {
        grade.expectNoDevice();
        notVerified(goal, "the video did not draw on this host");
        return false;
    }
    const bool second = signals.wait(kMonitorClean, 2);
    check(second, "a video that drew its first picture draws its second on its own clock");
    return second;
}

// S15.114: at a slow speed, a stall longer than the file's own interval earns no clean-play credit,
// however long the speed-scaled interval is.
//
// GOAL: S15.114 -- a picture drawn later than one of the file's own intervals is not counted as clean
//       play at any speed: at speed 0.5 on the one-picture-every-2-s file (a picture every 4 s), a render
//       thread held up 7 s after a picture draws the next about 3 s late, and the 7 s gap before it adds
//       nothing toward cleanPlayAfter, so the death count still stands when that picture is announced.
//       False for an allowance of the interval divided by the speed (4 s here), under which the stall
//       counts as play and the count is cleared by it.
// WHY-THIS-SHAPE: 0.5, because at 1 or 2 the speed-scaled interval is not longer than the file's own, so
//       the two allowances agree. Late by 3 s: halfway between the file's 2 s and the scaled 4 s.
//       cleanPlayAfter is 5.5 s, above the 4 s gap before the hold and below the 7 s across it. The count
//       is read as soon as that picture is announced; a run whose gaps do not bracket 5.5 s that way, or
//       whose next picture came before the read, is reported as not graded. The first two pictures come
//       through firstTwoPictures(): a missing second is the daemon's clock and fails.
// CONTROL: in advanceSessionLocked (backend/daemon/wallpaper_manager.cpp) the on-time allowance's
//       `std::min(intervalAtSpeed(timing, speed), ownInterval(timing))` reduced to
//       `intervalAtSpeed(timing, speed)`, via control_batch.sh, --phase '^slowStallCredit$', 2026-10-14 ->
//       "FAIL: at speed 0.5, a stall longer than the file's own interval earns no clean-play credit", "the
//       count is 0" with 7005 ms across the held picture, NVIDIA. Restored. The stopped clock: see firstTwoPictures().
void runSlowStallCreditChecks(const WallpaperManager::Config& base, const std::string& clip) {
    const char* kGoal = "S15.114 (at a slow speed, a stall longer than the file's own interval earns no clean-play "
                        "credit)";
    const wallpaperx::testing::ScratchDir scratch("videoslowstall");
    if (!scratch.ok()) {
        notVerified(kGoal, "no scratch directory could be created");
        return;
    }
    WallpaperManager::Config config = base;
    config.configFile = scratch.path() / "config.json";
    config.stateDir = scratch.path() / "state";
    constexpr auto kCleanPlayAfter = std::chrono::milliseconds(5500);
    config.cleanPlayAfter = kCleanPlayAfter;
    writeConfig(config.configFile, {{kMonitorClean, "video", clip}});
    {
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

    PhaseGrade grade("the slow-stall credit phase");
    FrameReadyLog signals;
    PictureLog pictures;
    WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};
    Collector collector(manager, kMonitorClean, ":1.slowstall");
    RenderWorker worker(manager, [&](int id, uint64_t g) {
        pictures.record(id, g);
        signals.record(id, g);
        collector.announced(id, g);
    });
    collector.attach(worker);
    WorkerUnblock unblock{worker};
    const std::vector<int> restored = manager.restoreSaved();
    manager.recordStartingVideos();
    check(countNow() == 1, "the video starts this run carrying one death (the harness's premise)");
    manager.setWallpaperProperties(kMonitorClean, {{"speed", 0.5}});
    wallpaperx::testing::nameScreen(manager, kMonitorClean, "DP-SLOWSTALL");
    for (const int id : restored) worker.post(id);
    if (!firstTwoPictures(signals, grade, kGoal)) return;
    const Clock::time_point heldFrom = Clock::now();
    worker.postTask([](RenderWorker::TaskState state) {
        if (state == RenderWorker::TaskState::Run) std::this_thread::sleep_for(std::chrono::milliseconds(7000));
    });
    const bool third = signals.wait(kMonitorClean, 3);
    const int afterStall = countNow();
    const uint64_t latestAtRead = latestGeneration(pictures, kMonitorClean);
    check(third, "the video draws again after the render thread is held up");
    if (!third) return;

    const std::vector<PictureLog::Event> shown = pictures.forMonitor(kMonitorClean);
    const std::optional<Clock::time_point> first = firstSeen(shown, 1);
    const std::optional<Clock::time_point> second = firstSeen(shown, 2);
    const std::optional<Clock::time_point> stalled = firstSeen(shown, 3);
    if (!first || !second || !stalled) {
        notVerified(kGoal, "the first three pictures' announcements were not all recorded");
        return;
    }
    const auto ms = [](Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };
    const double beforeHold = ms(*second - *first);
    const double stalledGap = ms(*stalled - *second);
    std::printf("video_wallpaper_test: at speed 0.5, %.0f ms between the first two pictures, %.0f ms across the "
                "held-up one (the hold began %.0f ms after the second); the count read %d with %llu picture(s) "
                "announced\n",
                beforeHold, stalledGap, ms(heldFrom - *second), afterStall,
                static_cast<unsigned long long>(latestAtRead));
    std::fflush(stdout);
    const double limit = ms(kCleanPlayAfter);
    // Late against the 4 s the picture was due after: past the file's 2 s, short of the scaled 4 s.
    const double late = stalledGap - 4000;
    if (beforeHold > limit - 100 || stalledGap < limit + 100 || late < 2400 || late > 3600 || latestAtRead != 3) {
        notVerified(kGoal, "the gaps did not bracket cleanPlayAfter as this check needs (at most " +
                               std::to_string(limit - 100) + " ms before the hold, at least " +
                               std::to_string(limit + 100) + " ms across it with the picture 2.4-3.6 s late, and the "
                               "count read before the next picture)");
        return;
    }
    reportUnless(afterStall == 1, "at speed 0.5, a stall longer than the file's own interval earns no clean-play credit",
                 "the count is " + std::to_string(afterStall) + ", " + collector.describe());
}

// The death count the playing record carries for kMonitorClean: 0 when it names none, -1 with no record.
int deathsIn(const std::filesystem::path& stateDir) {
    const std::optional<wallpaperx::PlayingHistory> h = wallpaperx::readPlayingRecord(stateDir);
    if (!h) return -1;
    for (const wallpaperx::PlayingVideo& v : h->videos) {
        if (v.monitorId == kMonitorClean) return v.deaths;
    }
    return 0;
}

double millisBetween(Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration<double, std::milli>(to - from).count();
}

// GOAL: S15.118 -- raising the speed during a wait re-times only what is left of it. On the
//       one-picture-every-2-s file at speed 0.5 (a picture every 4 s), raised to 2 about 2.4 s into a wait:
//       the waiting picture comes the rest of its 2 s, at 2, after the change (about 400 ms) -- not at once
//       and not at the old time -- and is on time, so the 2.8 s since the picture before it is clean play
//       and clears the death count. False for a wait re-timed as a whole at the new speed (its due time
//       1.4 s in the past: drawn at once, judged late, no credit) and for a wait that keeps the old speed.
// WHY-THIS-SHAPE: cleanPlayAfter is 2 s. The first clock-drawn picture only starts the credit, so the gap
//       across the change is all the credit there is when the next picture is announced, and it clears the
//       count only if that picture was on time; the count is read before the one after it, 1 s later. The
//       release time is bounded on both sides of the re-timed moment, halfway to each wrong answer, from
//       the measured moment of the change. A change outside 2.0-3.4 s into the wait, or a gap the host
//       stretched past what brackets cleanPlayAfter, is reported as not graded.
// CONTROL: via control_batch.sh, --phase '^midWaitRaise$', 2026-10-14, each restored, in
//       VideoSession::Waiting::retime (backend/daemon/wallpaper_manager.cpp): (a) reduced to `speed =
//       newSpeed; return;`, the whole wait re-timed -> both lines red, "0 ms after the change" and "the
//       count is 1"; (b) returning at once, the old speed kept -> the release line alone, 1600 ms; (c) for "is drawn": retime ending with `left =
//       std::chrono::hours(1); from = now;` -> that line, here and in the next two phases; (d) for the
//       one-death premise: restoreSaved()'s `if (deaths > 0)` before `m_startingVideos.push_back` made
//       `if (false)` -> "FAIL: the video starts this run carrying one death", here and in dueSlowdown.
void runMidWaitRaiseChecks(const WallpaperManager::Config& base, const std::string& clip) {
    const char* kGoal = "S15.118 (a speed raised during a wait re-times only what is left of it)";
    const wallpaperx::testing::ScratchDir scratch("videomidwait");
    if (!scratch.ok()) {
        notVerified(kGoal, "no scratch directory could be created");
        return;
    }
    WallpaperManager::Config config = base;
    config.configFile = scratch.path() / "config.json";
    config.stateDir = scratch.path() / "state";
    constexpr auto kCleanPlayAfter = std::chrono::milliseconds(2000);
    config.cleanPlayAfter = kCleanPlayAfter;
    writeConfig(config.configFile, {{kMonitorClean, "video", clip}});
    {
        wallpaperx::PlayingRecord record(config.stateDir);
        record.start({{kMonitorClean, clip, 1}});
    }

    PhaseGrade grade("the mid-wait speed-raise phase");
    FrameReadyLog signals;
    PictureLog pictures;
    WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};
    Collector collector(manager, kMonitorClean, ":1.midwait");
    RenderWorker worker(manager, [&](int id, uint64_t g) {
        pictures.record(id, g);
        signals.record(id, g);
        collector.announced(id, g);
    });
    collector.attach(worker);
    WorkerUnblock unblock{worker};
    const std::vector<int> restored = manager.restoreSaved();
    manager.recordStartingVideos();
    check(deathsIn(config.stateDir) == 1, "the video starts this run carrying one death (the harness's premise)");
    manager.setWallpaperProperties(kMonitorClean, {{"speed", 0.5}});
    wallpaperx::testing::nameScreen(manager, kMonitorClean, "DP-MIDWAIT");
    for (const int id : restored) worker.post(id);
    if (!firstTwoPictures(signals, grade, kGoal)) return;
    const std::optional<Clock::time_point> second = firstSeen(pictures, kMonitorClean, 2);
    if (!second) {
        notVerified(kGoal, "the second picture's announcement was not recorded");
        return;
    }
    std::this_thread::sleep_until(*second + std::chrono::milliseconds(2400));
    manager.setWallpaperProperties(kMonitorClean, {{"speed", 2.0}});
    worker.wake();
    const Clock::time_point changed = Clock::now();
    const bool third = settlesWithin(std::chrono::seconds(10), [&] { return latestGeneration(pictures, kMonitorClean) >= 3; });
    const int afterThird = deathsIn(config.stateDir);
    const uint64_t latestAtRead = latestGeneration(pictures, kMonitorClean);
    check(third, "the picture waiting when the speed was raised is drawn");
    if (!third) return;

    const std::optional<Clock::time_point> shown = firstSeen(pictures, kMonitorClean, 3);
    const double changeAfter = millisBetween(*second, changed);
    const double released = shown ? millisBetween(changed, *shown) : -1.0;
    const double gap = shown ? millisBetween(*second, *shown) : -1.0;
    // The 2 s of file time between the two pictures, run at 0.5 until the change and at 2 after.
    const double expected = (2000.0 - changeAfter * 0.5) / 2.0;
    const double keptOldSpeed = 4000.0 - changeAfter;
    std::printf("video_wallpaper_test: speed 0.5 raised to 2 %.0f ms into a wait; the picture came %.0f ms later "
                "(re-timed: %.0f ms, the old speed: %.0f ms), %.0f ms after the one before it; the count read %d with "
                "%llu picture(s) announced\n",
                changeAfter, released, expected, keptOldSpeed, gap, afterThird,
                static_cast<unsigned long long>(latestAtRead));
    std::fflush(stdout);
    if (!shown || changeAfter < 2000 || changeAfter > 3400) {
        notVerified(kGoal, "the speed change did not land 2.0-3.4 s into the 4 s wait, so there was no wait left to "
                           "re-time");
        return;
    }
    reportUnless(released >= expected / 2 && released <= expected + (keptOldSpeed - expected) / 2,
                 "a speed raised during a wait releases the waiting picture once the rest of its time has run at "
                 "the new speed",
                 "it came " + std::to_string(released) + " ms after the change, re-timed it is due after " +
                     std::to_string(expected) + " ms");
    if (gap < 2100 || latestAtRead != 3) {
        notVerified(kGoal, "the gap across the change was not at least 100 ms over cleanPlayAfter, or the next "
                           "picture came before the count was read");
        return;
    }
    reportUnless(afterThird == 0, "a picture released by a speed raised during its wait is on time, and the wait counts "
                                  "as clean play",
                 "the count is " + std::to_string(afterThird) + ", " + collector.describe());
}

// GOAL: S15.119 -- a draw outside the clock, of a picture waiting under a speed change the clock has not
//       yet seen, keeps the new speed's schedule. On the one-picture-every-2-s file at speed 0.5, 2 s into
//       a 4 s wait, the speed is raised to 2 and a resize redraws the waiting picture at once: its place on
//       the schedule is the re-timed one (about 500 ms after the change) and the next picture follows 1 s
//       after that. False for a redraw that reads the wait at the old speed: it schedules from the old due
//       time and the next picture comes about 3 s after the change.
// WHY-THIS-SHAPE: no wake after the speed change and the redraw posted at once, so the render worker,
//       which runs a job before its clock round, draws before the clock re-times anything. The redraw's
//       own picture must come within 150 ms of the change, or it was the clock that drew it, and the
//       phase says so. The bound on the next picture lies halfway between the two answers.
// CONTROL: `waiting->retime(now, speed);` removed from markShown (backend/daemon/wallpaper_manager.cpp),
//       via control_batch.sh, --phase '^renderPathRetime$', 2026-10-14 -> "FAIL: a redraw after a speed
//       change the clock has not seen keeps the new speed's schedule", the next picture 3000 ms after the
//       change against 1500 due, the redraw 1 ms after it, NVIDIA. Restored. "Goes on playing": the
//       retime `hours(1)` break of the phase above -> red. Restored.
void runRenderPathRetimeChecks(const WallpaperManager::Config& base, const std::string& clip) {
    const char* kGoal = "S15.119 (a redraw after a speed change the clock has not seen keeps the new speed's schedule)";
    PhaseGrade grade("the redraw re-timing phase");
    FrameReadyLog signals;
    PictureLog pictures;
    WallpaperManager manager(base);
    CaptureGradeOnExit captureGrade{grade, manager};
    Collector collector(manager, kMonitorClean, ":1.retime");
    RenderWorker worker(manager, [&](int id, uint64_t g) {
        pictures.record(id, g);
        signals.record(id, g);
        collector.announced(id, g);
    });
    collector.attach(worker);
    WorkerUnblock unblock{worker};
    const std::string screen = "DP-RETIME";
    wallpaperx::testing::nameScreen(manager, kMonitorClean, screen);
    manager.requestWallpaper("video", clip, kMonitorClean, {{"speed", 0.5}});
    worker.post(kMonitorClean);
    if (!signals.wait(kMonitorClean, 1)) {
        grade.expectNoDevice();
        notVerified(kGoal, "the video did not draw on this host");
        return;
    }
    const std::optional<Clock::time_point> first = firstSeen(pictures, kMonitorClean, 1);
    if (!first) {
        notVerified(kGoal, "the first picture's announcement was not recorded");
        return;
    }
    std::this_thread::sleep_until(*first + std::chrono::milliseconds(2000));
    manager.setWallpaperProperties(kMonitorClean, {{"speed", 2.0}});
    std::vector<int> redraw;
    manager.setOutputSize(screen, kResizedWidth, kResizedHeight, redraw);
    const Clock::time_point asked = Clock::now();
    for (const int id : redraw) worker.post(id);
    if (redraw != std::vector<int>{kMonitorClean}) {
        notVerified(kGoal, "the resize did not name the monitor for a redraw, which the resize phase grades");
        return;
    }
    const bool third = settlesWithin(std::chrono::seconds(10), [&] { return latestGeneration(pictures, kMonitorClean) >= 3; });
    check(third, "the video goes on playing after a redraw under a new speed");
    if (!third) return;

    const std::optional<Clock::time_point> redrawn = firstSeen(pictures, kMonitorClean, 2);
    const std::optional<Clock::time_point> next = firstSeen(pictures, kMonitorClean, 3);
    const double changeAfter = millisBetween(*first, asked);
    const double drawnAfter = redrawn ? millisBetween(asked, *redrawn) : -1.0;
    const double nextAfter = next ? millisBetween(asked, *next) : -1.0;
    const double expected = (2000.0 - changeAfter * 0.5) / 2.0 + 1000.0;
    const double oldSchedule = 4000.0 - changeAfter + 1000.0;
    std::printf("video_wallpaper_test: speed raised %.0f ms into a wait with a redraw; the redraw came %.0f ms later "
                "and the next picture %.0f ms after the change (the new speed's schedule: %.0f ms, the old: %.0f ms)\n",
                changeAfter, drawnAfter, nextAfter, expected, oldSchedule);
    std::fflush(stdout);
    if (!redrawn || !next || drawnAfter > 150 || changeAfter < 1800 || changeAfter > 2800) {
        notVerified(kGoal, "the redraw was not drawn within 150 ms of a change 1.8-2.8 s into the wait, so the clock "
                           "may have seen the speed first");
        return;
    }
    reportUnless(nextAfter >= expected - 150 && nextAfter <= expected + (oldSchedule - expected) / 2,
                 "a redraw after a speed change the clock has not seen keeps the new speed's schedule",
                 "the next picture came " + std::to_string(nextAfter) + " ms after the change, due after " +
                     std::to_string(expected) + " ms");
}

// GOAL: S15.120 -- lowering the speed after a picture is already due changes nothing about that picture:
//       held up past its time by a stall, it is drawn as soon as the render thread is free, and the stall
//       stays a stall, earning no clean-play credit. On the one-picture-every-2-s file at speed 2 (a
//       picture a second), the render thread held 3.4 s after a picture and the speed lowered to 0.5 at
//       2 s, once the next was due. False for a wait re-timed as a whole at the new speed (the picture then
//       waits to 4 s, 600 ms past the hold), and for a re-time that moves a due picture's time to the moment
//       the clock sees the change (the stall then reads as on time).
// WHY-THIS-SHAPE: 0.5 is the lowest speed, and its allowance is the file's own 2 s, so the picture must be
//       more than 2 s late to be a stall: the hold makes it 2.4 s late. A whole re-time would release it
//       4 s after the picture before, so the bound is half the distance from the hold's end to that, at most
//       300 ms; a hold ending past 3.5 s leaves under 250 ms and is reported as not graded. cleanPlayAfter is 1 s, and the first
//       clock-drawn picture only starts the credit, so the 3.4 s gap clears the count only if that picture
//       is judged on time; the next picture is 4 s off, so the count read after this one is its answer. A
//       change that did not land between the due time and the end of the hold is reported as not graded.
// CONTROL: via control_batch.sh, --phase '^dueSlowdown$', 2026-10-14, each restored, in
//       VideoSession::Waiting::retime (backend/daemon/wallpaper_manager.cpp): (a) reduced to `speed =
//       newSpeed; return;` -> both lines red, "598 ms after the hold ended" and "the count is 0"; (b)
//       `from = changed;` made `from = now;`, so a due picture's time moves to the change -> the credit
//       line alone, "the count is 0", NVIDIA. "Is drawn": the retime `hours(1)` break -> red. (a) re-run
//       2026-10-15 after the bound became relative, in a private copy of the tree, Lavapipe and NVIDIA:
//       both lines red, "598 ms after the hold" against a 300 ms bound. The not-graded branch: the hold
//       made 3600 ms (test-side) -> "NOT VERIFIED ... did not end 3.2-3.5 s after the picture".
void runDueSlowdownChecks(const WallpaperManager::Config& base, const std::string& clip) {
    const char* kGoal = "S15.120 (a speed lowered after a picture is due neither delays it nor makes a stall clean)";
    const wallpaperx::testing::ScratchDir scratch("videodueslow");
    if (!scratch.ok()) {
        notVerified(kGoal, "no scratch directory could be created");
        return;
    }
    WallpaperManager::Config config = base;
    config.configFile = scratch.path() / "config.json";
    config.stateDir = scratch.path() / "state";
    config.cleanPlayAfter = std::chrono::milliseconds(1000);
    writeConfig(config.configFile, {{kMonitorClean, "video", clip}});
    {
        wallpaperx::PlayingRecord record(config.stateDir);
        record.start({{kMonitorClean, clip, 1}});
    }

    PhaseGrade grade("the due slow-down phase");
    FrameReadyLog signals;
    PictureLog pictures;
    WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};
    Collector collector(manager, kMonitorClean, ":1.dueslow");
    RenderWorker worker(manager, [&](int id, uint64_t g) {
        pictures.record(id, g);
        signals.record(id, g);
        collector.announced(id, g);
    });
    collector.attach(worker);
    WorkerUnblock unblock{worker};
    const std::vector<int> restored = manager.restoreSaved();
    manager.recordStartingVideos();
    check(deathsIn(config.stateDir) == 1, "the video starts this run carrying one death (the harness's premise)");
    manager.setWallpaperProperties(kMonitorClean, {{"speed", 2.0}});
    wallpaperx::testing::nameScreen(manager, kMonitorClean, "DP-DUESLOW");
    for (const int id : restored) worker.post(id);
    if (!firstTwoPictures(signals, grade, kGoal)) return;
    const std::optional<Clock::time_point> second = firstSeen(pictures, kMonitorClean, 2);
    if (!second) {
        notVerified(kGoal, "the second picture's announcement was not recorded");
        return;
    }
    auto holdFrom = std::make_shared<std::atomic<Clock::rep>>(0);
    auto holdTo = std::make_shared<std::atomic<Clock::rep>>(0);
    worker.postTask([holdFrom, holdTo](RenderWorker::TaskState state) {
        if (state != RenderWorker::TaskState::Run) return;
        holdFrom->store(Clock::now().time_since_epoch().count());
        std::this_thread::sleep_for(std::chrono::milliseconds(3400));
        holdTo->store(Clock::now().time_since_epoch().count());
    });
    std::this_thread::sleep_until(*second + std::chrono::milliseconds(2000));
    manager.setWallpaperProperties(kMonitorClean, {{"speed", 0.5}});
    worker.wake();
    const Clock::time_point changed = Clock::now();
    const bool third = settlesWithin(std::chrono::seconds(15), [&] { return latestGeneration(pictures, kMonitorClean) >= 3; });
    const int afterThird = deathsIn(config.stateDir);
    const uint64_t latestAtRead = latestGeneration(pictures, kMonitorClean);
    check(third, "the picture due when the speed was lowered is drawn");
    if (!third) return;

    const std::optional<Clock::time_point> shown = firstSeen(pictures, kMonitorClean, 3);
    const Clock::time_point heldFrom{Clock::duration(holdFrom->load())};
    const Clock::time_point heldTo{Clock::duration(holdTo->load())};
    const double holdStart = millisBetween(*second, heldFrom);
    const double changeAfter = millisBetween(*second, changed);
    const double holdEnd = millisBetween(*second, heldTo);
    const double drawnAfterHold = shown ? millisBetween(heldTo, *shown) : -1.0;
    const double gap = shown ? millisBetween(*second, *shown) : -1.0;
    std::printf("video_wallpaper_test: at speed 2, the render thread held %.0f-%.0f ms after a picture and the speed "
                "lowered to 0.5 at %.0f ms; the next picture came %.0f ms after the hold, %.0f ms after the one before "
                "it; the count read %d with %llu picture(s) announced\n",
                holdStart, holdEnd, changeAfter, drawnAfterHold, gap, afterThird,
                static_cast<unsigned long long>(latestAtRead));
    std::fflush(stdout);
    // Late past the file's own 2 s against the 1 s it was due after, which only a hold of 3.2 s or more makes.
    if (!shown || holdFrom->load() == 0 || holdTo->load() == 0 || holdStart > 800 || changeAfter < 1050 ||
        changeAfter > holdEnd - 50) {
        notVerified(kGoal, "the hold did not begin before the picture was due, or the speed change did not land "
                           "between its due time and the end of the hold");
        return;
    }
    if (holdEnd < 3200 || holdEnd > 3500) {
        notVerified(kGoal, "the hold did not end 3.2-3.5 s after the picture before it, so the picture was not more "
                           "than 2 s late or a whole re-time would land too close to the hold's end to tell apart");
        return;
    }
    reportUnless(drawnAfterHold <= std::min(300.0, (4000.0 - holdEnd) / 2),
                 "a picture already due when its speed is lowered is drawn as soon as the render thread is free",
                 "it came " + std::to_string(drawnAfterHold) + " ms after the hold ended");
    if (gap < 1100 || latestAtRead != 3) {
        notVerified(kGoal, "the gap across the hold was not at least 100 ms over cleanPlayAfter, or the next picture "
                           "came before the count was read");
        return;
    }
    reportUnless(afterThird == 1, "a speed lowered during a stall does not make the stall clean play",
                 "the count is " + std::to_string(afterThird) + ", " + collector.describe());
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

// --------------------------------------------------------------------------- //
// Step 15 chunk 3c: the pause setting stops and resumes a playing video, and a video whose decoder was
// released resumes on the picture it stopped on. Over the bus, because the method, the manager and the
// worker being woken are the chain under test.
// --------------------------------------------------------------------------- //

// The counter fixture the resume phases play: 320 pictures, 30 a second, each drawing its own frame
// number (renderer/CMakeLists.txt's counter recipe).
std::string counterClipPath() { return std::string(WALLPAPERX_COUNTER_VIDEO); }
std::string longGopClipPath() { return std::string(WALLPAPERX_VIDEO_ASSET_DIR) + "/wallpaper_4k_long_gop.mp4"; }

// Why a served frame could not be read, by whose doing. Only `hostLimit` is a reason to skip.
struct ReadFailure {
    std::string hostLimit;
    std::string daemonFault;
    // Already printed as FAIL (harness); nothing read after it is graded.
    bool harness = false;
};

// Reads the frame number a counter fixture draws into every picture, from a frame served over the bus:
// by mapping a LINEAR dma_buf, or by importing an opaque fd into GL on the GPU that exported it -- the
// plugin's own importer. Nothing, with the reason filed by whose doing it was, when it cannot.
class CounterReader {
public:
    std::optional<int> read(const wallpaperx::testing::Frame& frame, ReadFailure& failure) {
        if (frame.handleType == 1 && frame.drmModifier == wallpaperx::testing::kDrmModifierLinear &&
            !frame.planes.empty()) {
            const wallpaperx::testing::MappedFrame mapped(frame.fd.get(), frame.memorySize);
            if (!mapped.valid()) {
                failure.hostLimit = "the exported fd could not be mapped: " + mapped.error();
                return std::nullopt;
            }
            std::vector<unsigned char> pixels;
            pixels.reserve(static_cast<size_t>(frame.width) * frame.height * 4);
            for (uint32_t y = 0; y < frame.height; ++y) {
                const unsigned char* row = mapped.row(frame.planes[0].get<0>(), frame.planes[0].get<1>(), y,
                                                      static_cast<size_t>(frame.width) * 4);
                if (row == nullptr) {
                    failure.daemonFault = "the frame's published layout (offset " +
                                          std::to_string(frame.planes[0].get<0>()) + ", row pitch " +
                                          std::to_string(frame.planes[0].get<1>()) + ") puts row " + std::to_string(y) +
                                          " outside its " + std::to_string(frame.memorySize) + " bytes";
                    return std::nullopt;
                }
                pixels.insert(pixels.end(), row, row + static_cast<size_t>(frame.width) * 4);
            }
            return wallpaperx::testing::counterFromBgra(pixels, frame.width, frame.height);
        }
#if WALLPAPERX_HAVE_FRAME_IMPORT
        if (!m_glReady) {
            std::array<uint8_t, 16> uuid{};
            if (frame.deviceUuid.size() == uuid.size()) std::copy(frame.deviceUuid.begin(), frame.deviceUuid.end(), uuid.begin());
            if (!m_gl.createMatching(uuid, m_glWhy)) {
                failure.hostLimit = "no GL context on the exporting GPU: " + m_glWhy;
                return std::nullopt;
            }
            m_glReady = true;
        }
        wallpaperx::FrameImport::Request request = wallpaperx::testing::requestFromFrame(frame);
        if (request.fd < 0) {
            failure.harness = true;
            return std::nullopt;
        }
        if (wallpaperx::FrameImport::supportsSync()) {
            for (const sdbus::UnixFd& sync : frame.syncFds) {
                const int fd = ::dup(sync.get());
                if (fd < 0) {
                    wallpaperx::testing::harnessFailure(std::string("a sync descriptor could not be duplicated: ") +
                                                        std::strerror(errno));
                    failure.harness = true;
                } else {
                    request.syncFds.push_back(fd);
                }
            }
        }
        const wallpaperx::FrameImport::Result imported =
            failure.harness ? wallpaperx::FrameImport::Result{} : wallpaperx::FrameImport::import(request);
        // import() sets to -1 what it consumed.
        if (request.fd >= 0) ::close(request.fd);
        for (const int fd : request.syncFds) {
            if (fd >= 0) ::close(fd);
        }
        if (failure.harness) return std::nullopt;
        if (imported.texture == 0) {
            // Either side: this host's GL refusing, or metadata the daemon published that no importer
            // could take. Nothing here tells the two apart, so it is not graded.
            failure.hostLimit = "the frame could not be imported (this host's GL, or the daemon's published "
                                "metadata): " + imported.error;
            return std::nullopt;
        }
        std::string why;
        const std::vector<unsigned char> pixels = wallpaperx::testing::readSampled(imported.texture, frame.width,
                                                                                   frame.height, why);
        wallpaperx::FrameImport::release(imported);
        if (pixels.empty()) {
            failure.hostLimit = why;
            return std::nullopt;
        }
        return wallpaperx::testing::counterFromBgra(pixels, frame.width, frame.height);
#else
        failure.hostLimit = "this build has no GL importer, and the export is not a LINEAR dma_buf that could be mapped";
        return std::nullopt;
#endif
    }

private:
#if WALLPAPERX_HAVE_FRAME_IMPORT
    wallpaperx::testing::HeadlessGl m_gl;
    bool m_glReady = false;
    std::string m_glWhy;
#endif
};

// One served picture: its generation -- one per picture committed -- and the frame number read from it.
struct ReadPicture {
    uint64_t generation = 0;
    int counter = -1;
};

// A consumer that takes each newly announced picture, as the KDE plugin does, reads its frame number
// and gives it straight back. Only on a NEW generation: a release re-announces the frame it frees, and
// fetching on those would be the polling consumer runVisibilityChecks warns about -- and, while the
// video is stopped, a task posted to the worker that would wake it. A fetch serves the newest picture,
// so a slow consumer can skip one; each reading keeps its generation, and a check about two successive
// pictures uses only readings whose generations are successive.
class CountedPictures {
public:
    CountedPictures(sdbus::IProxy& proxy, const PictureLog& pictures, int32_t monitorId, CounterReader& reader)
        : m_proxy(proxy), m_pictures(pictures), m_monitorId(monitorId), m_reader(reader) {}

    // The picture served now, whatever its generation; nothing when it could not be read.
    std::optional<ReadPicture> current() {
        std::optional<wallpaperx::testing::Frame> frame;
        try {
            frame.emplace(getFrame(m_proxy, m_monitorId));
        } catch (const sdbus::Error& e) {
            // The daemon's own answer: no importer or readback on this host produces it.
            if (m_failure.daemonFault.empty()) {
                m_failure.daemonFault = "the daemon refused to serve the picture on screen: " + e.getName() + ": " +
                                        e.getMessage();
            }
            return std::nullopt;
        }
        const std::optional<int> counter = readInto(*frame);
        giveBack(*frame);
        if (!counter) return std::nullopt;
        return ReadPicture{frame->generation, *counter};
    }
    // Every picture read, in the order they were announced.
    const std::vector<ReadPicture>& pictures() const { return m_pictures_read; }
    // How many pictures read from index `from` on are of a generation after `generation`.
    long newerThan(size_t from, uint64_t generation) const {
        return std::count_if(m_pictures_read.begin() + static_cast<long>(from), m_pictures_read.end(),
                             [&](const ReadPicture& p) { return p.generation > generation; });
    }

    // Takes pictures until `until` holds or `limit` passes; whether it held.
    bool takeUntil(const std::function<bool()>& until, Clock::duration limit) {
        const Clock::time_point deadline = Clock::now() + limit;
        for (;;) {
            takeNew();
            if (until()) return true;
            if (Clock::now() >= deadline || failed()) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    bool takeAtLeast(size_t more, Clock::duration limit) {
        const size_t target = m_pictures_read.size() + more;
        return takeUntil([&] { return m_pictures_read.size() >= target; }, limit);
    }
    // Takes pictures until none new for `quiet`; false when it never went quiet inside `limit`.
    bool takeUntilQuiet(std::chrono::milliseconds quiet, std::chrono::seconds limit) {
        Clock::time_point lastChange = Clock::now();
        const Clock::time_point deadline = Clock::now() + limit;
        while (Clock::now() < deadline && !failed()) {
            if (takeNew()) {
                lastChange = Clock::now();
                continue;
            }
            if (Clock::now() - lastChange >= quiet) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
    // The first failure of each kind so far.
    const ReadFailure& failure() const { return m_failure; }
    bool failed() const {
        return m_failure.harness || !m_failure.daemonFault.empty() || !m_failure.hostLimit.empty();
    }
    // True the first time only, so a failure asked about at several points is reported once.
    bool firstReport() { return !std::exchange(m_reported, true); }

private:
    bool takeNew() {
        const uint64_t newest = latestGeneration(m_pictures, m_monitorId);
        if (newest <= m_taken) return false;
        m_taken = newest;
        std::optional<wallpaperx::testing::Frame> frame;
        try {
            frame.emplace(getFrame(m_proxy, m_monitorId));
        } catch (const sdbus::Error&) {
            // A refusal to serve is an ordinary answer; the picture after it is taken instead.
            return true;
        }
        if (frame->generation > m_lastRead) {
            m_lastRead = frame->generation;
            if (const std::optional<int> counter = readInto(*frame)) {
                m_pictures_read.push_back(ReadPicture{frame->generation, *counter});
            }
        }
        giveBack(*frame);
        return true;
    }
    std::optional<int> readInto(const wallpaperx::testing::Frame& frame) {
        ReadFailure failure;
        const std::optional<int> counter = m_reader.read(frame, failure);
        if (m_failure.hostLimit.empty()) m_failure.hostLimit = failure.hostLimit;
        if (m_failure.daemonFault.empty()) m_failure.daemonFault = failure.daemonFault;
        m_failure.harness = m_failure.harness || failure.harness;
        return counter;
    }
    // The daemon refusing a buffer it has just served to this connection is its fault, not an answer.
    void giveBack(const wallpaperx::testing::Frame& frame) {
        try {
            wallpaperx::testing::releaseFrame(m_proxy, m_monitorId, frame);
        } catch (const sdbus::Error& e) {
            if (m_failure.daemonFault.empty()) {
                m_failure.daemonFault = "the daemon refused to take back buffer " + std::to_string(frame.bufferId) +
                                        " it had just served: " + e.getName() + ": " + e.getMessage();
            }
        }
    }

    sdbus::IProxy& m_proxy;
    const PictureLog& m_pictures;
    int32_t m_monitorId;
    CounterReader& m_reader;
    uint64_t m_taken = 0;
    uint64_t m_lastRead = 0;
    std::vector<ReadPicture> m_pictures_read;
    ReadFailure m_failure;
    bool m_reported = false;
};

// Whether the taker's readings stopped, and if so reports why, by whose doing: the daemon's fault is a
// FAIL, a harness fault was already printed as one, and only this host's limit is a skip.
// GOAL: a served frame the daemon lays out or takes back wrongly is graded as the daemon's failure,
//       never as a reading this host could not make.
// WHY-THIS-SHAPE: one failure field would put "a row outside its memory" and "no GL context" in the
//       same skip; the reader files each where it arises, since only it knows which one it hit.
// CONTROL: via control_batch.sh, --phase '^resume$', 2026-09-30, each alone, reverted, in
//       backend/daemon/dbus_service.cpp: GetFrame publishing each plane's offset plus the memory's size ->
//       on video_wallpaper_lavapipe (a mapped LINEAR dma_buf) "FAIL: the daemon's served frame is readable as
//       published, and taken back when given back", printing "the frame's published layout (offset 76800, row
//       pitch 640) puts row 0 outside its 76800 bytes"; DbusService::releaseBuffer refusing every id -> the same
//       line on video_wallpaper, "the daemon refused to take back buffer 1 it had just served". A descriptor
//       the test could not duplicate (test-side, dup(-1), by hand, 2026-09-30) prints only "FAIL (harness):
//       the frame's descriptor could not be duplicated", no FAIL against the daemon. A GetFrame refusal of
//       the picture on screen is the daemon's too: completeFrame() answering NoFrame for a generation it
//       already served, via control_batch.sh, --phase '^resume$', 2026-10-01 -> the same line, printing "the
//       daemon refused to serve the picture on screen: io.wallpaperx.Daemon.Error.NoFrame".
bool readingStopped(CountedPictures& taker, const char* goals) {
    if (!taker.failed()) return false;
    if (!taker.firstReport()) return true;
    const ReadFailure& failure = taker.failure();
    if (!failure.daemonFault.empty()) {
        check(false, "the daemon's served frame is readable as published, and taken back when given back");
        std::fprintf(stderr, "  %s\n", failure.daemonFault.c_str());
    } else if (!failure.harness) {
        notVerified(goals, "the pictures could not be read back on this host: " + failure.hostLimit);
    }
    return true;
}

// A manager with a service on a connection of its own, and a proxy to it that records FrameReady. The
// phase's grade is captured once the bus has stopped and before the manager is destroyed, by member order.
struct BusRig {
    BusRig(const WallpaperManager::Config& config, PhaseGrade& grade) : manager(config), captureGrade{grade, manager} {
        serviceConn = sdbus::createSessionBusConnection();
        service.emplace(*serviceConn, manager);
        serviceConn->enterEventLoopAsync();
        daemonName = serviceConn->getUniqueName();
        proxy = sdbus::createProxy(sdbus::ServiceName{daemonName}, sdbus::ObjectPath{DbusService::kObjectPath});
        proxy->uponSignal("FrameReady")
            .onInterface(DbusService::kInterface)
            .call([this](int32_t monitorId, uint64_t generation) { pictures.record(monitorId, generation); });
    }
    // The proxy first, then the service, then the manager: a signal still in flight names `pictures`.
    ~BusRig() {
        proxy.reset();
        service.reset();
        serviceConn.reset();
    }
    BusRig(const BusRig&) = delete;
    BusRig& operator=(const BusRig&) = delete;

    PictureLog pictures;
    WallpaperManager manager;
    CaptureGradeOnExit captureGrade;
    std::unique_ptr<sdbus::IConnection> serviceConn;
    std::optional<DbusService> service;
    std::string daemonName;
    std::unique_ptr<sdbus::IProxy> proxy;
};

constexpr int32_t kMonitorPause = 0;

// `base` is copied into the config this phase's manager is built from; every wait below reads its intervals.
void runPauseChecks(const WallpaperManager::Config& base, const std::string& clip) {
    using wallpaperx::testing::setMonitorOutput;
    using wallpaperx::testing::setMonitorPause;
    using wallpaperx::testing::setOutputFocus;
    using wallpaperx::testing::setWallpaperProperties;
    const char* kGoals = "S15.146-S15.149 (the pause setting stops a playing video, and what resumes it)";
    // Long enough that the worker's own deadline for a paused session is never what resumes it: every
    // resume below must come from the call that ended the pause waking it.
    WallpaperManager::Config config = base;
    config.pausedSessionRelease = std::chrono::seconds(60);
    const wallpaperx::testing::ScratchDir scratch("videopause");
    if (!scratch.ok()) {
        notVerified(kGoals, "no scratch directory could be created");
        return;
    }
    config.configFile = scratch.path() / "config.json";
    PhaseGrade grade("the pause phase");
    BusRig rig(config, grade);
    sdbus::IProxy& proxy = *rig.proxy;
    const std::string screen = "DP-PAUSE";
    GiveBack giveBack(proxy, "the daemon takes back every buffer it served to the pause phase's consumer");

    uint64_t collected = 0;
    // Takes every new picture until it goes quiet for kQuietWindow (true) or `limit` passes (false).
    const auto collectUntilQuiet = [&](std::chrono::seconds limit) {
        Clock::time_point lastChange = Clock::now();
        const Clock::time_point deadline = Clock::now() + limit;
        while (Clock::now() < deadline) {
            if (const uint64_t now = latestGeneration(rig.pictures, kMonitorPause); now > collected) {
                collected = now;
                lastChange = Clock::now();
                giveBack.take(kMonitorPause);
                continue;
            }
            if (Clock::now() - lastChange >= kQuietWindow) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    };
    // Takes pictures until one newer than `after` has been shown, or `limit` passes.
    const auto playsPast = [&](uint64_t after, std::chrono::seconds limit) {
        const Clock::time_point deadline = Clock::now() + limit;
        while (Clock::now() < deadline) {
            if (const uint64_t now = latestGeneration(rig.pictures, kMonitorPause); now > collected) {
                collected = now;
                giveBack.take(kMonitorPause);
                if (now > after) return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    };
    const auto pausedNow = [&proxy]() {
        const StatusReply status = getStatusOrExplain(proxy);
        const nlohmann::json* entry = wallpaperx::testing::pauseStatus(status, std::to_string(kMonitorPause));
        return entry != nullptr && entry->value("paused", false);
    };

    setMonitorOutput(proxy, kMonitorPause, screen);
    setWallpaper(proxy, "video", clip, kMonitorPause);
    if (!settlesWithin(std::chrono::seconds(30), [&] { return latestGeneration(rig.pictures, kMonitorPause) > 0; })) {
        grade.expectNoDevice();
        notVerified(kGoals, "the video render on this host produced no frame, so there was nothing to pause");
        return;
    }
    // The premise every stop below is measured against: this consumer keeps the video playing.
    const uint64_t startedAt = latestGeneration(rig.pictures, kMonitorPause);
    if (!playsPast(startedAt + 5, std::chrono::seconds(10))) {
        notVerified(kGoals, "the video did not play for a collecting consumer, so a pause would establish nothing");
        return;
    }

    // GOAL: S15.146 -- always_paused stops a video its consumer keeps collecting, GetStatus says it is
    //       paused, and play starts it again with nothing else asked for.
    // WHY-THIS-SHAPE: the stop is a quiescence with a deadline while the consumer goes on collecting,
    //       so it is attributable to the setting alone; the consumer fetches only on a new picture, so
    //       it cannot wake a stopped worker. The paused session's own release is 60 s away here, so the
    //       resume within 10 s can only have come from SetMonitorPause waking the worker.
    // CONTROL: via control_batch.sh, --phase '^pause$', 2026-09-30, each alone, reverted. In
    //       playingSeqLocked() (backend/daemon/wallpaper_manager.cpp), `|| pausedLocked(monitorId)`
    //       removed -> "FAIL: always_paused stops a video its consumer keeps collecting, and GetStatus says
    //       so", after the 25 s limit. In DbusService::setMonitorPause (backend/daemon/dbus_service.cpp),
    //       `m_worker.wake();` deleted -> "FAIL: setting it back to play starts it again, with nothing else
    //       asked for", alone.
    setMonitorPause(proxy, kMonitorPause, "always_paused");
    const bool stopped = collectUntilQuiet(std::chrono::seconds(25));
    check(stopped && pausedNow(), "always_paused stops a video its consumer keeps collecting, and GetStatus says so");
    if (!stopped) {
        notVerified(kGoals, "the paused video never stopped, so nothing below has a stopped video to resume");
        return;
    }
    const uint64_t pausedAt = latestGeneration(rig.pictures, kMonitorPause);

    // GOAL: S15.149 -- a speed changed while paused shows nothing: no picture is drawn until the pause
    //       ends.
    // CONTROL: via control_batch.sh, --phase '^pause$', 2026-09-30, a render owed on every speed change --
    //       `++it->second.requestSeq;` in WallpaperManager::setWallpaperProperties
    //       (backend/daemon/wallpaper_manager.cpp) and `m_worker.post(monitorId);` for the wake in
    //       DbusService::setWallpaperProperties (backend/daemon/dbus_service.cpp) -> "FAIL: a speed changed
    //       while paused shows nothing", printing "1 picture(s) shown", alone. Reverted.
    {
        setWallpaperProperties(proxy, kMonitorPause, {{"speed", sdbus::Variant(0.5)}});
        const bool quiet = collectUntilQuiet(std::chrono::seconds(10));
        const uint64_t after = latestGeneration(rig.pictures, kMonitorPause);
        check(quiet && after == pausedAt, "a speed changed while paused shows nothing");
        if (after != pausedAt) std::fprintf(stderr, "  %llu picture(s) shown\n", static_cast<unsigned long long>(after - pausedAt));
        setWallpaperProperties(proxy, kMonitorPause, {{"speed", sdbus::Variant(1.0)}});
    }

    setMonitorPause(proxy, kMonitorPause, "play");
    const bool resumed = playsPast(pausedAt, std::chrono::seconds(10));
    check(resumed && !pausedNow(), "setting it back to play starts it again, with nothing else asked for");

    // GOAL: S15.147 -- a focus report ending because its sender left the bus starts a video it had
    //       paused again, with no other call.
    // WHY-THIS-SHAPE: the report comes from a second connection; the video is stopped by it, and the
    //       connection is then closed. Nothing else happens, so the worker has to be woken by the departure.
    // CONTROL: via control_batch.sh, --phase '^pause$', 2026-09-30, DbusService::forgetClient
    //       (backend/daemon/dbus_service.cpp) keeping `m_manager.forgetOutputFocus(client)` and dropping
    //       its `m_worker.wake()` -> "FAIL: the video plays again once the connection that paused it
    //       leaves the bus". Reverted.
    {
        setMonitorPause(proxy, kMonitorPause, "paused_while_focused");
        bool stoppedByReport = false;
        {
            auto reporter = sdbus::createProxy(sdbus::ServiceName{rig.daemonName}, sdbus::ObjectPath{DbusService::kObjectPath});
            setOutputFocus(*reporter, screen, true, false);
            stoppedByReport = collectUntilQuiet(std::chrono::seconds(25)) && pausedNow();
        }
        check(stoppedByReport, "a focused window reported by a second connection stops the video (the premise)");
        if (stoppedByReport) {
            const uint64_t stoppedAt = latestGeneration(rig.pictures, kMonitorPause);
            check(playsPast(stoppedAt, std::chrono::seconds(10)),
                  "the video plays again once the connection that paused it leaves the bus");
        }
    }

    // GOAL: S15.148 -- moving a screen's name away from a monitor drops the report on it, and a video it
    //       had paused plays again with no other call.
    // CONTROL: via control_batch.sh, --phase '^pause$', 2026-09-30, the `m_worker.wake();` after the
    //       redraws in DbusService::setMonitorOutput (backend/daemon/dbus_service.cpp) deleted -> "FAIL: a
    //       monitor given another screen name plays again once the report that paused it is dropped".
    //       Reverted.
    {
        setOutputFocus(proxy, screen, true, false);
        const bool stoppedByReport = collectUntilQuiet(std::chrono::seconds(25)) && pausedNow();
        check(stoppedByReport, "a focused window reported on this connection stops the video (the premise)");
        if (stoppedByReport) {
            const uint64_t stoppedAt = latestGeneration(rig.pictures, kMonitorPause);
            setMonitorOutput(proxy, kMonitorPause, "DP-PAUSE-2");
            check(playsPast(stoppedAt, std::chrono::seconds(10)),
                  "a monitor given another screen name plays again once the report that paused it is dropped");
        }
    }
}

constexpr int32_t kMonitorResume = 0;

// The frame number the monitor's served frame shows, read in process. Empty, with why, when unreadable.
std::optional<int> servedCounter(WallpaperManager& manager, int monitorId, std::string& reason) {
    wallpaperx::FrameExport exported = exportDelivered(manager, monitorId);
    if (!exported.handle) {
        reason = "no frame was served";
        return std::nullopt;
    }
    const FrameLayout layout = layoutOf(*exported.handle);
    const std::vector<unsigned char> image = readServedBgra(layout, reason);
    if (exported.handle->frame.fd >= 0) close(exported.handle->frame.fd);
    if (exported.handle->acquireFd >= 0) close(exported.handle->acquireFd);
    if (image.size() < size_t{layout.width} * layout.height * 4) return std::nullopt;
    const std::optional<int> counter = wallpaperx::testing::counterFromBgra(image, layout.width, layout.height);
    if (!counter) reason = "the frame number could not be read from the frame";
    return counter;
}

// A video that will not play on -- paused, or unwatched -- with its next picture already decoded, laid
// out again: each its own manager, the counter fixture on a 4:3 screen so every layout here reads whole.
void runStoppedRelayoutChecks(const WallpaperManager::Config& base) {
    using wallpaperx::PositionMode;
    const char* kGoals = "S15.803-S15.804 (a stopped video laid out again, alone on its file, shows the picture on screen)";
    const std::string counterClip = counterClipPath();
    if (!std::filesystem::exists(counterClip)) {
        notVerified(kGoals, "the counter fixture " + counterClip + " was not built");
        return;
    }
    g_files.watch(counterClip);
    constexpr int kStopped = 0;
    struct Variant {
        const char* what;
        bool pause;
    };
    for (const Variant& v : {Variant{"a paused video", true}, Variant{"an unwatched video", false}}) {
        PhaseGrade grade("the stopped-relayout phase");
        WallpaperManager::Config config = base;
        config.unwatchedAfter = v.pause ? std::chrono::milliseconds(3600000) : std::chrono::milliseconds(600);
        config.pausedSessionRelease = std::chrono::hours(1);
        config.idleSessionRelease = std::chrono::hours(1);
        WallpaperManager manager(config);
        CaptureGradeOnExit captureGrade{grade, manager};
        wallpaperx::testing::nameScreen(manager, kStopped, screenOf(kStopped));
        std::vector<int> owed;
        manager.setOutputSize(screenOf(kStopped), 160, 120, owed);
        manager.requestWallpaper("video", counterClip, kStopped);
        if (!renderCounted(manager, kStopped).outcome.rendered) {
            grade.expectNoDevice();
            notVerified(kGoals, "the first video render on this host produced no frame");
            return;
        }
        manager.setWallpaperProperties(kStopped, {{"position", PositionMode::Stretch}});
        manager.renderPending(kStopped);

        // Three pictures shown by the clock, then one round right after a show: it decodes the next
        // picture and waits for its time, which is the state under test.
        int shown = 0;
        for (const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
             shown < 3 && std::chrono::steady_clock::now() < deadline;) {
            for (const wallpaperx::ClockRound::Shown& s : manager.advanceVideo(std::chrono::steady_clock::now()).shown) {
                if (s.monitorId == kStopped) ++shown;
            }
            if (shown < 3) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        bool decodedAhead = false;
        for (int attempt = 0; shown >= 3 && attempt < 20 && !decodedAhead; ++attempt) {
            g_decodeCalls.store(0);
            bool showedAgain = false;
            for (const wallpaperx::ClockRound::Shown& s : manager.advanceVideo(std::chrono::steady_clock::now()).shown) {
                showedAgain = showedAgain || s.monitorId == kStopped;
            }
            decodedAhead = !showedAgain && g_decodeCalls.load() > 0;
        }
        if (v.pause) manager.setMonitorPause(kStopped, "always_paused");
        std::string reason;
        const std::optional<int> frozen = servedCounter(manager, kStopped, reason);
        if (!decodedAhead || !frozen) {
            check(false, (std::string(v.what) + " shows three pictures, decodes the next ahead and is read (the premise)").c_str());
            std::fprintf(stderr, "  %d shown, %s, %s\n", shown, decodedAhead ? "decoded ahead" : "not decoded ahead",
                         frozen ? ("showing " + std::to_string(*frozen)).c_str() : reason.c_str());
            continue;
        }
        if (!v.pause) std::this_thread::sleep_for(config.unwatchedAfter + std::chrono::milliseconds(200));

        // GOAL: S15.803 -- a layout change on a video that will not play on, with its next picture already
        //       decoded and no other screen on the same file playing, shows the picture that was on screen,
        //       not the one decoded ahead; S15.804 -- only the first change may reopen the file, every later
        //       one is a plain redraw with no open. (Beside a playing screen it takes theirs: S15.862.)
        // WHY-THIS-SHAPE: read from the served frame after every change, each change owed and drawn. The first
        //       change's open count is printed and bounded (at most one), not required to be one: reopening is
        //       how the daemon gets back to the on-screen picture today, not the property.
        // CONTROL: control_batch.sh 2026-10-18, phase positionVideo, NVIDIA and Lavapipe, each alone, reverted, in
        //       renderPassLocked (backend/daemon/wallpaper_manager.cpp): `if (!playsOn)` made `if (!playsOn &&
        //       false)` -> "...shows the picture that was on screen" for both variants, "on screen 3, then read 4,
        //       4, 4, 4"; `!pausedLocked(monitorId) &&` deleted -> the paused variant alone; `watchedLocked(...)`
        //       made `true` -> the unwatched variant alone; `std::holds_alternative<VideoSession::Waiting>(...) &&`
        //       deleted -> "...later changes open nothing" for both, "1 open(s) at the first change, 3 at the later ones".
        // COVERAGE-ASSERT: every change is asserted owed and drawn, so each reading is of a new draw.
        g_files.resetCounts();
        const PositionMode modes[] = {PositionMode::Free, PositionMode::Stretch, PositionMode::Free, PositionMode::Stretch};
        // An unwatched video read by a consumer is watched again, so it takes the first two changes only.
        const int changes = v.pause ? 4 : 2;
        int firstOpens = -1, laterOpens = 0, drawn = 0;
        std::string readings;
        bool allFrozen = true;
        for (int i = 0; i < changes; ++i) {
            const int opensBefore = g_files.opens(counterClip);
            const bool changeOwed = manager.setWallpaperProperties(kStopped, {{"position", modes[i]}});
            const bool rendered = manager.renderPending(kStopped).rendered;
            const int opens = g_files.opens(counterClip) - opensBefore;
            if (changeOwed && rendered) ++drawn;
            if (i == 0) {
                firstOpens = opens;
            } else {
                laterOpens += opens;
            }
            if (!v.pause && i + 1 < changes) continue;
            const std::optional<int> now = servedCounter(manager, kStopped, reason);
            readings += (readings.empty() ? "" : ", ") + (now ? std::to_string(*now) : "unreadable (" + reason + ")");
            allFrozen = allFrozen && now == frozen;
        }
        check(drawn == changes, (std::string(v.what) + ": every layout change is owed and drawn").c_str());
        reportUnless(allFrozen, (std::string(v.what) + " laid out again shows the picture that was on screen").c_str(),
                     "on screen " + std::to_string(*frozen) + ", then read " + readings);
        reportUnless(firstOpens <= 1 && laterOpens == 0,
                     (std::string(v.what) + " laid out again: the first change opens the file at most once, later "
                                            "changes open nothing").c_str(),
                     std::to_string(firstOpens) + " open(s) at the first change, " + std::to_string(laterOpens) +
                         " at the later ones");
        std::printf("%s laid out again: on screen %d, read %s; %d open(s) at the first change\n", v.what, *frozen,
                    readings.c_str(), firstOpens);
    }
}

// Where each counter picture lies in the fixture, from libavcodec driven directly.
struct CounterOrder {
    std::vector<int> ids;
    // The picture shown after `id`: the next in the file, or the first after the last.
    std::optional<int> after(int id) const {
        const auto it = std::find(ids.begin(), ids.end(), id);
        if (it == ids.end()) return std::nullopt;
        return std::next(it) == ids.end() ? ids.front() : *std::next(it);
    }
};

std::optional<CounterOrder> counterOrder(const std::string& clip, const char* goals) {
    std::string why;
    const std::optional<wallpaperx::testing::CounterReference> reference =
        wallpaperx::testing::decodeCounterReference(clip, why);
    if (!reference || !reference->distinctAndReadable()) {
        wallpaperx::testing::harnessFailure(std::string(goals) + ": the counter fixture " + clip +
                                            " could not be decoded independently: " +
                                            (why.empty() ? std::string("its pictures are not each readable once") : why));
        return std::nullopt;
    }
    return CounterOrder{reference->ids};
}

std::string describeRead(const std::vector<ReadPicture>& read) {
    std::string text;
    for (size_t i = 0; i < read.size() && i < 4; ++i) {
        text += (text.empty() ? "" : ", ") + std::to_string(read[i].counter) + " (generation " +
                std::to_string(read[i].generation) + ")";
    }
    return text.empty() ? "nothing" : text;
}

// The frame numbers of the two pictures committed next after `stopped`, when both were read: a fetch
// serves the newest picture, so a consumer the host delayed can miss one, and then there is no answer.
std::optional<std::pair<int, int>> nextTwo(const std::vector<ReadPicture>& shown, const ReadPicture& stopped) {
    std::optional<int> first;
    std::optional<int> second;
    for (const ReadPicture& p : shown) {
        if (p.generation == stopped.generation + 1) first = p.counter;
        if (p.generation == stopped.generation + 2) second = p.counter;
    }
    if (!first || !second) return std::nullopt;
    return std::make_pair(*first, *second);
}

enum class Graded { Passed, Failed, NotGraded };

// Whether the two pictures after `stopped` are `first` and `second` (either unset: not compared). Fewer
// than two new pictures after the resume is a video that did not resume, and fails; two or more of which
// the consumer missed one of the pair is reported as not graded, never read either way. The caller
// reports a reading that stopped first, so it is never taken for a stalled video.
// GOAL: a resume that shows nothing new fails the check it was meant to answer; only a consumer that read
//       new pictures but missed one of the pair is a skip.
// WHY-THIS-SHAPE: counted from readings newer than the stop, not from all of `shown`, which can hold the
//       stopped picture itself read again by the hand-over. One function for every resume phase: a copy
//       per phase is how the eviction phase kept skipping a stalled resume.
// CONTROL: via control_batch.sh, --phase '^resume$', re-run 2026-10-16 on NVIDIA at half speed, DbusService::setMonitorPause
//       (backend/daemon/dbus_service.cpp) returning before the manager call for "play" -> "FAIL: a pause
//       ended inside its delay carries on from the open decoder, with the next picture", printing "stopped
//       on 11 (generation 12); 0 new picture(s) within 20 s of the resume: nothing"; S15.152's line red
//       beside it, the video never resumed. Reverted.
Graded gradeNextTwo(const char* what, const std::vector<ReadPicture>& shown, const ReadPicture& stopped,
                    std::optional<int> first, std::optional<int> second) {
    const std::optional<std::pair<int, int>> next = nextTwo(shown, stopped);
    const auto newer = std::count_if(shown.begin(), shown.end(),
                                     [&](const ReadPicture& p) { return p.generation > stopped.generation; });
    if (!next && newer < 2) {
        check(false, what);
        std::fprintf(stderr, "  stopped on %d (generation %llu); %ld new picture(s) within 20 s of the resume: %s\n",
                     stopped.counter, static_cast<unsigned long long>(stopped.generation), static_cast<long>(newer),
                     describeRead(shown).c_str());
        return Graded::Failed;
    }
    if (!next) {
        notVerified(what, "the consumer did not read both pictures after the stop (read: " + describeRead(shown) +
                              "), so which they were is not known");
        return Graded::NotGraded;
    }
    const bool right = (!first || next->first == *first) && (!second || next->second == *second);
    check(right, what);
    if (!right) {
        std::fprintf(stderr, "  stopped on %d (generation %llu), then showed %d and %d\n", stopped.counter,
                     static_cast<unsigned long long>(stopped.generation), next->first, next->second);
    }
    return right ? Graded::Passed : Graded::Failed;
}

// S15.151-S15.155 on one monitor playing the counter fixture: the pictures a resume shows, read from
// the served frames.
void runResumeChecks(const WallpaperManager::Config& base, const std::string& counterClip, const std::string& still) {
    using wallpaperx::testing::setMonitorOutput;
    using wallpaperx::testing::setMonitorPause;
    using wallpaperx::testing::setOutputVisible;
    const char* kGoals = "S15.151-S15.155 (a video resumes on the picture it stopped on)";
    const wallpaperx::testing::ScratchDir scratch("videoresume");
    if (!scratch.ok()) {
        notVerified(kGoals, "no scratch directory could be created");
        return;
    }
    const std::optional<CounterOrder> order = counterOrder(counterClip, kGoals);
    if (!order) return;
    // The fixture's own 4:3, so covering the monitor crops nothing and the counter grid is whole.
    WallpaperManager::Config config = base;
    config.width = 160;
    config.height = 120;
    config.idleSessionRelease = std::chrono::seconds(2);
    // Distinct from the idle release, so a daemon using one for the other is seen.
    config.pausedSessionRelease = std::chrono::seconds(4);
    config.configFile = scratch.path() / "config.json";
    config.stateDir = scratch.path() / "state";
    g_files.watch(counterClip);

    PhaseGrade grade("the resume phase");
    BusRig rig(config, grade);
    sdbus::IProxy& proxy = *rig.proxy;
    CounterReader reader;
    CountedPictures taker(proxy, rig.pictures, kMonitorResume, reader);
    const std::string screen = "DP-RESUME";
    const auto unanswerable = [&]() { return readingStopped(taker, kGoals); };

    setMonitorOutput(proxy, kMonitorResume, screen);
    // Half speed, the slowest allowed, so the consumer rarely misses a picture; when it does, the check
    // it would have graded reports that instead of guessing.
    wallpaperx::testing::setWallpaperWithProperties(proxy, "video", counterClip, kMonitorResume,
                                                    {{"speed", sdbus::Variant(0.5)}});
    if (!settlesWithin(std::chrono::seconds(30), [&] { return latestGeneration(rig.pictures, kMonitorResume) > 0; })) {
        grade.expectNoDevice();
        notVerified(kGoals, "the counter video produced no frame on this host");
        return;
    }
    // COVERAGE-ASSERT for every check below: the served pictures are read back as the fixture's frame
    // numbers, each the one after the picture committed before it. A reader that read nothing, or read
    // the same number from every frame, would make each "showed F" below mean nothing. Only readings of
    // successive generations are compared, and at least five such pairs must have been.
    // CONTROL: test-side, via control_batch.sh, --phase '^resume$', 2026-10-16, NVIDIA and Lavapipe:
    //       counterFromBgra given the frame's height as its width at both of its calls -> "FAIL: the served
    //       pictures read back as the fixture's frame numbers, in order", "11 successive pair(s) compared",
    //       every reading -1, on each driver. Breaking the first call alone left NVIDIA green. Restored.
    {
        taker.takeAtLeast(12, std::chrono::seconds(20));
        if (unanswerable()) return;
        const std::vector<ReadPicture>& read = taker.pictures();
        int compared = 0;
        bool inOrder = true;
        for (size_t i = 1; i < read.size(); ++i) {
            if (read[i].generation != read[i - 1].generation + 1) continue;
            ++compared;
            if (order->after(read[i - 1].counter) != read[i].counter) inOrder = false;
        }
        check(inOrder && compared >= 5, "the served pictures read back as the fixture's frame numbers, in order");
        if (!inOrder || compared < 5) {
            std::fprintf(stderr, "  %d successive pair(s) compared; read: %s ...\n", compared, describeRead(read).c_str());
            return;
        }
    }

    // Stops the video with `stop` while still collecting; the picture now on screen.
    const auto stopOn = [&](const std::function<void()>& stop) -> std::optional<ReadPicture> {
        stop();
        if (!taker.takeUntilQuiet(kQuietWindow, std::chrono::seconds(25))) return std::nullopt;
        return taker.current();
    };
    // After `resume`, the pictures read while the next two are shown.
    const auto resumeAndRead = [&](const std::function<void()>& resume) -> std::vector<ReadPicture> {
        const size_t from = taker.pictures().size();
        resume();
        taker.takeAtLeast(2, std::chrono::seconds(20));
        return std::vector<ReadPicture>(taker.pictures().begin() + static_cast<long>(from), taker.pictures().end());
    };
    const auto nextTwoAre = [&](const char* what, const std::vector<ReadPicture>& shown, const ReadPicture& stopped,
                                std::optional<int> first, std::optional<int> second) {
        if (unanswerable()) return;
        gradeNextTwo(what, shown, stopped, first, second);
    };

    // GOAL: S15.153 -- a pause ended inside pausedSessionRelease carries on from the open decoder: the
    //       next picture is the successor of the one on screen, and the file is not opened again.
    // WHY-THIS-SHAPE: one second of pause against a four-second delay; both the reopen count and the
    //       picture are asserted, since a daemon that released and resumed exactly would show the same
    //       picture again rather than its successor. The two pictures are identified by generation as
    //       well as by frame number, so a consumer the host delayed past one is a skip, not a verdict.
    // CONTROL: via control_batch.sh, --phase '^resume$', re-run 2026-10-16 on NVIDIA at half speed, advanceSessionLocked()
    //       (backend/daemon/wallpaper_manager.cpp) releasing a paused session at once -- `paused ?
    //       std::chrono::steady_clock::duration::zero() : m_config.idleSessionRelease` -> "FAIL: a pause
    //       ended inside its delay does not open the file again" and "FAIL: a pause ended inside its delay
    //       carries on from the open decoder, with the next picture", printing "stopped on 11 (generation
    //       12), then showed 11 and 12" -- the same picture again -- beside S15.152's line (0.603 s).
    //       Reverted.
    {
        const std::optional<ReadPicture> frozen = stopOn([&] { setMonitorPause(proxy, kMonitorResume, "always_paused"); });
        if (unanswerable()) return;
        // CONTROL: via control_batch.sh, 2026-10-16, NVIDIA: WallpaperManager::setMonitorPause
        //       (backend/daemon/wallpaper_manager.cpp) ignoring "always_paused" -> this line red. Reverted.
        if (!frozen) {
            check(false, "always_paused stops the counter video (the premise of the resume checks)");
            return;
        }
        g_files.resetCounts();
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const std::vector<ReadPicture> shown = resumeAndRead([&] { setMonitorPause(proxy, kMonitorResume, "play"); });
        check(g_files.opens(counterClip) == 0, "a pause ended inside its delay does not open the file again");
        const std::optional<int> successor = order->after(frozen->counter);
        nextTwoAre("a pause ended inside its delay carries on from the open decoder, with the next picture", shown,
                   *frozen, successor, successor ? order->after(*successor) : std::nullopt);
    }

    // GOAL: S15.152 -- a paused session is released pausedSessionRelease after the pause, not at the
    //       unwatched delay and not never.
    // WHY-THIS-SHAPE: measured from the SetMonitorPause call to the file's close, with a deadline and a
    //       failure branch; the consumer keeps collecting, so the monitor stays watched and only the pause
    //       stops it. The lower bound is the delay less a margin, so a release at the 2 s idle delay fails.
    // CONTROL: via control_batch.sh, --phase '^resume$', re-run 2026-10-16 on NVIDIA at half speed, each alone, reverted, in
    //       advanceSessionLocked() (backend/daemon/wallpaper_manager.cpp): the paused delay replaced by
    //       m_config.idleSessionRelease -> "FAIL: a paused session is released after pausedSessionRelease,
    //       not sooner or later", released 2.013 s after the pause; a paused round returning
    //       `session.idleSince.reset(); return std::nullopt;` -> the same line, "not released after 20 s".
    // GOAL: S15.151's pause half -- after that release the resume shows the picture that was on screen
    //       when the video stopped, then its successor.
    // CONTROL: via control_batch.sh, --phase '^resume$', re-run 2026-10-16 on NVIDIA at half speed, each alone, reverted, in
    //       backend/daemon/wallpaper_manager.cpp: `startAt` passed as std::nullopt to the VideoPipeline built
    //       in videoSessionLocked()'s `open` lambda -> "FAIL: a video whose paused session was released
    //       resumes on the picture it stopped on, then its successor", printing "stopped on 13 (generation
    //       14), then showed 0 and 1", the hidden and hand-over halves below red the same way, and the three
    //       S15.155 lines (a fresh open seeks nothing); releaseVideoSessionLocked() never recording
    //       `releasedAt` -> the same six lines. Both re-run 2026-10-24 on NVIDIA and Lavapipe, each alone, with
    //       those quotes; on Lavapipe two of the three S15.155 lines.
    // GOAL: S15.155 -- a resume runs under no crash record: at the seek back and at the packets its
    //       catch-up decodes after it, no record for this monitor is on disk, so a death there is not
    //       a quarantine at the next start (resumeBlame grades that it is counted as a playing death).
    // WHY-THIS-SHAPE: read at the calls themselves, on the render thread; a record read before and after
    //       the resume would miss one written only around the seek or the catch-up.
    // CONTROL: via control_batch.sh, --phase '^resume$', re-run 2026-10-16, NVIDIA at half speed, each alone, reverted, in
    //       backend/daemon/wallpaper_manager.cpp: videoSessionLocked()'s `open` lambda emplacing `applying` for
    //       an open at a position too -> "FAIL: the resume's seek back runs under no crash record", "1 seek(s),
    //       1 under the record"; an ApplyingRecord around advanceSessionLocked()'s CatchingUp decode -> "FAIL: nor
    //       does the catch-up decode after it", "3 packet(s) after them, 3 under the record". Each line alone.
    // COVERAGE-ASSERT: at least one seek and one packet after it were observed during the resume --
    //       red under the `startAt` break above, where nothing seeks.
    {
        const Clock::time_point pausedAt = Clock::now();
        g_files.resetCounts();
        const std::optional<ReadPicture> frozen = stopOn([&] { setMonitorPause(proxy, kMonitorResume, "always_paused"); });
        if (unanswerable()) return;
        // CONTROL: via control_batch.sh, --phase '^resume$', 2026-10-16, NVIDIA: WallpaperManager::setMonitorPause
        //       (backend/daemon/wallpaper_manager.cpp) ignoring the second "always_paused" -> only this line red. Reverted.
        if (!frozen) {
            check(false, "always_paused stops the counter video again");
            return;
        }
        const bool released = settlesWithin(std::chrono::seconds(20), [&] { return g_files.closes(counterClip) >= 1; });
        const double seconds = std::chrono::duration<double>(Clock::now() - pausedAt).count();
        const double due = std::chrono::duration<double>(config.pausedSessionRelease).count();
        std::printf("video_wallpaper_test: the paused session was released %.3f s after the pause (due at %.3f s)\n",
                    released ? seconds : -1.0, due);
        std::fflush(stdout);
        check(released && seconds >= due - 0.1 && seconds <= due + 1.5,
              "a paused session is released after pausedSessionRelease, not sooner or later");
        if (!released) {
            std::fprintf(stderr, "  not released after 20 s; open files: %s\n", describeFiles(g_files.live()).c_str());
            return;
        }
        g_recordWatch.arm(config.stateDir, kMonitorResume, counterClip);
        const std::vector<ReadPicture> shown = resumeAndRead([&] { setMonitorPause(proxy, kMonitorResume, "play"); });
        g_recordWatch.disarm();
        nextTwoAre("a video whose paused session was released resumes on the picture it stopped on, then its successor",
                   shown, *frozen, frozen->counter, order->after(frozen->counter));
        const int seeks = g_recordWatch.seeks();
        const int packets = g_recordWatch.packets();
        std::printf("video_wallpaper_test: the resume made %d seek(s), %d under the record; %d packet(s) after them, %d "
                    "under the record\n",
                    seeks, g_recordWatch.seeksCovered(), packets, g_recordWatch.packetsCovered());
        check(seeks >= 1 && packets >= 1, "the resume's seek back and the decode after it were observed");
        check(seeks >= 1 && g_recordWatch.seeksCovered() == 0, "the resume's seek back runs under no crash record");
        check(packets >= 1 && g_recordWatch.packetsCovered() == 0, "nor does the catch-up decode after it");
    }

    // GOAL: S15.151's hidden half -- a video whose screen was reported not drawn, and whose session was
    //       released at the unwatched delay, resumes on the picture it stopped on when the screen is drawn
    //       again, then its successor.
    // CONTROL: the two S15.151 breaks above, re-run 2026-10-24 -> "FAIL: a video whose hidden session was released resumes on
    //       the picture it stopped on, then its successor", printing "stopped on 1 (generation 16), then
    //       showed 0 and 1". Reverted.
    {
        g_files.resetCounts();
        const std::optional<ReadPicture> frozen = stopOn([&] { setOutputVisible(proxy, screen, false); });
        if (unanswerable()) return;
        const bool released =
            frozen && settlesWithin(std::chrono::seconds(20), [&] { return g_files.closes(counterClip) >= 1; });
        // CONTROL: via control_batch.sh, --phase '^resume$', 2026-10-16, NVIDIA: WallpaperManager::setOutputVisible
        //       (backend/daemon/wallpaper_manager.cpp) ignoring a hide -> only this line red. Reverted.
        check(released, "a hidden counter video stops and its session is released (the premise)");
        if (!released) return;
        const std::vector<ReadPicture> shown = resumeAndRead([&] { setOutputVisible(proxy, screen, true); });
        nextTwoAre("a video whose hidden session was released resumes on the picture it stopped on, then its successor",
                   shown, *frozen, frozen->counter, order->after(frozen->counter));
    }

    // GOAL: S15.151's hand-over half -- a video whose session was released because its consumer stopped
    //       collecting resumes, at the next hand-over, on the picture it stopped on, then its successor.
    // WHY-THIS-SHAPE: the consumer stops fetching altogether, so the monitor goes unwatched and the
    //       session is released at the idle delay; nothing else is asked. Its next fetch is then the
    //       hand-over that reopens the session, and also reads the stopped picture -- so the frozen
    //       reading and the resume are one call, and no fetch can land between the release and it.
    //       The wait is for two generations newer than the stopped one, not two readings: the stopped
    //       generation, announced while nobody collected, is read again by the taker, and counting it left
    //       one new picture to grade whenever the catch-up's rounds gave that second reading room.
    // CONTROL: via control_batch.sh, --phase '^resume$', re-run 2026-10-16, NVIDIA at half speed,
    //       WallpaperManager::recordDeliveredLocked (backend/daemon/wallpaper_manager.cpp) also resetting
    //       `releasedAt` -> this line red, printing "stopped on 27 (generation 30), then showed 0 and 1". Reverted.
    //       Unbroken it reads "27 (generation 30), 27 (generation 31), 28 (generation 32)": the first is the
    //       taker's second reading of the stop.
    {
        g_files.resetCounts();
        const bool released = settlesWithin(std::chrono::seconds(20), [&] { return g_files.closes(counterClip) >= 1; });
        // CONTROL: via control_batch.sh, --phase '^resume$', 2026-10-16, NVIDIA: advanceSessionLocked()
        //       (backend/daemon/wallpaper_manager.cpp) skipping every unpaused idle release from the second on (the hidden
        //       one above is the first) -> only this line red. Reverted.
        check(released, "a video nobody collects stops and its session is released (the premise)");
        if (!released) return;
        const size_t from = taker.pictures().size();
        const std::optional<ReadPicture> frozen = taker.current();
        if (unanswerable()) return;
        if (!frozen) return;
        taker.takeUntil([&] { return taker.newerThan(from, frozen->generation) >= 2; }, std::chrono::seconds(20));
        if (unanswerable()) return;
        const std::vector<ReadPicture> shown(taker.pictures().begin() + static_cast<long>(from), taker.pictures().end());
        std::printf("video_wallpaper_test: uncollected, released, then handed over %d (generation %llu); read after: %s\n",
                    frozen->counter, static_cast<unsigned long long>(frozen->generation), describeRead(shown).c_str());
        std::fflush(stdout);
        nextTwoAre("a video released because nobody collected it resumes, at the next hand-over, on the picture it "
                   "stopped on, then its successor",
                   shown, *frozen, frozen->counter, order->after(frozen->counter));
    }

    // GOAL: S15.154 -- where a released video stood is for that wallpaper only: set a still and then the
    //       same video file again, and the video starts at its first picture.
    // WHY-THIS-SHAPE: the still opens no session, so nothing between spends the recorded position; only the
    //       wallpaper it was recorded for decides whether it is used. Read while the screen is hidden, from
    //       the render the new wallpaper owes, which no clock round has moved on.
    // CONTROL: via control_batch.sh, --phase '^resume$', re-run 2026-10-24, NVIDIA, the
    //       `releasedAt->forSeq == wallpaperSeq` term dropped from videoSessionLocked()
    //       (backend/daemon/wallpaper_manager.cpp) -> "FAIL: a video set again after a still starts at its first
    //       picture, not where the old one stood", printing "stopped on 28; the video set again showed 28",
    //       alone. Reverted.
    {
        g_files.resetCounts();
        const std::optional<ReadPicture> frozen = stopOn([&] { setOutputVisible(proxy, screen, false); });
        if (unanswerable()) return;
        const bool released =
            frozen && settlesWithin(std::chrono::seconds(20), [&] { return g_files.closes(counterClip) >= 1; });
        if (!released || frozen->counter == order->ids.front()) {
            notVerified("S15.154", "the video was not released away from its first picture, so where it starts next "
                                   "could not tell the two answers apart");
            return;
        }
        uint64_t before = latestGeneration(rig.pictures, kMonitorResume);
        setWallpaper(proxy, "static", still, kMonitorResume);
        const bool stillShown = settlesWithin(std::chrono::seconds(20),
                                              [&] { return latestGeneration(rig.pictures, kMonitorResume) > before; });
        before = latestGeneration(rig.pictures, kMonitorResume);
        setWallpaper(proxy, "video", counterClip, kMonitorResume);
        const bool videoShown = stillShown && settlesWithin(std::chrono::seconds(20), [&] {
                                    return latestGeneration(rig.pictures, kMonitorResume) > before;
                                });
        // A render that never happened is not a verdict on where the video starts.
        // CONTROL: test-side, via control_batch.sh, 2026-10-16, NVIDIA: the counter video set as the still, which
        //       no image decoder reads -> "FAIL: the still and then the video set again are each drawn (the
        //       premise of S15.154)", "the still was never drawn", and no FAIL on the property's line. Restored.
        check(stillShown && videoShown, "the still and then the video set again are each drawn (the premise of S15.154)");
        if (!stillShown || !videoShown) {
            std::fprintf(stderr, "  %s was never drawn\n", stillShown ? "the video set again" : "the still");
            return;
        }
        const std::optional<ReadPicture> first = taker.current();
        if (unanswerable()) return;
        const bool fromStart = first && first->counter == order->ids.front();
        check(fromStart, "a video set again after a still starts at its first picture, not where the old one stood");
        if (!fromStart) {
            std::fprintf(stderr, "  stopped on %d; the video set again showed %s\n", frozen->counter,
                         first ? std::to_string(first->counter).c_str() : "nothing");
        }
    }
}

// S15.156: an evicted session resumes where it stood, like one the delay released.
void runResumeEvictionChecks(const WallpaperManager::Config& base, const std::string& counterClip,
                             const std::vector<std::string>& clips, const std::string& still) {
    using wallpaperx::testing::setMonitorOutput;
    using wallpaperx::testing::setOutputVisible;
    const char* kGoal = "S15.156 (an evicted video resumes on the picture it stopped on)";
    const std::optional<CounterOrder> order = counterOrder(counterClip, kGoal);
    if (!order) return;
    WallpaperManager::Config config = base;
    config.width = 160;
    config.height = 120;
    // Only an eviction may free the hidden monitor's session here.
    config.idleSessionRelease = std::chrono::seconds(120);
    config.pausedSessionRelease = std::chrono::seconds(120);
    g_files.watch(counterClip);

    PhaseGrade grade("the resume-eviction phase");
    BusRig rig(config, grade);
    sdbus::IProxy& proxy = *rig.proxy;
    CounterReader reader;
    CountedPictures taker(proxy, rig.pictures, kMonitorResume, reader);
    const auto screenFor = [](int id) { return "DP-EVICT-" + std::to_string(id); };

    setMonitorOutput(proxy, kMonitorResume, screenFor(kMonitorResume));
    wallpaperx::testing::setWallpaperWithProperties(proxy, "video", counterClip, kMonitorResume,
                                                    {{"speed", sdbus::Variant(0.5)}});
    if (!settlesWithin(std::chrono::seconds(30), [&] { return latestGeneration(rig.pictures, kMonitorResume) > 0; })) {
        grade.expectNoDevice();
        notVerified(kGoal, "the counter video produced no frame on this host");
        return;
    }
    taker.takeAtLeast(8, std::chrono::seconds(20));
    setOutputVisible(proxy, screenFor(kMonitorResume), false);
    const bool quiet = taker.takeUntilQuiet(kQuietWindow, std::chrono::seconds(25));
    const std::optional<ReadPicture> frozen = quiet ? taker.current() : std::nullopt;
    if (readingStopped(taker, kGoal)) return;
    // CONTROL: via control_batch.sh, 2026-10-16, NVIDIA: WallpaperManager::setOutputVisible
    //       (backend/daemon/wallpaper_manager.cpp) ignoring a hide -> this line red. Reverted.
    if (!frozen) {
        check(false, "a hidden counter video stops (the premise of the eviction check)");
        return;
    }

    // GOAL: S15.156 -- a session evicted for another screen's decoder records where its video stood, and
    //       the video resumes there: the picture on screen when it stopped, then its successor.
    // WHY-THIS-SHAPE: the idle and paused delays are two minutes, so only the eighth other video's open can
    //       free the hidden monitor's session -- the only one not on a drawn screen. The close is asserted
    //       before the resume. A reopen never evicts -- it waits for a free slot -- so another monitor is
    //       then given a still, whose render frees that monitor's session and runs the round that reopens.
    // CONTROL: via control_batch.sh, --phase '^resumeEviction$', re-run 2026-10-18 on NVIDIA, closeVideoSessionLocked() in
    //       place of releaseVideoSessionLocked() in evictIdleVideoSessionLocked()
    //       (backend/daemon/wallpaper_manager.cpp) -> "FAIL: a video whose session was evicted resumes on the
    //       picture it stopped on, then its successor", printing "stopped on 7 (generation 8), then showed 0
    //       and 1". Reverted.
    g_files.resetCounts();
    for (int id = 1; id <= static_cast<int>(WallpaperManager::kMaxVideoSessions); ++id) {
        setMonitorOutput(proxy, id, screenFor(id));
        setWallpaper(proxy, "video", clips[static_cast<size_t>(id - 1)], id);
    }
    const bool evicted = settlesWithin(std::chrono::seconds(60), [&] { return g_files.closes(counterClip) >= 1; });
    // CONTROL: via control_batch.sh, re-run 2026-10-18, NVIDIA: evictIdleVideoSessionLocked()
    //       (backend/daemon/wallpaper_manager.cpp) never choosing a victim -> this line red. Reverted.
    check(evicted, "the hidden video's session is evicted for the eighth other video (the premise)");
    if (!evicted) return;
    const size_t from = taker.pictures().size();
    setOutputVisible(proxy, screenFor(kMonitorResume), true);
    setWallpaper(proxy, "static", still, 1);
    taker.takeAtLeast(2, std::chrono::seconds(30));
    if (readingStopped(taker, kGoal)) return;
    const std::vector<ReadPicture> shown(taker.pictures().begin() + static_cast<long>(from), taker.pictures().end());
    if (gradeNextTwo("a video whose session was evicted resumes on the picture it stopped on, then its successor", shown,
                     *frozen, frozen->counter, order->after(frozen->counter)) == Graded::Failed) {
        const StatusReply status = getStatusOrExplain(proxy);
        const nlohmann::json* entry = wallpaperStatus(status, std::to_string(kMonitorResume));
        std::fprintf(stderr, "  stopped on %d, then showed %s; monitor %d: %s\n", frozen->counter,
                     describeRead(shown).c_str(), kMonitorResume, entry != nullptr ? entry->dump().c_str() : "no status");
    }
}

// S15.175: a resume whose open at the recorded position fails falls back to the first picture, and the
// position is spent by the attempt.
void runResumeFallbackChecks(const WallpaperManager::Config& base, const std::string& counterClip) {
    using wallpaperx::testing::setMonitorOutput;
    using wallpaperx::testing::setOutputSize;
    using wallpaperx::testing::setOutputVisible;
    const char* kGoal = "S15.175 (a resume that cannot open at its position starts at the first picture)";
    const std::optional<CounterOrder> order = counterOrder(counterClip, kGoal);
    if (!order) return;
    const wallpaperx::testing::ScratchDir scratch("videofallback");
    if (!scratch.ok()) {
        notVerified(kGoal, "no scratch directory could be created");
        return;
    }
    WallpaperManager::Config config = base;
    config.width = 160;
    config.height = 120;
    config.idleSessionRelease = std::chrono::seconds(2);
    config.configFile = scratch.path() / "config.json";
    config.stateDir = scratch.path() / "state";
    g_files.watch(counterClip);
    // Seeks this far in are a resume's: the loop's rewind aims at the first packet.
    constexpr double kFarSeconds = 0.5;
    constexpr int kFarPicture = 30;

    PhaseGrade grade("the resume-fallback phase");
    BusRig rig(config, grade);
    sdbus::IProxy& proxy = *rig.proxy;
    CounterReader reader;
    CountedPictures taker(proxy, rig.pictures, kMonitorResume, reader);
    const std::string screen = "DP-FALLBACK";
    setMonitorOutput(proxy, kMonitorResume, screen);
    setWallpaper(proxy, "video", counterClip, kMonitorResume);
    if (!settlesWithin(std::chrono::seconds(30), [&] { return latestGeneration(rig.pictures, kMonitorResume) > 0; })) {
        grade.expectNoDevice();
        notVerified(kGoal, "the counter video produced no frame on this host");
        return;
    }
    const auto statusLine = [&] {
        const StatusReply status = getStatusOrExplain(proxy);
        const nlohmann::json* entry = wallpaperStatus(status, std::to_string(kMonitorResume));
        return entry != nullptr ? entry->dump() : std::string("no status");
    };
    // Plays past kFarPicture, hides the screen and waits for the release; the picture it stopped on.
    const auto playThenRelease = [&]() -> std::optional<ReadPicture> {
        taker.takeUntil([&] { return !taker.pictures().empty() && taker.pictures().back().counter >= kFarPicture + 10; },
                        std::chrono::seconds(20));
        if (readingStopped(taker, kGoal)) return std::nullopt;
        g_files.resetCounts();
        setOutputVisible(proxy, screen, false);
        const bool quiet = taker.takeUntilQuiet(kQuietWindow, std::chrono::seconds(25));
        const std::optional<ReadPicture> frozen = quiet ? taker.current() : std::nullopt;
        if (readingStopped(taker, kGoal)) return std::nullopt;
        const bool released =
            frozen && settlesWithin(std::chrono::seconds(20), [&] { return g_files.closes(counterClip) >= 1; });
        if (!released || frozen->counter < kFarPicture) {
            check(false, "the counter video plays past its first second, stops when hidden and is released (the premise)");
            std::fprintf(stderr, "  stopped on %s; %s\n", frozen ? std::to_string(frozen->counter).c_str() : "nothing",
                         released ? "released" : "never released");
            return std::nullopt;
        }
        return frozen;
    };

    // GOAL: S15.175 -- a resume whose open at the recorded position fails as the file's fault is opened again
    //       at once from the first picture: the screen shows picture 0 then 1, with no error, and the position
    //       is tried once.
    // WHY-THIS-SHAPE: the first read after the resume's seek fails with EIO, which the decoder reports as the
    //       file's failure; nothing after it is failed, so the open from the start succeeds. Without the
    //       fallback the screen stays blank, since a render-pass failure blamed on the file is not retried.
    //       That read is inside the open only on the device, in its reset-point scan; a software open reads
    //       nothing after its seek, so on Lavapipe this read is the catch-up's, and S15.417's path answers.
    // CONTROL: via control_run.sh, --phase '^resumeFallback$', re-run 2026-10-08 on NVIDIA, reverted: in
    //       videoSessionLocked() (backend/daemon/wallpaper_manager.cpp) the `catch (const WallpaperContentError&)`
    //       fallback made to rethrow at once -> "FAIL: a resume whose open at its position fails starts at once at
    //       the first picture", printing "stopped on 40 (generation 41); 0 new picture(s) within 20 s of the
    //       resume: nothing", has_frame false and last_error "VideoDecoder: cannot read the wallpaper (Input/output
    //       error)", beside the served-frame line (GetFrame answering RenderFailed). Reverted.
    // COVERAGE-ASSERT: the injected read failure was spent, so the fallback was at stake.
    std::optional<ReadPicture> frozen = playThenRelease();
    if (!frozen) return;
    {
        g_farRead.arm(kFarSeconds, 1, /*alsoNextOpen=*/false);
        const size_t from = taker.pictures().size();
        setOutputVisible(proxy, screen, true);
        taker.takeAtLeast(2, std::chrono::seconds(20));
        g_farRead.disarm();
        if (readingStopped(taker, kGoal)) return;
        const std::vector<ReadPicture> shown(taker.pictures().begin() + static_cast<long>(from), taker.pictures().end());
        std::printf("video_wallpaper_test: the open at %d failed its read; %d far seek(s), %d read(s) failed; read after: %s\n",
                    frozen->counter, g_farRead.farSeeks(), g_farRead.injected(), describeRead(shown).c_str());
        std::fflush(stdout);
        check(g_farRead.injected() == 1, "the read after the resume's seek was failed (the premise)");
        check(g_farRead.farSeeks() == 1, "a resume whose open at its position fails tries the position once");
        if (gradeNextTwo("a resume whose open at its position fails starts at once at the first picture", shown, *frozen,
                         order->ids.front(), order->after(order->ids.front())) == Graded::Failed) {
            std::fprintf(stderr, "  monitor: %s\n", statusLine().c_str());
        }
    }

    // GOAL: S15.175's second half -- the position is spent by the attempt: when the open at the position and
    //       the open from the first picture both fail, the next render of the same wallpaper opens from the
    //       first picture and seeks to the position no more.
    // WHY-THIS-SHAPE: the read after the far seek fails, and so does the open() of the file that follows it;
    //       then a new size is reported, which re-renders the same wallpaper without changing it -- setting it
    //       again would make the recorded position another wallpaper's and prove nothing.
    // CONTROL: via control_run.sh, --phase '^resumeFallback$', re-run 2026-10-08 on NVIDIA, reverted: in
    //       videoSessionLocked() (backend/daemon/wallpaper_manager.cpp) `it->second.releasedAt.reset();` moved to
    //       after a successful open -> both lines red, alone (re-run 2026-10-24, NVIDIA, control_batch.sh; with
    //       the reset deleted outright, both lines and the S15.417 catch-up lines below), "then showed 40 and
    //       41" -- the old position, where 0 was due -- and "1 far seek(s) at the re-render". Reverted.
    // COVERAGE-ASSERT: both opens were failed and the screen went blank before the re-render.
    frozen = playThenRelease();
    if (!frozen) return;
    {
        g_farRead.arm(kFarSeconds, 1, /*alsoNextOpen=*/true);
        const uint64_t before = latestGeneration(rig.pictures, kMonitorResume);
        setOutputVisible(proxy, screen, true);
        const bool blank = settlesWithin(std::chrono::seconds(20), [&] {
            const StatusReply status = getStatusOrExplain(proxy);
            const nlohmann::json* entry = wallpaperStatus(status, std::to_string(kMonitorResume));
            return entry != nullptr && !entry->value("has_frame", true);
        });
        const bool bothFailed = blank && g_farRead.injected() == 1 && g_openFailure.left() == 0 &&
                                latestGeneration(rig.pictures, kMonitorResume) == before;
        g_farRead.disarm();
        g_openFailure.disarm();
        check(bothFailed, "the open at the position and the open from the first picture both failed (the premise)");
        if (!bothFailed) {
            std::fprintf(stderr, "  %d read(s) failed; monitor: %s\n", g_farRead.injected(), statusLine().c_str());
            return;
        }
        g_farRead.arm(kFarSeconds, 0, /*alsoNextOpen=*/false);
        const size_t from = taker.pictures().size();
        setOutputSize(proxy, screen, 192, 144);
        taker.takeAtLeast(2, std::chrono::seconds(20));
        g_farRead.disarm();
        if (readingStopped(taker, kGoal)) return;
        const std::vector<ReadPicture> shown(taker.pictures().begin() + static_cast<long>(from), taker.pictures().end());
        std::printf("video_wallpaper_test: after both opens failed, the re-render made %d far seek(s); read after: %s\n",
                    g_farRead.farSeeks(), describeRead(shown).c_str());
        std::fflush(stdout);
        check(g_farRead.farSeeks() == 0, "after both opens failed, the next render seeks to the position no more");
        // The screen stayed blank since `before`, so the re-render's pictures are the generations after it.
        const Graded graded = gradeNextTwo("after both opens failed, the next render opens from the first picture",
                                           shown, ReadPicture{before, frozen->counter}, order->ids.front(),
                                           order->after(order->ids.front()));
        if (graded == Graded::Failed || g_farRead.farSeeks() != 0) {
            std::fprintf(stderr, "  %d far seek(s) at the re-render; monitor: %s\n", g_farRead.farSeeks(),
                         statusLine().c_str());
        }
    }

    // GOAL: S15.417 -- a content error in a resume's catch-up, after its open succeeded, gives the position up
    //       as a failed open does: the screen keeps its frame and no failure is recorded until the file, opened
    //       again, shows its first picture and then the next.
    // WHY-THIS-SHAPE: the read failed is the first after the catch-up sent a packet, so it lands in a clock
    //       round on either driver -- on the device the open's own reset-point scan reads after the seek, and
    //       failing that read is the S15.175 open fallback above, not this. The status is read while the render
    //       thread is held in the reopen's first decode: polled instead, the failure path's dropped frame and
    //       recorded error were cleared by that reopen before any poll landed on Lavapipe.
    // CONTROL: via control_batch.sh, --phase '^resumeFallback$', re-run 2026-10-18 on video_wallpaper_lavapipe,
    //       reverted: advanceSessionLocked()'s `if (atPosition && named != members.end() && givesUpPosition(...))`
    //       given `false &&` after `atPosition` (backend/daemon/wallpaper_manager.cpp), so the ordinary playback-failure path
    //       runs -> "FAIL: a catch-up that fails as the file's fault keeps the frame and records no failure",
    //       "has_frame was false; last_error: VideoDecoder: cannot read the wallpaper (Input/output error)", with
    //       S15.425's line below. The pictures after it were 0 and 1 even so: the budgeted reopen hides the
    //       difference from every other line here.
    // COVERAGE-ASSERT: the injected read failure was spent, and the reopen's decode was held.
    // Shared by S15.417 and S15.425, which differ only in the error the read after the catch-up's first packet
    // fails with. False when the reading stopped or the premise failed, and the phase goes no further.
    const auto failedCatchUp = [&](int err, const char* failedHow, const char* premise, const char* keptLine,
                                   const char* firstPictureLine) {
        struct Release {
            ~Release() { g_decodeGate.release(); }
        } release;
        // An earlier phase's fire stays recorded in the gate until it is armed again, and waitFired() would
        // return on it at once; armed with a path no packet names, it forgets that and cannot fire before the
        // read failure arms it for real.
        g_decodeGate.arm(CallGate::Mode::Hold, "no packet names this");
        g_farRead.arm(kFarSeconds, 1, /*alsoNextOpen=*/false, /*afterSend=*/true, /*holdNextDecode=*/true, err);
        const size_t from = taker.pictures().size();
        setOutputVisible(proxy, screen, true);
        const bool held = g_decodeGate.waitFired(std::chrono::seconds(20)) && g_farRead.injected() == 1;
        const StatusReply status = getStatusOrExplain(proxy);
        g_decodeGate.release();
        const nlohmann::json* entry = wallpaperStatus(status, std::to_string(kMonitorResume));
        const bool frameKept = entry != nullptr && entry->value("has_frame", false);
        const std::string error = entry != nullptr ? entry->value("last_error", std::string()) : "no status";
        taker.takeUntil([&] { return taker.newerThan(from, frozen->generation) >= 2; }, std::chrono::seconds(20));
        g_farRead.disarm();
        if (readingStopped(taker, kGoal)) return false;
        const std::vector<ReadPicture> shown(taker.pictures().begin() + static_cast<long>(from), taker.pictures().end());
        std::printf("video_wallpaper_test: the catch-up from %d %s; %d read(s) failed, the reopen's decode %s; "
                    "read after: %s\n",
                    frozen->counter, failedHow, g_farRead.injected(), held ? "held" : "never came",
                    describeRead(shown).c_str());
        std::fflush(stdout);
        check(g_farRead.injected() == 1 && held, premise);
        if (g_farRead.injected() != 1 || !held) return false;
        reportUnless(frameKept && error.empty(), keptLine,
                     std::string(frameKept ? "the frame stayed" : "has_frame was false") + "; last_error: " +
                         (error.empty() ? "none" : error));
        if (gradeNextTwo(firstPictureLine, shown, *frozen, order->ids.front(), order->after(order->ids.front())) ==
            Graded::Failed) {
            std::fprintf(stderr, "  monitor: %s\n", statusLine().c_str());
        }
        return true;
    };
    frozen = playThenRelease();
    if (!frozen) return;
    if (!failedCatchUp(EIO, "failed a read",
                       "the read after the catch-up's first packet was failed, and the reopen after it was held (the "
                       "premise)",
                       "a catch-up that fails as the file's fault keeps the frame and records no failure",
                       "a catch-up that fails as the file's fault plays the file from its first picture")) {
        return;
    }

    // GOAL: S15.425 -- a memory shortage in a resume's catch-up, met by a clock round, gives the position up as
    //       a content error does: the screen keeps its frame, nothing is recorded, and the file, opened again,
    //       shows its first picture and then the next.
    // WHY-THIS-SHAPE: S15.417's shape with ENOMEM for EIO -- the decoder reports it as the daemon running short,
    //       not as the file. A give-up keyed on the file's fault alone records the shortage and drops the frame;
    //       the budgeted reopen then shows 0 and 1 anyway, so the status read during the held reopen is what
    //       tells the two apart.
    // CONTROL: via control_run.sh, --phase '^resumeFallback$', re-run 2026-10-09 through the shared failedCatchUp,
    //       Lavapipe, reverted: givesUpPosition() (backend/daemon/wallpaper_manager.cpp) returning false for a
    //       shortage -> "FAIL: a catch-up that runs short of memory keeps the frame and records no failure",
    //       "has_frame was false; last_error: VideoDecoder: out of memory", with S15.426's two lines below.
    // COVERAGE-ASSERT: the injected shortage was spent, and the reopen's decode was held.
    frozen = playThenRelease();
    if (!frozen) return;
    if (!failedCatchUp(ENOMEM, "ran short",
                       "the read after the catch-up's first packet ran short, and the reopen after it was held (the "
                       "premise)",
                       "a catch-up that runs short of memory keeps the frame and records no failure",
                       "a catch-up that runs short of memory plays the file from its first picture")) {
        return;
    }

    // GOAL: S15.426 -- the same shortage met by a render that must catch up at once (a new size reported while
    //       the video is released) draws the file at its first picture, then the next, and records no failure.
    // WHY-THIS-SHAPE: the size changes while the screen is hidden, so the first render after the release is at
    //       a size no image fits: it cannot keep the frame and catches up itself (caughtUpSessionLocked), and no
    //       clock round can reach the catch-up first. The shortage is read after a packet was sent, so it lands in
    //       that catch-up's decode and not in the open, on either driver.
    // CONTROL: via control_batch.sh, --phase '^resumeFallback$', re-run 2026-10-18, Lavapipe, reverted: caughtUpSessionLocked()
    //       (backend/daemon/wallpaper_manager.cpp) rethrowing every failure, so the render pass records the shortage
    //       -> "FAIL: a render that must catch up and runs short of memory draws the file from its first picture",
    //       with "and records no failure".
    // COVERAGE-ASSERT: the injected shortage was spent.
    frozen = playThenRelease();
    if (!frozen) return;
    {
        g_farRead.arm(kFarSeconds, 1, /*alsoNextOpen=*/false, /*afterSend=*/true, /*holdNextDecode=*/false, ENOMEM);
        const size_t from = taker.pictures().size();
        setOutputSize(proxy, screen, 176, 132);
        setOutputVisible(proxy, screen, true);
        taker.takeUntil([&] { return taker.newerThan(from, frozen->generation) >= 2; }, std::chrono::seconds(20));
        g_farRead.disarm();
        if (readingStopped(taker, kGoal)) return;
        const std::vector<ReadPicture> shown(taker.pictures().begin() + static_cast<long>(from), taker.pictures().end());
        const StatusReply status = getStatusOrExplain(proxy);
        const nlohmann::json* entry = wallpaperStatus(status, std::to_string(kMonitorResume));
        const std::string error = entry != nullptr ? entry->value("last_error", std::string()) : "no status";
        std::printf("video_wallpaper_test: the resized resume from %d ran short; %d read(s) failed; read after: %s\n",
                    frozen->counter, g_farRead.injected(), describeRead(shown).c_str());
        std::fflush(stdout);
        check(g_farRead.injected() == 1, "the read after the resized render's first catch-up packet ran short (the premise)");
        if (g_farRead.injected() != 1) return;
        if (gradeNextTwo("a render that must catch up and runs short of memory draws the file from its first picture",
                         shown, *frozen, order->ids.front(), order->after(order->ids.front())) == Graded::Failed) {
            std::fprintf(stderr, "  monitor: %s\n", statusLine().c_str());
        }
        reportUnless(error.empty(), "and records no failure", "last_error: " + error);
    }
}

// S15.412 and S15.414: a 4K video whose only keyframe is its first resumes far into the file while another
// monitor's pictures are asked for throughout.
void runResumeHoldChecks(const WallpaperManager::Config& base, const std::string& longGop, const std::string& still) {
    using wallpaperx::testing::setMonitorOutput;
    using wallpaperx::testing::setOutputVisible;
    const char* kGoals = "S15.412 and S15.414 (a resume's catch-up never holds the GPU for long, and its frame stays served)";
    WallpaperManager::Config config = playsUnwatched(base);
    config.idleSessionRelease = std::chrono::seconds(2);
    g_files.watch(longGop);
    constexpr int32_t kResuming = 0;
    constexpr int32_t kOther = 1;
    const std::string screen = "DP-HOLD-0";

    PhaseGrade grade("the resume-hold phase");
    BusRig rig(config, grade);
    sdbus::IProxy& proxy = *rig.proxy;
    setMonitorOutput(proxy, kOther, "DP-HOLD-1");
    setWallpaper(proxy, "static", still, kOther);
    setMonitorOutput(proxy, kResuming, screen);
    wallpaperx::testing::setWallpaperWithProperties(proxy, "video", longGop, kResuming, {{"speed", sdbus::Variant(2.0)}});
    constexpr uint64_t kPlayed = 150;
    if (!settlesWithin(std::chrono::seconds(60), [&] { return latestGeneration(rig.pictures, kResuming) >= kPlayed; })) {
        if (latestGeneration(rig.pictures, kResuming) == 0) grade.expectNoDevice();
        notVerified(kGoals, "the 4K video did not show " + std::to_string(kPlayed) + " pictures within 60 s on this host (" +
                                std::to_string(latestGeneration(rig.pictures, kResuming)) + " shown)");
        return;
    }
    const auto ms = [](Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };

    struct Answer {
        Clock::time_point asked;
        Clock::time_point answered;
        int32_t monitor = 0;
        bool served = false;
        uint64_t generation = 0;
        std::string refusal;
    };
    struct Resume {
        uint64_t stopped = 0;
        std::vector<Answer> answers;
        std::string probeFault;
        Clock::time_point resumeAt;
        Clock::time_point gaveUpAt;
        std::optional<Clock::time_point> firstNew;
        int catchUpSends = 0;
    };
    // Hides the screen until its session is released, then shows it again with every packet delayed by
    // `delayMs`, while frame requests are asked from a thread of its own, on a connection of its own: each
    // answer's wait is a lower bound on how long the GPU lock was held while the request was queued.
    const auto resumeOnce = [&](int delayMs) -> std::optional<Resume> {
        g_files.resetCounts();
        setOutputVisible(proxy, screen, false);
        const bool released = settlesWithin(std::chrono::seconds(20), [&] { return g_files.closes(longGop) >= 1; });
        check(released && latestGeneration(rig.pictures, kOther) > 0,
              "a hidden 4K video stops and its session is released, beside a drawn still (the premise)");
        if (!released || latestGeneration(rig.pictures, kOther) == 0) return std::nullopt;
        Resume r;
        r.stopped = latestGeneration(rig.pictures, kResuming);
        std::atomic<bool> resumedAsked{false};
        const int sendsBefore = g_decodeCalls.load();
        struct Undelay {
            ~Undelay() { g_sendDelayMs.store(0); }
        } undelay;
        {
            auto probeProxy =
                sdbus::createProxy(sdbus::ServiceName{rig.daemonName}, sdbus::ObjectPath{DbusService::kObjectPath});
            std::jthread probe([&](const std::stop_token& stop) {
                try {
                    for (uint64_t i = 0; !stop.stop_requested(); ++i) {
                        // The resuming monitor only once it was shown again: a fetch on a hidden, released
                        // monitor is a hand-over, which reopens it.
                        const int32_t monitor = (i % 2 == 1 && resumedAsked.load()) ? kResuming : kOther;
                        Answer answer;
                        answer.monitor = monitor;
                        answer.asked = Clock::now();
                        try {
                            const wallpaperx::testing::Frame frame = getFrame(*probeProxy, monitor);
                            answer.answered = Clock::now();
                            answer.served = true;
                            answer.generation = frame.generation;
                            wallpaperx::testing::releaseFrame(*probeProxy, monitor, frame);
                        } catch (const sdbus::Error& e) {
                            answer.answered = Clock::now();
                            answer.refusal = e.getName() + ": " + e.getMessage();
                        }
                        r.answers.push_back(std::move(answer));
                    }
                } catch (const std::exception& e) {
                    r.probeFault = e.what();
                }
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            g_sendDelayMs.store(delayMs);
            r.resumeAt = Clock::now();
            setOutputVisible(proxy, screen, true);
            resumedAsked.store(true);
            if (settlesWithin(std::chrono::seconds(20),
                              [&] { return latestGeneration(rig.pictures, kResuming) > r.stopped; })) {
                r.catchUpSends = g_decodeCalls.load() - sendsBefore;
                r.firstNew = firstSeen(rig.pictures, kResuming, r.stopped + 1);
            }
            r.gaveUpAt = Clock::now();
            g_sendDelayMs.store(0);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        if (!r.probeFault.empty()) {
            wallpaperx::testing::harnessFailure("the frame-request thread failed: " + r.probeFault);
            return std::nullopt;
        }
        return r;
    };
    // A decoder that catches this file up faster than kMeasurable -- Lavapipe's software one takes about 130 ms
    // -- leaves one picture's draw too close to the whole catch-up to tell a held lock from a sliced one. The
    // resume is then repeated with each packet delayed kSimulatedMs inside the decoder call, which the daemon
    // pays under the GPU lock as it would a slow decode, and said so.
    constexpr double kMeasurable = 400;
    constexpr int kSimulatedMs = 8;
    std::optional<Resume> resumed = resumeOnce(0);
    if (!resumed) return;
    if (resumed->firstNew && ms(*resumed->firstNew - resumed->resumeAt) < kMeasurable) {
        std::printf("video_wallpaper_test: this host caught the 4K video up in %.0f ms; resuming it again with each "
                    "packet delayed %d ms in the decoder\n",
                    ms(*resumed->firstNew - resumed->resumeAt), kSimulatedMs);
        std::fflush(stdout);
        resumed = resumeOnce(kSimulatedMs);
        if (!resumed) return;
    }
    const uint64_t stopped = resumed->stopped;
    const std::vector<Answer>& answers = resumed->answers;
    const Clock::time_point resumeAt = resumed->resumeAt;
    const std::optional<Clock::time_point> firstNew = resumed->firstNew;
    const int catchUpSends = resumed->catchUpSends;

    // GOAL: S15.414 -- while a resume catches up, its monitor's frame stays served and no new generation
    //       appears: every request for it until the resumed picture is shown is answered with the picture
    //       it stopped on.
    // WHY-THIS-SHAPE: requests answered before the first new generation was announced, or before the wait
    //       for one gave up -- graded ahead of "it resumes", since a frame lost during the catch-up can stop
    //       the video resuming at all, and those answers are then the evidence. The generation is compared,
    //       not the pixels, which on this fixture name no picture; S15.151 grades which picture comes next.
    //       Since the window ends at the first announcement, "no new generation" here catches only a commit
    //       served before it was announced; an early picture announced as new is S15.151's to see.
    // CONTROL: via control_run.sh, --phase '^resumeHold$', 2026-10-08, NVIDIA, reverted: renderPassLocked()
    //       (backend/daemon/wallpaper_manager.cpp) clearing `hasFrame` where it finds the frame it keeps for a
    //       CatchingUp session -> "FAIL: a resuming monitor serves the picture it stopped on until the resumed one
    //       is shown", "6756 of 6756 answer(s) were not generation 150; the first: refused: ...NoFrame", beside
    //       "FAIL: the hidden 4K video resumes when its screen is drawn again" -- the clock skips a frameless monitor.
    // COVERAGE-ASSERT: at least five such answers. A lock held through the catch-up leaves fewer, so S15.412's
    //       two controls turn this premise red as well.
    const Clock::time_point windowEnd = firstNew.value_or(resumed->gaveUpAt);
    int during = 0;
    int wrong = 0;
    std::string firstWrong;
    for (const Answer& a : answers) {
        if (a.monitor != kResuming || a.asked < resumeAt || a.answered >= windowEnd) continue;
        ++during;
        if (!a.served || a.generation != stopped) {
            ++wrong;
            if (firstWrong.empty()) {
                firstWrong = a.served ? "generation " + std::to_string(a.generation) : "refused: " + a.refusal;
            }
        }
    }
    check(during >= 5, "the resuming monitor's frame was asked for at least five times during its catch-up (the premise)");
    reportUnless(wrong == 0, "a resuming monitor serves the picture it stopped on until the resumed one is shown",
                 std::to_string(wrong) + " of " + std::to_string(during) + " answer(s) were not generation " +
                     std::to_string(stopped) + "; the first: " + firstWrong);

    check(firstNew.has_value(), "the hidden 4K video resumes when its screen is drawn again");
    if (!firstNew) return;
    const Clock::duration catchUp = *firstNew - resumeAt;

    // GOAL: S15.412 -- while a monitor resumes a long-keyframe video, no frame request waits behind the
    //       catch-up: neither the reopen render nor any clock round holds the GPU lock for more than a
    //       small part of it. False for a reopen that decodes forward to its picture inside one render,
    //       and for clock rounds that drop pictures without a time limit.
    // WHY-THIS-SHAPE: measured as the longest wait of a frame request overlapping the catch-up, against a
    //       quarter of the catch-up's own length -- not a fixed number of milliseconds, which would be a
    //       verdict on this machine's decoder speed. Requests alternate between a still on another monitor
    //       and the resuming one; both take the GPU lock. A frozen catch-up holds it for nearly all of its
    //       length, a sliced one for one picture and the 4 ms slice.
    // CONTROL: via control_run.sh, --phase '^resumeHold$', 2026-10-08, each alone, reverted, in
    //       backend/daemon/wallpaper_manager.cpp. renderPassLocked()'s `keepsFrame` forced false, so the reopen
    //       render catches up in one go -> "FAIL: no frame request waits behind a resume's catch-up for long",
    //       "the longest wait was 918 ms of a 909 ms catch-up" on NVIDIA (unbroken: 17-24 ms of about 910 ms),
    //       and 1458 ms of 1444 ms on Lavapipe with the slowed decode. decodeUntil() ignoring its deadline ->
    //       the same line, 890 ms of 885 ms, NVIDIA.
    //       Re-run 2026-10-19 (T7), RTX, decodeUntil() now on takeReady(): held, this line and the premise line
    //       ("asked for at least five times during its catch-up"), the catch-up running in one go.
    //       The `keepsFrame` break re-run 2026-10-19 (T7b), RTX: held, this line.
    // COVERAGE-ASSERT: the catch-up sent at least 100 packets -- a resume near the keyframe would prove
    //       nothing -- and a request overlapped it. Not "many requests": a lock held throughout leaves room
    //       for only one or two. The graded catch-up lasted kMeasurable or more; see resumeOnce's caller.
    double longest = 0;
    int overlapping = 0;
    for (const Answer& a : answers) {
        if (a.asked >= *firstNew || a.answered <= resumeAt) continue;
        ++overlapping;
        longest = std::max(longest, ms(a.answered - a.asked));
    }
    std::printf("video_wallpaper_test: the 4K resume after %llu picture(s) took %.0f ms and %d packet(s); %d frame "
                "request(s) overlapped it, the longest waiting %.1f ms\n",
                static_cast<unsigned long long>(stopped), ms(catchUp), catchUpSends, overlapping, longest);
    std::fflush(stdout);
    check(catchUpSends >= 100 && overlapping >= 1,
          "the 4K resume decoded at least 100 pictures to catch up, with a frame request during it (the premise)");
    if (ms(catchUp) < kMeasurable) {
        notVerified("S15.412", "this host caught the 4K video up in " + std::to_string(ms(catchUp)) +
                                   " ms even with its decode slowed, too close to one picture's draw to tell a held "
                                   "lock from a sliced one");
        return;
    }
    reportUnless(longest < ms(catchUp) / 4, "no frame request waits behind a resume's catch-up for long",
                 "the longest wait was " + std::to_string(longest) + " ms of a " + std::to_string(ms(catchUp)) +
                     " ms catch-up");
}

// S15.415: a resume interrupted while it catches up. Each packet sent to the decoder is delayed
// so the catch-up spans seconds of clock rounds and the phase can act inside it.
void runResumeCatchUpChecks(const WallpaperManager::Config& base, const std::string& counterClip) {
    using wallpaperx::testing::setMonitorOutput;
    using wallpaperx::testing::setOutputVisible;
    const char* kGoals = "S15.415 (a resume interrupted while it catches up)";
    const std::optional<CounterOrder> order = counterOrder(counterClip, kGoals);
    if (!order) return;
    WallpaperManager::Config config = base;
    config.width = 160;
    config.height = 120;
    config.idleSessionRelease = std::chrono::seconds(2);
    g_files.watch(counterClip);
    // The fixture's keyframes are 24 pictures apart; a stop this far past one leaves a catch-up of ten.
    constexpr int kKeyframeInterval = 24;
    constexpr int kStopFrom = 9;
    constexpr int kStopTo = 14;
    constexpr int kSendDelayMs = 150;
    struct Undelay {
        ~Undelay() { g_sendDelayMs.store(0); }
    } undelay;

    PhaseGrade grade("the resume-catch-up phase");
    BusRig rig(config, grade);
    sdbus::IProxy& proxy = *rig.proxy;
    CounterReader reader;
    CountedPictures taker(proxy, rig.pictures, kMonitorResume, reader);
    const std::string screen = "DP-CATCHUP";
    setMonitorOutput(proxy, kMonitorResume, screen);
    wallpaperx::testing::setWallpaperWithProperties(proxy, "video", counterClip, kMonitorResume,
                                                    {{"speed", sdbus::Variant(0.5)}});
    if (!settlesWithin(std::chrono::seconds(30), [&] { return latestGeneration(rig.pictures, kMonitorResume) > 0; })) {
        grade.expectNoDevice();
        notVerified(kGoals, "the counter video produced no frame on this host");
        return;
    }
    // Plays to a picture well past a keyframe, hides the screen and waits for the release.
    const auto playThenRelease = [&]() -> std::optional<ReadPicture> {
        taker.takeUntil(
            [&] {
                if (taker.pictures().empty()) return false;
                const int at = taker.pictures().back().counter % kKeyframeInterval;
                return at >= kStopFrom && at <= kStopTo;
            },
            std::chrono::seconds(30));
        if (readingStopped(taker, kGoals)) return std::nullopt;
        g_files.resetCounts();
        setOutputVisible(proxy, screen, false);
        const bool quiet = taker.takeUntilQuiet(kQuietWindow, std::chrono::seconds(25));
        const std::optional<ReadPicture> frozen = quiet ? taker.current() : std::nullopt;
        if (readingStopped(taker, kGoals)) return std::nullopt;
        const bool released =
            frozen && settlesWithin(std::chrono::seconds(20), [&] { return g_files.closes(counterClip) >= 1; });
        const bool farIn = frozen && frozen->counter % kKeyframeInterval >= 4;
        // CONTROL: via control_batch.sh, 2026-10-16, NVIDIA: WallpaperManager::setOutputVisible
        //       (backend/daemon/wallpaper_manager.cpp) ignoring a hide -> this line red, "stopped on nothing;
        //       never released". Reverted.
        check(released && farIn, "the counter video stops well past a keyframe when hidden, and is released (the premise)");
        if (!released || !farIn) {
            std::fprintf(stderr, "  stopped on %s; %s\n", frozen ? std::to_string(frozen->counter).c_str() : "nothing",
                         released ? "released" : "never released");
            return std::nullopt;
        }
        return frozen;
    };
    // Shows the screen with every packet delayed and waits for the first packet of the catch-up.
    const auto resumeSlowly = [&]() {
        const int sends = g_decodeCalls.load();
        g_sendDelayMs.store(kSendDelayMs);
        setOutputVisible(proxy, screen, true);
        return settlesWithin(std::chrono::seconds(20), [&] { return g_decodeCalls.load() > sends; });
    };

    // GOAL: S15.415 -- a video released while it is still catching up keeps the position it was resuming
    //       to: shown again later, it resumes on the picture it first stopped on, then its successor.
    // WHY-THIS-SHAPE: the screen is hidden again inside the first packet of the slowed catch-up, so the
    //       session is released holding no picture of its own; a release that recorded only a picture the
    //       session drew records nothing, and the video starts again from its first picture.
    // CONTROL: via control_batch.sh, --phase '^resumeCatchUp$', re-run 2026-10-24, NVIDIA, alone, reverted:
    //       releaseVideoSessionLocked() (backend/daemon/wallpaper_manager.cpp) treating only Undrawn as having
    //       drawn nothing, so a CatchingUp session records its pipeline's (empty) shownPosition() -> "FAIL: a video
    //       released while it catches up resumes, next time, on the picture it stopped on, then its successor",
    //       "stopped on 9 (generation 10), then showed 0 and 1".
    // COVERAGE-ASSERT: no new generation appeared between the two releases, so the catch-up was cut short.
    {
        const std::optional<ReadPicture> frozen = playThenRelease();
        if (!frozen) return;
        g_files.resetCounts();
        // CONTROL: via control_batch.sh, --phase '^resumeCatchUp$', 2026-10-16, re-run 2026-10-18 (T2), NVIDIA:
        //       reopenVideosLocked() (backend/daemon/wallpaper_manager.cpp) never owing a render to a playing video
        //       with no session (`bool owed = false;`) -> this line red. Reverted.
        if (!resumeSlowly()) {
            check(false, "a hidden counter video shown again sends its decoder a packet (the premise)");
            return;
        }
        setOutputVisible(proxy, screen, false);
        const bool released = settlesWithin(std::chrono::seconds(20), [&] { return g_files.closes(counterClip) >= 1; });
        g_sendDelayMs.store(0);
        const uint64_t latest = latestGeneration(rig.pictures, kMonitorResume);
        // CONTROL: via control_batch.sh, --phase '^resumeCatchUp$', 2026-10-16, graded mechanically 2026-10-18 (T2),
        //       NVIDIA: advanceSessionLocked() (backend/daemon/wallpaper_manager.cpp) returning before
        //       releaseVideoSessionLocked() for a CatchingUp session -> this line red. Reverted.
        check(released, "a video hidden again while it catches up is released (the premise)");
        if (!released) return;
        if (latest != frozen->generation) {
            notVerified("S15.415", "the slowed catch-up reached its picture before the screen was hidden again (generation " +
                                       std::to_string(latest) + " after " + std::to_string(frozen->generation) + ")");
            return;
        }
        const size_t from = taker.pictures().size();
        setOutputVisible(proxy, screen, true);
        taker.takeUntil([&] { return taker.newerThan(from, frozen->generation) >= 2; }, std::chrono::seconds(20));
        if (readingStopped(taker, kGoals)) return;
        const std::vector<ReadPicture> shown(taker.pictures().begin() + static_cast<long>(from), taker.pictures().end());
        gradeNextTwo("a video released while it catches up resumes, next time, on the picture it stopped on, then its "
                     "successor",
                     shown, *frozen, frozen->counter, order->after(frozen->counter));
    }
}

// S15.427: a GPU fault met by a resume's catch-up, on the clock's path or on a render that must catch up
// at once. One path per phase: the injected loss leaves that phase's device lost.
void runResumeGpuFaultChecks(const WallpaperManager::Config& base, const std::string& counterClip, bool inRenderPass) {
    using wallpaperx::testing::setMonitorOutput;
    using wallpaperx::testing::setOutputSize;
    using wallpaperx::testing::setOutputVisible;
    const char* kGoal = "S15.427 (a GPU fault in a resume's catch-up is reported, and the file is not opened again)";
    WallpaperManager::Config config = playsUnwatched(base);
    config.width = 160;
    config.height = 120;
    config.idleSessionRelease = std::chrono::seconds(2);
    g_files.watch(counterClip);
    struct Undo {
        ~Undo() {
            g_decodeGate.release();
            g_submitDeviceLost.store(0);
        }
    } undo;

    PhaseGrade grade(inRenderPass ? "the resume-gpu-fault-render phase" : "the resume-gpu-fault phase");
    BusRig rig(config, grade);
    sdbus::IProxy& proxy = *rig.proxy;
    const std::string screen = "DP-RESUMEGPU";
    setMonitorOutput(proxy, kMonitorResume, screen);
    setWallpaper(proxy, "video", counterClip, kMonitorResume);
    if (!settlesWithin(std::chrono::seconds(30), [&] { return latestGeneration(rig.pictures, kMonitorResume) > 0; })) {
        grade.expectNoDevice();
        notVerified(kGoal, "the counter video produced no frame on this host");
        return;
    }
    // Stopped part way in, so a resume has somewhere to go back to.
    settlesWithin(std::chrono::seconds(20), [&] { return latestGeneration(rig.pictures, kMonitorResume) >= 40; });
    g_files.resetCounts();
    setOutputVisible(proxy, screen, false);
    const bool released = settlesWithin(std::chrono::seconds(20), [&] { return g_files.closes(counterClip) >= 1; });
    check(released, "a hidden counter video is released (the premise)");
    if (!released) return;
    const std::optional<wallpaperx::MonitorStatus> before = monitorStatus(rig.manager, kMonitorResume);
    const uint64_t frozen = before ? before->generation : 0;
    if (rig.manager.snapshot().health.gpu.status != wallpaperx::HealthStatus::Ok || !before ||
        !before->lastError.empty()) {
        check(false, "GPU health is ok and the monitor has no error standing before the fault (the premise)");
        return;
    }

    // GOAL: S15.427 -- a GPU fault met by a resume's catch-up is the GPU's, not the position's: it reaches GPU
    //       health, and the file is not opened again from its first picture -- on the clock's path and on a
    //       render that must catch up at once.
    // WHY-THIS-SHAPE: the render thread is held at the catch-up's first packet and the device loss armed there,
    //       so the next submission is the catch-up's own conversion -- nothing else submits while the screen is
    //       hidden or catching up. Reopens are counted once the daemon has finished with the fault: its error
    //       stands with no render owed (a retry is owed in the same lock that records it, and serviced only
    //       after its open), or the file was opened again. A fixed sleep instead would grade a reopen later
    //       than the sleep as none. The render-pass variant reports a new size while hidden, so the first render
    //       after the release cannot keep the frame and catches up itself.
    // CONTROL: via control_run.sh, --phase '^resumeGpuFault(Render)?$', 2026-10-09, video_wallpaper_lavapipe,
    //       reverted: givesUpPosition() (backend/daemon/wallpaper_manager.cpp) giving the position up for every
    //       failure -> in both phases "FAIL: a GPU fault in a resume's catch-up is recorded against the GPU" (the
    //       reopened file's render had cleared it) and "FAIL: ... does not open the file again" (re-run 2026-10-09:
    //       settled on the file opened again, 2 opens). On '^resumeGpuFault$' only, the clock path's failure branch
    //       (a) retrying after a GPU fault (`!failure.gpuFault &&` removed) -> "FAIL: ... does not open the file
    //       again", "2 open(s), generation 40 after 40": the error was recorded first, then the reopen; (b) owing a
    //       render it never posts (`++requestSeq` after recordFailureLocked) -> "FAIL: the daemon finishes with a
    //       GPU fault in a resume's catch-up", "... last_error: Device::submit: vkQueueSubmit failed with
    //       VK_ERROR_DEVICE_LOST, a render owed". All reverted.
    // COVERAGE-ASSERT: the injected loss was spent, after the catch-up's first packet and before any picture.
    g_files.resetCounts();
    g_decodeGate.arm(CallGate::Mode::Hold);
    if (inRenderPass) setOutputSize(proxy, screen, 176, 132);
    setOutputVisible(proxy, screen, true);
    const bool atCatchUp = g_decodeGate.waitFired(std::chrono::seconds(20));
    if (atCatchUp) g_submitDeviceLost.store(1);
    g_decodeGate.release();
    const bool spent = atCatchUp && settlesWithin(std::chrono::seconds(20), [] { return g_submitDeviceLost.load() == 0; });
    const bool recorded = spent && settlesWithin(std::chrono::seconds(10), [&] {
                              return rig.manager.snapshot().health.gpu.status == wallpaperx::HealthStatus::Fail;
                          });
    std::string settledOn = "nothing";
    const bool settled = spent && settlesWithin(std::chrono::seconds(20), [&] {
                             const std::optional<wallpaperx::MonitorStatus> status =
                                 monitorStatus(rig.manager, kMonitorResume);
                             if (g_files.opens(counterClip) > 1) settledOn = "the file opened again";
                             else if (status && !status->lastError.empty() && !status->renderOutstanding)
                                 settledOn = "the error standing with no render owed";
                             else return false;
                             return true;
                         });
    const int opens = g_files.opens(counterClip);
    const std::optional<wallpaperx::MonitorStatus> after = monitorStatus(rig.manager, kMonitorResume);
    const uint64_t latest = after ? after->generation : 0;
    std::printf("video_wallpaper_test: GPU fault in a %s catch-up: %s, settled on %s; %d open(s), generation %llu "
                "after %llu; gpu: %s\n",
                inRenderPass ? "render pass's" : "clock round's", spent ? "injected" : "never injected",
                settledOn.c_str(), opens, static_cast<unsigned long long>(latest),
                static_cast<unsigned long long>(frozen), rig.manager.snapshot().health.gpu.detail.c_str());
    std::fflush(stdout);
    check(spent, "the device loss was injected at the resume's catch-up (the premise)");
    if (!spent) return;
    reportUnless(recorded, "a GPU fault in a resume's catch-up is recorded against the GPU",
                 "gpu health: " + rig.manager.snapshot().health.gpu.detail);
    // A daemon still working on the fault after 20 s has not answered whether it reopens, so this is a failure
    // of its own and the reopen line below is not graded on it.
    reportUnless(settled, "the daemon finishes with a GPU fault in a resume's catch-up",
                 "after 20 s neither an error standing with no render owed nor a reopen; last_error: " +
                     (after ? after->lastError : std::string("no status")) +
                     (after && after->renderOutstanding ? ", a render owed" : ""));
    if (!settled) return;
    reportUnless(opens == 1 && latest == frozen, "a GPU fault in a resume's catch-up does not open the file again",
                 std::to_string(opens) + " open(s), generation " + std::to_string(latest) + " after " +
                     std::to_string(frozen));
}

// S15.428: a GPU failure recorded while a released video resumes, after the render that kept its frame, is
// cleared by the resumed picture -- whether drawn in the round its catch-up finished or in a later one.
void runResumeHealthChecks(const WallpaperManager::Config& base, const std::string& counterClip, const std::string& still) {
    using wallpaperx::testing::setMonitorOutput;
    using wallpaperx::testing::setOutputSize;
    using wallpaperx::testing::setOutputVisible;
    const char* kGoal = "S15.428 (the resumed picture clears a GPU failure the kept-frame render would have)";
    WallpaperManager::Config config = playsUnwatched(base);
    config.width = 160;
    config.height = 120;
    // Long enough that a screen hidden for a moment in the later-round half keeps its session.
    config.idleSessionRelease = std::chrono::seconds(5);
    g_files.watch(counterClip);
    // The fixture's keyframes are 24 pictures apart; a stop this far past one leaves a catch-up of several rounds.
    constexpr uint64_t kKeyframeInterval = 24;
    constexpr int kSendDelayMs = 150;
    constexpr int32_t kStill = 1;
    struct Undo {
        ~Undo() {
            g_sendDelayMs.store(0);
            g_framebufferGate.release();
            g_framebufferLostGate.release();
        }
    } undo;

    PhaseGrade grade("the resume-health phase");
    BusRig rig(config, grade);
    sdbus::IProxy& proxy = *rig.proxy;
    const std::string screen = "DP-HEALTHV";
    const std::string stillScreen = "DP-HEALTHS";
    setMonitorOutput(proxy, kMonitorResume, screen);
    setMonitorOutput(proxy, kStill, stillScreen);
    wallpaperx::testing::setWallpaperWithProperties(proxy, "video", counterClip, kMonitorResume,
                                                    {{"speed", sdbus::Variant(0.5)}});
    setWallpaper(proxy, "static", still, kStill);
    const auto generation = [&] { return latestGeneration(rig.pictures, kMonitorResume); };
    const auto gpu = [&] { return rig.manager.snapshot().health.gpu; };
    if (!settlesWithin(std::chrono::seconds(30),
                       [&] { return generation() > 0 && latestGeneration(rig.pictures, kStill) > 0; })) {
        grade.expectNoDevice();
        notVerified(kGoal, "the counter video and the still were not both drawn on this host");
        return;
    }
    // The video's generation as committed, read under the daemon's state lock -- not as announced on the bus,
    // which can lag the commit.
    const auto committed = [&] {
        const std::optional<wallpaperx::MonitorStatus> status = monitorStatus(rig.manager, kMonitorResume);
        return status ? status->generation : 0;
    };
    // Plays to a picture well past a keyframe -- generation g shows picture g - 1, every picture drawn in turn
    // -- hides the screen and waits for the release; the committed generation it stopped on.
    const auto stopPastKeyframe = [&]() -> std::optional<uint64_t> {
        settlesWithin(std::chrono::seconds(30), [&] {
            const uint64_t at = (generation() - 1) % kKeyframeInterval;
            return at >= 9 && at <= 14;
        });
        g_files.resetCounts();
        setOutputVisible(proxy, screen, false);
        const bool released = settlesWithin(std::chrono::seconds(20), [&] { return g_files.closes(counterClip) >= 1; });
        // CONTROL: via control_batch.sh, --phase '^resumeHealth$', 2026-10-16, NVIDIA:
        //       WallpaperManager::setOutputVisible (backend/daemon/wallpaper_manager.cpp) ignoring a hide -> this
        //       line red. Reverted.
        check(released, "a hidden counter video is released (the premise)");
        if (!released) return std::nullopt;
        g_files.resetCounts();
        return committed();
    };
    // The still's next render loses the device: its screen is given a size only it is drawn at. Settles on the
    // first snapshot holding either the failure or a video picture past `frozen`, so whether the failure was
    // recorded before that picture is read from one acquisition of the daemon's state.
    struct StillFailure {
        bool failed = false;
        bool beforePicture = false;
    };
    const auto failStill = [&](uint32_t width, uint32_t height, uint64_t frozen) {
        g_framebufferLostGate.arm(CallGate::Mode::Fail, extentOf(width, height));
        setOutputSize(proxy, stillScreen, width, height);
        StillFailure out;
        const bool fired = g_framebufferLostGate.waitFired(std::chrono::seconds(20));
        out.failed = fired && settlesWithin(std::chrono::seconds(20), [&] {
                         const WallpaperManager::Snapshot snap = rig.manager.snapshot();
                         const auto it = snap.monitors.find(kMonitorResume);
                         const uint64_t at = it != snap.monitors.end() ? it->second.generation : 0;
                         const bool fail = snap.health.gpu.status == wallpaperx::HealthStatus::Fail;
                         if (!fail && at == frozen) return false;
                         out.beforePicture = fail && at == frozen;
                         return true;
                     });
        g_framebufferLostGate.release();
        return out;
    };
    const auto clearedBy = [&](uint64_t frozen, std::string& detail) {
        const bool shown = settlesWithin(std::chrono::seconds(20), [&] { return generation() > frozen; });
        const bool cleared =
            shown && settlesWithin(std::chrono::seconds(2), [&] { return gpu().status == wallpaperx::HealthStatus::Ok; });
        detail = shown ? "gpu health still: " + gpu().detail : "the resumed picture was never drawn";
        return cleared;
    };

    // GOAL: S15.428 -- a GPU failure recorded after the render that resumed a released video kept its frame,
    //       and before the clock draws the resumed picture, is cleared by that picture, as the render's own
    //       commit would have cleared it.
    // WHY-THIS-SHAPE: every packet is slowed so the catch-up spans rounds; once it has sent its first, a still on
    //       another screen fails as a lost device. The failure must land before the resumed picture -- a stop
    //       near a keyframe can finish first, and that is reported as not verified, never graded. Which came
    //       first is read from one snapshot holding both health and the committed generation: the bus-announced
    //       generation can lag the commit, and would grade a clear that preceded the failure as a missing clear.
    //       Health is read once the resumed picture is announced, which comes after its commit.
    // CONTROL: via control_batch.sh, --phase '^resumeHealth$', re-run 2026-10-16 at half speed on both drivers, reverted: the
    //       `if (delivered)` publish in advanceSessionLocked() (backend/daemon/wallpaper_manager.cpp) made
    //       `if (false && delivered)` -> "FAIL: the resumed picture, drawn in the round its catch-up finished,
    //       clears a GPU failure recorded since the kept-frame render", "gpu health still: OffscreenTargetManager:
    //       vkCreateFramebuffer failed with VK_ERROR_DEVICE_LOST". The later-round half then fails at its premise:
    //       the failure left standing stops the next resume (reopenVideosLocked posts nothing while it stands).
    //       Test-side, via control_batch.sh, 2026-10-16, NVIDIA: waiting for the resumed picture's commit before failing the still -> "NOT VERIFIED
    //       -- S15.428's same-round half", no FAIL, and the later-round half green after the still's redraw.
    {
        const std::optional<uint64_t> frozen = stopPastKeyframe();
        if (!frozen) return;
        const int sends = g_decodeCalls.load();
        g_sendDelayMs.store(kSendDelayMs);
        setOutputVisible(proxy, screen, true);
        const bool catchingUp = settlesWithin(std::chrono::seconds(20), [&] { return g_decodeCalls.load() > sends; });
        const StillFailure still = catchingUp ? failStill(kFaultWidth, kFaultHeight, *frozen) : StillFailure{};
        g_sendDelayMs.store(0);
        // CONTROL: via control_batch.sh, --phase '^resumeHealth$', 2026-10-16, NVIDIA: reopenVideosLocked()
        //       (backend/daemon/wallpaper_manager.cpp) never owing a render to a playing video with no session -> only this
        //       line red. Reverted.
        check(catchingUp && still.failed,
              "the resumed video catches up, and a still's render records a GPU failure (the premise)");
        if (!catchingUp || !still.failed) return;
        if (!still.beforePicture) {
            notVerified("S15.428's same-round half", "the catch-up from generation " + std::to_string(*frozen) +
                                                         " reached its picture before the failure was recorded");
            // Nothing cleared the failure, and while it stands no resume is posted, so the later-round half would
            // fail its premise on a correct daemon. A render of the still that succeeds clears it.
            setOutputSize(proxy, stillScreen, kFaultWidth + 16, kFaultHeight + 16);
            if (!settlesWithin(std::chrono::seconds(20), [&] { return gpu().status == wallpaperx::HealthStatus::Ok; })) {
                notVerified("S15.428's later-round half", "GPU health stayed failed after the still drew again: " +
                                                              gpu().detail);
                return;
            }
        } else {
            std::string detail;
            reportUnless(clearedBy(*frozen, detail),
                         "the resumed picture, drawn in the round its catch-up finished, clears a GPU failure recorded "
                         "since the kept-frame render",
                         detail);
        }
    }

    // GOAL: S15.428's later-round half -- the same when the catch-up finishes in a round that cannot draw (the
    //       screen stopped being drawn meanwhile), so the resumed picture is drawn in a later round.
    // WHY-THIS-SHAPE: the render thread is held at the conversion's framebuffer (the clip's own extent), which
    //       exists only in the catch-up's last step; the screen is hidden there, so that round's draw check finds
    //       it not playing and leaves the picture undrawn. A flag read at the start of the catch-up round would
    //       have been spent by then.
    // CONTROL: via control_batch.sh, --phase '^resumeHealth$', re-run 2026-10-16, NVIDIA at half speed, reverted: the
    //       condition guarding `delivered = healthCheck(...)` in advanceSessionLocked()
    //       (backend/daemon/wallpaper_manager.cpp) made `resuming && session.openedAt` -- read at the start of the
    //       round -> only this line red, "gpu health still: OffscreenTargetManager: vkCreateFramebuffer failed with
    //       VK_ERROR_DEVICE_LOST"; the same-round line above stayed green.
    // COVERAGE-ASSERT: the hold was reached, and nothing was committed or released before the screen was shown
    //       again (the committed generation, not the bus's, which can lag).
    {
        const std::optional<uint64_t> frozen = stopPastKeyframe();
        if (!frozen) return;
        g_framebufferGate.arm(CallGate::Mode::Hold, extentOf(320, 240));
        setOutputVisible(proxy, screen, true);
        const bool held = g_framebufferGate.waitFired(std::chrono::seconds(20));
        if (held) setOutputVisible(proxy, screen, false);
        g_framebufferGate.release();
        check(held, "the resumed video's catch-up reaches its last step (the premise)");
        if (!held) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const bool slipped = committed() == *frozen && g_files.closes(counterClip) == 0;
        const bool failed = slipped && failStill(kFaultWidth + 8, kFaultHeight + 8, *frozen).beforePicture;
        // CONTROL: via control_batch.sh, --phase '^resumeHealth$', 2026-10-16, NVIDIA: advanceSessionLocked()'s draw
        //       check (backend/daemon/wallpaper_manager.cpp) dropping its `playingSeqLocked(...) != playing` term, so the
        //       caught-up picture is drawn on a hidden screen -> only this line red. Reverted.
        check(slipped && failed, "the caught-up picture was left undrawn, and a still's render then records a GPU failure "
                                 "(the premise)");
        if (!slipped || !failed) return;
        setOutputVisible(proxy, screen, true);
        std::string detail;
        reportUnless(clearedBy(*frozen, detail),
                     "the resumed picture, drawn a round after its catch-up finished, clears a GPU failure recorded "
                     "since the kept-frame render",
                     detail);
    }
}

// S15.418: a death during a resume, staged in a child, counts as a playing death and leaves no crash record.
void runResumeBlameChecks(const std::string& counterClip) {
    const char* kGoal = "S15.418 (a death during a resume counts against the video as a playing death)";
    const wallpaperx::testing::ScratchDir scratch("videoresumeblame");
    if (!scratch.ok()) {
        notVerified(kGoal, "no scratch directory could be created");
        return;
    }
    // GOAL: S15.418 -- a fatal signal in a resume's seek back, or in its catch-up decode, names the video in
    //       the playing record, as a death while it plays does, and leaves no crash record: the next start
    //       counts it as one of the video's playing deaths rather than quarantining the wallpaper.
    // WHY-THIS-SHAPE: a child of this binary plays the counter video for 40 pictures, is hidden until its
    //       session is released, and is shown again; it dies of a real memory fault at the seek or at the
    //       first packet after it, on the render thread. Both spans, since the open and the clock round each
    //       hold their own guard.
    // CONTROL: via control_run.sh, --phase '^resumeBlame$', 2026-10-08, NVIDIA, each alone, reverted, in
    //       backend/daemon/wallpaper_manager.cpp. `resuming.emplace(*blame)` removed from videoSessionLocked()'s
    //       `open` lambda -> "FAIL: a death in a resume's seek back is counted as a playing death of that video",
    //       "the record names no video". `const PlayingBlame working(*session.blame);` removed from
    //       advanceSessionLocked() -> the same for "a death in a resume's catch-up decode". The `open` lambda
    //       emplacing `applying` for an open at a position too -> "FAIL: a death in a resume's seek back leaves no
    //       crash record, ...", "a crash record names '.../counter_long.mp4'"; an ApplyingRecord around the
    //       CatchingUp decode -> the same for the catch-up decode. The last three re-run 2026-10-18 (T2) via
    //       control_batch.sh, NVIDIA, against the current advanceSessionLocked()/videoSessionLocked().
    // COVERAGE-ASSERT: a staged death that never came is reported by stageDeath() as not verified, never
    //       passed; how many of the two were reached is printed.
    int graded = 0;
    for (const char* scenario : {"resume-seek", "resume-catch-up"}) {
        const std::filesystem::path dir = scratch.path() / scenario;
        writeConfig(dir / "config.json", {{kMonitorBlamed, "video", counterClip}});
        const StagedDeath death =
            stageDeath("S15.418", dir / "config.json", dir / "state", {scenario, std::to_string(kMonitorBlamed), counterClip});
        if (!death.died) continue;
        ++graded;
        const std::string where = std::string(scenario) == "resume-seek" ? "a death in a resume's seek back"
                                                                         : "a death in a resume's catch-up decode";
        reportUnless(death.playing && death.playing->died && death.playing->died->monitorId == kMonitorBlamed &&
                         death.playing->died->path == counterClip && death.playing->signal == SIGSEGV,
                     (where + " is counted as a playing death of that video").c_str(), describeDeath(death));
        const std::optional<wallpaperx::CrashRecord> record = wallpaperx::readCrashRecord(dir / "state", kMonitorBlamed);
        reportUnless(!record, (where + " leaves no crash record, so the next start does not quarantine it").c_str(),
                     record ? "a crash record names '" + record->path + "'" : std::string());
    }
    std::printf("video_wallpaper_test: %d of 2 staged resume deaths were reached\n", graded);

    // GOAL: S15.874 -- a death in a render pass's catch-up decode on a shared session is blamed on that
    //       pass's monitor, not on the session's first screen.
    // WHY-THIS-SHAPE: the child drives every pass and round on one thread, so the second screen's pass is
    //       the first thing to decode the session's catch-up: the first screen reopened keeping its frame and
    //       no round ran between. The first screen opened the session, so a guard on the session's line names it.
    // CONTROL: control_batch.sh 2026-10-18, phase resumeBlame, reverted: caughtUpSessionLocked()'s
    //       `const PlayingBlame working(blame);` (backend/daemon/wallpaper_manager.cpp) made
    //       `working(*session.blame)` -> this line on both entries, "the record names monitor 0".
    const std::filesystem::path dir = scratch.path() / "shared-catch-up";
    writeConfig(dir / "config.json", {{kMonitorBlamed, "video", counterClip}, {kMonitorBystander, "video", counterClip}});
    const StagedDeath shared = stageDeath("S15.874", dir / "config.json", dir / "state",
                                          {"shared-catch-up", std::to_string(kMonitorBlamed), counterClip, "",
                                           std::to_string(kMonitorBystander)});
    if (shared.died) {
        reportUnless(shared.playing && shared.playing->died && shared.playing->died->monitorId == kMonitorBystander &&
                         shared.playing->died->path == counterClip,
                     "a death in a render pass's catch-up on a shared decoder names that pass's screen",
                     describeDeath(shared));
    }
}

// S15.157: time paused is not clean play. Manager-level, like runCleanPlayHideChecks: the playing record
// is what is read.
void runCleanPlayPauseChecks(const WallpaperManager::Config& base, const std::string& clip, bool withinOneRound) {
    const std::string what = withinOneRound ? "a pause begun and ended while the worker ran no round" : "a 4 s pause";
    const char* kGoal = "S15.157 (time paused is not clean play)";
    const wallpaperx::testing::ScratchDir scratch("videocleanpause");
    if (!scratch.ok()) {
        notVerified(kGoal, "no scratch directory could be created");
        return;
    }
    // Never unwatched: with the worker held the consumer cannot collect, and an unwatched stop would end
    // the stretch in the pause's place.
    WallpaperManager::Config config = playsUnwatched(base);
    config.configFile = scratch.path() / "config.json";
    config.stateDir = scratch.path() / "state";
    config.cleanPlayAfter = std::chrono::seconds(3);
    config.pausedSessionRelease = std::chrono::seconds(30);
    writeConfig(config.configFile, {{kMonitorClean, "video", clip}});
    {
        wallpaperx::PlayingRecord record(config.stateDir);
        record.start({{kMonitorClean, clip, 1}});
    }

    PhaseGrade grade("the clean-play pause phase");
    FrameReadyLog signals;
    WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};
    Collector collector(manager, kMonitorClean, ":1.cleanpause");
    RenderWorker worker(manager, [&](int id, uint64_t g) {
        signals.record(id, g);
        collector.announced(id, g);
    });
    collector.attach(worker);
    WorkerUnblock unblock{worker};
    const std::vector<int> restored = manager.restoreSaved();
    manager.recordStartingVideos();
    check(deathsIn(config.stateDir) == 1, "the video starts this run carrying one death (the harness's premise)");
    wallpaperx::testing::nameScreen(manager, kMonitorClean, "DP-CLEANPAUSE");
    for (const int id : restored) worker.post(id);
    if (!signals.wait(kMonitorClean, 1)) {
        grade.expectNoDevice();
        notVerified(kGoal, "the video did not draw on this host");
        return;
    }

    // GOAL: S15.157 -- time paused is not clean play: a pause of four seconds between two visible
    //       stretches that together fall short of cleanPlayAfter leaves the death count standing, whether
    //       the clock ran rounds during the pause or the pause began and ended while it ran none.
    // WHY-THIS-SHAPE: as runCleanPlayHideChecks, with the pause in place of the hide; the second run holds
    //       the worker in a task across the whole pause, so no clock round ever sees it and only the call
    //       that paused can end the stretch. The monitor never goes unwatched, or the consumer's silence
    //       during the hold would end the stretch instead. A run whose visible time reached cleanPlayAfter
    //       is reported as not graded.
    // CONTROL: via control_batch.sh, --phase '^cleanPlayPause', 2026-09-30, NVIDIA, each reverted, in
    //       backend/daemon/wallpaper_manager.cpp. THREE DEFENCES, measured: endPlayIfPausedLocked() made a
    //       no-op, the paused round's `if (!playing) it->second.cleanPlay.lastShown.reset();` deleted, or
    //       advanceSessionLocked()'s `onTime` forced true, each alone leaves both runs GREEN -- the first
    //       picture after a pause is late, and a late picture earns no credit. `onTime` forced true with
    //       endPlayIfPausedLocked() a no-op -> "FAIL: a pause begun and ended while the worker ran no round
    //       is not counted as play", "the count is 0", the other run green; with the paused round's drop
    //       deleted as well -> both runs red, "the count is 0".
    const Clock::time_point firstShown = Clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    double visibleBefore = 0;
    if (withinOneRound) {
        HeldWorker hold(worker);
        visibleBefore = std::chrono::duration<double>(Clock::now() - firstShown).count();
        if (!hold.held()) {
            notVerified(kGoal, "the worker could not be held, so no pause could begin and end between two rounds");
            return;
        }
        manager.setMonitorPause(kMonitorClean, "always_paused");
        std::this_thread::sleep_for(std::chrono::seconds(4));
        manager.setMonitorPause(kMonitorClean, "play");
    } else {
        manager.setMonitorPause(kMonitorClean, "always_paused");
        worker.wake();
        visibleBefore = std::chrono::duration<double>(Clock::now() - firstShown).count();
        std::this_thread::sleep_for(std::chrono::seconds(4));
        manager.setMonitorPause(kMonitorClean, "play");
    }
    const Clock::time_point shownAgain = Clock::now();
    worker.wake();
    const size_t before = signals.countFor(kMonitorClean);
    const bool resumed = settlesWithin(std::chrono::seconds(10), [&] { return signals.countFor(kMonitorClean) >= before + 2; });
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const int afterPause = deathsIn(config.stateDir);
    const double visibleAfter = std::chrono::duration<double>(Clock::now() - shownAgain).count();
    check(resumed, what + ": the video plays again after it");
    if (visibleBefore + visibleAfter >= 3.0) {
        notVerified(kGoal, "the visible stretches took " + std::to_string(visibleBefore + visibleAfter) +
                               " s, not less than cleanPlayAfter, so a clear proves nothing");
        return;
    }
    reportUnless(afterPause == 1, (what + " is not counted as play").c_str(),
                 "the count is " + std::to_string(afterPause) + ", " + collector.describe());
}

}  // namespace

// Clock rounds at the real time until `done` holds of one round's shown screens, or 5 s pass (false).
// Monitors 0..monitors-1 left owing a render are rendered here, as the worker would.
bool clockRoundsUntil(WallpaperManager& manager, int monitors, const std::function<bool(const std::set<int>&)>& done) {
    for (const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
         std::chrono::steady_clock::now() < deadline;) {
        std::set<int> shown;
        const wallpaperx::ClockRound round = manager.advanceVideo(std::chrono::steady_clock::now());
        for (const wallpaperx::ClockRound::Shown& s : round.shown) shown.insert(s.monitorId);
        for (int id = 0; id < monitors; ++id) {
            if (owesRender(manager, id)) manager.renderPending(id);
        }
        if (done(shown)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

int liveCount(const std::string& path) { return static_cast<int>(g_files.live().count(path)); }

std::string watchedCopy(const std::string& from, const char* name) {
    const std::string path = testAssetDir() + "/" + name;
    std::filesystem::copy_file(from, path, std::filesystem::copy_options::overwrite_existing);
    g_files.watch(path);
    return path;
}

// S15.823's render half and S15.824: a shared decoder outlives one screen's failed pass, and is released
// on the session's terms, not one screen's. Clock rounds past the release delays are run at made-up
// times, which is what the release reads; nothing here sleeps them out.
void runSharedReleaseChecks(const WallpaperManager::Config& base, const std::string& clip) {
    const char* kGoals = "S15.823-S15.824 (a shared decoder's render failures and releases)";
    PhaseGrade grade("the shared-release phase");
    WallpaperManager::Config config = base;
    config.unwatchedAfter = std::chrono::hours(1);
    config.idleSessionRelease = std::chrono::seconds(2);
    config.pausedSessionRelease = std::chrono::seconds(6);
    WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};
    const std::string broken = watchedCopy(clip, "shared_release_broken.mp4");
    const std::string released = watchedCopy(clip, "shared_release_released.mp4");
    static constexpr int kKeep = 0, kBroken = 1, kPlays = 2, kHidden = 3, kMonitors = 4;
    for (int id = 0; id < kMonitors; ++id) wallpaperx::testing::nameScreen(manager, id, screenOf(id));
    const auto rounds = [&manager](const std::function<bool(const std::set<int>&)>& done) {
        return clockRoundsUntil(manager, kMonitors, done);
    };
    const auto share = [&](const std::string& path, int a, int b) {
        manager.requestWallpaper("video", path, a);
        const PassCost first = renderCounted(manager, a);
        manager.requestWallpaper("video", path, b);
        const PassCost second = renderCounted(manager, b);
        const bool both = rounds([&](const std::set<int>& s) { return s.count(a) && s.count(b); });
        if (first.outcome.rendered && second.outcome.rendered && first.opens == 1 && second.opens == 0 && both) return true;
        std::fprintf(stderr, "  %s; %s; %s\n", describe(first).c_str(), describe(second).c_str(),
                     both ? "both shown" : "not both shown within 5 s");
        return false;
    };

    manager.requestWallpaper("video", broken, kKeep);
    if (!renderCounted(manager, kKeep).outcome.rendered) {
        grade.expectNoDevice();
        notVerified(kGoals, "the first video render on this host produced no frame");
        return;
    }

    // GOAL: S15.823's render half -- a pass that fails for one screen's own reason (its images failing to
    //       build) takes only that screen out: the shared decoder stays open and the other screen plays on.
    // CONTROL: control_batch.sh 2026-10-18, phase sharedRelease, both entries, reverted: renderPassLocked's
    //       dropFailedClip (backend/daemon/wallpaper_manager.cpp) `if (clipBroken)` made `if (true)` ->
    //       "...leaves the shared decoder open".
    if (!share(broken, kKeep, kBroken)) {
        check(false, "two screens share a decoder before one screen's pass fails");
    } else {
        std::vector<int> owed;
        manager.setOutputSize(screenOf(kBroken), kResizedWidth, kResizedHeight, owed);
        g_framebufferFailGate.arm(CallGate::Mode::Fail, extentOf(kResizedWidth, kResizedHeight));
        const PassCost failed = renderCounted(manager, kBroken);
        check(!failed.outcome.rendered, "the screen whose images fail to build is not drawn (the premise)");
        reportUnless(liveCount(broken) == 1 && failed.closes == 0,
                     "a render failure that is not the decoder's leaves the shared decoder open",
                     describe(failed) + "; open: " + describeFiles(g_files.live()));
        reportUnless(rounds([](const std::set<int>& s) { return s.count(kKeep) > 0; }),
                     "and the other screen goes on being drawn from it", "no picture within 5 s");
    }

    // GOAL: S15.824 -- a shared decoder is not released while any of its screens plays, however long
    //       another has been stopped; with none playing and one paused, the paused delay applies, not the
    //       idle one; and the release records a position for every screen, not only the first.
    // WHY-THIS-SHAPE: the rounds run at made-up times far past both delays, since that time is what the
    //       release compares; the record is read as the seek the hidden screen's reopen makes, because a
    //       screen with no position recorded reopens at the file's start and seeks nothing.
    // CONTROL: control_batch.sh 2026-10-18, phase sharedRelease, both entries, each alone, reverted
    //       (backend/daemon/wallpaper_manager.cpp): advanceSessionLocked's `playing` made
    //       `anyIn(Playing) && !anyIn(Stopped)` -> "...not released while one of its screens plays";
    //       `paused = paused || pausedLocked(...)` made `paused = pausedLocked(...)` -> "...kept past the idle
    //       delay"; releaseVideoSessionLocked() recording only the first member -> "...every screen" (re-run
    //       2026-10-24 on NVIDIA and Lavapipe, alone).
    if (!share(released, kPlays, kHidden)) {
        check(false, "two screens share a decoder before the release checks");
        return;
    }
    manager.setOutputVisible(screenOf(kHidden), false);
    g_files.resetCounts();
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 4; ++i) manager.advanceVideo(t0 + i * std::chrono::seconds(5));
    reportUnless(g_files.closes(released) == 0 && liveCount(released) == 1,
                 "a shared decoder is not released while one of its screens plays, however long another is stopped",
                 std::to_string(g_files.closes(released)) + " close(s)");

    manager.setMonitorPause(kPlays, "always_paused");
    const auto t1 = std::chrono::steady_clock::now();
    manager.advanceVideo(t1);
    manager.advanceVideo(t1 + config.idleSessionRelease + std::chrono::milliseconds(500));
    const int closesPastIdle = g_files.closes(released);
    manager.advanceVideo(t1 + config.pausedSessionRelease + std::chrono::milliseconds(500));
    const int closesPastPaused = g_files.closes(released);
    reportUnless(closesPastIdle == 0,
                 "with one screen paused and none playing, the shared decoder is kept past the idle delay",
                 std::to_string(closesPastIdle) + " close(s)");
    reportUnless(closesPastPaused == 1, "and it is released once the paused delay passes",
                 std::to_string(closesPastPaused) + " close(s)");
    if (closesPastPaused != 1) return;

    g_farRead.arm(0.0, 0, false);
    manager.setOutputVisible(screenOf(kHidden), true);
    const bool reopened = rounds([&](const std::set<int>&) { return liveCount(released) == 1; });
    const int seeks = g_farRead.farSeeks();
    g_farRead.disarm();
    check(reopened, "the screen shown again reopens the released file within 5 s");
    if (reopened) {
        reportUnless(seeks >= 1, "a release records the position of every screen of the shared decoder, not only the first",
                     "the second screen's reopen made no seek, so it had no position recorded");
    }
}

// S15.825: under the session ceiling, a shared session one of whose screens still shows its current
// picture is never the one closed for room, whatever its other screens do.
void runSharedEvictionChecks(const WallpaperManager::Config& base, const std::vector<std::string>& clips) {
    const char* kGoals = "S15.825 (eviction of a shared decoder)";
    PhaseGrade grade("the shared-eviction phase");
    WallpaperManager::Config config = base;
    config.unwatchedAfter = std::chrono::hours(1);
    WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};
    static_assert(WallpaperManager::kMaxVideoSessions == 8, "the monitor plan below fills exactly eight sessions");
    constexpr int kHiddenFirst = 0, kShowing = 1, kNinth = 9;
    for (int id = 0; id <= kNinth; ++id) wallpaperx::testing::nameScreen(manager, id, screenOf(id));

    manager.requestWallpaper("video", clips[0], kHiddenFirst);
    const PassCost first = renderCounted(manager, kHiddenFirst);
    if (!first.outcome.rendered) {
        grade.expectNoDevice();
        notVerified(kGoals, "the first video render on this host produced no frame");
        return;
    }
    manager.requestWallpaper("video", clips[0], kShowing);
    const PassCost joined = renderCounted(manager, kShowing);
    if (!joined.outcome.rendered || joined.opens != 0) {
        check(false, ("a second screen joins the first file's decoder (the premise) -- " + describe(joined)).c_str());
        return;
    }
    // GOAL: S13.126 restated -- the ceiling counts files, not screens: nine screens on eight files all
    //       open and draw, closing nothing.
    // CONTROL: control_batch.sh 2026-10-18, phase sharedEviction, both entries, reverted: videoSessionLocked()'s
    //       ceiling test (backend/daemon/wallpaper_manager.cpp) adding the first session's extra screens to
    //       m_videoSessions.size() -> this line.
    int opened = 0;
    int closed = 0;
    for (int i = 1; i <= 7; ++i) {
        manager.requestWallpaper("video", clips[static_cast<size_t>(i)], i + 1);
        const PassCost cost = renderCounted(manager, i + 1);
        if (cost.outcome.rendered && cost.opens == 1) ++opened;
        closed += cost.closes;
    }
    reportUnless(opened == 7 && closed == 0 && g_files.live().size() == 8,
                 "nine screens on eight files all draw under the eight-session ceiling, closing nothing",
                 std::to_string(opened) + " of 7 opened, " + std::to_string(closed) + " closed; open: " +
                     describeFiles(g_files.live()));
    if (opened != 7 || g_files.live().size() != 8) return;

    // GOAL: S15.825 -- with the oldest session shared by a hidden screen and one still showing its
    //       picture, and a younger session whose screen owes a render, room for a ninth file is made by
    //       closing the younger one.
    // WHY-THIS-SHAPE: the shared session is the least recently used and its FIRST screen is the hidden
    //       one, so a daemon that judged a session by one screen would pick it.
    // CONTROL: control_batch.sh 2026-10-18, phase sharedEviction, both entries, reverted:
    //       evictIdleVideoSessionLocked()'s any_of (backend/daemon/wallpaper_manager.cpp) over the first
    //       member only -> "...not the shared one with a screen still showing".
    manager.setOutputVisible(screenOf(kHiddenFirst), false);
    std::vector<int> owed;
    manager.setOutputSize(screenOf(2), kResizedWidth, kResizedHeight, owed);
    manager.requestWallpaper("video", clips[8], kNinth);
    const PassCost ninth = renderCounted(manager, kNinth);
    reportUnless(ninth.outcome.rendered && liveCount(clips[0]) == 1 && liveCount(clips[1]) == 0,
                 "room for a ninth file closes the session with no screen showing, not the shared one with a screen "
                 "still showing",
                 describe(ninth) + "; open: " + describeFiles(g_files.live()));
}

// S15.860-S15.868: a paused screen on a decoder another screen plays from, read by the counter
// fixture's frame numbers. Every pass and clock round runs on this thread with no worker.
void runPauseRejoinChecks(const WallpaperManager::Config& base) {
    using wallpaperx::PositionMode;
    const char* kGoals = "S15.860-S15.868 (a paused screen on a shared decoder)";
    const std::string counterClip = counterClipPath();
    if (!std::filesystem::exists(counterClip)) {
        notVerified(kGoals, "the counter fixture " + counterClip + " was not built");
        return;
    }
    const std::optional<CounterOrder> order = counterOrder(counterClip, kGoals);
    if (!order) return;
    g_files.watch(counterClip);
    PhaseGrade grade("the pause-rejoin phase");
    WallpaperManager::Config config = base;
    config.unwatchedAfter = std::chrono::hours(1);
    config.idleSessionRelease = std::chrono::seconds(2);
    config.pausedSessionRelease = std::chrono::seconds(6);
    WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};
    static constexpr int kPlays = 0, kPaused = 1, kMonitors = 2;
    std::vector<int> owed;
    for (int id = 0; id < kMonitors; ++id) {
        wallpaperx::testing::nameScreen(manager, id, screenOf(id));
        manager.setOutputSize(screenOf(id), 160, 120, owed);
    }
    std::string reason;
    const auto counterOn = [&](int id) { return servedCounter(manager, id, reason); };
    const auto text = [](std::optional<int> n) { return n ? std::to_string(*n) : std::string("unreadable"); };
    // `a` is `b`'s picture or the one after it: a redraw takes the picture the decoder holds, which
    // while it waits to be shown is the playing screen's next.
    const auto withinOne = [&](std::optional<int> a, std::optional<int> b) {
        return a && b && (*a == *b || order->after(*b) == *a);
    };

    // kPlays' frame number after every round that shows it; each must be the picture after the last.
    std::optional<int> lastPlays;
    int compared = 0;
    std::string jumps;
    const auto takeJumps = [&] {
        std::string j = jumps.empty() ? "none" : jumps;
        jumps.clear();
        return j;
    };
    const auto rounds = [&](const std::function<bool(const std::set<int>&)>& done) {
        return clockRoundsUntil(manager, kMonitors, [&](const std::set<int>& shown) {
            if (shown.count(kPlays)) {
                const std::optional<int> now = counterOn(kPlays);
                if (lastPlays && now) {
                    ++compared;
                    if (order->after(*lastPlays) != *now) {
                        jumps += (jumps.empty() ? "" : ", ") + std::to_string(*lastPlays) + " -> " + std::to_string(*now);
                    }
                }
                lastPlays = now;
            }
            return done(shown);
        });
    };
    const auto showsOf = [&](int id, int count) {
        int seen = 0;
        return rounds([&, id, count](const std::set<int>& s) { return (seen += static_cast<int>(s.count(id))) >= count; });
    };

    manager.requestWallpaper("video", counterClip, kPlays, {{"position", PositionMode::Stretch}});
    if (!renderCounted(manager, kPlays).outcome.rendered) {
        grade.expectNoDevice();
        notVerified(kGoals, "the first video render on this host produced no frame");
        return;
    }
    manager.requestWallpaper("video", counterClip, kPaused, {{"position", PositionMode::Stretch}});
    const PassCost joined = renderCounted(manager, kPaused);
    int bothShown = 0;
    const bool together = rounds([&](const std::set<int>& s) { return (bothShown += s.count(kPlays) && s.count(kPaused)) >= 3; });
    if (!joined.outcome.rendered || joined.opens != 0 || !together || !lastPlays) {
        check(false, "two screens on the counter file share its decoder and are drawn together (the premise)");
        std::fprintf(stderr, "  %s; %s\n", describe(joined).c_str(), together ? "drawn together" : "not drawn together within 5 s");
        return;
    }

    // GOAL: S15.860 -- a paused screen sharing a decoder keeps its picture while the other screen plays on.
    // CONTROL: control_batch.sh 2026-10-18, phase pauseRejoin, both entries, reverted: motionLocked()
    //       (backend/daemon/wallpaper_manager.cpp) not counting a pause as stopped -> this line.
    manager.setMonitorPause(kPaused, "always_paused");
    const std::optional<int> frozen = counterOn(kPaused);
    bool pausedDrawn = false;
    int playsShown = 0;
    const bool ranOn = rounds([&](const std::set<int>& s) {
        pausedDrawn = pausedDrawn || s.count(kPaused) > 0;
        return (playsShown += static_cast<int>(s.count(kPlays))) >= 10;
    });
    const std::optional<int> held = counterOn(kPaused);
    if (!ranOn || !frozen) {
        check(false, "the other screen shows ten pictures while one is paused, and the paused one is read (the premise)");
        return;
    }
    reportUnless(held == frozen && !pausedDrawn, "a paused screen on a shared decoder keeps its picture while the other plays on",
                 "paused on " + text(frozen) + ", read " + text(held) + ", the other at " + text(lastPlays) +
                     (pausedDrawn ? "; the clock drew the paused screen" : ""));

    // GOAL: S15.862-S15.864 -- a paused screen resized or laid out again beside a playing one shows the
    //       shared picture (the playing screen's, or the one after it), opens no decoder, and the playing
    //       screen goes on showing every picture in turn.
    // WHY-THIS-SHAPE: "keeps its rate" is read as no picture skipped on the playing screen, by frame number
    //       after every round that shows it; a count of pictures a second would grade the scheduler.
    // CONTROL: control_batch.sh 2026-10-18, phase pauseRejoin, both entries, each alone, reverted
    //       (backend/daemon/wallpaper_manager.cpp): both halves of the rule undone -- renderPassLocked's
    //       `if (!othersPlay)` made `if (true)` and videoSessionLocked()'s join refused to any screen with a
    //       picture to keep -> "...shows the playing screen's picture" (3 beside 13) and "...opens no decoder"
    //       (1 open per change); either half alone stays green here, the other half covers it. Each alone has
    //       its own check at the end of this phase: S15.866 (the join) and the speed-change redraw (the stay). Two extra decodes
    //       of the shared pipeline where the paused screen stays -> "...shows every picture in turn".
    // COVERAGE-ASSERT: the playing screen's 24 pictures after the redraws (six each) are all compared.
    struct Change {
        const char* what;
        std::function<void()> apply;
    };
    const Change changes[] = {
        {"resized", [&] { manager.setOutputSize(screenOf(kPaused), 200, 150, owed); }},
        {"resized back", [&] { manager.setOutputSize(screenOf(kPaused), 160, 120, owed); }},
        {"laid out freely", [&] { manager.setWallpaperProperties(kPaused, {{"position", PositionMode::Free}}); }},
        {"stretched again", [&] { manager.setWallpaperProperties(kPaused, {{"position", PositionMode::Stretch}}); }},
    };
    int redrawn = 0;
    std::string pictures, opened;
    bool allShared = true, noneOpened = true;
    compared = 0;
    takeJumps();
    for (const Change& c : changes) {
        c.apply();
        const bool wasOwed = owesRender(manager, kPaused);
        const int opensBefore = g_files.opens(counterClip);
        const bool drawn = manager.renderPending(kPaused).rendered;
        const int opens = g_files.opens(counterClip) - opensBefore;
        const std::optional<int> paused = counterOn(kPaused);
        const std::optional<int> plays = counterOn(kPlays);
        if (wasOwed && drawn) ++redrawn;
        allShared = allShared && withinOne(paused, plays);
        noneOpened = noneOpened && opens == 0 && liveCount(counterClip) == 1;
        pictures += std::string(pictures.empty() ? "" : "; ") + c.what + ": " + text(paused) + " beside " + text(plays);
        opened += std::string(opened.empty() ? "" : ", ") + std::to_string(opens);
        if (!showsOf(kPlays, 6)) {
            check(false, "the playing screen goes on being drawn after the paused one is redrawn (within 5 s)");
            return;
        }
    }
    check(redrawn == 4, "every change to the paused screen is owed and drawn");
    reportUnless(allShared, "a paused screen redrawn beside a playing one shows the playing screen's picture", pictures);
    reportUnless(noneOpened, "a paused screen redrawn beside a playing one opens no decoder",
                 "opens per change: " + opened + "; open: " + describeFiles(g_files.live()));
    const bool inTurn = jumps.empty();
    reportUnless(compared >= 24 && inTurn,
                 "the playing screen shows every picture in turn while the paused one is redrawn beside it",
                 std::to_string(compared) + " compared; skipped: " + takeJumps());
    std::printf("paused screen redrawn beside a playing one: %s\n", pictures.c_str());

    // GOAL: S15.861 -- on resume the paused screen rejoins where the other is: the first round that draws
    //       both draws the same picture on both, with no decoder opened.
    // CONTROL: control_batch.sh 2026-10-18, phase pauseRejoin, both entries, reverted
    //       (backend/daemon/wallpaper_manager.cpp): advanceSessionLocked's split sending a paused member away
    //       keeping its picture, and videoSessionLocked() never joining a screen with a recorded position ->
    //       this line, "38 beside 38; 1 open(s)". The first alone stays green: the resume joins back.
    g_files.resetCounts();
    manager.setMonitorPause(kPaused, "play");
    std::optional<int> rejoinedAt;
    const bool rejoined = rounds([&](const std::set<int>& s) {
        if (s.count(kPlays) && s.count(kPaused)) rejoinedAt = counterOn(kPaused);
        return rejoinedAt.has_value();
    });
    reportUnless(rejoined && rejoinedAt == lastPlays && g_files.opens(counterClip) == 0,
                 "a paused screen resumed beside a playing one rejoins at the other's picture",
                 (rejoined ? "drawn together: " + text(rejoinedAt) + " beside " + text(lastPlays) : std::string("not drawn together within 5 s")) +
                     "; " + std::to_string(g_files.opens(counterClip)) + " open(s)");

    // GOAL: S15.867 -- a screen split off by its speed and folded back when the speeds match jumps to the
    //       other screen's picture, and the other screen's playback goes on without a jump.
    // WHY-THIS-SHAPE: the split screen runs at twice the speed long enough to be well ahead, so a fold
    //       into its session would show as a jump on the other screen.
    // CONTROL: control_batch.sh 2026-10-18, phase pauseRejoin, both entries, reverted: mergeSharedSessionsLocked()'s
    //       `keepFirst` (backend/daemon/wallpaper_manager.cpp) given `<` for `>=` -> "...without a jump".
    manager.setWallpaperProperties(kPaused, {{"speed", 2.0}});
    const bool split = rounds([&](const std::set<int>&) { return liveCount(counterClip) == 2; }) && showsOf(kPaused, 20);
    const std::optional<int> ahead = counterOn(kPaused);
    const std::optional<int> behind = lastPlays;
    if (!split || !ahead || ahead == behind) {
        check(false, "a screen set to twice the speed splits off and runs ahead of the other (the premise)");
        std::fprintf(stderr, "  %s; %s against %s\n", split ? "split" : "not split within 5 s", text(ahead).c_str(),
                     text(behind).c_str());
        return;
    }
    takeJumps();
    manager.setWallpaperProperties(kPaused, {{"speed", 1.0}});
    std::optional<int> foldedAt;
    const bool folded = rounds([&](const std::set<int>& s) {
        if (liveCount(counterClip) == 1 && s.count(kPlays) && s.count(kPaused)) foldedAt = counterOn(kPaused);
        return foldedAt.has_value();
    });
    reportUnless(folded && foldedAt == lastPlays, "a screen folded back after a speed change jumps to the other screen's picture",
                 "ran ahead at " + text(ahead) + " against " + text(behind) + "; " +
                     (folded ? "then " + text(foldedAt) + " beside " + text(lastPlays) : std::string("not folded within 5 s")));
    std::printf("speed split and fold: ran ahead at %s against %s, folded at %s beside %s; the other's jumps: %s\n",
                text(ahead).c_str(), text(behind).c_str(), text(foldedAt).c_str(), text(lastPlays).c_str(), jumps.c_str());
    const bool noJump = jumps.empty();
    reportUnless(folded && noJump, "and the other screen plays on through the fold without a jump",
                 "skipped: " + takeJumps());

    // GOAL: S15.865 -- with every screen on the file paused, a redraw keeps the screen's own picture.
    // CONTROL: control_batch.sh 2026-10-18, phase pauseRejoin, both entries, reverted: renderPassLocked's
    //       `if (!othersPlay)` (backend/daemon/wallpaper_manager.cpp) made `if (false)` -> this line.
    manager.setMonitorPause(kPaused, "always_paused");
    const std::optional<int> ownPicture = counterOn(kPaused);
    const bool movedOn = showsOf(kPlays, 10);
    manager.setMonitorPause(kPlays, "always_paused");
    const std::optional<int> otherPicture = counterOn(kPlays);
    lastPlays.reset();
    if (!movedOn || !ownPicture || !otherPicture || ownPicture == otherPicture) {
        check(false, "the other screen plays ten pictures on and is paused too (the premise)");
        return;
    }
    manager.setOutputSize(screenOf(kPaused), 200, 150, owed);
    const bool allPausedDrawn = owesRender(manager, kPaused) && manager.renderPending(kPaused).rendered;
    const std::optional<int> kept = counterOn(kPaused);
    reportUnless(allPausedDrawn && kept == ownPicture,
                 "with every screen on the file paused, a redraw keeps the screen's own picture",
                 "own " + text(ownPicture) + ", the other on " + text(otherPicture) + ", read " + text(kept) +
                     (allPausedDrawn ? "" : "; not drawn"));

    // GOAL: S15.868 -- a release records each screen's own picture: the screen resumed first reopens on
    //       its own picture, not the other screen's. S15.866 -- the other, still paused and released, joins
    //       the resumed screen's decoder when it is redrawn: no decoder opened, the resumed one's picture.
    // WHY-THIS-SHAPE: the S15.865 redraw above leaves kPaused in a session of its own, and a release of two
    //       one-screen sessions cannot mix their pictures; so kPlays plays once more, which folds kPaused back
    //       in behind it, and pauses ten pictures on. kPaused is then not the session's first, so a release
    //       that recorded the first screen's picture for all would reopen it ten pictures off.
    // CONTROL: control_batch.sh 2026-10-24, phase pauseRejoin, reverted (backend/daemon/wallpaper_manager.cpp):
    //       releaseVideoSessionLocked() recording the first member's picture for every member -> "...reopens
    //       on its own picture", "own 49, the other's 69; reopened on 69", alone, NVIDIA and Lavapipe (green
    //       before the fold was added); never recording `releasedAt` -> the same line, "reopened on 0";
    //       videoSessionLocked()'s join exception reduced to `keepsPicture && startAt` -> "...joins the resumed
    //       screen's decoder", with the later speed-change premise, NVIDIA and Lavapipe.
    // COVERAGE-ASSERT: the fold premise reads one decoder open, kPaused still on its own picture, and kPlays
    //       on another, so the release is of one shared session holding two different pictures.
    manager.setMonitorPause(kPlays, "play");
    const bool refolded = showsOf(kPlays, 10) && liveCount(counterClip) == 1;
    manager.setMonitorPause(kPlays, "always_paused");
    const std::optional<int> sharedOther = counterOn(kPlays);
    const std::optional<int> pausedStill = counterOn(kPaused);
    if (!refolded || !sharedOther || pausedStill != ownPicture || withinOne(sharedOther, ownPicture)) {
        check(false, "kPlays playing again folds kPaused back into its decoder, and both pause on different pictures "
                     "(the premise)");
        std::fprintf(stderr, "  %s; kPaused on %s (own %s), kPlays on %s\n",
                     refolded ? "folded" : "not folded within 5 s", text(pausedStill).c_str(), text(ownPicture).c_str(),
                     text(sharedOther).c_str());
        return;
    }
    const auto t1 = std::chrono::steady_clock::now();
    manager.advanceVideo(t1);
    manager.advanceVideo(t1 + config.pausedSessionRelease + std::chrono::milliseconds(500));
    if (liveCount(counterClip) != 0) {
        check(false, "both paused screens' decoder is released once the paused delay passes (the premise)");
        return;
    }
    g_files.resetCounts();
    manager.setMonitorPause(kPaused, "play");
    // The first picture drawn after the resume, by a render pass or a clock round, whichever comes first.
    std::optional<int> reopenedAt;
    const bool reopenDrawn = clockRoundsUntil(manager, 0, [&](const std::set<int>& shown) {
        if (owesRender(manager, kPaused) && manager.renderPending(kPaused).rendered) {
            reopenedAt = counterOn(kPaused);
            return true;
        }
        if (shown.count(kPaused)) reopenedAt = counterOn(kPaused);
        return shown.count(kPaused) > 0;
    });
    reportUnless(reopenedAt && withinOne(reopenedAt, ownPicture) && g_files.opens(counterClip) == 1,
                 "a screen resumed after its shared decoder's release reopens on its own picture",
                 "own " + text(ownPicture) + ", the other's " + text(sharedOther) + "; reopened on " +
                     (reopenedAt ? text(reopenedAt) : reopenDrawn ? "nothing readable (" + reason + ")" : std::string("nothing within 5 s")) +
                     ", " + std::to_string(g_files.opens(counterClip)) + " open(s)");
    if (!reopenedAt || !showsOf(kPaused, 3)) {
        check(false, "the resumed screen plays on from its reopened decoder (the premise)");
        return;
    }
    const int opensBefore = g_files.opens(counterClip);
    manager.setOutputSize(screenOf(kPlays), 200, 150, owed);
    const bool joinDrawn = owesRender(manager, kPlays) && manager.renderPending(kPlays).rendered;
    const int joinOpens = g_files.opens(counterClip) - opensBefore;
    const std::optional<int> joinedAt = counterOn(kPlays);
    const std::optional<int> resumedAt = counterOn(kPaused);
    reportUnless(joinDrawn && joinOpens == 0 && liveCount(counterClip) == 1 && withinOne(joinedAt, resumedAt),
                 "a paused screen whose decoder was released joins the resumed screen's decoder when redrawn",
                 std::string(joinDrawn ? "drawn" : "not drawn") + ", " + std::to_string(joinOpens) + " open(s), showing " +
                     text(joinedAt) + " beside " + text(resumedAt) + "; open: " + describeFiles(g_files.live()));

    // Every render the manager says is owed, for both screens, as the worker would post and run it.
    std::string owedRun;
    const auto runOwed = [&] {
        for (int id = 0; id < kMonitors; ++id) {
            if (!owesRender(manager, id)) continue;
            manager.renderPending(id);
            owedRun += (owedRun.empty() ? "" : ", ") + std::to_string(id);
        }
    };
    if (!joinDrawn || joinOpens != 0 || !showsOf(kPaused, 3)) {
        check(false, "kPlays, paused, shares the playing screen's decoder before the speed-change cases (the premise)");
        return;
    }

    // GOAL: S15.880 -- a paused screen sharing a decoder whose speed is changed opens no decoder over the
    //       following clock rounds while it stays paused, and keeps its picture.
    // WHY-THIS-SHAPE: driven as the daemon runs: after the change and after every round, every render the
    //       manager reports owed for either screen is run, so a redraw the change or the split posts is not
    //       skipped by a test that renders only the playing screen. Ten rounds showing the playing screen. The
    //       layout is passed again with the speed: properties are replaced whole, and a dropped layout owes a
    //       redraw by design (which takes the playing screen's picture, S15.862).
    // CONTROL: control_batch.sh 2026-10-18, phase pauseRejoin, both entries, reverted: reopenVideosLocked()
    //       (backend/daemon/wallpaper_manager.cpp) owing a render to every video screen with no session
    //       (`bool owed = playingSeqLocked(...).has_value();` made `bool owed = true;`) -> this line.
    {
        const std::optional<int> pausedOn = counterOn(kPlays);
        g_files.resetCounts();
        owedRun.clear();
        manager.setWallpaperProperties(kPlays, {{"position", PositionMode::Stretch}, {"speed", 2.0}});
        runOwed();
        int playsShown = 0;
        const bool ranOn = rounds([&](const std::set<int>& s) {
            runOwed();
            return (playsShown += static_cast<int>(s.count(kPaused))) >= 10;
        });
        const std::optional<int> after = counterOn(kPlays);
        if (!ranOn || !pausedOn) {
            check(false, "the playing screen shows ten pictures after the paused one's speed change (the premise)");
            return;
        }
        reportUnless(g_files.opens(counterClip) == 0 && after == pausedOn,
                     "a paused screen whose shared decoder's speed is changed opens no decoder and keeps its picture",
                     std::to_string(g_files.opens(counterClip)) + " open(s); paused on " + text(pausedOn) + ", read " +
                         text(after) + "; renders run for: " + (owedRun.empty() ? "none" : owedRun));
    }

    // GOAL: S15.862 under a pending speed change -- a paused screen redrawn beside a playing one, after its
    //       speed was changed and before any clock round split it off, opens no decoder.
    // WHY-THIS-SHAPE: the one case where renderPassLocked()'s "stay in the session" branch is alone: leaving and
    //       rejoining (the join exception for a screen with a picture to keep) cannot rejoin a session at
    //       another speed, so only staying avoids the open. S15.862's own redraws are at one speed, where the
    //       rejoin covers a broken stay; S15.866's line above is the join exception's own.
    // CONTROL: control_batch.sh 2026-10-18, phase pauseRejoin, both entries, reverted: renderPassLocked()'s
    //       `if (!othersPlay)` (backend/daemon/wallpaper_manager.cpp) made `if (true)`, alone -> this line.
    {
        manager.setWallpaperProperties(kPlays, {{"position", PositionMode::Stretch}, {"speed", 1.0}});
        manager.setOutputSize(screenOf(kPlays), 160, 120, owed);
        const bool rejoinedDrawn = owesRender(manager, kPlays) && manager.renderPending(kPlays).rendered;
        if (!rejoinedDrawn || liveCount(counterClip) != 1 || !showsOf(kPaused, 3)) {
            check(false, "the paused screen at speed 1 rejoins the playing one's decoder when redrawn (the premise)");
            return;
        }
        const int before = g_files.opens(counterClip);
        manager.setWallpaperProperties(kPlays, {{"position", PositionMode::Stretch}, {"speed", 2.0}});
        manager.setOutputSize(screenOf(kPlays), 200, 150, owed);
        const bool drawn = owesRender(manager, kPlays) && manager.renderPending(kPlays).rendered;
        const int opens = g_files.opens(counterClip) - before;
        reportUnless(drawn && opens == 0,
                     "a paused screen redrawn beside a playing one after a speed change opens no decoder",
                     std::string(drawn ? "drawn" : "not drawn") + ", " + std::to_string(opens) + " open(s); open: " +
                         describeFiles(g_files.live()));
    }
}

// Counts onProcessorDecodingChanged() calls, so a wait for a scan's answer wakes on the notice.
class ScanNotices {
public:
    void attach(WallpaperManager& manager) {
        manager.onProcessorDecodingChanged([this] {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                ++m_count;
            }
            m_wake.notify_all();
        });
    }
    // Returns once monitorId reports "parts". The notice is the wake-up; the 200 ms re-read is a stated
    // fallback so a missing notice cannot stall this.
    // WHY-THIS-SHAPE: no deadline of its own. The scan runs at nice 19 in the idle I/O class, so any fixed
    //       bound grades host load; an answer that never comes hangs the phase into its ctest TIMEOUT, which
    //       is red, and the line printed every 10 s names what it was waiting for.
    // CONTROL: test-side, by hand, 2026-10-24 (T19a), restored: the first wait aimed at monitor 99, which has
    //       no wallpaper, run under `ctest --timeout 45` -> "processorScan: still waiting for monitor 99 to report
    //       parts (a first scan) after 10 s" (and 20, 30, 40 s), then "***Timeout", Failed.
    void waitParts(const WallpaperManager& manager, int monitorId, const char* what) {
        const auto start = std::chrono::steady_clock::now();
        auto nextNote = start + std::chrono::seconds(10);
        for (;;) {
            uint64_t seen = 0;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                seen = m_count;
            }
            const std::optional<wallpaperx::MonitorStatus> status = monitorStatus(manager, monitorId);
            if (status && status->processorDecoding &&
                status->processorDecoding->state == wallpaperx::ProcessorDecoding::Parts) {
                return;
            }
            if (const auto now = std::chrono::steady_clock::now(); now >= nextNote) {
                std::fprintf(stderr, "processorScan: still waiting for monitor %d to report parts (%s) after %lld s\n",
                             monitorId, what,
                             static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(now - start).count()));
                nextNote = now + std::chrono::seconds(10);
            }
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait_for(lock, std::chrono::milliseconds(200), [&] { return m_count != seen; });
        }
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_wake;
    uint64_t m_count = 0;
};

// S15.954 in the manager: a scan answer is reused, not read again, on a re-apply, for another screen and after a
// restart, and a non-video wallpaper withdraws the screen's queued scan. Every file is a copy of counter_gap.mp4,
// whose scan answer alone ("missing reference") makes the screen report "parts", so no render is needed.
void runProcessorScanChecks(const WallpaperManager::Config& base, const std::string& still) {
    const char* kGoals = "S15.954 (scan answers are reused for an unchanged file without reading it)";
    const std::string gapFixture =
        (std::filesystem::path(WALLPAPERX_COUNTER_VIDEO).parent_path() / "counter_gap.mp4").string();
    std::error_code ec;
    if (!std::filesystem::exists(gapFixture, ec)) {
        notVerified(kGoals, "the fixture " + gapFixture + " was not generated");
        return;
    }
    const wallpaperx::testing::ScratchDir scratch("processorscan");
    if (!scratch.ok()) {
        notVerified(kGoals, "no scratch directory could be created for the state and config");
        return;
    }
    const auto copyOf = [&](const char* name) {
        const std::string path = (scratch.path() / name).string();
        std::filesystem::copy_file(gapFixture, path, std::filesystem::copy_options::overwrite_existing);
        g_files.watch(path);
        return path;
    };
    const std::string shown = copyOf("scan_shown.mp4");
    WallpaperManager::Config config = base;
    config.stateDir = scratch.path() / "state";
    config.configFile = scratch.path() / "config.json";
    std::filesystem::create_directories(config.stateDir, ec);
    constexpr int kScreen = 0, kOther = 1, kHeld = 2, kQueued = 3, kSentinel = 4;
    const auto parts = [](ScanNotices& notices, const WallpaperManager& manager, int id, const char* what) {
        notices.waitParts(manager, id, what);
    };

    {
        ScanNotices notices;
        WallpaperManager first(config);
        notices.attach(first);
        const int before = g_files.scanOpens();
        first.requestWallpaper("video", shown, kScreen);
        parts(notices, first, kScreen, "a first scan");
        // COVERAGE-ASSERT: the scan's opens are counted, so every "0 reads" below can fail. Test-side control, by hand,
        //       2026-10-24 (T18c), restored: FileLedger::opened()'s `++m_scanOpens` removed -> this NOT VERIFIED,
        //       "0 scan open(s)", ctest Skipped.
        if (g_files.scanOpens() - before != 1) {
            notVerified(kGoals, "a first scan was not seen reading its file (" +
                                    std::to_string(g_files.scanOpens() - before) + " scan open(s)); the reads below "
                                    "cannot be counted");
            return;
        }
    }

    {
        ScanNotices notices;
        WallpaperManager second(config);
        notices.attach(second);
        const int before = g_files.scanOpens();
        // GOAL: S15.954 -- after a restart the answer comes from the saved record: the unchanged file is not read.
        // WHY-THIS-SHAPE: a fresh manager, so no in-memory answer or per-path cache can stand in for the record;
        //       counted after the screen reports "parts", which only the finished scan job sets.
        // CONTROL: control_batch.sh 2026-10-24 (T18c), --phase '^processorScan$', reverted: startRestoredScans()
        //       (backend/daemon/wallpaper_manager.cpp) handing every video std::nullopt instead of its record entry
        //       -> "FAIL: S15.954 after a restart the scan answer comes from the saved record, not a read".
        //       heldScanLocked() never returning an answer (nothing is saved) -> the same line, and the two below.
        //       Both re-run 2026-10-24 (G5) after startRestoredScans() and heldScanLocked() changed: the same lines.
        const std::vector<int> restored = second.restoreSaved();
        second.startRestoredScans(restored);
        parts(notices, second, kScreen, "a restored screen");
        const int reads = g_files.scanOpens() - before;
        reportUnless(reads == 0, "S15.954 after a restart the scan answer comes from the saved record, not a read",
                     std::to_string(reads) + " read(s)");


        // GOAL: S15.954 -- re-applying the same unchanged video reuses its answer (knownScanLocked) instead of
        //       reading the file again.
        // WHY-THIS-SHAPE: right after a restore, whose answer came from the record and so is not in the scan
        //       thread's per-path cache of files it read: only the request's own reuse can save the read here.
        // CONTROL: control_batch.sh 2026-10-24 (T18c), --phase '^processorScan$', reverted: knownScanLocked()
        //       (backend/daemon/wallpaper_manager.cpp) never returning an answer -> "FAIL: S15.954 re-applying the
        //       same unchanged video reuses its scan answer, no read", and the other-screen line below. Re-run
        //       2026-10-24 (G5) on knownScanLocked(path, now): the same two lines.
        const int beforeReapply = g_files.scanOpens();
        second.requestWallpaper("video", shown, kScreen);
        parts(notices, second, kScreen, "a re-applied screen");
        const int reapplyReads = g_files.scanOpens() - beforeReapply;
        reportUnless(reapplyReads == 0, "S15.954 re-applying the same unchanged video reuses its scan answer, no read",
                     std::to_string(reapplyReads) + " read(s)");

    }

    {
        ScanNotices notices;
        WallpaperManager third(config);
        notices.attach(third);
        const std::vector<int> restored = third.restoreSaved();
        third.startRestoredScans(restored);
        parts(notices, third, kScreen, "a restored screen, third start");
        // GOAL: S15.954 -- another screen given the same unchanged file reuses the answer a screen holds
        //       (heldScanLocked through knownScanLocked) instead of reading it.
        // WHY-THIS-SHAPE: again right after a restore, for the cache reason above.
        // CONTROL: control_batch.sh 2026-10-24 (T18c), --phase '^processorScan$', reverted: knownScanLocked()'s
        //       `state.path == path` (backend/daemon/wallpaper_manager.cpp) made `!=` -> "FAIL: S15.954 another screen
        //       on the same unchanged file reuses the answer, no read" (and the re-apply line above). Re-run
        //       2026-10-24 (G5) on the foundFor(now) form: the same two lines.
        const int before = g_files.scanOpens();
        third.requestWallpaper("video", shown, kOther);
        parts(notices, third, kOther, "a second screen");
        const int reads = g_files.scanOpens() - before;
        reportUnless(reads == 0, "S15.954 another screen on the same unchanged file reuses the answer, no read",
                     std::to_string(reads) + " read(s)");


        // GOAL: S15.954 -- a non-video wallpaper given to a screen withdraws its scan: a queued scan of its old
        //       video never reads the file.
        // WHY-THIS-SHAPE: the scan thread is held inside its open() of another file (g_scanOpenGate), so the
        //       withdrawn scan is provably still queued when the still is applied; a sentinel scan queued after
        //       it is the barrier, since the one scan thread works its queue in order.
        // CONTROL: control_batch.sh 2026-10-24 (T18c), --phase '^processorScan$', reverted: requestWallpaper()'s
        //       `m_processorCheck.forget(monitorId);` (backend/daemon/wallpaper_manager.cpp) deleted -> "FAIL:
        //       S15.954 a non-video wallpaper withdraws the screen's queued scan: it never reads". Re-run
        //       2026-10-24 (G5) after requestWallpaper() changed: the same line.
        const std::string held = copyOf("scan_held.mp4");
        const std::string queued = copyOf("scan_queued.mp4");
        const std::string sentinel = copyOf("scan_sentinel.mp4");
        const int beforeCancel = g_files.scanOpens();
        g_scanOpenGate.arm(held);
        third.requestWallpaper("video", held, kHeld);
        if (!g_scanOpenGate.waitHeld(std::chrono::seconds(10))) {
            g_scanOpenGate.release();
            notVerified("S15.954 (a non-video wallpaper withdraws the screen's scan)",
                        "the scan thread never reached its open of the held file");
            return;
        }
        third.requestWallpaper("video", queued, kQueued);
        third.requestWallpaper("static", still, kQueued);
        third.requestWallpaper("video", sentinel, kSentinel);
        g_scanOpenGate.release();
        if (g_scanOpenGate.expired()) {
            notVerified("S15.954 (a non-video wallpaper withdraws the screen's scan)", "the held open timed out");
            return;
        }
        parts(notices, third, kSentinel, "the sentinel scan");
        const int cancelReads = g_files.scanOpens() - beforeCancel;
        reportUnless(cancelReads == 2, "S15.954 a non-video wallpaper withdraws the screen's queued scan: it never reads",
                     std::to_string(cancelReads) + " read(s), 2 due (the held and the sentinel file)");

    }
}

// S15.956: two screens on one file whose bytes were replaced. Every file is a copy of counter_gap.mp4, so a scan answer
// alone makes a screen report "parts", and the replacement has a new identity (a new inode) with the same content.
void runSharedScanChecks(const WallpaperManager::Config& base) {
    const char* kGoal = "S15.956 (the scan answer for the file as it is now wins)";
    const std::string gapFixture =
        (std::filesystem::path(WALLPAPERX_COUNTER_VIDEO).parent_path() / "counter_gap.mp4").string();
    std::error_code ec;
    if (!std::filesystem::exists(gapFixture, ec)) {
        notVerified(kGoal, "the fixture " + gapFixture + " was not generated");
        return;
    }
    const wallpaperx::testing::ScratchDir scratch("sharedscan");
    if (!scratch.ok()) {
        notVerified(kGoal, "no scratch directory could be created for the state and config");
        return;
    }
    const std::string path = (scratch.path() / "shared.mp4").string();
    std::filesystem::copy_file(gapFixture, path, ec);
    g_files.watch(path);
    WallpaperManager::Config config = base;
    config.stateDir = scratch.path() / "state";
    config.configFile = scratch.path() / "config.json";
    std::filesystem::create_directories(config.stateDir, ec);
    // The stale answer sits on the lower id, which every per-path walk visits first.
    constexpr int kStale = 0, kFresh = 1;

    {
        ScanNotices notices;
        WallpaperManager first(config);
        notices.attach(first);
        first.requestWallpaper("video", path, kStale);
        notices.waitParts(first, kStale, "the first screen's scan");
        first.requestWallpaper("video", path, kFresh);
        notices.waitParts(first, kFresh, "the second screen");

        const std::string next = path + ".next";
        std::filesystem::copy_file(gapFixture, next, ec);
        std::filesystem::rename(next, path, ec);
        struct stat after {};
        if (ec || ::stat(path.c_str(), &after) != 0) {
            notVerified(kGoal, "the shared file could not be replaced");
            return;
        }

        // GOAL: S15.956 -- a screen re-applied to the replaced file is answered by reading it, never by the stale
        //       answer the other screen holds. False if that answer is handed out and used.
        // WHY-THIS-SHAPE: counted reads, not the status while the scan runs: GetStatus never shows an answer carried
        //       into a scan, so a status check here cannot fail. Two guards keep the stale answer out --
        //       knownScanLocked()'s identity filter and the scan's own identity check of what it is handed -- and a
        //       read is what both missing would remove.
        // CONTROL: control_batch.sh 2026-10-24 (T19b), --phase '^processorScanShared$', restored:
        //       knownScanLocked()'s foundFor(now) made `true` (backend/daemon/wallpaper_manager.cpp) together with
        //       ProcessorCheck::run()'s `before == job.saved->file` made `true` (backend/daemon/processor_check.cpp)
        //       -> "FAIL: S15.956 a screen re-applied to a replaced file is answered by reading it". Either alone
        //       stays green: the other guard still forces the read.
        const int before = g_files.scanOpens();
        first.requestWallpaper("video", path, kFresh);
        notices.waitParts(first, kFresh, "the re-applied screen");
        const int reads = g_files.scanOpens() - before;
        std::printf("processorScanShared: the re-applied screen's answer took %d read(s) of the replaced file\n", reads);
        reportUnless(reads == 1, "S15.956 a screen re-applied to a replaced file is answered by reading it, not by the "
                                 "stale answer another screen holds",
                     std::to_string(reads) + " read(s), 1 due");
        if (reads != 1) return;
    }

    // GOAL: S15.956 -- the saved record keeps the answer for the file as it is now: a restart answers both screens
    //       from it without reading the file.
    // WHY-THIS-SHAPE: graded by a restart's reads rather than by parsing processor-scans.json: a stale entry fails the
    //       scan's identity check and costs exactly the read counted here, and the record's layout stays its own.
    // CONTROL: control_batch.sh 2026-10-24 (T19b), --phase '^processorScanShared$', restored: saveScans()'s rank
    //       without its foundFor(now) term (backend/daemon/wallpaper_manager.cpp) -> "FAIL: S15.956 the saved scan
    //       answer is the one for the file as it is now: a restart reads nothing".
    ScanNotices notices;
    WallpaperManager second(config);
    notices.attach(second);
    const int before = g_files.scanOpens();
    second.startRestoredScans(second.restoreSaved());
    notices.waitParts(second, kStale, "a restored screen");
    notices.waitParts(second, kFresh, "the other restored screen");
    const int reads = g_files.scanOpens() - before;
    std::printf("processorScanShared: the restart read the file %d time(s)\n", reads);
    reportUnless(reads == 0, "S15.956 the saved scan answer is the one for the file as it is now: a restart reads nothing",
                 std::to_string(reads) + " read(s)");
}

// S15.820-S15.824: screens showing one file at one speed share one decoder. Every pass and clock round
// runs on this thread with no worker, so the file ledger counts only what this function drove.
void runDecodeOnceChecks(const WallpaperManager::Config& base, const std::string& clip, const std::string& onePicture) {
    using wallpaperx::PositionMode;
    const char* kGoals = "S15.820-S15.824 (screens on one file share one decoder)";
    PhaseGrade grade("the decode-once phase");
    WallpaperManager::Config config = base;
    config.unwatchedAfter = std::chrono::hours(1);  // plays with no consumer for the whole phase
    WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};
    const auto copyOf = [](const std::string& from, const char* name) {
        const std::string path = testAssetDir() + "/" + name;
        std::filesystem::copy_file(from, path, std::filesystem::copy_options::overwrite_existing);
        g_files.watch(path);
        return path;
    };
    const std::string shared = copyOf(clip, "decode_once_shared.mp4");
    const std::string speeds = copyOf(clip, "decode_once_speeds.mp4");
    const std::string failing = copyOf(clip, "decode_once_failing.mp4");
    const std::string rewritten = copyOf(clip, "decode_once_rewritten.mp4");
    static constexpr int kCover = 0, kFit = 1, kThird = 2, kSlow = 3, kFast = 4, kFailA = 5, kFailB = 6, kOld = 7, kNew = 8;
    for (int id = kCover; id <= kNew; ++id) wallpaperx::testing::nameScreen(manager, id, screenOf(id));

    const auto roundsUntil = [&manager](const std::function<bool(const std::set<int>&)>& done) {
        return clockRoundsUntil(manager, kNew + 1, done);
    };
    const auto live = [](const std::string& path) { return liveCount(path); };

    manager.requestWallpaper("video", shared, kCover);
    const PassCost first = renderCounted(manager, kCover);
    if (!first.outcome.rendered) {
        grade.expectNoDevice();
        notVerified(kGoals, "the first video render on this host produced no frame");
        return;
    }
    // GOAL: as in the session phase -- the scan's read of each file is not counted as a decoder's open.
    // CONTROL: control_batch.sh 2026-10-24 (T18a), --phase '^decodeOnce$', reverted: lowerPriority()'s setpriority
    //       deleted (backend/daemon/processor_check.cpp) -> this line red, "committed, 2 open(s), 1 close(s), 3
    //       packet(s)". Test-side, by hand, restored: every watched open filed as the scan's -> this line, "0 open(s)".
    reportUnless(first.opens == 1 && first.decodes > 0,
                 "a first screen on a file opens it and decodes a picture (the counters work)", describe(first));

    // GOAL: S15.820 -- a second and a third screen given the same unchanged file at the same speed open no
    //       file: one decoder serves all three, and each screen is drawn with its own placement.
    // WHY-THIS-SHAPE: opens are counted per pass on the only thread running passes, so a join cannot be
    //       confused with a decoder a clock round opened. Placement is read after a round that drew all
    //       three, so it grades the shared clock's per-screen blit, not the render pass's own draw.
    // CONTROL: control_batch.sh 2026-10-18, phase decodeOnce, both entries, reverted: videoSessionLocked()'s join
    //       loop (backend/daemon/wallpaper_manager.cpp) given `continue` for every session -> "...open nothing:
    //       they join its decoder"; advanceSessionLocked's `placement(it->second.properties)` made the first
    //       member's placement -> "...the Fit screen's picture from the shared decoder is fitted".
    manager.requestWallpaper("video", shared, kFit, {{"position", PositionMode::Fit}});
    const PassCost fit = renderCounted(manager, kFit);
    manager.requestWallpaper("video", shared, kThird);
    const PassCost third = renderCounted(manager, kThird);
    reportUnless(fit.outcome.rendered && third.outcome.rendered && fit.opens == 0 && third.opens == 0,
                 "a second and a third screen on the same file open nothing: they join its decoder",
                 describe(fit) + "; " + describe(third));
    reportUnless(live(shared) == 1, "three screens on one file hold one open decoder",
                 "open: " + describeFiles(g_files.live()));
    const bool allThree = roundsUntil([](const std::set<int>& s) { return s.count(kCover) && s.count(kFit) && s.count(kThird); });
    check(allThree, "one clock round draws the shared picture on all three screens within 5 s");
    if (allThree) {
        std::string reason;
        const std::optional<bool> coverBlack = leftEdgeBlack(manager, kCover, reason);
        const std::optional<bool> fitBlack = leftEdgeBlack(manager, kFit, reason);
        reportUnless(coverBlack.has_value() && !*coverBlack,
                     "the Cover screen's picture from the shared decoder is cover-cropped (no black edge)",
                     coverBlack ? "the left edge is black" : "unreadable: " + reason);
        reportUnless(fitBlack.value_or(false), "the Fit screen's picture from the shared decoder is fitted (black edge)",
                     fitBlack ? "the left edge is not black" : "unreadable: " + reason);
        reportUnless(g_files.opens(shared) == 0, "and the shared clock opened no second decoder for them",
                     std::to_string(g_files.opens(shared)) + " open(s)");
    }

    // GOAL: S15.822 -- one screen set to another speed is split onto a decoder of its own, and when the
    //       speeds match again the two fold back into one decoder showing one picture on both screens.
    // WHY-THIS-SHAPE: both waits have a deadline that fails the check; the fold is read off the open
    //       set and a round drawing both screens, since the position a fold jumps to has no reader here.
    // CONTROL: control_batch.sh 2026-10-18, phase decodeOnce, both entries, reverted (backend/daemon/wallpaper_manager.cpp):
    //       advanceSessionLocked's split `if (speedOf(member) == speed) return false;` made `return false;`
    //       -> "...split onto a decoder of its own"; mergeSharedSessionsLocked() clearing `standing` before
    //       its pairing loop -> "...fold back into one decoder" (re-run 2026-10-18, T2, both entries).
    manager.requestWallpaper("video", speeds, kSlow);
    const PassCost slow = renderCounted(manager, kSlow);
    manager.requestWallpaper("video", speeds, kFast);
    const PassCost fast = renderCounted(manager, kFast);
    if (!slow.outcome.rendered || !fast.outcome.rendered || slow.opens != 1 || fast.opens != 0) {
        check(false, ("two screens on a second file share its decoder before a speed change -- " + describe(slow) +
                      "; " + describe(fast)).c_str());
    } else {
        g_files.resetCounts();
        manager.setWallpaperProperties(kFast, {{"speed", 2.0}});
        const bool split = roundsUntil([&](const std::set<int>& s) { return live(speeds) == 2 && s.count(kFast); });
        reportUnless(split, "a screen set to another speed is split onto a decoder of its own and drawn from it",
                     "open: " + describeFiles(g_files.live()));
        if (split) {
            manager.setWallpaperProperties(kFast, {{"speed", 1.0}});
            const bool folded = roundsUntil(
                [&](const std::set<int>& s) { return live(speeds) == 1 && s.count(kSlow) && s.count(kFast); });
            reportUnless(folded, "when the speeds match again they fold back into one decoder drawing both screens",
                         "open: " + describeFiles(g_files.live()));
        }
    }

    // GOAL: S15.823 -- a decoder failure in a shared session closes it and is recorded on every screen
    //       playing from it, not only the screen it is blamed on.
    // CONTROL: control_batch.sh 2026-10-18 (T3), phase decodeOnce, both entries, reverted: advanceSessionLocked's
    //       `failing` set (backend/daemon/wallpaper_manager.cpp) taking only the first screen hit, `if (hit &&
    //       failing.none())` -> "FAIL: a shared decoder's failure is recorded on every screen showing it".
    manager.requestWallpaper("video", failing, kFailA);
    const PassCost failA = renderCounted(manager, kFailA);
    manager.requestWallpaper("video", failing, kFailB);
    const PassCost failB = renderCounted(manager, kFailB);
    if (!failA.outcome.rendered || !failB.outcome.rendered || failB.opens != 0) {
        check(false, ("two screens on a third file share its decoder before it fails -- " + describe(failA) + "; " +
                      describe(failB)).c_str());
    } else {
        const auto errorOf = [&manager](int id) {
            const std::optional<wallpaperx::MonitorStatus> status = monitorStatus(manager, id);
            return status ? status->lastError : std::string();
        };
        g_playbackFailure.arm();
        const bool failed = roundsUntil([&](const std::set<int>&) { return !errorOf(kFailA).empty() || !errorOf(kFailB).empty(); });
        g_playbackFailure.disarm();
        check(failed, "a failure injected into the shared decoder is recorded within 5 s");
        if (failed) {
            reportUnless(!errorOf(kFailA).empty() && !errorOf(kFailB).empty(),
                         "a shared decoder's failure is recorded on every screen showing it",
                         "first: '" + errorOf(kFailA).substr(0, 80) + "', second: '" + errorOf(kFailB).substr(0, 80) + "'");
        }
    }

    // GOAL: S15.821 -- a screen given a path whose file was rewritten in place after another screen
    //       opened it is not joined to that decoder: it opens its own.
    // WHY-THIS-SHAPE: last in the phase, since the first screen's decoder now reads bytes that changed
    //       under it and no round may run it again.
    // CONTROL: control_batch.sh 2026-10-18, phase decodeOnce, both entries, reverted: `|| session.file != file`
    //       deleted from videoSessionLocked()'s join test (backend/daemon/wallpaper_manager.cpp) ->
    //       "...rewritten in place ... opens a decoder of its own".
    manager.requestWallpaper("video", rewritten, kOld);
    const PassCost old = renderCounted(manager, kOld);
    if (!old.outcome.rendered || old.opens != 1) {
        check(false, ("a screen opens the file that is then rewritten -- " + describe(old)).c_str());
        return;
    }
    std::filesystem::copy_file(onePicture, rewritten, std::filesystem::copy_options::overwrite_existing);
    manager.requestWallpaper("video", rewritten, kNew);
    const PassCost fresh = renderCounted(manager, kNew);
    reportUnless(fresh.outcome.rendered && fresh.opens == 1 && live(rewritten) == 2,
                 "a screen given a file rewritten in place since another screen opened it opens a decoder of its own",
                 describe(fresh) + "; open: " + describeFiles(g_files.live()));
}

// Two screens on one copy of the counter file, sharing its decoder, each manager its own. Every pass
// and clock round runs on this thread with no worker. Monitor 0 opens the file, so it is the
// session's first screen.
class SharedCounterRig {
public:
    static constexpr int kFirst = 0, kSecond = 1, kMonitors = 2;

    SharedCounterRig(const WallpaperManager::Config& config, const std::string& path, const CounterOrder& order)
        : manager(config), path(path), m_order(order) {
        std::vector<int> owed;
        for (int id = 0; id < kMonitors; ++id) {
            wallpaperx::testing::nameScreen(manager, id, screenOf(id));
            manager.setOutputSize(screenOf(id), 160, 120, owed);
        }
    }

    // False, having said why, unless the second screen joined the first's decoder and both are drawn.
    bool share(const char* what) {
        using wallpaperx::PositionMode;
        manager.requestWallpaper("video", path, kFirst, {{"position", PositionMode::Stretch}});
        const PassCost first = renderCounted(manager, kFirst);
        manager.requestWallpaper("video", path, kSecond, {{"position", PositionMode::Stretch}});
        const PassCost second = renderCounted(manager, kSecond);
        int together = 0;
        const bool drawn = rounds([&](const std::set<int>& s) { return (together += s.count(kFirst) && s.count(kSecond)) >= 3; });
        if (first.outcome.rendered && first.opens == 1 && second.outcome.rendered && second.opens == 0 && drawn) return true;
        check(false, (std::string(what) + ": two screens on the counter file share its decoder and are drawn together "
                                          "(the premise) -- " + describe(first) + "; " + describe(second) +
                      (drawn ? "" : "; not drawn together within 5 s")).c_str());
        return false;
    }

    std::optional<int> counter(int id) { return servedCounter(manager, id, m_reason); }
    std::string error(int id) const {
        const std::optional<wallpaperx::MonitorStatus> status = monitorStatus(manager, id);
        return status ? status->lastError : std::string();
    }
    bool rounds(const std::function<bool(const std::set<int>&)>& done) { return clockRoundsUntil(manager, kMonitors, done); }
    bool showsOf(int id, int count) {
        int seen = 0;
        return rounds([&](const std::set<int>& s) { return (seen += static_cast<int>(s.count(id))) >= count; });
    }
    // The first picture `id` is drawn with by the rounds from here, its render passes included.
    std::optional<int> firstShown(int id) {
        std::optional<int> first;
        rounds([&](const std::set<int>& s) {
            if (s.count(id)) first = counter(id);
            return s.count(id) > 0;
        });
        return first;
    }
    // `a` is `b` or the picture after it.
    bool withinOne(std::optional<int> a, std::optional<int> b) const { return a && b && (*a == *b || m_order.after(*b) == *a); }
    static std::string text(std::optional<int> n) { return n ? std::to_string(*n) : std::string("unreadable"); }

    WallpaperManager manager;
    const std::string path;

private:
    const CounterOrder& m_order;
    std::string m_reason;
};

// S15.869-S15.871: a failure that breaks a shared decoder costs only the screens it is about.
void runSharedFailureChecks(const WallpaperManager::Config& base, const std::string& still) {
    using wallpaperx::PositionMode;
    const char* kGoals = "S15.869-S15.871 (a shared decoder's failure costs only the screens it is about)";
    const std::string counterClip = counterClipPath();
    if (!std::filesystem::exists(counterClip)) {
        notVerified(kGoals, "the counter fixture " + counterClip + " was not built");
        return;
    }
    const std::optional<CounterOrder> order = counterOrder(counterClip, kGoals);
    if (!order) return;
    WallpaperManager::Config config = base;
    config.unwatchedAfter = std::chrono::hours(1);
    config.idleSessionRelease = std::chrono::seconds(60);
    config.pausedSessionRelease = std::chrono::seconds(60);
    constexpr int kA = SharedCounterRig::kFirst, kB = SharedCounterRig::kSecond;

    // GOAL: S15.869 -- a render pass on a paused screen whose draw fails for a reason that is not a GPU
    //       fault breaks the shared decoder; the playing screen keeps its picture, records no error, and
    //       resumes from its OWN picture, not the failing screen's (ten pictures back) nor the start.
    // WHY-THIS-SHAPE: the failing screen is paused so the two positions differ; a failure on two screens
    //       at one picture could not tell "its own" from "the failing screen's". The failure is the draw's
    //       submit, so the session is closed for everyone (the premise reads no file left open).
    // CONTROL: control_batch.sh 2026-10-18, phase sharedFailure, both entries, each reverted, in
    //       closeFailedVideoSessionLocked() (backend/daemon/wallpaper_manager.cpp): the others' `releasedAt`
    //       not recorded -> "...the playing screen resumes from its own picture" ("its own 13, the failing
    //       screen's 3, resumed on 1"; re-run 2026-10-24 on both, with S15.870's and S15.871's lines); `if (failure.gpuFault)` made
    //       `if (true)` -> "...leaves the playing screen its picture and no error".
    {
        PhaseGrade grade("the shared-failure phase (a failed render pass)");
        SharedCounterRig rig(config, watchedCopy(counterClip, "shared_failure_pass.mp4"), *order);
        CaptureGradeOnExit captureGrade{grade, rig.manager};
        if (!rig.share("S15.869")) {
            if (!rig.counter(kA)) grade.expectNoDevice();
            return;
        }
        rig.manager.setMonitorPause(kB, "always_paused");
        const std::optional<int> frozen = rig.counter(kB);
        const bool ranOn = rig.showsOf(kA, 10);
        const std::optional<int> lastA = rig.counter(kA);
        g_submitFailGate.arm(CallGate::Mode::Fail);
        rig.manager.setWallpaperProperties(kB, {{"position", PositionMode::Fit}});
        const bool owed = owesRender(rig.manager, kB);
        const bool drawn = rig.manager.renderPending(kB).rendered;
        const bool fired = g_submitFailGate.waitFired(std::chrono::seconds(0));
        g_submitFailGate.release();
        if (!ranOn || !frozen || !lastA || frozen == lastA || !owed || drawn || !fired || liveCount(rig.path) != 0) {
            check(false, ("S15.869: the paused screen's pass fails at its draw and closes the shared decoder (the premise) "
                          "-- paused on " + SharedCounterRig::text(frozen) + ", the other on " + SharedCounterRig::text(lastA) +
                          (owed ? "" : "; not owed") + (drawn ? "; drawn" : "") + (fired ? "" : "; the failure never fired") +
                          "; open: " + describeFiles(g_files.live())).c_str());
        } else {
            const std::string errorA = rig.error(kA);
            const std::optional<int> keptA = rig.counter(kA);
            reportUnless(errorA.empty() && keptA == lastA,
                         "another screen's failed render pass leaves the playing screen its picture and no error",
                         "error '" + errorA.substr(0, 80) + "', showing " + SharedCounterRig::text(keptA) + " where it had " +
                             SharedCounterRig::text(lastA));
            g_files.resetCounts();
            const std::optional<int> resumedA = rig.firstShown(kA);
            reportUnless(rig.withinOne(resumedA, lastA),
                         "after another screen's failed render pass, the playing screen resumes from its own picture",
                         "its own " + SharedCounterRig::text(lastA) + ", the failing screen's " + SharedCounterRig::text(frozen) +
                             ", resumed on " + SharedCounterRig::text(resumedA) + ", " +
                             std::to_string(g_files.opens(rig.path)) + " open(s)");
        }
    }

    // GOAL: S15.870 -- a decode failure in the clock round fails the playing screen only: the paused
    //       screen sharing the decoder keeps its picture, records no error, and resumes from its own.
    // WHY-THIS-SHAPE: the rounds that meet the failure are bare advanceVideo() calls, so no render pass
    //       reopens anything before the paused screen is read; the playing screen is then given a still,
    //       so the resumed one has no decoder to join and opens at whatever position was recorded for it.
    // CONTROL: control_batch.sh 2026-10-18, phase sharedFailure, both entries, each reverted
    //       (backend/daemon/wallpaper_manager.cpp): advanceSessionLocked()'s `hit` made true for every screen
    //       -> "...the paused screen keeps its picture and records no error"; closeFailedVideoSessionLocked()
    //       not recording the others' `releasedAt` -> "...plays on from its own picture" (re-run 2026-10-24).
    {
        PhaseGrade grade("the shared-failure phase (a paused screen through a decode failure)");
        SharedCounterRig rig(config, watchedCopy(counterClip, "shared_failure_paused.mp4"), *order);
        CaptureGradeOnExit captureGrade{grade, rig.manager};
        if (!rig.share("S15.870")) return;
        rig.manager.setMonitorPause(kB, "always_paused");
        const std::optional<int> frozen = rig.counter(kB);
        const bool ranOn = rig.showsOf(kA, 10);
        const int injectedBefore = g_playbackFailure.injected();
        g_playbackFailure.arm(std::numeric_limits<int>::max(), 1);
        bool failed = false;
        for (const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
             !failed && std::chrono::steady_clock::now() < deadline;) {
            rig.manager.advanceVideo(std::chrono::steady_clock::now());
            failed = !rig.error(kA).empty();
            if (!failed) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        g_playbackFailure.disarm();
        if (!ranOn || !frozen || !failed || g_playbackFailure.injected() != injectedBefore + 1) {
            check(false, ("S15.870: one injected decode failure fails the playing screen within 5 s (the premise) -- "
                          "paused on " + SharedCounterRig::text(frozen) + (failed ? "" : "; no failure recorded")).c_str());
        } else {
            const std::string errorB = rig.error(kB);
            const std::optional<int> keptB = rig.counter(kB);
            reportUnless(errorB.empty() && keptB == frozen,
                         "after a shared decoder fails in the clock round, the paused screen keeps its picture and records "
                         "no error",
                         "error '" + errorB.substr(0, 80) + "', showing " + SharedCounterRig::text(keptB) + " where it had " +
                             SharedCounterRig::text(frozen));
            rig.manager.requestWallpaper("static", still, kA);
            const bool stillDrawn = rig.manager.renderPending(kA).rendered;
            rig.manager.setMonitorPause(kB, "play");
            g_files.resetCounts();
            const std::optional<int> resumedB = rig.firstShown(kB);
            check(stillDrawn, "S15.870: the failed screen draws the still it is given");
            reportUnless(rig.withinOne(resumedB, frozen) && g_files.opens(rig.path) == 1,
                         "the paused screen resumed after the shared decoder failed plays on from its own picture",
                         "paused on " + SharedCounterRig::text(frozen) + ", resumed on " + SharedCounterRig::text(resumedB) +
                             ", " + std::to_string(g_files.opens(rig.path)) + " open(s)");
        }
    }

    // GOAL: S15.871 -- a clock round whose second screen's draw fails (not a GPU fault) leaves the first
    //       screen, already drawn that round, with that round's picture and no error, and it resumes from it.
    // WHY-THIS-SHAPE: the failing round must be one that draws and decodes nothing else, so that the one
    //       submission let through is the first screen's draw: rounds run until one decodes and converts the
    //       next picture without drawing it, and the next is run when that picture is due. The premise reads
    //       only the second screen failed; the first being announced is part of what is graded.
    // CONTROL: control_batch.sh 2026-10-18, phase sharedFailure, both entries, each reverted
    //       (backend/daemon/wallpaper_manager.cpp): advanceSessionLocked()'s `drawing ? screen.monitorId ==
    //       *drawing` made `drawing ? true` -> "...the screen drawn before it keeps that round's picture and no
    //       error"; closeFailedVideoSessionLocked() not recording the others' `releasedAt` -> "and it resumes
    //       from that round's picture" (re-run 2026-10-24).
    {
        PhaseGrade grade("the shared-failure phase (a later screen's draw)");
        SharedCounterRig rig(config, watchedCopy(counterClip, "shared_failure_draw.mp4"), *order);
        CaptureGradeOnExit captureGrade{grade, rig.manager};
        if (!rig.share("S15.871")) return;
        std::optional<std::chrono::steady_clock::time_point> due;
        for (const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
             !due && std::chrono::steady_clock::now() < deadline;) {
            const auto now = std::chrono::steady_clock::now();
            const wallpaperx::ClockRound round = rig.manager.advanceVideo(now);
            if (round.shown.empty() && round.nextDue && *round.nextDue > now) {
                due = round.nextDue;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        const std::optional<int> before = rig.counter(kA);
        std::set<int> shown;
        if (due) {
            std::this_thread::sleep_until(*due);
            g_submitFailAfter.store(1);
            for (const auto& s : rig.manager.advanceVideo(std::chrono::steady_clock::now()).shown) shown.insert(s.monitorId);
        }
        const bool fired = g_submitFailAfter.exchange(-1) == -1;
        // Whether the first screen's picture was announced is graded below, not here: a round that fails
        // every screen withdraws it, which is the defect, not a premise gone wrong.
        if (!due || !before || !fired || shown.count(kB) || rig.error(kB).empty()) {
            check(false, ("S15.871: a round fails the second screen's draw (the premise) -- " +
                          std::string(due ? "" : "no waiting round within 5 s; ") + (fired ? "" : "the failure never fired; ") +
                          "second " + (shown.count(kB) ? "shown" : "not shown") +
                          (rig.error(kB).empty() ? ", no error on it" : "")).c_str());
        } else {
            const std::string errorA = rig.error(kA);
            const std::optional<int> drawnA = rig.counter(kA);
            reportUnless(errorA.empty() && shown.count(kA) && drawnA && *drawnA == order->after(*before),
                         "when a later screen's draw fails in a clock round, the screen drawn before it keeps that round's "
                         "picture and no error",
                         "error '" + errorA.substr(0, 80) + "', " + (shown.count(kA) ? "announced" : "not announced") +
                             ", showing " + SharedCounterRig::text(drawnA) + " after " + SharedCounterRig::text(before));
            const std::optional<int> resumedA = rig.firstShown(kA);
            reportUnless(rig.withinOne(resumedA, drawnA), "and it resumes from that round's picture",
                         "drawn " + SharedCounterRig::text(drawnA) + ", resumed on " + SharedCounterRig::text(resumedA));
        }
    }
}

// S15.872, S15.873, S15.878, S15.879: which screens share a decoder around a speed change, and when two
// sessions on one file fold together.
void runSharedSpeedChecks(const WallpaperManager::Config& base) {
    const char* kGoals = "S15.872-S15.879 (sharing a decoder around stopped screens and speed changes)";
    const std::string counterClip = counterClipPath();
    if (!std::filesystem::exists(counterClip)) {
        notVerified(kGoals, "the counter fixture " + counterClip + " was not built");
        return;
    }
    const std::optional<CounterOrder> order = counterOrder(counterClip, kGoals);
    if (!order) return;
    WallpaperManager::Config config = base;
    config.unwatchedAfter = std::chrono::hours(1);
    config.idleSessionRelease = std::chrono::seconds(60);
    config.pausedSessionRelease = std::chrono::seconds(60);
    constexpr int kA = SharedCounterRig::kFirst, kB = SharedCounterRig::kSecond;

    {
        PhaseGrade grade("the shared-speed phase (stopped sessions, then a speed change)");
        SharedCounterRig rig(config, watchedCopy(counterClip, "shared_speed_stopped.mp4"), *order);
        CaptureGradeOnExit captureGrade{grade, rig.manager};
        if (!rig.share("S15.872")) {
            if (!rig.counter(kA)) grade.expectNoDevice();
            return;
        }
        rig.manager.setMonitorPause(kB, "always_paused");
        const bool ranOn = rig.showsOf(kA, 10);
        rig.manager.setMonitorPause(kA, "always_paused");
        std::vector<int> owed;
        rig.manager.setOutputSize(screenOf(kB), 200, 150, owed);
        const PassCost apart = renderCounted(rig.manager, kB);
        if (!ranOn || !apart.outcome.rendered || apart.opens != 1 || liveCount(rig.path) != 2) {
            check(false, ("S15.872: with both screens paused, the one redrawn reopens at its own picture, so two "
                          "sessions hold the file (the premise) -- " + describe(apart) + "; open: " +
                          describeFiles(g_files.live())).c_str());
            return;
        }

        // GOAL: S15.872 -- two sessions on one file whose screens are all stopped are not folded into one.
        // CONTROL: control_batch.sh 2026-10-18, phase sharedSpeed, both entries, reverted:
        //       mergeSharedSessionsLocked()'s `if (!standing[a].second.moves && !standing[b].second.moves)
        //       continue;` (backend/daemon/wallpaper_manager.cpp) deleted -> this line.
        g_files.resetCounts();
        size_t fewest = 2;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 5; ++i) {
            rig.manager.advanceVideo(t0 + i * std::chrono::milliseconds(200));
            fewest = std::min(fewest, static_cast<size_t>(liveCount(rig.path)));
        }
        reportUnless(fewest == 2 && g_files.closes(rig.path) == 0,
                     "two sessions on one file whose screens are all stopped are not merged",
                     "fewest open across five rounds: " + std::to_string(fewest) + ", " +
                         std::to_string(g_files.closes(rig.path)) + " close(s)");

        // GOAL: S15.879 -- a session whose screen now asks for another speed is not folded into a session
        //       at its cached speed before its own round retimes it: the changed screen plays on in its own
        //       session, never sent out and reopened.
        // WHY-THIS-SHAPE: the speed is changed and play resumed with no round between, so the next round's
        //       merge, which runs before any session's own round, sees the cached speed (1) equal to the
        //       other session's while the live one (2) differs.
        // CONTROL: control_batch.sh 2026-10-18, phase sharedSpeed, both entries, reverted:
        //       mergeSharedSessionsLocked()'s `if (!speedSettledLocked(session)) continue;`
        //       (backend/daemon/wallpaper_manager.cpp) deleted -> this line.
        g_files.resetCounts();
        rig.manager.setWallpaperProperties(kA, {{"speed", 2.0}});
        rig.manager.setMonitorPause(kA, "play");
        fewest = 2;
        int shownA = 0;
        const bool played = rig.rounds([&](const std::set<int>& s) {
            fewest = std::min(fewest, static_cast<size_t>(liveCount(rig.path)));
            return (shownA += static_cast<int>(s.count(kA))) >= 10;
        });
        reportUnless(played && fewest == 2 && g_files.opens(rig.path) == 0,
                     "a session whose screen just changed speed is not merged before its own round retimes it, and that "
                     "screen plays on without a reopen",
                     std::string(played ? "" : "not shown ten times within 5 s; ") + "fewest open " +
                         std::to_string(fewest) + ", " + std::to_string(g_files.opens(rig.path)) + " open(s)");
    }

    // GOAL: S15.873 -- a speed change on the paused FIRST screen of a shared session sends only that screen
    //       out: the other plays on in the same session, no file opened.
    // WHY-THIS-SHAPE: the changed screen is the session's first, so a session that took its speed from its
    //       first screen would retime and send the playing one out to reopen. Only the playing screen's
    //       renders are run, so every open counted is one that screen made.
    // CONTROL: control_batch.sh 2026-10-18, phase sharedSpeed, both entries, reverted: advanceSessionLocked()'s
    //       speed choice (backend/daemon/wallpaper_manager.cpp) replaced by `session.speed =
    //       speedOf(session.members.front());` -> this line.
    {
        PhaseGrade grade("the shared-speed phase (a paused first screen's speed)");
        SharedCounterRig rig(config, watchedCopy(counterClip, "shared_speed_paused_first.mp4"), *order);
        CaptureGradeOnExit captureGrade{grade, rig.manager};
        if (!rig.share("S15.873")) return;
        rig.manager.setMonitorPause(kA, "always_paused");
        rig.manager.setWallpaperProperties(kA, {{"speed", 2.0}});
        g_files.resetCounts();
        // Only the playing screen's renders run: the changed one may owe a redraw, which would reopen it
        // at its own picture -- an open that is not the one this check is about.
        int shownB = 0;
        bool playsOn = false;
        for (const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
             !playsOn && std::chrono::steady_clock::now() < deadline;) {
            for (const auto& s : rig.manager.advanceVideo(std::chrono::steady_clock::now()).shown) shownB += s.monitorId == kB;
            if (owesRender(rig.manager, kB)) rig.manager.renderPending(kB);
            playsOn = shownB >= 10;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        reportUnless(playsOn && g_files.opens(rig.path) == 0 && liveCount(rig.path) == 1,
                     "a speed change on a paused first screen sends only it out; the other plays on without a reopen",
                     std::string(playsOn ? "" : "not shown ten times within 5 s; ") +
                         std::to_string(g_files.opens(rig.path)) + " open(s); open: " + describeFiles(g_files.live()));
    }

    // GOAL: S15.878 -- a screen at the old speed does not join a session whose screen just asked for a new
    //       one before that session's round: it opens its own, and the changed screen is neither sent out
    //       nor reopened.
    // CONTROL: control_batch.sh 2026-10-18, phase sharedSpeed, both entries, reverted: `|| !speedSettledLocked(session)`
    //       deleted from videoSessionLocked()'s join (backend/daemon/wallpaper_manager.cpp) -> "...opens its
    //       own" and "...neither sent out nor reopened".
    {
        using wallpaperx::PositionMode;
        PhaseGrade grade("the shared-speed phase (a join against a stale speed)");
        SharedCounterRig rig(config, watchedCopy(counterClip, "shared_speed_join.mp4"), *order);
        CaptureGradeOnExit captureGrade{grade, rig.manager};
        rig.manager.requestWallpaper("video", rig.path, kA, {{"position", PositionMode::Stretch}});
        const PassCost first = renderCounted(rig.manager, kA);
        if (!first.outcome.rendered || !rig.showsOf(kA, 3)) {
            check(false, ("S15.878: one screen opens and plays the file (the premise) -- " + describe(first)).c_str());
            return;
        }
        rig.manager.setWallpaperProperties(kA, {{"speed", 2.0}});
        rig.manager.requestWallpaper("video", rig.path, kB, {{"position", PositionMode::Stretch}});
        const PassCost joined = renderCounted(rig.manager, kB);
        reportUnless(joined.outcome.rendered && joined.opens == 1,
                     "a screen at the old speed does not join a session whose screen just asked for another: it opens its own",
                     describe(joined));
        g_files.resetCounts();
        const bool playsOn = rig.showsOf(kA, 10);
        reportUnless(playsOn && g_files.opens(rig.path) == 0 && liveCount(rig.path) == 2,
                     "and the screen whose speed changed is neither sent out nor reopened",
                     std::string(playsOn ? "" : "not shown ten times within 5 s; ") +
                         std::to_string(g_files.opens(rig.path)) + " open(s); open: " + describeFiles(g_files.live()));
    }
}

// S15.876, on the render worker so a teardown can be held.
void runSharedGpuFaultChecks(const WallpaperManager::Config& base, const std::string& onePicture) {
    const char* kGoals = "S15.876 (a GPU fault in a shared pass's conversion)";
    const wallpaperx::testing::ScratchDir scratch("videosharedgpu");
    if (!scratch.ok()) {
        notVerified(kGoals, "no scratch directory could be created for the state and config");
        return;
    }
    WallpaperManager::Config config = base;
    config.stateDir = scratch.path() / "state";
    config.configFile = scratch.path() / "config.json";
    const std::string path = watchedCopy(onePicture, "shared_gpu_fault.mp4");
    constexpr int kOther = 30, kFaulted = 31;

    PhaseGrade grade("the shared GPU-fault phase");
    FrameReadyLog signals;
    WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};
    RenderWorker worker(manager, [&signals](int monitorId, uint64_t generation) { signals.record(monitorId, generation); });
    WorkerUnblock unblock{worker};
    const auto show = [&](int id) {
        wallpaperx::testing::nameScreen(manager, id, screenOf(id));
        manager.requestWallpaper("video", path, id);
        worker.post(id);
        return signals.wait(id, 1) && drainWorker(worker);
    };
    if (!show(kOther)) {
        grade.expectNoDevice();
        notVerified(kGoals, "the first video render on this host produced no frame");
        return;
    }
    if (!show(kFaulted) || liveCount(path) != 1) {
        check(false, ("S15.876: a second screen joins the one-picture file's decoder (the premise) -- open: " +
                      describeFiles(g_files.live())).c_str());
        return;
    }

    // GOAL: S15.876 -- a GPU fault in a render pass's conversion, on a decoder shared with another screen,
    //       frees the session and the other screen's images with no crash record naming the file on disk,
    //       so a start after a death in that teardown still applies the file.
    // WHY-THIS-SHAPE: a one-picture file, because its clock round decodes the picture again and parks
    //       without converting it, so the re-posted pass's convert is the one with work to submit; the
    //       re-post changes size and back, so it rebuilds nothing and that submit is its first. The
    //       thread is held as the file is closed, the end of the teardown, and a start built from the state
    //       copied then (runFailedPassChecks shows that copy refusing while a record is on disk).
    // CONTROL: control_batch.sh 2026-10-18, phase sharedGpuFault, both entries, reverted: renderPassLocked()'s pre-commit
    //       `if (failure) tearDownFailed();` (backend/daemon/wallpaper_manager.cpp) given the teardown
    //       without `applying.reset()` -> "...no crash record naming the file is on disk" and "...still
    //       applies the file".
    g_submitDeviceLost.store(1);
    g_closeFileGate.arm(CallGate::Mode::Hold, path);
    std::vector<int> notPosted;
    manager.setOutputSize(screenOf(kFaulted), kResizedWidth, kResizedHeight, notPosted);
    manager.setOutputSize(screenOf(kFaulted), kTargetWidth, kTargetHeight, notPosted);
    worker.post(kFaulted);
    const bool closing = g_closeFileGate.waitFired(kGateLimit);
    check(closing, "S15.876: a GPU fault in a shared pass tears the shared session down (the premise)");
    if (closing) {
        const std::optional<wallpaperx::CrashRecord> record = wallpaperx::readCrashRecord(config.stateDir, kFaulted);
        reportUnless(!record.has_value(),
                     "while a GPU fault in a shared pass's conversion frees the session, no crash record naming the file "
                     "is on disk",
                     "the record on disk: " + describeRecord(record));
        const RestartVerdict restart = startAfterDeathNow(config, kFaulted);
        if (restartGraded(restart)) {
            reportUnless(!restart.refused, "a start after dying in that teardown still applies the file",
                         "the start refused it: " + restart.lastError);
        }
    }
    g_closeFileGate.release();
    check(drainWorker(worker), "S15.876: the faulted pass finishes once its teardown is let go");
    const std::optional<wallpaperx::MonitorStatus> other = monitorStatus(manager, kOther);
    check(g_submitDeviceLost.load() == 0 && other && !other->lastError.empty(),
          "S15.876: the injected device loss was spent and recorded on the other screen too (the premise)");
}

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
    const std::string rate240 = rate240ClipPath();
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

        // Every phase runs guarded: an exception escaping one fails that phase by name, and the
        // phases after it still run (test_phase.h).
        using wallpaperx::testing::guardedPhase;
        const auto phase = [](const char* name, auto&& body) {
            if (phaseSelected(name)) guardedPhase(name, body);
        };
        const bool busRan = phaseSelected("bus");
        if (busRan) guardedPhase("bus", [&] {
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
        });

        // COVERAGE-ASSERT for the whole file ledger, placed after the first phase that is known to
        // have drawn a video. Everything below reads "how many times was this file opened", and a
        // ledger watching the wrong call answers zero to all of them -- which is the expected
        // answer for several, so a blind ledger would read as a clean run. Kept as the whole-run
        // assert; each phase that grades a zero also proves the ledger for itself through
        // resetCountsAfterOpenOf(), since under WALLPAPERX_TEST_PHASE this one need not run.
        // A harness fault, for resetCountsAfterOpenOf()'s reason: a video was just drawn, so an empty
        // ledger is this file's hook failing, not the daemon.
        // CONTROL: the `open` hook's `g_files.opened(fd, path)` deleted, via control_batch.sh, --phase
        //       '^bus$', 2026-09-29 -> "FAIL (harness): the file ledger saw no open of a wallpaper file
        //       ..." and "fault(s) in this test file, not in the daemon". Restored.
        if (busRan && g_files.totalOpens() < 1) {
            wallpaperx::testing::harnessFailure(
                "the file ledger saw no open of a wallpaper file although a video was just drawn over the bus, so "
                "the open() hook no longer sees the daemon's opens and every open count below is unmeasured");
        }

        phase("undecodable", [&] { runUndecodableChecks(config, junk); });
        phase("resize", [&] { runResizeChecks(config, onePicture); });
        phase("crashRecordSpan", [&] { runCrashRecordSpanChecks(config, onePicture); });
        phase("session", [&] { runSessionChecks(config, clips, still, shaderRow); });
        phase("failedPass", [&] { runFailedPassChecks(config, parked, still); });
        phase("evictionRecord", [&] { runEvictionRecordCheck(config, parked); });
        phase("shutdown", [&] { runShutdownChecks(config, parked[0]); });
        phase("clock", [&] { runClockChecks(config, clip, still); });
        phase("onePicture", [&] { runOnePictureChecks(config, onePicture); });
        phase("parkedRelease", [&] { runParkedReleaseChecks(config, onePicture); });
        phase("offSwitch", [&] { runOffSwitchChecks(config, clip); });
        phase("watchEvidence", [&] { runWatchEvidenceChecks(config, clip, still); });

        // A bus of its own, and a second phase that owns the daemon's well-known name: the
        // visibility goals are about the whole chain -- the method, the manager, and the worker
        // being woken -- and a manager-level phase would have to stand in for the last of those.
        phase("visibility", [&] {
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
        });

        // The same chain for a wallpaper's speed: the method, the manager and the worker's wake.
        phase("speed", [&] {
            PhaseGrade grade("the speed phase");
            BusRig rig(config, grade);
            runSpeedChecks(*rig.proxy, rig.pictures, clip, still, grade);
        });

        // Step 15 chunk 3c: the pause setting, and a video resuming where it stood. Each builds its own
        // manager, service and proxy; the resume phases read pictures by the frame number drawn in them.
        phase("pause", [&] { runPauseChecks(config, clip); });
        const bool resumeSelected = phaseSelected("resume");
        const bool resumeEvictionSelected = phaseSelected("resumeEviction");
        const bool resumeFallbackSelected = phaseSelected("resumeFallback");
        const bool resumeCatchUpSelected = phaseSelected("resumeCatchUp");
        const bool resumeBlameSelected = phaseSelected("resumeBlame");
        const bool resumeGpuFaultSelected = phaseSelected("resumeGpuFault");
        const bool resumeGpuFaultRenderSelected = phaseSelected("resumeGpuFaultRender");
        const bool resumeHealthSelected = phaseSelected("resumeHealth");
        if (resumeSelected || resumeEvictionSelected || resumeFallbackSelected || resumeCatchUpSelected ||
            resumeBlameSelected || resumeGpuFaultSelected || resumeGpuFaultRenderSelected || resumeHealthSelected) {
            const std::string counterClip = counterClipPath();
            if (!std::filesystem::exists(counterClip)) {
                notVerified("S15.151-S15.156, S15.175, S15.415-S15.418, S15.425-S15.428", "the counter fixture " + counterClip +
                                                   " was not generated, so no resumed picture could be identified");
            } else {
                if (resumeSelected) guardedPhase("resume", [&] { runResumeChecks(config, counterClip, still); });
                if (resumeEvictionSelected) {
                    guardedPhase("resumeEviction", [&] { runResumeEvictionChecks(config, counterClip, clips, still); });
                }
                if (resumeFallbackSelected) {
                    guardedPhase("resumeFallback", [&] { runResumeFallbackChecks(config, counterClip); });
                }
                if (resumeCatchUpSelected) {
                    guardedPhase("resumeCatchUp", [&] { runResumeCatchUpChecks(config, counterClip); });
                }
                if (resumeBlameSelected) guardedPhase("resumeBlame", [&] { runResumeBlameChecks(counterClip); });
                if (resumeGpuFaultSelected) {
                    guardedPhase("resumeGpuFault", [&] { runResumeGpuFaultChecks(config, counterClip, false); });
                }
                if (resumeGpuFaultRenderSelected) {
                    guardedPhase("resumeGpuFaultRender", [&] { runResumeGpuFaultChecks(config, counterClip, true); });
                }
                if (resumeHealthSelected) {
                    guardedPhase("resumeHealth", [&] { runResumeHealthChecks(config, counterClip, still); });
                }
            }
        }
        phase("resumeHold", [&] {
            const std::string longGop = longGopClipPath();
            if (std::filesystem::exists(longGop)) {
                runResumeHoldChecks(config, longGop, still);
            } else {
                notVerified("S15.412, S15.414", "the 4K fixture " + longGop + " was not generated, so no long catch-up "
                                                "was exercised");
            }
        });
        phase("cleanPlayPause", [&] { runCleanPlayPauseChecks(config, clip, /*withinOneRound=*/false); });
        phase("cleanPlayPauseHeld", [&] { runCleanPlayPauseChecks(config, clip, /*withinOneRound=*/true); });

        phase("evictionWatch", [&] { runEvictionWatchChecks(config, clips); });
        phase("slowPicture", [&] {
            if (std::filesystem::exists(slow)) {
                runSlowPictureChecks(config, slow);
            } else {
                notVerified("S13.158", "the slow-picture fixture " + slow +
                                           " was not generated, so the case that froze on a real desktop -- a video "
                                           "whose pictures are farther apart than the daemon's unwatched interval -- "
                                           "was not exercised at all");
            }
        });
        phase("irregularWallpaper", [&] { runIrregularWallpaperChecks(config, clip, still); });
        // Each rate phase at the frame-rate setting 120 (S15.895) and, with "60" in its name, at the default 60
        // (S15.896).
        const bool clampSelected = phaseSelected("intervalClamp");
        const bool clamp60Selected = phaseSelected("intervalClamp60");
        const bool capSelected = phaseSelected("capBudget");
        const bool cap60Selected = phaseSelected("capBudget60");
        if (std::filesystem::exists(fast) && std::filesystem::exists(gap)) {
            if (clampSelected) guardedPhase("intervalClamp", [&] { runIntervalClampChecks(config, fast, gap, 120); });
            if (clamp60Selected) guardedPhase("intervalClamp60", [&] { runIntervalClampChecks(config, fast, gap, 60); });
            if ((capSelected || cap60Selected) && std::filesystem::exists(rate120)) {
                if (capSelected) guardedPhase("capBudget", [&] { runCapBudgetChecks(config, rate120, 120); });
                if (cap60Selected) guardedPhase("capBudget60", [&] { runCapBudgetChecks(config, rate120, 60); });
            } else if (capSelected || cap60Selected) {
                notVerified("S13.183", "the fixture " + rate120 + " was not generated, so the cap's budget was not "
                                       "exercised");
            }
        } else if (clampSelected || clamp60Selected || capSelected || cap60Selected) {
            notVerified("S13.175-S13.176",
                        "the interval-clamp fixtures " + fast + " and " + gap +
                            " were not both generated, so neither bound on the gap between two pictures was "
                            "exercised -- a file declaring a thousand pictures a second would go ungraded");
        }
        if (phaseSelected("thinRate60")) {
            guardedPhase("thinRate60", [&] {
                for (const char* codec : {"h264", "hevc", "av1"}) runThinRateChecks(config, codec, 60);
            });
        }
        if (phaseSelected("thinRate120")) {
            guardedPhase("thinRate120", [&] {
                for (const char* codec : {"h264", "hevc", "av1"}) runThinRateChecks(config, codec, 120);
            });
        }
        if (phaseSelected("thinSlice")) guardedPhase("thinSlice", [&] { runThinSliceChecks(config, fast); });
        if (phaseSelected("thinShared")) guardedPhase("thinShared", [&] { runThinSharedChecks(config); });
        if (phaseSelected("capShared")) {
            guardedPhase("capShared", [&] {
                for (const double slow : {50.0, 75.0}) runCapSharedChecks(config, slow);
            });
        }
        if (phaseSelected("capNoTop")) guardedPhase("capNoTop", [&] { runCapNoTopChecks(config, fast); });
        const auto highRateAt = [&](int frameRate) {
            if (std::filesystem::exists(rate240)) {
                runHighRateChecks(config, rate240, frameRate);
            } else {
                notVerified("S15.109-S15.110", "the fixture " + rate240 +
                                                   " was not generated, so playback above the frame-rate cap was not "
                                                   "exercised");
            }
        };
        phase("highRate", [&] { highRateAt(120); });
        phase("highRate60", [&] { highRateAt(60); });
        phase("handoverLog", [&] { runHandoverLogChecks(config, clip); });
        phase("departure", [&] { runDepartureChecks(config, onePicture); });
        phase("resumeCredit", [&] { runResumeCreditChecks(config, clip, still, onePicture); });
        phase("memoryRetry", [&] { runMemoryRetryChecks(config, clips); });
        phase("keptStill", [&] { runKeptStillChecks(config, clips); });
        phase("positionVideo", [&] {
            runPositionVideoChecks(config, clip, onePicture);
            runStoppedRelayoutChecks(config);
        });
        phase("relaidRelease", [&] { runRelaidReleaseChecks(config, clip, onePicture); });
        phase("layoutDrag", [&] { runLayoutDragChecks(config); });
        phase("playbackRetry", [&] { runPlaybackRetryChecks(config, clips); });
        phase("gpuFault", [&] { runGpuFaultChecks(config, clips, still); });
        phase("shortage", [&] { runShortageChecks(config, clips); });
        phase("openAtRequest", [&] { runOpenAtRequestChecks(config, still); });
        // Two hides: one past the five seconds the rule before the fix tolerated between pictures,
        // one well inside it. The second consumes nothing, so the clearing half rides on the first.
        phase("cleanPlayHide6s",
              [&] { runCleanPlayHideChecks(config, clip, std::chrono::seconds(6), /*gradeClear=*/true); });
        phase("cleanPlayHide3s",
              [&] { runCleanPlayHideChecks(config, clip, std::chrono::seconds(3), /*gradeClear=*/false); });
        // At the three speeds only a file of seconds-long pictures makes waits long enough to land a change in.
        const auto onSlowFile = [&](const char* goal, auto run) {
            if (std::filesystem::exists(slow)) {
                run(config, slow);
            } else {
                notVerified(goal, "the fixture " + slow + " was not generated");
            }
        };
        phase("slowStallCredit", [&] { onSlowFile("S15.114", runSlowStallCreditChecks); });
        phase("midWaitRaise", [&] { onSlowFile("S15.118", runMidWaitRaiseChecks); });
        phase("renderPathRetime", [&] { onSlowFile("S15.119", runRenderPathRetimeChecks); });
        phase("dueSlowdown", [&] { onSlowFile("S15.120", runDueSlowdownChecks); });
        phase("playbackBlame", [&] { runPlaybackBlameChecks(config, clips, still, onePicture); });
        phase("decodeOnce", [&] { runDecodeOnceChecks(config, clip, onePicture); });
        phase("processorScan", [&] { runProcessorScanChecks(config, still); });
        phase("processorScanShared", [&] { runSharedScanChecks(config); });
        phase("sharedRelease", [&] { runSharedReleaseChecks(config, clip); });
        phase("sharedEviction", [&] { runSharedEvictionChecks(config, clips); });
        phase("pauseRejoin", [&] { runPauseRejoinChecks(config); });
        phase("sharedFailure", [&] { runSharedFailureChecks(config, still); });
        phase("sharedSpeed", [&] { runSharedSpeedChecks(config); });
        phase("sharedGpuFault", [&] { runSharedGpuFaultChecks(config, onePicture); });
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
