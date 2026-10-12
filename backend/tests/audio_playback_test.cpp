// SPDX-License-Identifier: GPL-2.0-or-later
//
// A video's sound, in four tiers: AudioDecoder alone; AudioOutput against a PipeWire server this test
// starts; the daemon's steering of it (WallpaperManager and its RenderWorker, in-process); and fatal
// signals on the sound thread, in children of this binary.
//
// Every PipeWire this binary talks to is one it starts itself, in the runtime directory isolated_run.sh
// made for the entry, and stops before the phase ends. It refuses to make a sound client anywhere
// else, and never removes a socket it did not make. The entry has no session manager, so a stand-in
// here links each stream to the server's null sink. What the code under test hands PipeWire is
// recorded by interposing pw_stream_queue_buffer: every buffer, with the time PipeWire reports it will
// be heard. That time is read from pw_stream_get_time_n by the same formula production uses
// (heardAtOfNextBuffer in renderer/audio/audio_output.cpp), so every timing check here assumes
// production reads that report correctly: a misreading of its units or latency terms moves both sides
// alike and is not detected. Nothing here measures when a frame really leaves the sink.

#include "audio/audio_decoder.h"
#include "audio/audio_output.h"
#include "daemon/crash_record.h"
#include "daemon/dbus_service.h"
#include "daemon/render_worker.h"
#include "daemon/wallpaper_manager.h"

#include "support/forked_child.h"
#include "tests/client/daemon_client.h"
#include "tests/client/scratch_dir.h"
#include "tests/self_exec.h"
#include "tests/stream_capture.h"
#include "tests/test_phase.h"
#include "tests/test_report.h"

#include <nlohmann/json.hpp>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/param.h>
#include <spa/param/port-config.h>
#include <spa/pod/builder.h>

#include <sdbus-c++/sdbus-c++.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// ---------------------------------------------------------------------------------------------- //
// What the code under test hands PipeWire and FFmpeg, recorded or broken on purpose
// ---------------------------------------------------------------------------------------------- //

namespace probe {

int64_t nowNs() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1'000'000'000LL + t.tv_nsec;
}

struct BufferRecord {
    int64_t queuedAt = 0;
    int64_t heardAt = 0;  // when its first frame is heard, as PipeWire reports it
    uint64_t first = 0;   // its first frame's index in g_left
    uint32_t frames = 0;
    float peak = 0.0f;
};

constexpr size_t kMaxRecords = size_t{1} << 15;
constexpr uint64_t kMaxFrames = 48000ULL * 90;
std::mutex g_recordMutex;
BufferRecord g_records[kMaxRecords];
float g_left[kMaxFrames];
float g_right[kMaxFrames];
size_t g_recordCount = 0;
uint64_t g_frameCount = 0;
bool g_overflow = false;
std::atomic<int64_t> g_lastLoudQueuedAt{0};

std::atomic<int> g_streamsMade{0};
std::atomic<bool> g_faultInConnect{false};
// pw_stream_connect calls still to fail, as a server refusing a stream does.
std::atomic<int> g_refuseConnects{0};
std::atomic<int> g_streamsDestroyed{0};

// What AudioOutput gave each stream and context. Its contexts are told from the stand-in's by their
// application name.
struct StreamListener {
    pw_stream* stream = nullptr;
    const pw_stream_events* events = nullptr;
    void* data = nullptr;
};
std::mutex g_pwMutex;
std::vector<std::string> g_mediaNames;
std::vector<StreamListener> g_listeners;
std::set<pw_context*> g_engineContexts;
std::atomic<int> g_engineContextsMade{0};
std::atomic<int> g_engineConnectAttempts{0};

// Seeks in a sound stream (AudioDecoder's; the picture decoder seeks its video stream): when, and to
// what file time.
struct SoundSeek {
    int64_t at = 0;
    int64_t targetNs = 0;
};
std::mutex g_seekMutex;
std::vector<SoundSeek> g_soundSeeks;

// The next av_read_frame of a picture decoder sleeps this long, once; and the next
// g_slowPictureReads of them sleep g_slowPictureMs each, as reading a heavy file would.
std::atomic<int> g_stallPictureMs{0};
std::atomic<int> g_slowPictureReads{0};
std::atomic<int> g_slowPictureMs{0};

// swr_convert and swr_free are called by AudioDecoder alone, which runs on AudioOutput's thread alone.
enum SwrMode : int { kSwrPass = 0, kSwrFault, kSwrFailThenFaultFree, kSwrStallOnce, kSwrFail };
std::atomic<int> g_swrFailed{0};
std::atomic<uint64_t> g_swrCalls{0};
std::atomic<int> g_swrMode{kSwrPass};
std::atomic<int> g_stallMs{0};
std::atomic<int64_t> g_stallStartedAt{0};
std::atomic<int64_t> g_stallEndedAt{0};
std::atomic<bool> g_faultInSwrFree{false};

// av_find_best_stream for audio is AudioDecoder's alone (the picture decoder asks for video): refusing
// it stands for a file whose sound stream cannot be read.
std::atomic<bool> g_refuseAudioStream{false};
std::atomic<int> g_audioStreamsRefused{0};

std::atomic<int64_t> g_lastStreamMadeAt{0};

// read() fails with EIO on these descriptors, g_readFailuresArmed times, from the first read after the
// first av_seek_frame that follows g_armReadFailure.
std::atomic<int> g_failReadFds[4] = {-1, -1, -1, -1};
std::atomic<bool> g_armReadFailure{false};
std::atomic<int> g_readFailuresArmed{1};
std::atomic<int> g_readsToFail{0};
std::atomic<int> g_readsFailed{0};

// The next poll() whose first descriptor is an eventfd fails, as AudioOutput's own wait would.
std::atomic<bool> g_failEventPoll{false};

// Video streams opened are given no average rate and a guessed base rate of 1000 a second.
std::atomic<bool> g_unknownAverageRate{false};
std::atomic<int> g_averageRatesHidden{0};

// avformat_open_input calls on a URL containing g_openWatch.
std::mutex g_openMutex;
std::string g_openWatch;
std::atomic<int> g_opens{0};

// A real store to a page that cannot be written: SIGSEGV, never the trap a null store compiles to.
[[noreturn]] void faultNow() {
    void* page = ::mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page != MAP_FAILED) *static_cast<volatile char*>(page) = 1;
    std::abort();
}

void record(pw_stream* stream, pw_buffer* buffer) {
    if (buffer == nullptr || buffer->buffer == nullptr || buffer->buffer->n_datas == 0) return;
    const spa_data& d = buffer->buffer->datas[0];
    if (d.data == nullptr || d.chunk == nullptr) return;
    int64_t heard = nowNs();
    pw_time t{};
    if (pw_stream_get_time_n(stream, &t, sizeof t) == 0 && t.now != 0 && t.rate.denom != 0) {
        heard = t.now + std::max<int64_t>(t.delay, 0) * 1'000'000'000 * t.rate.num / t.rate.denom +
                static_cast<int64_t>(t.buffered + t.queued) * 1'000'000'000 / 48000;
    }
    const auto* samples = static_cast<const float*>(d.data);
    const uint32_t frames = d.chunk->size / (2 * sizeof(float));
    std::lock_guard<std::mutex> lock(g_recordMutex);
    if (g_recordCount >= kMaxRecords || g_frameCount + frames > kMaxFrames) {
        g_overflow = true;
        return;
    }
    float peak = 0.0f;
    for (uint32_t i = 0; i < frames; ++i) {
        g_left[g_frameCount + i] = samples[2 * i];
        g_right[g_frameCount + i] = samples[2 * i + 1];
        peak = std::max({peak, std::fabs(samples[2 * i]), std::fabs(samples[2 * i + 1])});
    }
    const int64_t queued = nowNs();
    g_records[g_recordCount++] = {queued, heard, g_frameCount, frames, peak};
    g_frameCount += frames;
    if (peak > 0.2f) g_lastLoudQueuedAt.store(queued);
}

}  // namespace probe

extern "C" int pw_stream_queue_buffer(pw_stream* stream, pw_buffer* buffer) {
    using Fn = int (*)(pw_stream*, pw_buffer*);
    static const Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "pw_stream_queue_buffer"));
    probe::record(stream, buffer);
    return real(stream, buffer);
}

extern "C" pw_stream* pw_stream_new(pw_core* core, const char* name, pw_properties* props) {
    using Fn = pw_stream* (*)(pw_core*, const char*, pw_properties*);
    static const Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "pw_stream_new"));
    probe::g_streamsMade.fetch_add(1);
    probe::g_lastStreamMadeAt.store(probe::nowNs());
    {
        const char* media = props != nullptr ? pw_properties_get(props, PW_KEY_MEDIA_NAME) : nullptr;
        std::lock_guard<std::mutex> lock(probe::g_pwMutex);
        probe::g_mediaNames.push_back(media != nullptr ? media : "(none)");
    }
    return real(core, name, props);
}

extern "C" int pw_stream_connect(pw_stream* stream, enum spa_direction direction, uint32_t target_id,
                                 enum pw_stream_flags flags, const struct spa_pod** params, uint32_t n_params) {
    using Fn = int (*)(pw_stream*, enum spa_direction, uint32_t, enum pw_stream_flags, const struct spa_pod**, uint32_t);
    static const Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "pw_stream_connect"));
    if (probe::g_faultInConnect.load()) probe::faultNow();
    for (int left = probe::g_refuseConnects.load(); left > 0;) {
        if (probe::g_refuseConnects.compare_exchange_weak(left, left - 1)) return -EPIPE;
    }
    return real(stream, direction, target_id, flags, params, n_params);
}

extern "C" void pw_stream_add_listener(pw_stream* stream, spa_hook* listener, const pw_stream_events* events,
                                       void* data) {
    using Fn = void (*)(pw_stream*, spa_hook*, const pw_stream_events*, void*);
    static const Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "pw_stream_add_listener"));
    {
        std::lock_guard<std::mutex> lock(probe::g_pwMutex);
        probe::g_listeners.push_back({stream, events, data});
    }
    real(stream, listener, events, data);
}

extern "C" void pw_stream_destroy(pw_stream* stream) {
    using Fn = void (*)(pw_stream*);
    static const Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "pw_stream_destroy"));
    probe::g_streamsDestroyed.fetch_add(1);
    real(stream);
}

extern "C" pw_context* pw_context_new(pw_loop* loop, pw_properties* props, size_t size) {
    using Fn = pw_context* (*)(pw_loop*, pw_properties*, size_t);
    static const Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "pw_context_new"));
    const char* app = props != nullptr ? pw_properties_get(props, PW_KEY_APP_NAME) : nullptr;
    const bool engine = app != nullptr && std::strcmp(app, "WallpaperX") == 0;
    pw_context* context = real(loop, props, size);
    if (engine) {
        probe::g_engineContextsMade.fetch_add(1);
        std::lock_guard<std::mutex> lock(probe::g_pwMutex);
        probe::g_engineContexts.insert(context);
    }
    return context;
}

extern "C" pw_core* pw_context_connect(pw_context* context, pw_properties* props, size_t size) {
    using Fn = pw_core* (*)(pw_context*, pw_properties*, size_t);
    static const Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "pw_context_connect"));
    {
        std::lock_guard<std::mutex> lock(probe::g_pwMutex);
        if (probe::g_engineContexts.count(context) != 0) probe::g_engineConnectAttempts.fetch_add(1);
    }
    return real(context, props, size);
}

extern "C" int avformat_seek_file(AVFormatContext* s, int index, int64_t min, int64_t ts, int64_t max, int flags) {
    using Fn = int (*)(AVFormatContext*, int, int64_t, int64_t, int64_t, int);
    static const Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "avformat_seek_file"));
    if (s != nullptr && index >= 0 && static_cast<unsigned>(index) < s->nb_streams &&
        s->streams[index]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
        const int64_t target = av_rescale_q(ts, s->streams[index]->time_base, AVRational{1, 1'000'000'000});
        std::lock_guard<std::mutex> lock(probe::g_seekMutex);
        probe::g_soundSeeks.push_back({probe::nowNs(), target});
    }
    return real(s, index, min, ts, max, flags);
}

// Only a picture decoder reads a container whose video stream it has not discarded.
extern "C" int av_read_frame(AVFormatContext* s, AVPacket* pkt) {
    using Fn = int (*)(AVFormatContext*, AVPacket*);
    static const Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "av_read_frame"));
    if ((probe::g_stallPictureMs.load() > 0 || probe::g_slowPictureReads.load() > 0) && s != nullptr) {
        bool pictures = false;
        for (unsigned i = 0; i < s->nb_streams; ++i) {
            const AVStream& st = *s->streams[i];
            if (st.codecpar->codec_type == AVMEDIA_TYPE_VIDEO && st.discard != AVDISCARD_ALL) pictures = true;
        }
        if (pictures) {
            if (const int ms = probe::g_stallPictureMs.exchange(0); ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            }
            if (probe::g_slowPictureReads.fetch_sub(1) > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(probe::g_slowPictureMs.load()));
            } else {
                probe::g_slowPictureReads.store(0);
            }
        }
    }
    return real(s, pkt);
}

extern "C" int swr_convert(struct SwrContext* s, uint8_t* const* out, int out_count, const uint8_t* const* in,
                           int in_count) {
    using Fn = int (*)(struct SwrContext*, uint8_t* const*, int, const uint8_t* const*, int);
    static const Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "swr_convert"));
    probe::g_swrCalls.fetch_add(1);
    switch (probe::g_swrMode.load()) {
        case probe::kSwrFault: probe::faultNow();
        case probe::kSwrFailThenFaultFree:
            probe::g_faultInSwrFree.store(true);
            return AVERROR(EIO);
        case probe::kSwrFail: probe::g_swrFailed.fetch_add(1); return AVERROR(EIO);
        case probe::kSwrStallOnce:
            if (probe::g_swrMode.exchange(probe::kSwrPass) == probe::kSwrStallOnce) {
                probe::g_stallStartedAt.store(probe::nowNs());
                std::this_thread::sleep_for(std::chrono::milliseconds(probe::g_stallMs.load()));
                probe::g_stallEndedAt.store(probe::nowNs());
            }
            break;
        default: break;
    }
    return real(s, out, out_count, in, in_count);
}

extern "C" void swr_free(struct SwrContext** s) {
    using Fn = void (*)(struct SwrContext**);
    static const Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "swr_free"));
    if (probe::g_faultInSwrFree.load()) probe::faultNow();
    real(s);
}

extern "C" int av_find_best_stream(AVFormatContext* ic, enum AVMediaType type, int wanted, int related,
                                   const AVCodec** decoder, int flags) {
    using Fn = int (*)(AVFormatContext*, enum AVMediaType, int, int, const AVCodec**, int);
    static const Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "av_find_best_stream"));
    if (type == AVMEDIA_TYPE_AUDIO && probe::g_refuseAudioStream.load()) {
        probe::g_audioStreamsRefused.fetch_add(1);
        return AVERROR_INVALIDDATA;
    }
    return real(ic, type, wanted, related, decoder, flags);
}

extern "C" ssize_t read(int fd, void* buffer, size_t count) {
    using Fn = ssize_t (*)(int, void*, size_t);
    static const Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "read"));
    if (fd >= 0) {
        for (const auto& f : probe::g_failReadFds) {
            if (f.load() != fd) continue;
            for (int left = probe::g_readsToFail.load(); left > 0;) {
                if (probe::g_readsToFail.compare_exchange_weak(left, left - 1)) {
                    probe::g_readsFailed.fetch_add(1);
                    errno = EIO;
                    return -1;
                }
            }
        }
    }
    return real(fd, buffer, count);
}

extern "C" int av_seek_frame(AVFormatContext* s, int stream, int64_t ts, int flags) {
    using Fn = int (*)(AVFormatContext*, int, int64_t, int);
    static const Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "av_seek_frame"));
    const int r = real(s, stream, ts, flags);
    if (probe::g_armReadFailure.exchange(false)) probe::g_readsToFail.store(probe::g_readFailuresArmed.load());
    return r;
}

extern "C" int poll(struct pollfd* fds, nfds_t n, int timeout) {
    using Fn = int (*)(struct pollfd*, nfds_t, int);
    static const Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "poll"));
    if (n >= 1 && probe::g_failEventPoll.load()) {
        char link[64];
        const ssize_t got = ::readlink(("/proc/self/fd/" + std::to_string(fds[0].fd)).c_str(), link, sizeof link - 1);
        if (got > 0) {
            link[got] = '\0';
            if (std::strcmp(link, "anon_inode:[eventfd]") == 0 && probe::g_failEventPoll.exchange(false)) {
                errno = EBADF;
                return -1;
            }
        }
    }
    return real(fds, n, timeout);
}

extern "C" int avformat_find_stream_info(AVFormatContext* ic, AVDictionary** options) {
    using Fn = int (*)(AVFormatContext*, AVDictionary**);
    static const Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "avformat_find_stream_info"));
    const int r = real(ic, options);
    if (r >= 0 && probe::g_unknownAverageRate.load()) {
        for (unsigned i = 0; i < ic->nb_streams; ++i) {
            AVStream& st = *ic->streams[i];
            if (st.codecpar->codec_type != AVMEDIA_TYPE_VIDEO) continue;
            st.avg_frame_rate = AVRational{0, 0};
            st.r_frame_rate = AVRational{1000, 1};
            probe::g_averageRatesHidden.fetch_add(1);
        }
    }
    return r;
}

extern "C" int avformat_open_input(AVFormatContext** ps, const char* url, const AVInputFormat* fmt,
                                   AVDictionary** options) {
    using Fn = int (*)(AVFormatContext**, const char*, const AVInputFormat*, AVDictionary**);
    static const Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "avformat_open_input"));
    {
        std::lock_guard<std::mutex> lock(probe::g_openMutex);
        if (url != nullptr && !probe::g_openWatch.empty() && std::strstr(url, probe::g_openWatch.c_str()) != nullptr) {
            probe::g_opens.fetch_add(1);
        }
    }
    return real(ps, url, fmt, options);
}

namespace {

using namespace std::chrono_literals;
using wallpaperx::AudioDecoder;
using wallpaperx::AudioOutput;
using wallpaperx::AudioTrack;
using wallpaperx::RenderWorker;
using wallpaperx::ScreenFocus;
using wallpaperx::WallpaperManager;
using wallpaperx::testing::harnessFailure;
using wallpaperx::testing::notVerified;
using wallpaperx::testing::phaseSelected;
using Clock = std::chrono::steady_clock;

constexpr int kRate = 48000;
constexpr int64_t kMs = 1'000'000;

void check(bool condition, const std::string& what, const std::string& detail = {}) {
    wallpaperx::testing::check(condition, what.c_str());
    if (!condition && !detail.empty()) std::fprintf(stderr, "  %s\n", detail.c_str());
}

std::string fmt(const char* format, double a, double b = 0, double c = 0) {
    char text[256];
    std::snprintf(text, sizeof text, format, a, b, c);
    return text;
}

int64_t ns(Clock::time_point t) { return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count(); }

std::string asset(const char* name) { return std::string(WALLPAPERX_VIDEO_ASSET_DIR) + "/" + name; }

// ---------------------------------------------------------------------------------------------- //
// Reading the record
// ---------------------------------------------------------------------------------------------- //

struct Capture {
    std::vector<probe::BufferRecord> buffers;
    std::vector<float> left;
    std::vector<float> right;
    bool overflow = false;

    int64_t heardAt(const probe::BufferRecord& b, uint64_t frame) const {
        return b.heardAt + static_cast<int64_t>(frame - b.first) * 1'000'000'000 / kRate;
    }
};

void resetCapture() {
    std::lock_guard<std::mutex> lock(probe::g_recordMutex);
    probe::g_recordCount = 0;
    probe::g_frameCount = 0;
    probe::g_overflow = false;
}

Capture takeCapture() {
    std::lock_guard<std::mutex> lock(probe::g_recordMutex);
    Capture c;
    c.buffers.assign(probe::g_records, probe::g_records + probe::g_recordCount);
    c.left.assign(probe::g_left, probe::g_left + probe::g_frameCount);
    c.right.assign(probe::g_right, probe::g_right + probe::g_frameCount);
    c.overflow = probe::g_overflow;
    return c;
}

struct Window {
    size_t buffers = 0;
    float peak = 0.0f;
};

// Buffers queued in [from, to).
Window windowOf(const Capture& c, int64_t from, int64_t to) {
    Window w;
    for (const probe::BufferRecord& b : c.buffers) {
        if (b.queuedAt < from || b.queuedAt >= to) continue;
        ++w.buffers;
        w.peak = std::max(w.peak, b.peak);
    }
    return w;
}

// Heard times of every frame above `threshold` that follows at least `quietBefore` frames below it:
// the start of each beep.
std::vector<int64_t> onsets(const Capture& c, float threshold, uint32_t quietBefore) {
    std::vector<int64_t> out;
    uint32_t quiet = quietBefore;
    for (const probe::BufferRecord& b : c.buffers) {
        for (uint64_t i = b.first; i < b.first + b.frames; ++i) {
            if (std::fabs(c.left[i]) > threshold) {
                if (quiet >= quietBefore) out.push_back(c.heardAt(b, i));
                quiet = 0;
            } else {
                ++quiet;
            }
        }
    }
    return out;
}

// The heard time of the first frame above `threshold` in a buffer queued at or after `from`.
std::optional<int64_t> firstSound(const Capture& c, int64_t from, float threshold) {
    for (const probe::BufferRecord& b : c.buffers) {
        if (b.queuedAt < from) continue;
        for (uint64_t i = b.first; i < b.first + b.frames; ++i) {
            if (std::fabs(c.left[i]) > threshold) return c.heardAt(b, i);
        }
    }
    return std::nullopt;
}

// Inside sound that has started: runs of silence longer than `longerThan` frames, and dips -- runs of
// at least `dipFrames` frames under `dipLevel` between louder frames. Counted from the first frame
// above `dipLevel`, over `analysed` frames, on the left channel or the right.
struct Continuity {
    std::vector<std::pair<int64_t, uint64_t>> gaps;  // heard time of the gap's start, its length in frames
    std::pair<int64_t, uint64_t> trailing{};         // silence still running at the end of the record
    size_t dips = 0;
    uint64_t analysed = 0;
};

Continuity continuity(const Capture& c, uint64_t longerThan, float dipLevel, uint64_t dipFrames, bool right = false) {
    Continuity out;
    bool started = false;
    uint64_t silentRun = 0;
    int64_t silentFrom = 0;
    uint64_t lowRun = 0;
    const std::vector<float>& samples = right ? c.right : c.left;
    for (const probe::BufferRecord& b : c.buffers) {
        for (uint64_t i = b.first; i < b.first + b.frames; ++i) {
            const float v = std::fabs(samples[i]);
            if (!started) {
                if (v < dipLevel) continue;
                started = true;
            }
            ++out.analysed;
            if (v <= 1e-3f) {
                if (silentRun == 0) silentFrom = c.heardAt(b, i);
                ++silentRun;
            } else {
                if (silentRun > longerThan) out.gaps.push_back({silentFrom, silentRun});
                silentRun = 0;
            }
            if (v < dipLevel) {
                ++lowRun;
            } else {
                if (lowRun >= dipFrames) ++out.dips;
                lowRun = 0;
            }
        }
    }
    if (silentRun > longerThan) out.trailing = {silentFrom, silentRun};
    return out;
}

// The largest sample heard in [from, to), on the left channel or the right.
float peakHeard(const Capture& c, int64_t from, int64_t to, bool right = false) {
    float peak = 0.0f;
    const std::vector<float>& s = right ? c.right : c.left;
    for (const probe::BufferRecord& b : c.buffers) {
        for (uint64_t i = b.first; i < b.first + b.frames; ++i) {
            const int64_t at = c.heardAt(b, i);
            if (at >= from && at < to) peak = std::max(peak, std::fabs(s[i]));
        }
    }
    return peak;
}

// The longest run of frames at or under 1e-3 heard in [from, to), and how many frames were heard there.
std::pair<uint64_t, uint64_t> longestSilence(const Capture& c, int64_t from, int64_t to, bool right) {
    uint64_t longest = 0, run = 0, heard = 0;
    const std::vector<float>& s = right ? c.right : c.left;
    for (const probe::BufferRecord& b : c.buffers) {
        for (uint64_t i = b.first; i < b.first + b.frames; ++i) {
            const int64_t at = c.heardAt(b, i);
            if (at < from || at >= to) continue;
            ++heard;
            run = std::fabs(s[i]) <= 1e-3f ? run + 1 : 0;
            longest = std::max(longest, run);
        }
    }
    return {longest, heard};
}

std::vector<probe::SoundSeek> soundSeeksSince(int64_t from) {
    std::lock_guard<std::mutex> lock(probe::g_seekMutex);
    std::vector<probe::SoundSeek> out;
    for (const probe::SoundSeek& s : probe::g_soundSeeks) {
        if (s.at >= from) out.push_back(s);
    }
    return out;
}

// ---------------------------------------------------------------------------------------------- //
// A private PipeWire, and a stand-in session manager that links its streams
// ---------------------------------------------------------------------------------------------- //

// Everything that could point a sound client at a server other than one in XDG_RUNTIME_DIR: the list
// isolated_run.sh unsets, which test_isolation_wrapper checks this array against. Checked again here,
// so this binary never starts a server, or lets the daemon look for one, while any of them is set.
const char* const kSoundServerVariables[] = {
    "PIPEWIRE_RUNTIME_DIR", "PIPEWIRE_REMOTE", "PIPEWIRE_CORE", "PIPEWIRE_CONFIG_DIR", "PIPEWIRE_CONFIG_NAME",
    "PIPEWIRE_CONFIG_PREFIX", "PIPEWIRE_MODULE_DIR", "PIPEWIRE_PROPS", "PIPEWIRE_NODE", "SPA_PLUGIN_DIR",
    "SPA_DATA_DIR", "PULSE_SERVER", "PULSE_RUNTIME_PATH", "PULSE_STATE_PATH", "PULSE_COOKIE", "PULSE_CLIENTCONFIG",
    "PULSE_CONFIG_PATH", "PULSE_SINK", "PULSE_SOURCE", "JACK_DEFAULT_SERVER", "JACK_PROMISCUOUS_SERVER",
    "ALSA_CONFIG_PATH", "USERPROFILE",
};

// Why XDG_RUNTIME_DIR is not one this binary may make a sound client in, or nothing. Only the
// directory isolated_run.sh made for this entry will do: <its scratch>/wallpaperx-test.*/runtime,
// beside the scratch's owner lock file, private to this user, with the wrapper's record of the
// caller present. Refusing only known-bad directories (a login session's, the caller's) would accept
// a live session whose runtime directory lies elsewhere, where a server start removes its socket.
std::string runtimeDirFault() {
    // Present even when empty: the wrapper exports it whatever the caller had.
    const char* caller = std::getenv("WALLPAPERX_CALLER_XDG_RUNTIME_DIR");
    if (caller == nullptr) return "not run through isolated_run.sh (WALLPAPERX_CALLER_XDG_RUNTIME_DIR is absent)";
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (runtime == nullptr || runtime[0] != '/') return "XDG_RUNTIME_DIR is unset or not absolute";
    const std::filesystem::path dir = std::filesystem::path(runtime).lexically_normal();
    const std::string text = dir.string();
    if (text.rfind("/run/user/", 0) == 0) return "XDG_RUNTIME_DIR=" + text + " is a login session's runtime directory";
    const std::string callerDir = std::filesystem::path(caller).lexically_normal().string();
    if (!callerDir.empty() && (text == callerDir || text.rfind(callerDir + "/", 0) == 0)) {
        return "XDG_RUNTIME_DIR=" + text + " is the caller's own runtime directory or inside it";
    }
    const std::filesystem::path scratch = dir.parent_path();
    struct stat st {};
    if (dir.filename() != "runtime" || scratch.filename().string().rfind("wallpaperx-test.", 0) != 0 ||
        ::stat((scratch / "owner").c_str(), &st) != 0) {
        return "XDG_RUNTIME_DIR=" + text + " is not a runtime directory isolated_run.sh made (<scratch>/wallpaperx-test.*/" +
               "runtime beside its owner file)";
    }
    if (::stat(text.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != ::getuid() || (st.st_mode & 077) != 0) {
        return "XDG_RUNTIME_DIR=" + text + " is not a directory private to this user";
    }
    return {};
}

std::string soundIsolationFault() {
    if (std::string why = runtimeDirFault(); !why.empty()) return why;
    for (const char* name : kSoundServerVariables) {
        if (const char* value = std::getenv(name)) return std::string(name) + " is set (" + value + ")";
    }
    return {};
}

// A sound client may be made here only once soundIsolationFault() -- or, for the decoy phase that sets
// sound-server variables on purpose, runtimeDirFault() -- has nothing to say. A refusal is the
// harness's fault, never a skip.
bool soundClientAllowed(const char* goals, bool decoy = false) {
    const std::string why = decoy ? runtimeDirFault() : soundIsolationFault();
    if (why.empty()) return true;
    harnessFailure(std::string("no sound client was made for ") + goals + ": " + why +
                   " -- it could reach a sound server outside this entry");
    return false;
}

std::string runtimeSocket() { return std::string(std::getenv("XDG_RUNTIME_DIR")) + "/pipewire-0"; }

class PrivatePipeWire {
public:
    enum class Start { Running, Unavailable, Refused };

    ~PrivatePipeWire() { stop(); }

    Start start(std::string& why) {
        why = soundIsolationFault();
        if (!why.empty()) return Start::Refused;
        const std::string binary = WALLPAPERX_PIPEWIRE_BINARY;
        if (binary.empty() || ::access(binary.c_str(), X_OK) != 0) {
            why = "no pipewire binary was found when this build was configured";
            return Start::Unavailable;
        }
        const std::string socket = runtimeSocket();
        // GOAL: S15.540 -- no server is started, and no socket removed, anywhere but the runtime directory
        //       isolated_run.sh made for the entry, and never a pipewire-0 no server of this process made.
        //       False if a bare run with some other runtime directory deletes a live server's socket.
        // WHY-THIS-SHAPE: soundIsolationFault() above confirms the wrapper's directory rather than refusing
        //       known-bad ones, and the socket is removed only once this process has run a server here.
        // CONTROL: control_run.sh 2026-10-11 inside .claude/tools/sandbox.sh, reverted, each a change to
        //       renderer/tests/support/isolated_run.sh, phase engineRamp: WALLPAPERX_CALLER_XDG_RUNTIME_DIR not
        //       exported -> "FAIL (harness): no private sound server was started for S15.505 ...: not run
        //       through isolated_run.sh"; its runtime directory named `elsewhere` -> "... is not a runtime
        //       directory isolated_run.sh made"; a file planted as `pipewire-0` in it -> "... or its lock is
        //       already there and no server of this process made it; it is left alone".
        // Removed only once a server of this process's has run here, which is what could have left
        // them. Before that, one is someone else's: a server in a runtime directory not this entry's.
        struct stat existing {};
        if (s_started == 0 && (::lstat(socket.c_str(), &existing) == 0 || ::lstat((socket + ".lock").c_str(), &existing) == 0)) {
            why = socket + " or its lock is already there and no server of this process made it; it is left alone";
            return Start::Refused;
        }
        ::unlink(socket.c_str());
        ::unlink((socket + ".lock").c_str());
        const char* tmp = std::getenv("TMPDIR");
        m_log = std::string(tmp != nullptr && *tmp != '\0' ? tmp : "/tmp") + "/private-pipewire-" +
                std::to_string(++s_started) + ".log";
        std::vector<std::string> args{binary, "-c", WALLPAPERX_PRIVATE_PIPEWIRE_CONF};
        std::vector<char*> argv;
        for (std::string& a : args) argv.push_back(a.data());
        argv.push_back(nullptr);
        const pid_t parent = ::getpid();
        const pid_t pid = ::fork();
        if (pid < 0) {
            why = "fork failed";
            return Start::Unavailable;
        }
        if (pid == 0) {
            const int log = ::open(m_log.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
            if (log >= 0) {
                ::dup2(log, STDOUT_FILENO);
                ::dup2(log, STDERR_FILENO);
            }
            wallpaperx::testing::execChildOrDie(parent, argv[0], argv.data(), nullptr);
        }
        m_pid = pid;
        const auto deadline = Clock::now() + 5s;
        for (;;) {
            struct stat st {};
            if (::stat(socket.c_str(), &st) == 0 && S_ISSOCK(st.st_mode)) return Start::Running;
            int status = 0;
            if (::waitpid(m_pid, &status, WNOHANG) == m_pid) {
                m_pid = -1;
                why = "the private pipewire exited before opening its socket; its log: " + logTail();
                return Start::Unavailable;
            }
            if (Clock::now() >= deadline) {
                stop();
                why = "the private pipewire did not open " + socket + " within 5 s; its log: " + logTail();
                return Start::Unavailable;
            }
            std::this_thread::sleep_for(10ms);
        }
    }

    void stop() {
        if (m_pid <= 0) return;
        ::kill(m_pid, SIGTERM);
        int status = 0;
        const auto deadline = Clock::now() + 3s;
        while (::waitpid(m_pid, &status, WNOHANG) == 0) {
            if (Clock::now() >= deadline) {
                ::kill(m_pid, SIGKILL);
                ::waitpid(m_pid, &status, 0);
                break;
            }
            std::this_thread::sleep_for(10ms);
        }
        m_pid = -1;
    }

    std::string logTail() const {
        std::ifstream in(m_log);
        std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return all.size() > 600 ? all.substr(all.size() - 600) : all;
    }

private:
    static inline int s_started = 0;
    pid_t m_pid = -1;
    std::string m_log;
};

// What a session manager would do for a stream named "wallpaperx": give it DSP output ports and link
// them to the null sink's inputs, `delay` after the stream appears. Event-driven, on a loop of its own.
class StandInLinker {
public:
    ~StandInLinker() { stop(); }

    bool start(std::chrono::milliseconds delay, std::string& why) {
        m_delay = delay;
        pw_init(nullptr, nullptr);
        m_loop = pw_thread_loop_new("stand-in-linker", nullptr);
        if (m_loop == nullptr) {
            why = "pw_thread_loop_new failed";
            return false;
        }
        pw_properties* props = pw_properties_new("module.rt", "false", "support.dbus", "false", nullptr);
        m_context = props != nullptr ? pw_context_new(pw_thread_loop_get_loop(m_loop), props, 0) : nullptr;
        if (m_context == nullptr || pw_thread_loop_start(m_loop) != 0) {
            why = "the stand-in's PipeWire context could not be made";
            return false;
        }
        pw_thread_loop_lock(m_loop);
        m_core = pw_context_connect(m_context, nullptr, 0);
        if (m_core != nullptr) {
            m_registry = pw_core_get_registry(m_core, PW_VERSION_REGISTRY, 0);
            static const pw_registry_events events = [] {
                pw_registry_events e{};
                e.version = PW_VERSION_REGISTRY_EVENTS;
                e.global = &StandInLinker::onGlobal;
                e.global_remove = &StandInLinker::onGlobalRemove;
                return e;
            }();
            pw_registry_add_listener(m_registry, &m_registryListener, &events, this);
            m_timer = pw_loop_add_timer(pw_thread_loop_get_loop(m_loop), &StandInLinker::onTimer, this);
        }
        pw_thread_loop_unlock(m_loop);
        if (m_core == nullptr) {
            why = std::string("the stand-in could not connect to the private pipewire (") + std::strerror(errno) + ")";
            return false;
        }
        return true;
    }

    void stop() {
        if (m_loop == nullptr) return;
        pw_thread_loop_lock(m_loop);
        for (pw_proxy* link : m_links) pw_proxy_destroy(link);
        m_links.clear();
        for (auto& [id, node] : m_nodes) {
            if (node.proxy != nullptr) pw_proxy_destroy(node.proxy);
        }
        m_nodes.clear();
        if (m_timer != nullptr) pw_loop_destroy_source(pw_thread_loop_get_loop(m_loop), m_timer);
        m_timer = nullptr;
        if (m_registry != nullptr) {
            spa_hook_remove(&m_registryListener);
            pw_proxy_destroy(reinterpret_cast<pw_proxy*>(m_registry));
            m_registry = nullptr;
        }
        if (m_core != nullptr) pw_core_disconnect(m_core);
        m_core = nullptr;
        pw_thread_loop_unlock(m_loop);
        pw_thread_loop_stop(m_loop);
        if (m_context != nullptr) pw_context_destroy(m_context);
        m_context = nullptr;
        pw_thread_loop_destroy(m_loop);
        m_loop = nullptr;
    }

private:
    struct Node {
        pw_proxy* proxy = nullptr;
        int64_t configureAt = 0;
        bool configured = false;
        bool linked = false;
    };
    struct Port {
        uint32_t node = 0;
        std::string name;
        bool output = false;
    };

    static void onGlobal(void* data, uint32_t id, uint32_t, const char* type, uint32_t, const spa_dict* props) {
        auto& self = *static_cast<StandInLinker*>(data);
        if (props == nullptr) return;
        if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
            const char* name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
            if (name == nullptr) return;
            if (std::strcmp(name, "test-sink") == 0) self.m_sink = id;
            if (std::strcmp(name, "wallpaperx") != 0) return;
            Node node;
            node.proxy = static_cast<pw_proxy*>(pw_registry_bind(self.m_registry, id, PW_TYPE_INTERFACE_Node,
                                                                 PW_VERSION_NODE, 0));
            node.configureAt = probe::nowNs() + self.m_delay.count() * kMs;
            self.m_nodes[id] = node;
            self.service();
        } else if (std::strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
            const char* node = spa_dict_lookup(props, PW_KEY_NODE_ID);
            const char* name = spa_dict_lookup(props, PW_KEY_PORT_NAME);
            const char* direction = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION);
            if (node == nullptr || name == nullptr || direction == nullptr) return;
            self.m_ports[id] = Port{static_cast<uint32_t>(std::strtoul(node, nullptr, 10)), name,
                                    std::strcmp(direction, "out") == 0};
            self.service();
        }
    }

    static void onGlobalRemove(void* data, uint32_t id) {
        auto& self = *static_cast<StandInLinker*>(data);
        self.m_ports.erase(id);
        if (const auto it = self.m_nodes.find(id); it != self.m_nodes.end()) {
            if (it->second.proxy != nullptr) pw_proxy_destroy(it->second.proxy);
            self.m_nodes.erase(it);
        }
    }

    static void onTimer(void* data, uint64_t) { static_cast<StandInLinker*>(data)->service(); }

    // On the loop's thread: configures the streams that are due and links those whose ports exist.
    void service() {
        const int64_t now = probe::nowNs();
        int64_t nextDue = 0;
        for (auto& [id, node] : m_nodes) {
            if (!node.configured && node.configureAt <= now) {
                configure(node);
                node.configured = true;
            } else if (!node.configured && (nextDue == 0 || node.configureAt < nextDue)) {
                nextDue = node.configureAt;
            }
            if (node.configured && !node.linked && m_sink != SPA_ID_INVALID) node.linked = link(id);
        }
        if (nextDue != 0 && m_timer != nullptr) {
            const int64_t wait = std::max<int64_t>(nextDue - now, 1'000'000);
            timespec value{static_cast<time_t>(wait / 1'000'000'000), static_cast<long>(wait % 1'000'000'000)};
            pw_loop_update_timer(pw_thread_loop_get_loop(m_loop), m_timer, &value, nullptr, false);
        }
    }

    void configure(Node& node) {
        uint8_t buffer[1024];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
        spa_audio_info_raw info{};
        info.format = SPA_AUDIO_FORMAT_F32P;
        info.rate = kRate;
        info.channels = 2;
        info.position[0] = SPA_AUDIO_CHANNEL_FL;
        info.position[1] = SPA_AUDIO_CHANNEL_FR;
        spa_pod_frame frame{};
        spa_pod_builder_push_object(&b, &frame, SPA_TYPE_OBJECT_ParamPortConfig, SPA_PARAM_PortConfig);
        spa_pod_builder_add(&b, SPA_PARAM_PORT_CONFIG_direction, SPA_POD_Id(SPA_DIRECTION_OUTPUT),
                            SPA_PARAM_PORT_CONFIG_mode, SPA_POD_Id(SPA_PARAM_PORT_CONFIG_MODE_dsp), 0);
        spa_pod_builder_prop(&b, SPA_PARAM_PORT_CONFIG_format, 0);
        spa_format_audio_raw_build(&b, SPA_PARAM_Format, &info);
        const auto* param = static_cast<const spa_pod*>(spa_pod_builder_pop(&b, &frame));
        pw_node_set_param(reinterpret_cast<pw_node*>(node.proxy), SPA_PARAM_PortConfig, 0, param);
    }

    bool link(uint32_t streamNode) {
        int done = 0;
        for (const char* channel : {"FL", "FR"}) {
            std::optional<uint32_t> out, in;
            for (const auto& [id, port] : m_ports) {
                if (port.node == streamNode && port.output && port.name == std::string("output_") + channel) out = id;
                if (port.node == m_sink && !port.output && port.name == std::string("playback_") + channel) in = id;
            }
            if (!out || !in) return false;
            if (m_linkedPorts.count(*out) != 0) {
                ++done;
                continue;
            }
            const std::string o = std::to_string(*out), i = std::to_string(*in), on = std::to_string(streamNode),
                              inNode = std::to_string(m_sink);
            pw_properties* props =
                pw_properties_new(PW_KEY_LINK_OUTPUT_NODE, on.c_str(), PW_KEY_LINK_OUTPUT_PORT, o.c_str(),
                                  PW_KEY_LINK_INPUT_NODE, inNode.c_str(), PW_KEY_LINK_INPUT_PORT, i.c_str(), nullptr);
            if (props == nullptr) return false;
            auto* proxy = static_cast<pw_proxy*>(pw_core_create_object(m_core, "link-factory", PW_TYPE_INTERFACE_Link,
                                                                       PW_VERSION_LINK, &props->dict, 0));
            pw_properties_free(props);
            if (proxy == nullptr) return false;
            m_links.push_back(proxy);
            m_linkedPorts.insert(*out);
            ++done;
        }
        return done == 2;
    }

    std::chrono::milliseconds m_delay{0};
    pw_thread_loop* m_loop = nullptr;
    pw_context* m_context = nullptr;
    pw_core* m_core = nullptr;
    pw_registry* m_registry = nullptr;
    spa_hook m_registryListener{};
    spa_source* m_timer = nullptr;
    uint32_t m_sink = SPA_ID_INVALID;
    std::map<uint32_t, Node> m_nodes;
    std::map<uint32_t, Port> m_ports;
    std::set<uint32_t> m_linkedPorts;
    std::vector<pw_proxy*> m_links;
};

// A private server and, unless `link` is false, the stand-in linking its streams. `ready` is false
// after it has said why: a refusal is the harness's fault, a missing server a skip.
struct SoundServer {
    SoundServer(const char* goals, bool link = true, std::chrono::milliseconds linkDelay = 0ms) {
        std::string why;
        switch (server.start(why)) {
            case PrivatePipeWire::Start::Refused:
                harnessFailure(std::string("no private sound server was started for ") + goals + ": " + why +
                               " -- the entry's environment could lead a sound client outside it");
                return;
            case PrivatePipeWire::Start::Unavailable: notVerified(goals, why); return;
            case PrivatePipeWire::Start::Running: break;
        }
        if (link && !linker.start(linkDelay, why)) {
            notVerified(goals, "the stand-in session manager could not start: " + why);
            return;
        }
        ready = true;
    }
    ~SoundServer() {
        linker.stop();
        server.stop();
    }
    PrivatePipeWire server;
    StandInLinker linker;
    bool ready = false;
};

// ---------------------------------------------------------------------------------------------- //
// The daemon, in-process
// ---------------------------------------------------------------------------------------------- //

struct Commit {
    uint64_t generation = 0;
    int64_t at = 0;
};

WallpaperManager::Config smallConfig() {
    WallpaperManager::Config config;
    config.width = 64;
    config.height = 48;
    // Nothing collects the pictures here, so "nobody is watching" must not stop them.
    config.unwatchedAfter = 1h;
    config.idleSessionRelease = 60s;
    config.pausedSessionRelease = 60s;
    return config;
}

struct Daemon {
    explicit Daemon(WallpaperManager::Config config)
        : manager(std::move(config)), worker(manager, [this](int, uint64_t generation) {
              std::lock_guard<std::mutex> lock(commitMutex);
              commits.push_back({generation, probe::nowNs()});
          }) {}
    ~Daemon() { worker.stop(); }

    std::vector<Commit> takeCommits() {
        std::lock_guard<std::mutex> lock(commitMutex);
        return commits;
    }
    size_t commitCount() {
        std::lock_guard<std::mutex> lock(commitMutex);
        return commits.size();
    }
    // Monitor 0 on DP-1 playing `file`, with `focus` reported first when there is one.
    void play(const std::string& file, std::optional<ScreenFocus> focus) {
        std::vector<int> redraw;
        manager.setMonitorOutput(0, "DP-1", redraw);
        if (focus) manager.setOutputFocus(":1.5", "DP-1", *focus, false);
        manager.requestWallpaper("video", file, 0);
        worker.post(0);
    }
    void focus(ScreenFocus f, bool maximized = false) {
        manager.setOutputFocus(":1.5", "DP-1", f, maximized);
        worker.wake();
    }
    void properties(const wallpaperx::WallpaperProperties& p) {
        manager.setWallpaperProperties(0, p);
        worker.wake();
    }
    bool waitForCommits(size_t n, std::chrono::milliseconds limit) {
        const auto deadline = Clock::now() + limit;
        while (commitCount() < n) {
            if (Clock::now() >= deadline) return false;
            std::this_thread::sleep_for(10ms);
        }
        return true;
    }

    std::mutex commitMutex;
    std::vector<Commit> commits;
    WallpaperManager manager;
    RenderWorker worker;
};

// The descriptors open on `file`, by its real path; nothing when /proc/self/fd cannot be read.
std::optional<std::vector<int>> descriptorList(const std::string& file) {
    std::error_code ec;
    const std::string want = std::filesystem::canonical(file, ec).string();
    DIR* dir = ::opendir("/proc/self/fd");
    if (dir == nullptr) return std::nullopt;
    std::vector<int> out;
    char target[4096];
    while (const dirent* e = ::readdir(dir)) {
        const char* end = e->d_name + std::strlen(e->d_name);
        int fd = -1;
        if (std::from_chars(e->d_name, end, fd).ptr != end || fd < 0) continue;
        const ssize_t n = ::readlink((std::string("/proc/self/fd/") + e->d_name).c_str(), target, sizeof target - 1);
        if (n <= 0) continue;
        target[n] = '\0';
        if (want == target) out.push_back(fd);
    }
    ::closedir(dir);
    return out;
}

// -1 when they cannot be listed, so a count compared with 0 cannot pass on a failed listing.
int descriptorsOn(const std::string& file) {
    const auto list = descriptorList(file);
    return list ? static_cast<int>(list->size()) : -1;
}

// Polls `done` every 20 ms until it holds or `limit` passes; false on the deadline.
bool waitFor(std::chrono::milliseconds limit, const std::function<bool()>& done) {
    const auto deadline = Clock::now() + limit;
    while (!done()) {
        if (Clock::now() >= deadline) return false;
        std::this_thread::sleep_for(20ms);
    }
    return true;
}

// ---------------------------------------------------------------------------------------------- //
// The daemon's sound states, step by step
// ---------------------------------------------------------------------------------------------- //

enum class Expect { Heard, Silent, Stopped };

struct Step {
    std::string what;
    std::function<void()> apply;
    Expect expect;
    int settleMs = 400;
    int measureMs = 600;
};

struct StepResult {
    int64_t applied = 0, from = 0, to = 0;
    uint64_t swrBefore = 0, swrAfter = 0;
    Window window;
};

std::vector<StepResult> runSteps(const std::vector<Step>& steps) {
    std::vector<StepResult> results;
    for (const Step& step : steps) {
        StepResult r;
        r.applied = probe::nowNs();
        step.apply();
        std::this_thread::sleep_for(std::chrono::milliseconds(step.settleMs));
        r.from = probe::nowNs();
        r.swrBefore = probe::g_swrCalls.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(step.measureMs));
        r.swrAfter = probe::g_swrCalls.load();
        r.to = probe::nowNs();
        results.push_back(r);
    }
    const Capture cap = takeCapture();
    for (StepResult& r : results) r.window = windowOf(cap, r.from, r.to);
    return results;
}

// Heard: loud and decoded. Silent: no sample above silence and nothing decoded. Stopped: silent from
// within a round of the change, and, once its fade is done, no buffer queued at all.
void gradeSteps(const std::string& rule, const std::vector<Step>& steps, const std::vector<StepResult>& results,
                const Capture& cap) {
    for (size_t i = 0; i < steps.size() && i < results.size(); ++i) {
        const Step& s = steps[i];
        const StepResult& r = results[i];
        const std::string detail = fmt("peak %.4f over %.0f buffers, %.0f swr_convert calls", r.window.peak,
                                       static_cast<double>(r.window.buffers),
                                       static_cast<double>(r.swrAfter - r.swrBefore));
        std::printf("audio_playback_test: %s, %s: %s\n", rule.c_str(), s.what.c_str(), detail.c_str());
        if (s.expect == Expect::Heard) {
            check(r.window.peak >= 0.25f, rule + ", " + s.what + ": the screen is heard", detail);
            continue;
        }
        if (s.expect == Expect::Silent) {
            check(r.window.peak <= 1e-4f, rule + ", " + s.what + ": the screen is silent", detail);
            check(r.swrAfter == r.swrBefore, rule + ", " + s.what + ": nothing of its sound is decoded", detail);
            continue;
        }
        const Window soon = windowOf(cap, r.applied + 100 * kMs, r.to);
        check(soon.peak <= 1e-4f, rule + ", " + s.what + ": the sound goes silent within a round",
              fmt("peak %.4f from 100 ms after the change", soon.peak));
        const Window later = windowOf(cap, r.applied + 300 * kMs, r.to);
        check(later.buffers == 0, rule + ", " + s.what + ": its stream stops being scheduled",
              fmt("%.0f buffers queued from 300 ms after the change", static_cast<double>(later.buffers)));
    }
}

// How long a heard video's sound may take to reach PipeWire after its third picture. A check that a
// video stays silent watches at least this long, or it would pass a sound that is merely late.
constexpr std::chrono::milliseconds kSoundArrivalDeadline{5000};

// Starts the daemon on `file` with the desktop focused and waits for its first heard sound, or says
// why it could not.
bool startHeard(Daemon& d, const std::string& file, const char* goals) {
    resetCapture();
    const int64_t started = probe::nowNs();
    d.play(file, ScreenFocus::Desktop);
    if (!d.waitForCommits(3, 15s)) {
        notVerified(goals, "the video drew no pictures within 15 s, so its sound has nothing to follow");
        return false;
    }
    if (!waitFor(kSoundArrivalDeadline, [&] { return probe::g_lastLoudQueuedAt.load() > started; })) {
        check(false, std::string(goals) + fmt(": a heard video's sound reaches PipeWire within %.0f s of its third picture",
                                              kSoundArrivalDeadline.count() / 1000.0));
        return false;
    }
    return true;
}

// GOAL: S15.493 -- sound reaches PipeWire exactly while the screen would be heard: silent with no focus
//       report, with focus "none", with a maximized window, with the wallpaper muted, with sound off on
//       every screen or on this one; heard with the desktop or a window focused and everything on. And
//       while it is not heard, none of its sound is decoded. False if any not-heard state still sounds
//       or decodes, or any heard state stays silent.
// WHY-THIS-SHAPE: each state is a window measured after a settle, on the tone fixture, so "heard" is a
//       peak near 0.5 and "silent" is exactly no sample; decoding is counted at swr_convert, which only
//       the sound decoder calls -- an audio descriptor's offset would not move for a file this small,
//       which FFmpeg reads whole. Each heard step follows a silent one, so a sound that never stops
//       cannot pass the silent steps, and one that never starts cannot pass the heard ones.
// CONTROL: control_run.sh 2026-10-11, reverted, in soundWanted(), backend/daemon/wallpaper_manager.cpp:
//       `wanted.heard &&` deleted -> "FAIL: heard rule, before any focus report: the screen is silent" (peak
//       0.3538) and the same for the other not-heard states; `wanted.heard` made `!wanted.heard` -> "FAIL:
//       heard rule, desktop focused: the screen is heard" (no buffers) beside the not-heard states. The
//       decode line, in heardTrackLocked: the not-wanted branch keeping its track at volume 0 instead of
//       stopping it -> "FAIL: heard rule, focus none: nothing of its sound is decoded" (peak 0.0000, 27
//       swr_convert calls) and the same for the other not-heard states after a heard one, the peaks green.
void runHeardChecks() {
    const char* kGoals = "S15.493 (sound only while heard)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    Daemon d(smallConfig());
    resetCapture();
    d.play(asset("wallpaper_tone.mp4"), std::nullopt);
    if (!d.waitForCommits(3, 15s)) {
        notVerified(kGoals, "the video drew no pictures within 15 s");
        return;
    }
    const std::vector<Step> steps = {
        {"before any focus report", [] {}, Expect::Silent},
        {"desktop focused", [&] { d.focus(ScreenFocus::Desktop); }, Expect::Heard, 900},
        {"focus none", [&] { d.focus(ScreenFocus::None); }, Expect::Silent},
        {"a window focused", [&] { d.focus(ScreenFocus::Window); }, Expect::Heard},
        {"a maximized window", [&] { d.focus(ScreenFocus::Window, true); }, Expect::Silent},
        {"desktop focused again", [&] { d.focus(ScreenFocus::Desktop); }, Expect::Heard},
        {"the wallpaper muted", [&] { d.properties({{"mute", true}}); }, Expect::Silent},
        {"the wallpaper unmuted", [&] { d.properties({}); }, Expect::Heard},
        {"sound off on every screen", [&] { d.manager.setSound(false); d.worker.wake(); }, Expect::Silent},
        {"sound on on every screen", [&] { d.manager.setSound(true); d.worker.wake(); }, Expect::Heard},
        {"sound off on this screen", [&] { d.manager.setMonitorSound(0, false); d.worker.wake(); }, Expect::Silent},
        {"sound on on this screen", [&] { d.manager.setMonitorSound(0, true); d.worker.wake(); }, Expect::Heard},
    };
    const std::vector<StepResult> results = runSteps(steps);
    gradeSteps("heard rule", steps, results, takeCapture());
}

// GOAL: S15.494 -- pausing the screen (always_paused) or hiding it stops its sound within a round: silent,
//       and once faded its stream is no longer scheduled at all, long before the session's release
//       delay; play and show bring it back. False if the sound plays on while the pictures are stopped, or
//       its stream keeps running silent.
// WHY-THIS-SHAPE: both release delays are a minute, so neither a release nor a closed track can be what
//       stops it. "Not scheduled" is no buffer queued at all, which a stream left active but fed silence
//       fails, as "silent" alone would not.
// CONTROL: control_run.sh 2026-10-11, reverted: in backend/daemon/wallpaper_manager.cpp
//       advanceSessionLocked, the stopped round's `session.audio->stop()` (the RenderOwed-aware line) deleted
//       -> "FAIL: stop rule, always_paused: the sound goes silent within a round" (peak 0.3539) and the same
//       for the hidden screen; in renderer/audio/audio_output.cpp service() (a line this round left
//       unchanged, so not re-run), `setActive(t, false);` deleted -> "FAIL: stop rule,
//       always_paused: its stream stops being scheduled" (47 buffers) and the same for the hidden screen.
void runStopChecks() {
    const char* kGoals = "S15.494 (stopped pictures stop the sound)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    Daemon d(smallConfig());
    if (!startHeard(d, asset("wallpaper_tone.mp4"), kGoals)) return;
    const std::vector<Step> steps = {
        {"playing", [] {}, Expect::Heard, 0},
        {"always_paused", [&] { d.manager.setMonitorPause(0, "always_paused"); d.worker.wake(); }, Expect::Stopped, 300,
         1000},
        {"playing again", [&] { d.manager.setMonitorPause(0, "play"); d.worker.wake(); }, Expect::Heard},
        {"screen hidden", [&] { d.manager.setOutputVisible("DP-1", false); d.worker.wake(); }, Expect::Stopped, 300,
         1000},
        {"screen shown", [&] { d.manager.setOutputVisible("DP-1", true); d.worker.wake(); }, Expect::Heard},
    };
    const std::vector<StepResult> results = runSteps(steps);
    const Capture cap = takeCapture();
    // COVERAGE-ASSERT: the stream was really running before the stops, so "no buffer" is a change.
    check(results.size() == steps.size() && results[0].window.buffers >= 15,
          "stop rule: the stream was being scheduled while heard, before the stops",
          fmt("%.0f buffers", results.empty() ? 0.0 : static_cast<double>(results[0].window.buffers)));
    gradeSteps("stop rule", steps, results, cap);
}

// GOAL: S15.495 -- at any speed but 1 the screen is silent, and at 1 again it is heard. (Part 3 of the
//       chunk will change this.) False if a stretched video plays its sound unstretched.
// CONTROL: control_run.sh 2026-10-11, reverted: `wanted.speed == 1.0 &&` deleted from soundWanted() in
//       backend/daemon/wallpaper_manager.cpp -> "FAIL: speed rule, speed 0.5: the screen is silent" (peak 0.3538)
//       and speed 2.
void runSpeedChecks() {
    const char* kGoals = "S15.495 (silent at a speed other than 1)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    Daemon d(smallConfig());
    if (!startHeard(d, asset("wallpaper_tone.mp4"), kGoals)) return;
    const std::vector<Step> steps = {
        {"speed 0.5", [&] { d.properties({{"speed", 0.5}}); }, Expect::Silent},
        {"speed 1", [&] { d.properties({}); }, Expect::Heard},
        {"speed 2", [&] { d.properties({{"speed", 2.0}}); }, Expect::Silent},
        {"speed 1 again", [&] { d.properties({{"speed", 1.0}}); }, Expect::Heard},
    };
    const std::vector<StepResult> results = runSteps(steps);
    gradeSteps("speed rule", steps, results, takeCapture());
}

// GOAL: S15.496 -- the wallpaper's volume v plays at v cubed of full level: 0.5 at an eighth of the peak
//       at 1. Volume 0 counts as not heard: silent within a round and its stream no longer scheduled.
//       False for a linear or ignored volume, or a volume 0 that keeps a stream running at zero gain.
// WHY-THIS-SHAPE: a ratio of two settled peaks of the same tone, so the fixture's own level cancels. The
//       volume 0 step is graded as a stop (no buffer queued), since a silent peak alone passes a stream
//       still fed at zero gain.
// CONTROL: control_run.sh 2026-10-12, reverted, backend/daemon/wallpaper_manager.cpp: heardTrackLocked's
//       volume made `setVolume(1.0f)` -> "FAIL: volume rule: volume 0.5 plays at 0.5 cubed of full level"
//       (ratio 1.0212) alone; `&& volume > 0.0f` taken out of SoundWanted::audible() (wallpaper_manager.h) ->
//       "FAIL: volume rule, volume 0: its stream stops being scheduled" (47 buffers).
void runVolumeChecks() {
    const char* kGoals = "S15.496 (volume, cubed)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    Daemon d(smallConfig());
    if (!startHeard(d, asset("wallpaper_tone.mp4"), kGoals)) return;
    // The first two are graded by their ratio below, not as Heard: 0.5 cubed is under the Heard threshold.
    const std::vector<Step> steps = {
        {"volume 1", [&] { d.properties({{"volume", 1.0}}); }, Expect::Heard},
        {"volume 0.5", [&] { d.properties({{"volume", 0.5}}); }, Expect::Heard},
        {"volume 0", [&] { d.properties({{"volume", 0.0}}); }, Expect::Stopped, 300, 1000},
    };
    std::vector<StepResult> results = runSteps(steps);
    if (results.size() != 3) return;
    const float full = results[0].window.peak, half = results[1].window.peak, none = results[2].window.peak;
    std::printf("audio_playback_test: volume rule: peak %.4f at 1, %.4f at 0.5, %.6f at 0\n", full, half, none);
    // The fixture's mono tone at 0.5, spread to two channels at -3 dB: 0.354.
    check(full >= 0.3f, "volume rule: at volume 1 the tone plays at its own level", fmt("peak %.4f", full));
    const double ratio = full > 0 ? half / full : 0;
    check(ratio > 0.105 && ratio < 0.145, "volume rule: volume 0.5 plays at 0.5 cubed of full level",
          fmt("ratio %.4f, wanted 0.125", ratio));
    gradeSteps("volume rule", {steps[2]}, {results[2]}, takeCapture());
}

// The Theil-Sen line through (n, at) -- the median of every pairwise slope, then the median
// intercept -- so up to about a quarter of the points can be far off it without moving it, where one
// late commit among six would drag a least-squares line by a third of its lateness. The fitted time of
// each point, and the largest distance of any point from it (printed, never graded).
std::pair<std::vector<int64_t>, int64_t> fitLine(const std::vector<std::pair<int64_t, int64_t>>& points) {
    const auto median = [](std::vector<double>& v) {
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
        return v[v.size() / 2];
    };
    const int64_t base = points.front().second;
    std::vector<double> slopes;
    for (size_t i = 0; i < points.size(); ++i) {
        for (size_t j = i + 1; j < points.size(); ++j) {
            if (points[j].first == points[i].first) continue;
            slopes.push_back(static_cast<double>(points[j].second - points[i].second) /
                             static_cast<double>(points[j].first - points[i].first));
        }
    }
    const double slope = median(slopes);
    std::vector<double> intercepts;
    for (const auto& [n, at] : points) intercepts.push_back(static_cast<double>(at - base) - slope * n);
    const double intercept = median(intercepts);
    std::vector<int64_t> fitted;
    int64_t worst = 0;
    for (const auto& [n, at] : points) {
        fitted.push_back(base + static_cast<int64_t>(std::llround(intercept + slope * n)));
        worst = std::max<int64_t>(worst, std::llabs(at - fitted.back()));
    }
    return {fitted, worst};
}

// Whether `commits` came one at a time, each generation the last plus one: what lets a generation
// say which picture it is. At least 30 of them; `why` says otherwise.
bool consecutiveCommits(const std::vector<Commit>& commits, std::string& why) {
    if (commits.size() < 30) {
        why = "only " + std::to_string(commits.size()) + " pictures were committed";
        return false;
    }
    for (size_t i = 1; i < commits.size(); ++i) {
        if (commits[i].generation != commits[i - 1].generation + 1) {
            why = fmt("generation %.0f followed %.0f, so a generation no longer says which picture it is",
                      static_cast<double>(commits[i].generation), static_cast<double>(commits[i - 1].generation));
            return false;
        }
    }
    return true;
}

// For every commit, the time its picture was scheduled for and its index in the pass, when the
// pictures came one at a time at about 30 a second from the first one. The scheduled time is the
// commit's place on the robust line through all of them, so a late wake of the render thread moves no
// picture's time. Empty, with `why`, otherwise.
std::map<uint64_t, std::pair<int64_t, int>> pictureIndex(const std::vector<Commit>& commits, int picturesPerPass,
                                                         std::string& why) {
    std::map<uint64_t, std::pair<int64_t, int>> out;
    if (!consecutiveCommits(commits, why)) return out;
    for (size_t i = 1; i < commits.size(); ++i) {
        const int64_t gap = commits[i].at - commits[i - 1].at;
        if (gap < 15 * kMs || gap > 55 * kMs) {
            why = fmt("the pictures were not committed at about 30 a second (generation %.0f came %.1f ms after the one "
                      "before), so a generation's place in time is not known",
                      static_cast<double>(commits[i].generation), gap / 1e6);
            return out;
        }
    }
    const uint64_t first = commits.front().generation;
    std::vector<std::pair<int64_t, int64_t>> points;
    for (const Commit& c : commits) points.push_back({static_cast<int64_t>(c.generation - first), c.at});
    const auto [fitted, worst] = fitLine(points);
    std::printf("audio_playback_test: the %zu commits sit at most %.1f ms from the line through them\n", commits.size(),
                worst / 1e6);
    for (size_t i = 0; i < commits.size(); ++i) {
        out[commits[i].generation] = {fitted[i], static_cast<int>(points[i].first % picturesPerPass)};
    }
    return out;
}

// Each beep heard after `from` set against the scheduled time of the picture at its timestamp: the
// nearest picture whose index is a multiple of `picturesPerBeep`. Returns how many were graded, -1
// when the run cannot be graded.
int gradeBeepsAgainstPictures(const std::string& rule, const Capture& cap, const std::vector<Commit>& commits,
                              int picturesPerPass, int picturesPerBeep, int64_t from, double toleranceMs,
                              int& passStartsGraded) {
    std::string why;
    const auto index = pictureIndex(commits, picturesPerPass, why);
    if (index.empty()) {
        notVerified(rule.c_str(), why);
        return -1;
    }
    int graded = 0;
    passStartsGraded = 0;
    for (const int64_t heard : onsets(cap, 0.1f, 480)) {
        if (heard < from) continue;
        std::optional<std::pair<uint64_t, std::pair<int64_t, int>>> best;
        for (const auto& [generation, picture] : index) {
            if (picture.second % picturesPerBeep != 0) continue;
            if (!best || std::llabs(heard - picture.first) < std::llabs(heard - best->second.first)) {
                best = {generation, picture};
            }
        }
        if (!best) continue;
        const int bestIndex = best->second.second;
        const double error = (heard - best->second.first) / 1e6;
        std::printf("audio_playback_test: %s: beep at picture %d heard %+.2f ms from its picture's scheduled time\n",
                    rule.c_str(), bestIndex, error);
        check(std::fabs(error) <= toleranceMs, rule + ": each beep is heard with the picture at its timestamp",
              fmt("picture %.0f: %+.2f ms", bestIndex, error));
        ++graded;
        if (bestIndex == 0 && best->first != commits.front().generation) ++passStartsGraded;
    }
    return graded;
}

// GOAL: S15.497 -- with the beeps fixture, each beep is heard within 12 ms of the scheduled time of the
//       picture at that beep's timestamp -- its place on the line through the run's commits -- across
//       loops. False for sound anchored a picture early or late (33 ms at 30 a second).
// WHY-THIS-SHAPE: which picture a commit is comes from its generation, valid only while pictures come
//       one by one at about 30 a second -- checked, and a run that dropped one is not graded rather than
//       mis-graded. Each beep is set against a robust (Theil-Sen) line through every commit, not its
//       picture's own commit, which carries the render thread's wake-up delay; a run is graded however
//       far single commits sit from that line, which is printed. Commit jitter itself is graded nowhere:
//       S13.183 and S15.109 grade pacing over a run, not one commit's distance from its schedule. The
//       nearest commit of ANY picture would not do: a beep a picture early sits on the one before.
// COVERAGE-ASSERT: at least eight beeps graded, and at least one at a pass start after a loop.
// CONTROL: control_run.sh 2026-10-11, reverted: ` - waiting.left` deleted from both the start anchor and the
//       schedule anchor in steerSoundLocked, backend/daemon/wallpaper_manager.cpp -> "FAIL: sync rule: each
//       beep is heard with the picture at its timestamp" (pictures 15, 30, 45: -33.33 ms).
void runSyncChecks() {
    const char* kGoals = "S15.497 (sound with its pictures)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    Daemon d(smallConfig());
    if (!startHeard(d, asset("wallpaper_beeps.mp4"), kGoals)) return;
    std::this_thread::sleep_for(5500ms);
    const std::vector<Commit> commits = d.takeCommits();
    const Capture cap = takeCapture();
    const std::optional<int64_t> first = firstSound(cap, 0, 1e-3f);
    if (!first) {
        check(false, "sync rule: the sound heard at the start is still in the record");
        return;
    }
    int passStarts = 0;
    const int graded = gradeBeepsAgainstPictures("sync rule", cap, commits, 60, 15, *first + 60 * kMs, 12.0, passStarts);
    if (graded < 0) return;
    check(graded >= 8 && passStarts >= 1, "sync rule: at least eight beeps were graded, one at a pass start after a loop",
          fmt("%.0f graded, %.0f at a pass start", graded, passStarts));
}

// GOAL: S15.498 -- a continuous tone plays through every loop with no silence longer than 10 ms, and
//       with no dip in level except the one switch fade at each loop: the sound is not moved while it
//       agrees with the pictures. False for a new pass anchored as a schedule restart (silence at the
//       loop) or a sound re-anchored every round (a dip at every picture).
// WHY-THIS-SHAPE: a dip is 24+ frames under 0.05 between louder frames -- a 440 Hz tone at 0.5 crosses
//       zero in two frames, and a 5 ms fade spends about 24 frames down there. One fade per loop is
//       allowed, plus two for a re-anchor a stall could earn. Loops are counted from the generations of
//       consecutive commits (60 pictures a pass), not from the timing line: counting needs no timing, so
//       a jittery run is still graded; only commits that skipped a generation leave it not verified.
//       The silence rule needs no pictures and is always graded.
// COVERAGE-ASSERT: at least six seconds of sound analysed; with consecutive commits, two loops crossed.
// CONTROL: control_run.sh 2026-10-11, reverted, in steerSoundLocked, backend/daemon/wallpaper_manager.cpp:
//       the drift path's `if (!next.startsPass) {` made `if (true) {` -> "FAIL: loop rule: no silence longer
//       than 10 ms inside the tone" (2 gaps; not re-run this round, its line untouched); `if
//       (std::chrono::abs(lead) <= kSoundDrift) return;` made `if (false) return;` -> "FAIL: loop rule: no
//       dip in the tone but one fade per loop" (219 dips, at most 5 allowed). Test-side, control_run.sh,
//       reverted: the pass counted as 600 pictures -> "FAIL: loop rule: the analysed sound crossed two loops"
//       (0 loops).
void runLoopChecks() {
    const char* kGoals = "S15.498 (a loop plays through)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    Daemon d(smallConfig());
    if (!startHeard(d, asset("wallpaper_tone.mp4"), kGoals)) return;
    std::this_thread::sleep_for(7000ms);
    const std::vector<Commit> commits = d.takeCommits();
    const Capture cap = takeCapture();
    const Continuity c = continuity(cap, 480, 0.05f, 24);
    constexpr uint64_t kPicturesPerPass = 60;
    std::string why;
    const bool counted = consecutiveCommits(commits, why);
    const int loops = counted ? static_cast<int>((commits.back().generation - commits.front().generation) / kPicturesPerPass) : 0;
    std::printf("audio_playback_test: loop rule: %.1f s analysed, %zu gaps over 10 ms, %zu dips, %d loops\n",
                c.analysed / double(kRate), c.gaps.size(), c.dips, loops);
    for (const auto& [at, frames] : c.gaps) std::printf("  a gap of %.2f ms\n", frames * 1000.0 / kRate);
    check(c.analysed >= 6ULL * kRate, "loop rule: at least six seconds of sound were analysed",
          fmt("%.1f s", c.analysed / double(kRate)));
    check(c.gaps.empty(), "loop rule: no silence longer than 10 ms inside the tone, at a loop or anywhere");
    if (!counted) {
        notVerified("S15.498 (one fade per loop)", why);
        return;
    }
    check(loops >= 2, "loop rule: the analysed sound crossed two loops", fmt("%.0f loops", loops));
    const int allowed = loops + 2;
    check(static_cast<int>(c.dips) <= allowed, "loop rule: no dip in the tone but one fade per loop",
          fmt("%.0f dips, at most %.0f allowed", static_cast<double>(c.dips), allowed));
}

// GOAL: S15.499 -- a session's audio descriptor goes when the session does: after a release while hidden
//       or paused the file has no descriptor open, and a session made again holds two (picture and sound).
//       False if a released track keeps its decoder, and with it the file.
// WHY-THIS-SHAPE: counted in /proc/self/fd by the file's real path, at times well past each one-second
//       release, since the audio thread closes the file after the session is gone.
// CONTROL: control_run.sh 2026-10-11, reverted: ~AudioTrack no longer setting `closed`
//       (renderer/audio/audio_output.cpp) -> "FAIL: release rule: after a release while hidden the file has
//       no descriptor open" (1) and "a session made again holds the file twice" (3).
void runReleaseChecks() {
    const char* kGoals = "S15.499 (a released session closes its sound)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    WallpaperManager::Config config = smallConfig();
    config.idleSessionRelease = 1s;
    config.pausedSessionRelease = 1s;
    Daemon d(config);
    const std::string file = asset("wallpaper_beeps.mp4");
    if (!startHeard(d, file, kGoals)) return;
    std::this_thread::sleep_for(500ms);
    const int playing = descriptorsOn(file);
    d.manager.setOutputVisible("DP-1", false);
    d.worker.wake();
    std::this_thread::sleep_for(2500ms);
    const int hidden = descriptorsOn(file);
    d.manager.setOutputVisible("DP-1", true);
    d.worker.wake();
    std::this_thread::sleep_for(1500ms);
    const int shown = descriptorsOn(file);
    d.manager.setMonitorPause(0, "always_paused");
    d.worker.wake();
    std::this_thread::sleep_for(2500ms);
    const int paused = descriptorsOn(file);
    std::printf("audio_playback_test: release rule: descriptors on the file: %d playing, %d hidden and released, %d "
                "shown, %d paused and released\n",
                playing, hidden, shown, paused);
    check(playing == 2, "release rule: a heard session holds the file twice, picture and sound", fmt("%.0f", playing));
    check(hidden == 0, "release rule: after a release while hidden the file has no descriptor open", fmt("%.0f", hidden));
    check(shown == 2, "release rule: a session made again holds the file twice", fmt("%.0f", shown));
    check(paused == 0, "release rule: after a release while paused the file has no descriptor open", fmt("%.0f", paused));
}

// GOAL: S15.500 -- a video with no sound plays its pictures and makes no PipeWire stream at all, and
//       decodes nothing of sound. False if a stream is made before the file is known to have sound.
// CONTROL: control_run.sh 2026-10-11, reverted: `ensureStream(t);` put first in AudioOutput::begin,
//       renderer/audio/audio_output.cpp -> "FAIL: no-sound rule: a video with no sound makes no PipeWire
//       stream" (1 stream made).
void runNoSoundChecks() {
    const char* kGoals = "S15.500 (a silent file makes no stream)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    Daemon d(smallConfig());
    const int streamsBefore = probe::g_streamsMade.load();
    const uint64_t swrBefore = probe::g_swrCalls.load();
    resetCapture();
    d.play(asset("wallpaper_bands.mp4"), ScreenFocus::Desktop);
    if (!d.waitForCommits(3, 15s)) {
        notVerified(kGoals, "the video drew no pictures within 15 s");
        return;
    }
    std::this_thread::sleep_for(1500ms);
    const size_t commits = d.commitCount();
    check(commits >= 10, "no-sound rule: a heard video with no sound plays its pictures",
          fmt("%.0f pictures in 1.5 s", static_cast<double>(commits)));
    check(probe::g_streamsMade.load() == streamsBefore, "no-sound rule: a video with no sound makes no PipeWire stream",
          fmt("%.0f streams made", static_cast<double>(probe::g_streamsMade.load() - streamsBefore)));
    check(probe::g_swrCalls.load() == swrBefore, "no-sound rule: and decodes nothing of sound");
}

// GOAL: S15.517 -- a heard video whose sound stream cannot be read plays its pictures, makes no PipeWire
//       stream, and says so in one line for the session, not again at each loop. False if a stream is
//       made before the sound is known to be readable, or the failed file is opened again every pass.
// WHY-THIS-SHAPE: the refusal is staged at av_find_best_stream for audio, which only the sound decoder
//       asks; a real file whose audio fails to open is not something ffmpeg will write. Four and a half
//       seconds of a two-second file, so two loops each send the track a new anchor to act on.
// COVERAGE-ASSERT: the refusal was reached at least once, so "no line, no stream" cannot pass unreached.
// CONTROL: control_run.sh 2026-10-11, reverted, in renderer/audio/audio_output.cpp AudioOutput::begin:
//       `ensureStream(t);` put first -> "FAIL: unreadable sound rule: no PipeWire stream is made" (1);
//       control_run.sh 2026-10-12, reverted: the open failure's `setContent(t, AudioTrack::Content::Failed);`
//       deleted -> "FAIL: unreadable sound rule: the failure is said once for the session, not at every loop"
//       (3 lines).
//       "The pictures play on" has no control: nothing on the sound thread can stop them short of a crash.
void runBadSoundChecks() {
    const char* kGoals = "S15.517 (unreadable sound)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    struct Restore {
        ~Restore() { probe::g_refuseAudioStream.store(false); }
    } restore;
    Daemon d(smallConfig());
    const int streamsBefore = probe::g_streamsMade.load();
    const int refusedBefore = probe::g_audioStreamsRefused.load();
    probe::g_refuseAudioStream.store(true);
    wallpaperx::testing::StreamCapture log(STDERR_FILENO);
    d.play(asset("wallpaper_beeps.mp4"), ScreenFocus::Desktop);
    const bool drew = d.waitForCommits(3, 15s);
    if (drew) std::this_thread::sleep_for(4500ms);
    const size_t commits = d.commitCount();
    log.restore();
    if (!drew) {
        notVerified(kGoals, "the video drew no pictures within 15 s");
        return;
    }
    const int refused = probe::g_audioStreamsRefused.load() - refusedBefore;
    const int lines = log.linesWith({"AudioOutput: no sound for a wallpaper:"});
    std::printf("audio_playback_test: unreadable sound rule: %zu pictures, %d refusals, %d lines, %d streams\n", commits,
                refused, lines, probe::g_streamsMade.load() - streamsBefore);
    check(refused >= 1, "unreadable sound rule: the sound decoder asked for the refused stream",
          fmt("%.0f refusals", refused));
    check(commits >= 90, "unreadable sound rule: the pictures play on", fmt("%.0f pictures in 4.5 s", commits));
    check(probe::g_streamsMade.load() == streamsBefore, "unreadable sound rule: no PipeWire stream is made",
          fmt("%.0f streams made", static_cast<double>(probe::g_streamsMade.load() - streamsBefore)));
    check(lines == 1, "unreadable sound rule: the failure is said once for the session, not at every loop",
          fmt("%.0f lines", lines));
}

// GOAL: S15.518 -- a screen shown again after its session was released is heard no earlier than the
//       first picture committed after the show. False if the sound of a re-made session starts before
//       its caught-up picture is on screen.
// WHY-THIS-SHAPE: the release is made certain -- a one-second idle release, two and a half seconds
//       hidden, and the file's descriptors counted at zero -- or the phase is not graded: a session that
//       was never released has no catch-up to precede.
//       The catch-up is slowed (ten picture reads of 40 ms): on the light fixture a sound started before
//       the catch-up is still heard after its quick picture, so a sound started early could not fail.
// CONTROL: control_run.sh 2026-10-11, reverted: advanceSessionLocked starting the sound at the catch-up,
//       before its decode and draw (`heardTrackLocked(...)->play({0, now})`), backend/daemon/wallpaper_manager.cpp
//       -> "FAIL: caught-up rule: the sound starts no earlier than the first picture after the show" (picture
//       +403.3 ms, sound +3.2 ms after the show). Test-side, by hand and reverted: the slow reads removed ->
//       "NOT VERIFIED -- S15.518 ... drew its picture 3.0 ms after the show".
void runCaughtUpChecks() {
    const char* kGoals = "S15.518 (sound after the caught-up picture)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    WallpaperManager::Config config = smallConfig();
    config.idleSessionRelease = 1s;
    Daemon d(config);
    const std::string file = asset("wallpaper_tone.mp4");
    if (!startHeard(d, file, kGoals)) return;
    d.manager.setOutputVisible("DP-1", false);
    d.worker.wake();
    std::this_thread::sleep_for(2500ms);
    if (const int open = descriptorsOn(file); open != 0) {
        notVerified(kGoals, fmt("the hidden session was not released (%.0f descriptors on its file)", open));
        return;
    }
    const size_t before = d.commitCount();
    // A heavy file's catch-up: the re-made session's first picture reads are slow, so a sound started
    // anywhere before that picture is drawn is heard well before it.
    probe::g_slowPictureMs.store(40);
    probe::g_slowPictureReads.store(10);
    const int64_t shownAt = probe::nowNs();
    d.manager.setOutputVisible("DP-1", true);
    d.worker.wake();
    std::this_thread::sleep_for(1500ms);
    probe::g_slowPictureReads.store(0);
    const std::vector<Commit> commits = d.takeCommits();
    const Capture cap = takeCapture();
    const std::optional<int64_t> heard = firstSound(cap, shownAt, 1e-3f);
    if (commits.size() <= before || !heard) {
        check(false, "caught-up rule: the screen shown again draws a picture and is heard",
              fmt("%.0f pictures after the show; heard: %.0f", static_cast<double>(commits.size() - before), heard ? 1 : 0));
        return;
    }
    const int64_t picture = commits[before].at;
    std::printf("audio_playback_test: caught-up rule: first picture %+.1f ms after the show, first sound %+.1f ms\n",
                (picture - shownAt) / 1e6, (*heard - shownAt) / 1e6);
    if (picture - shownAt < 150 * kMs) {
        notVerified(kGoals, fmt("the slowed catch-up drew its picture %.1f ms after the show, too soon for a sound started "
                                "before it to be told from one started after",
                                (picture - shownAt) / 1e6));
        return;
    }
    check(*heard >= picture, "caught-up rule: the sound starts no earlier than the first picture after the show",
          fmt("sound %+.2f ms from that picture's commit", (*heard - picture) / 1e6));
}

// GOAL: S15.516 -- SetSound and SetMonitorSound, called over the bus on a heard screen, take effect within
//       a round: silent from 150 ms after an "off", heard from 150 ms after an "on". False if the call
//       records the setting but nothing runs a round, so the sound changes only at the next picture.
// WHY-THIS-SHAPE: one picture every two seconds, and each call made 100 ms after a commit, once the
//       round after it has timed the next picture ~1.9 s off: only the call's own wake can run a round
//       inside the 450 ms window. A window that saw a commit is not graded, as another round ran in it.
//       Never during the file's last picture: sound started there takes another branch of the steering,
//       which phase lastPicture (S15.519) grades, and this phase is about the wake alone.
// COVERAGE-ASSERT: all four calls graded.
// CONTROL: control_run.sh 2026-10-11, reverted: the new `m_worker.wake();` in DbusService::setSound,
//       backend/daemon/dbus_service.cpp, deleted -> "FAIL: bus wake rule: SetSound(false) silences the screen
//       within a round" (peak 0.3538) and "SetSound(true) brings the sound back" (no buffers); the same in
//       setMonitorSound -> the two SetMonitorSound lines.
void runBusWakeChecks() {
    const char* kGoals = "S15.516 (the bus's sound switches act at once)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    WallpaperManager manager(smallConfig());
    std::unique_ptr<sdbus::IConnection> connection;
    try {
        connection = sdbus::createSessionBusConnection();
    } catch (const sdbus::Error& e) {
        notVerified(kGoals, std::string("no session bus in this entry: ") + e.what());
        return;
    }
    using wallpaperx::DbusService;
    DbusService service(*connection, manager);
    connection->enterEventLoopAsync();
    const auto proxy = sdbus::createProxy(sdbus::ServiceName{connection->getUniqueName()},
                                          sdbus::ObjectPath{DbusService::kObjectPath});
    std::vector<int> redraw;
    manager.setMonitorOutput(0, "DP-1", redraw);
    resetCapture();
    const int64_t started = probe::nowNs();
    wallpaperx::testing::setOutputFocusState(*proxy, "DP-1", "desktop", false);
    wallpaperx::testing::setWallpaper(*proxy, "video", asset("wallpaper_tone_slow.mp4"), 0);
    const auto generation = [&] {
        const auto snap = manager.snapshot();
        const auto it = snap.monitors.find(0);
        return it == snap.monitors.end() ? uint64_t{0} : it->second.generation;
    };
    const auto heardBy = Clock::now() + 15s;
    while (probe::g_lastLoudQueuedAt.load() <= started) {
        if (Clock::now() >= heardBy) {
            notVerified(kGoals, "the slow video was not heard within 15 s of SetWallpaper");
            return;
        }
        std::this_thread::sleep_for(20ms);
    }
    struct Call {
        const char* what;
        std::function<void()> call;
        bool heard;
    };
    const Call calls[] = {
        {"SetSound(false)", [&] { wallpaperx::testing::setSound(*proxy, false); }, false},
        {"SetSound(true)", [&] { wallpaperx::testing::setSound(*proxy, true); }, true},
        {"SetMonitorSound(0, false)", [&] { wallpaperx::testing::setMonitorSound(*proxy, 0, false); }, false},
        {"SetMonitorSound(0, true)", [&] { wallpaperx::testing::setMonitorSound(*proxy, 0, true); }, true},
    };
    struct Taken {
        int64_t at = 0;
        bool clean = false;
    };
    // The picture heard first is the file's first, and each commit is the next of its five. The last
    // one is never called on (see above).
    const uint64_t firstGeneration = generation();
    std::vector<Taken> taken;
    for (const Call& c : calls) {
        const auto drawnBy = Clock::now() + 9s;
        for (uint64_t last = generation();;) {
            if (Clock::now() >= drawnBy) {
                notVerified(kGoals, "no picture but the last was drawn for nine seconds, so no call could be timed");
                return;
            }
            const uint64_t now = generation();
            if (now != last && (now - firstGeneration) % 5 != 4) break;
            last = now;
            std::this_thread::sleep_for(2ms);
        }
        std::this_thread::sleep_for(100ms);
        const uint64_t g0 = generation();
        Taken t;
        t.at = probe::nowNs();
        c.call();
        std::this_thread::sleep_for(450ms);
        t.clean = generation() == g0;
        taken.push_back(t);
    }
    const Capture cap = takeCapture();
    int graded = 0;
    for (size_t i = 0; i < taken.size(); ++i) {
        const Call& c = calls[i];
        if (!taken[i].clean) {
            std::printf("audio_playback_test: bus wake rule: %s not graded: a picture was drawn in its window\n", c.what);
            continue;
        }
        ++graded;
        const Window w = windowOf(cap, taken[i].at + 150 * kMs, taken[i].at + 450 * kMs);
        const std::string detail = fmt("peak %.4f over %.0f buffers from 150 to 450 ms after the call", w.peak,
                                       static_cast<double>(w.buffers));
        std::printf("audio_playback_test: bus wake rule: %s: %s\n", c.what, detail.c_str());
        if (c.heard) {
            check(w.peak >= 0.25f, std::string("bus wake rule: ") + c.what + " brings the sound back within a round", detail);
        } else {
            check(w.peak <= 1e-4f, std::string("bus wake rule: ") + c.what + " silences the screen within a round", detail);
        }
    }
    check(graded == 4, "bus wake rule: all four calls were graded", fmt("%.0f of 4", graded));
}

// GOAL: S15.501 -- when the sound server goes away the pictures go on, the daemon says so in one line,
//       and once a server is back the sound returns by itself within about 5 s, before any loop could
//       restart it; one line says it is back. False if the sound waits for the next anchor (a loop, a
//       pause) to come back.
// GOAL: S15.513 -- and once back it plays on, in step with its pictures. False if the new stream inherits
//       the old one's playhead: the track is begun again under the request serial that playhead already
//       holds, so the callback never switches to the new segment, plays what was decoded before the loss,
//       and falls silent.
// WHY-THIS-SHAPE: the twelve-second fixture, lost about a second and a half in, so the retry five
//       seconds later falls well before the loop that would re-anchor the sound anyway; on a short
//       fixture a track that never retried would still come back at the loop and pass. "Back" alone is
//       fooled by the stale frames, which is why the steady right-channel tone must also play on.
// CONTROL: control_run.sh 2026-10-11, reverted, in renderer/audio/audio_output.cpp: loseOutput's
//       `dropStream(*t, now)` given a retry at the end of time -> "FAIL: reconnect rule: the sound comes back by
//       itself ..." (not back); its m_log deleted -> "... losing the sound server is said in one line" (0);
//       connect()'s "can be reached again" line deleted -> "... its return is said in one line" (0).
//       "The pictures go on" has no control: no sound-thread break short of a crash stops them.
//       S15.513: control_run.sh 2026-10-11, reverted: destroyStream's `t.playhead = {};` deleted -> "FAIL:
//       reconnect rule: once back, the sound plays on" (silent again for 1792 ms).
// Phase `reconnect` grades S15.501 and phase `reconnectPlaysOn` S15.513, each on its own run of the
// same scenario, so a break of one leaves the other's controls gradable.
void runReconnect(bool playsOn) {
    const char* kGoals = playsOn ? "S15.513 (sound plays on after the server returns)" : "S15.501 (the sound server lost and back)";
    auto sound = std::make_unique<SoundServer>(kGoals);
    if (!sound->ready) return;
    Daemon d(smallConfig());
    if (!startHeard(d, asset("wallpaper_beeps_long.mp4"), kGoals)) return;
    wallpaperx::testing::StreamCapture log(STDERR_FILENO);
    std::this_thread::sleep_for(1500ms);
    sound.reset();
    const int64_t lostAt = probe::nowNs();
    std::this_thread::sleep_for(1000ms);
    std::vector<Commit> commits = d.takeCommits();
    const auto during = std::count_if(commits.begin(), commits.end(), [&](const Commit& c) {
        return c.at >= lostAt + 200 * kMs && c.at < lostAt + 1000 * kMs;
    });
    if (!playsOn) {
        check(during >= 15, "reconnect rule: the pictures go on while the sound server is gone",
              fmt("%.0f pictures in 0.8 s", static_cast<double>(during)));
    }
    sound = std::make_unique<SoundServer>(kGoals);
    if (!sound->ready) return;
    const int64_t restartedAt = probe::nowNs();
    const auto deadline = Clock::now() + 8s;
    int64_t backAt = 0;
    while (Clock::now() < deadline) {
        if (const int64_t loud = probe::g_lastLoudQueuedAt.load(); loud > restartedAt) {
            backAt = loud;
            break;
        }
        std::this_thread::sleep_for(50ms);
    }
    if (backAt != 0) std::printf("audio_playback_test: reconnect rule: sound back %.2f s after the loss\n", (backAt - lostAt) / 1e9);
    std::this_thread::sleep_for(2000ms);
    commits = d.takeCommits();
    const Capture cap = takeCapture();
    log.restore();
    if (!playsOn) {
        check(backAt != 0, "reconnect rule: the sound comes back by itself within 8 s of a server returning, before a loop",
              backAt != 0 ? fmt("back %.2f s after the loss", (backAt - lostAt) / 1e9) : std::string("not back"));
        const int lostLines = log.linesWith({"AudioOutput:", "wallpapers are silent until it can be reached again"});
        const int backLines = log.linesWith({"AudioOutput: the sound server can be reached again"});
        check(lostLines == 1, "reconnect rule: losing the sound server is said in one line", fmt("%.0f lines", lostLines));
        check(backLines == 1, "reconnect rule: its return is said in one line", fmt("%.0f lines", backLines));
        // GOAL: S15.530 -- a server that comes back is reached when its socket appears, not at the next
        //       retry of a timer: the sound is back within 4 s of the loss, which a 5 s retry timer
        //       started at the loss can never be. False for an engine that only polls.
        // WHY-THIS-SHAPE: bounded from the loss, the one moment a timer engine's earliest return is fixed
        //       to; a bound from the server's return shrinks with a slow server start until the timer
        //       passes it. The server is back about 1 s after the loss; a run whose server took over 3 s
        //       to come back cannot tell a watch from the timer and is not verified, never passed.
        // CONTROL: control_run.sh 2026-10-11, reverted: the inotify watch never made (`m_serverWatch = -1;`)
        //       in AudioOutput's constructor, renderer/audio/audio_output.cpp -> "FAIL: reconnect rule: the sound
        //       is back within 4 s of the loss" (back 5.06 s after the loss, the server 1.01 s after it).
        if (backAt != 0) {
            if (restartedAt - lostAt > 3'000 * kMs) {
                notVerified("S15.530 (a server's return is seen at once)",
                            fmt("the private server took %.2f s to come back, too slow to tell a socket watch from a 5 s "
                                "retry timer",
                                (restartedAt - lostAt) / 1e9));
            } else {
                check(backAt - lostAt < 4'000 * kMs, "reconnect rule: the sound is back within 4 s of the loss",
                      fmt("back %.2f s after the loss, the server %.2f s after it", (backAt - lostAt) / 1e9,
                          (restartedAt - lostAt) / 1e9));
            }
        }
        return;
    }
    if (backAt == 0) {
        notVerified(kGoals, "the sound never came back, so whether it plays on is not asked (phase reconnect grades that)");
        return;
    }
    // The fixture's right channel is a steady tone: once back, the sound must not fall silent again.
    Continuity tone = continuity(cap, 480, 0.05f, 24, true);
    tone.gaps.push_back(tone.trailing);
    uint64_t silentAfter = 0;
    for (const auto& [at, frames] : tone.gaps) {
        if (at > backAt) silentAfter = std::max(silentAfter, frames);
    }
    check(silentAfter == 0, "reconnect rule: once back, the sound plays on",
          fmt("it fell silent again for %.0f ms within the two seconds after its return", silentAfter * 1000.0 / kRate));
    int passStarts = 0;
    const int graded = gradeBeepsAgainstPictures("reconnect rule", cap, commits, 360, 15, backAt + 100 * kMs, 12.0,
                                                 passStarts);
    if (graded >= 0) {
        check(graded >= 2, "reconnect rule: at least two beeps after the return were graded", fmt("%.0f", graded));
    }
}

// ---------------------------------------------------------------------------------------------- //
// AudioOutput alone
// ---------------------------------------------------------------------------------------------- //

// An AudioOutput of the test's own, made only where soundClientAllowed() agrees; `output` is null after
// a refusal, which has been reported. `onLine` sees each line after it is recorded, and may throw.
struct Engine {
    explicit Engine(const char* goals, bool decoy = false, std::function<void(const std::string&)> onLine = {},
                    AudioOutput::ContentScope scope = {}) {
        if (!soundClientAllowed(goals, decoy)) return;
        output = std::make_unique<AudioOutput>(
            [this, onLine = std::move(onLine)](const std::string& line) {
                {
                    std::lock_guard<std::mutex> lock(logMutex);
                    log.push_back(line);
                }
                if (onLine) onLine(line);
            },
            std::move(scope));
    }
    int linesWith(const char* text) {
        std::lock_guard<std::mutex> lock(logMutex);
        return static_cast<int>(std::count_if(log.begin(), log.end(),
                                              [&](const std::string& l) { return l.find(text) != std::string::npos; }));
    }
    std::mutex logMutex;
    std::vector<std::string> log;
    std::unique_ptr<AudioOutput> output;
};

// Each expected beep's heard time against when it should be heard: within [lo, hi] ms, and heard at
// all within 100 ms. Returns how many were graded.
int gradeBeepTimes(const std::string& rule, const Capture& cap, const std::vector<std::pair<std::string, int64_t>>& expected,
                   double lo, double hi) {
    const std::vector<int64_t> heard = onsets(cap, 0.1f, 480);
    int graded = 0;
    for (const auto& [label, at] : expected) {
        std::optional<int64_t> nearest;
        for (const int64_t h : heard) {
            if (!nearest || std::llabs(h - at) < std::llabs(*nearest - at)) nearest = h;
        }
        if (!nearest || std::llabs(*nearest - at) > 100 * kMs) {
            check(false, rule + ": every expected beep is heard within 100 ms of its time",
                  label + (nearest ? fmt(": the nearest sound is %+.1f ms away", (*nearest - at) / 1e6) : ": nothing heard"));
            continue;
        }
        const double error = (*nearest - at) / 1e6;
        std::printf("audio_playback_test: %s: %s heard %+.2f ms from its time\n", rule.c_str(), label.c_str(), error);
        check(error >= lo && error <= hi, rule + ": each beep is heard on time", label + fmt(": %+.2f ms", error));
        ++graded;
    }
    return graded;
}

std::vector<std::pair<std::string, int64_t>> beepsOf(int pass, int64_t passAt, double soundSeconds, int fromBeep = 0) {
    std::vector<std::pair<std::string, int64_t>> out;
    for (int k = fromBeep; k * 0.5 < soundSeconds - 1e-9; ++k) {
        out.push_back({"pass " + std::to_string(pass) + " beep " + std::to_string(k), passAt + k * 500 * kMs});
    }
    return out;
}

void sleepUntil(int64_t at) {
    const int64_t left = at - probe::nowNs();
    if (left > 0) std::this_thread::sleep_for(std::chrono::nanoseconds(left));
}

AudioTrack::Anchor anchorAt(int64_t fromNs, int64_t atNs) {
    return {std::chrono::nanoseconds(fromNs), Clock::time_point(std::chrono::nanoseconds(atNs))};
}

// Plays `file` from 0 at t0, then again from 0 at t0 + 2 s (announced 300 ms early, as a loop's next
// pass is), and grades every beep of both passes.
void runEngineLoop(const std::string& rule, const char* goals, const std::string& file, double soundSeconds) {
    SoundServer sound(goals);
    if (!sound.ready) return;
    resetCapture();
    Engine engine(goals);
    if (!engine.output) return;
    std::unique_ptr<AudioTrack> track = engine.output->track(file, "engine");
    const int64_t t0 = probe::nowNs() + 500 * kMs;
    track->play(anchorAt(0, t0));
    sleepUntil(t0 + 1700 * kMs);
    track->play(anchorAt(0, t0 + 2000 * kMs));
    sleepUntil(t0 + 3900 * kMs);
    track->stop();
    std::this_thread::sleep_for(200ms);
    const Capture cap = takeCapture();
    std::vector<std::pair<std::string, int64_t>> expected = beepsOf(0, t0, soundSeconds);
    for (auto& e : beepsOf(1, t0 + 2000 * kMs, soundSeconds)) expected.push_back(e);
    const int graded = gradeBeepTimes(rule, cap, expected, -2.0, 3.0);
    check(graded == static_cast<int>(expected.size()) && graded >= 8, rule + ": every beep of both passes was graded",
          fmt("%.0f of %.0f", graded, static_cast<double>(expected.size())));
}

// GOAL: S15.502 -- AudioOutput plays each beep within [-2, +3] ms of when its anchor says, the first
//       pass and the next one alike: the switch to a new anchor fades out over the 5 ms before it and
//       starts the new sound on time. False for a switch whose fade starts at the switch time, which
//       cuts the new pass's first 5 ms.
// WHY-THIS-SHAPE: driven without the daemon so the anchors are exact; the upper bound is +3 ms because a
//       beep at a segment's start is ramped in over 5 ms and crosses the onset level about a
//       millisecond in.
// CONTROL: control_run.sh 2026-10-11, reverted: render() in renderer/audio/audio_output.cpp starting the
//       switch and its fade at switchAt instead of kRampNs before (`frameAt + kRampNs >=` made `frameAt >=`
//       in switchDue and fadingToSwitch) -> "FAIL: engine loop rule: each beep is heard on time" (pass 1 beep
//       0: +6.68 ms).
void runEngineLoopChecks() {
    runEngineLoop("engine loop rule", "S15.502 (a switch between anchors)", asset("wallpaper_beeps.mp4"), 2.0);
}

// GOAL: S15.503 -- sound 50 ms shorter than its pictures stays aligned on the next pass: after the first
//       pass's sound runs out, the frames already decoded for the next pass are not played before it
//       starts. False when a segment's end is taken as everything written, so the next pass leaks in early.
// CONTROL: control_run.sh 2026-10-11, reverted: render()'s segment `end` made `written`, ignoring `next`
//       (playingEnd or newestStart), in renderer/audio/audio_output.cpp -> "FAIL: short sound rule: each beep is
//       heard on time" (pass 1 beeps 0-3: -49.92 ms).
void runEngineShortAudioChecks() {
    runEngineLoop("short sound rule", "S15.503 (sound shorter than its pictures)",
                  asset("wallpaper_beeps_short_audio.mp4"), 1.95);
}

// GOAL: S15.504 -- sound that can only start after its anchor's time starts that far into the file:
//       with the stream linked 250 ms late, the first pass's later beeps are still on time. False if a
//       late start plays from `from`, putting every beep 250 ms late.
// WHY-THIS-SHAPE: the stand-in delays linking, as a busy session manager would, so the lateness is set
//       by the test and not by the host's speed.
// CONTROL: control_run.sh 2026-10-11, reverted: `if (late > 0) p.skipTo += ...` deleted from render() in
//       renderer/audio/audio_output.cpp -> "FAIL: late start rule: every expected beep is heard within 100 ms
//       of its time" (four beeps, the nearest sound -246.0 to -248.6 ms away).
void runEngineLateStartChecks() {
    const char* kGoals = "S15.504 (a late start skips ahead)";
    SoundServer sound(kGoals, true, 250ms);
    if (!sound.ready) return;
    resetCapture();
    Engine engine(kGoals);
    if (!engine.output) return;
    std::unique_ptr<AudioTrack> track = engine.output->track(asset("wallpaper_beeps_long.mp4"), "engine");
    const int64_t t0 = probe::nowNs();
    track->play(anchorAt(0, t0));
    sleepUntil(t0 + 2300 * kMs);
    track->stop();
    std::this_thread::sleep_for(200ms);
    const Capture cap = takeCapture();
    const std::optional<int64_t> first = firstSound(cap, 0, 1e-3f);
    check(first && *first >= t0 + 200 * kMs, "late start rule: the stream really started late",
          first ? fmt("first sound %+.1f ms after the anchor", (*first - t0) / 1e6) : std::string("no sound"));
    const int graded = gradeBeepTimes("late start rule", cap, beepsOf(0, t0, 2.25, 1), -2.0, 3.0);
    check(graded >= 4, "late start rule: the beeps after the late start were graded", fmt("%.0f", graded));
}

// GOAL: S15.505 -- a change of volume moves the level over about 5 ms rather than in a step, and 0 is
//       silent. False for a gain set straight to its target, which clicks.
// WHY-THIS-SHAPE: the envelope is the largest sample over the 110 frames before each one -- one period
//       of the 440 Hz tone -- so a step drops it within a frame or two and a 240-frame ramp takes about
//       190 frames to fall from 90 % to 10 %.
// CONTROL: control_run.sh 2026-10-11, reverted: render()'s gain set straight to its target, in
//       renderer/audio/audio_output.cpp -> "FAIL: ramp rule: a volume change takes 2 to 10 ms, not a step" (18
//       frames on the re-run after the fix round's render() changes).
void runEngineRampChecks() {
    const char* kGoals = "S15.505 (volume ramps)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    resetCapture();
    Engine engine(kGoals);
    if (!engine.output) return;
    std::unique_ptr<AudioTrack> track = engine.output->track(asset("wallpaper_tone.mp4"), "engine");
    track->setVolume(1.0f);
    const int64_t t0 = probe::nowNs() + 300 * kMs;
    track->play(anchorAt(0, t0));
    sleepUntil(t0 + 1000 * kMs);
    const int64_t changedAt = probe::nowNs();
    track->setVolume(0.0f);
    std::this_thread::sleep_for(500ms);
    track->stop();
    std::this_thread::sleep_for(200ms);
    const Capture cap = takeCapture();
    // The level before: every buffer queued before the change was rendered before it.
    uint64_t start = 0;
    float before = 0;
    for (const probe::BufferRecord& b : cap.buffers) {
        if (b.queuedAt < changedAt) {
            before = std::max(before, b.peak);
            start = b.first + b.frames;
        }
    }
    int64_t at90 = -1, at10 = -1;
    for (uint64_t i = start; i < cap.left.size() && at10 < 0; ++i) {
        float envelope = 0;
        for (uint64_t j = i >= 109 ? i - 109 : 0; j <= i; ++j) envelope = std::max(envelope, std::fabs(cap.left[j]));
        if (at90 < 0 && envelope < 0.9f * before) at90 = static_cast<int64_t>(i);
        if (envelope < 0.1f * before) at10 = static_cast<int64_t>(i);
    }
    const Window after = windowOf(cap, changedAt + 100 * kMs, changedAt + 450 * kMs);
    check(before >= 0.3f && at90 >= 0 && at10 >= 0, "ramp rule: the tone played and then fell after the change",
          fmt("level %.3f before; fell at %.0f, %.0f", before, at90, at10));
    if (before >= 0.3f && at90 >= 0 && at10 >= 0) {
        const int64_t span = at10 - at90;
        std::printf("audio_playback_test: ramp rule: the level fell from 90 %% to 10 %% over %lld frames (%.2f ms)\n",
                    static_cast<long long>(span), span * 1000.0 / kRate);
        check(span >= 96 && span <= 480, "ramp rule: a volume change takes 2 to 10 ms, not a step",
              fmt("%.0f frames", static_cast<double>(span)));
    }
    check(after.peak <= 1e-4f, "ramp rule: volume 0 is silent", fmt("peak %.6f", after.peak));
}

// Plays the long fixture -- beeps left, a tone right -- stalls the sound decoder for `stallMs` once,
// 600 ms in, and grades the beeps after the stall. The tone's silence is the proof the stream ran dry.
void runUnderrun(const std::string& rule, const char* goals, int stallMs) {
    SoundServer sound(goals);
    if (!sound.ready) return;
    resetCapture();
    Engine engine(goals);
    if (!engine.output) return;
    std::unique_ptr<AudioTrack> track = engine.output->track(asset("wallpaper_beeps_long.mp4"), "engine");
    const int64_t t0 = probe::nowNs() + 400 * kMs;
    track->play(anchorAt(0, t0));
    sleepUntil(t0 + 600 * kMs);
    probe::g_stallStartedAt.store(0);
    probe::g_stallEndedAt.store(0);
    probe::g_stallMs.store(stallMs);
    probe::g_swrMode.store(probe::kSwrStallOnce);
    sleepUntil(t0 + 3800 * kMs);
    track->stop();
    std::this_thread::sleep_for(200ms);
    probe::g_swrMode.store(probe::kSwrPass);
    const Capture cap = takeCapture();
    const int64_t stallStarted = probe::g_stallStartedAt.load(), stallEnded = probe::g_stallEndedAt.load();
    uint64_t dry = 0;
    Continuity tone = continuity(cap, 480, 0.05f, 24, true);
    tone.gaps.push_back(tone.trailing);
    for (const auto& [at, frames] : tone.gaps) {
        if (at > stallStarted && at < stallEnded) dry = std::max(dry, frames);
    }
    std::printf("audio_playback_test: %s: the decoder stalled from %.0f to %.0f ms after the anchor; the stream went "
                "%.1f ms without sound\n",
                rule.c_str(), (stallStarted - t0) / 1e6, (stallEnded - t0) / 1e6, dry * 1000.0 / kRate);
    // COVERAGE-ASSERT: the stall is armed for the decoder's next conversion, so it starts only if the
    //       engine asks for more sound after 600 ms of play -- which one playing 3.8 s of a file must. Without
    //       this an engine that never refills runs dry, never meets the stall, and reaches the skip below.
    // CONTROL: control_run.sh 2026-10-11, reverted, phase engineLongUnderrun: render()'s `written !=
    //       p.askedAt` made `p.askedAt == 0` (ask once ever), renderer/audio/audio_output.cpp -> "FAIL: long
    //       underrun rule: the engine asked the decoder for more sound after 600 ms of play".
    if (stallStarted == 0) {
        check(false, rule + ": the engine asked the decoder for more sound after 600 ms of play",
              "no conversion from then to the end of the run, so the staged stall was never reached");
        return;
    }
    if (stallEnded == 0 || dry < 960) {
        notVerified(goals, "the staged stall did not leave the stream 20 ms without sound, so nothing was owed");
        return;
    }
    std::vector<std::pair<std::string, int64_t>> expected;
    for (auto& e : beepsOf(0, t0, 3.51)) {
        if (e.second > stallEnded + 150 * kMs) expected.push_back(e);
    }
    const int graded = gradeBeepTimes(rule, cap, expected, -2.0, 3.0);
    check(graded >= 2, rule + ": at least two beeps after the stall were graded", fmt("%.0f", graded));
}

// GOAL: S15.506 -- frames the stream could not play in time are owed and skipped once sound is there
//       again: after the sound decoder stalls long enough to leave the stream about 90 ms without sound,
//       the beeps are back on time. False if the sound resumes where it stopped, late by that stretch.
// WHY-THIS-SHAPE: the stall is staged in swr_convert, which only the sound decoder calls; 250 ms, so
//       the stretch without sound stays inside one fill (see S15.512 for a longer one). The dry stretch
//       is read from the right channel's tone, so a stall that starved nothing is a skip, not a pass.
// CONTROL: control_run.sh 2026-10-11, reverted: render()'s `p.skipTo = std::max(p.skipTo, position) + 1;`
//       made a no-op in renderer/audio/audio_output.cpp -> "FAIL: underrun rule: each beep is heard on time"
//       (five beeps, each +85.36 ms).
void runEngineUnderrunChecks() {
    runUnderrun("underrun rule", "S15.506 (frames owed after an underrun)", 250);
}

// GOAL: S15.512 -- after a stall longer than the sound decoded ahead (a second), the sound comes back on
//       time rather than staying silent. False for a callback that asks for more only on its way below
//       the low-water mark: owed frames larger than one fill keep it below that mark, so nothing asks again.
// CONTROL: control_run.sh 2026-10-11, reverted: render() in renderer/audio/audio_output.cpp asking only on
//       the way below the mark (askedAt used as a flag, cleared when not low) -> "FAIL: long underrun rule:
//       every expected beep is heard within 100 ms of its time" (four beeps, the nearest sound 1500 to 3000 ms
//       before; 2922.7 ms without sound).
void runEngineLongUnderrunChecks() {
    runUnderrun("long underrun rule", "S15.512 (sound back after a long underrun)", 1000);
}

// GOAL: S15.519 -- sound turned on while the file's last picture is on screen is heard within 150 ms, in
//       step with that picture (each beep left in the pass within 12 ms of the last picture's scheduled
//       time on the line through the commits, plus its distance from it), and after the loop is in step
//       with the new pass (its beeps at 0.5 and 1 s within 12 ms of the new pass's first scheduled time
//       plus their time). False if sound started there waits for the loop, plays the rest of the pass off
//       its schedule, or is never handed to the new pass.
// WHY-THIS-SHAPE: one picture every two seconds and the call 100 ms after the last one's commit, so only
//       the call's own round can start the sound inside the window graded "heard", and no commit may
//       land in it. Beeps, not tone_slow's steady tone: a tone sounds the same at any offset, so it can
//       say the sound is there but not that it is in step. The new pass's first beep is only held to
//       "not early, heard within 100 ms": the hand-over runs in the round at that picture's due time and
//       the late-start skip cuts its head by however late that round woke, a scheduler figure (+8.6 to +17.5 ms over four runs).
// COVERAGE-ASSERT: every expected beep graded, at least two before the loop and three after it.
// CONTROL: control_run.sh 2026-10-11, reverted, in steerSoundLocked, backend/daemon/wallpaper_manager.cpp:
//       `} else if (waiting.reaches) {` made `false && ...` -> "FAIL: last picture rule: sound turned on during
//       the last picture is heard within 150 ms" (peak 0.0000); 33 ms added to that branch's anchor time ->
//       "FAIL: last picture rule, rest of the pass: each beep is heard on time" (+32.97 ms, three beeps); the
//       kSoundDrift return made unconditional -> "FAIL: last picture rule, after the loop: every expected beep
//       is heard within 100 ms of its time" (no sound after the loop). The 33 ms control re-run with the
//       robust line -> the same line (+32.97 ms, three beeps); the other two not re-run, their lines
//       untouched. Graded against the robust line through the six commits, which one late commit cannot
//       move; how far commits sit from it is printed, not graded (see S15.497 for where pacing is).
void runLastPictureChecks() {
    const char* kGoals = "S15.519 (sound started on a pass's last picture)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    Daemon d(smallConfig());
    resetCapture();
    d.manager.setSound(false);
    d.play(asset("wallpaper_beeps_slow.mp4"), ScreenFocus::Desktop);
    // Five pictures a pass: the fifth commit is the file's last picture, the sixth the next pass's first.
    if (!d.waitForCommits(5, 15s)) {
        notVerified(kGoals, "the slow video did not reach its last picture within 15 s");
        return;
    }
    std::this_thread::sleep_for(100ms);
    const int64_t onAt = probe::nowNs();
    d.manager.setSound(true);
    d.worker.wake();
    if (!d.waitForCommits(6, 4s)) {
        notVerified(kGoals, "the next pass's first picture was not drawn within 4 s of the call");
        return;
    }
    std::this_thread::sleep_for(1300ms);
    const std::vector<Commit> commits = d.takeCommits();
    const Capture cap = takeCapture();
    std::vector<std::pair<int64_t, int64_t>> points;
    for (size_t i = 0; i < 6; ++i) {
        points.push_back({static_cast<int64_t>(i), commits[i].at});
        if (i == 0) continue;
        const int64_t gap = commits[i].at - commits[i - 1].at;
        if (commits[i].generation != commits[i - 1].generation + 1 || gap < 1850 * kMs || gap > 2150 * kMs) {
            notVerified(kGoals, fmt("the pictures did not come one every two seconds (commit %.0f came %.1f ms after the "
                                    "one before), so the fifth is not known to be the file's last",
                                    static_cast<double>(i), gap / 1e6));
            return;
        }
    }
    // The pictures' scheduled times, from the line through all six commits: one late wake of the render
    // thread moves its own commit, not the time the sound is graded against.
    const auto [fitted, worst] = fitLine(points);
    std::printf("audio_playback_test: last picture rule: the six commits sit at most %.1f ms from their line\n",
                worst / 1e6);
    const int64_t last = fitted[4], next = fitted[5];
    if (onAt < commits[4].at || onAt + 450 * kMs > commits[5].at) {
        notVerified(kGoals, "the call did not fall inside the last picture with 450 ms to spare");
        return;
    }
    const Window w = windowOf(cap, onAt + 150 * kMs, onAt + 450 * kMs);
    const std::string detail = fmt("peak %.4f over %.0f buffers from 150 to 450 ms after the call; the call came "
                                   "%.0f ms into the last picture",
                                   w.peak, static_cast<double>(w.buffers), (onAt - last) / 1e6);
    std::printf("audio_playback_test: last picture rule: %s\n", detail.c_str());
    check(w.peak >= 0.25f, "last picture rule: sound turned on during the last picture is heard within 150 ms", detail);

    std::vector<std::pair<std::string, int64_t>> rest;
    for (int k = 1; k <= 3; ++k) {
        if (last + k * 500 * kMs > onAt + 150 * kMs) rest.push_back({fmt("the beep at %.1f s", 8 + 0.5 * k), last + k * 500 * kMs});
    }
    const int restGraded = gradeBeepTimes("last picture rule, rest of the pass", cap, rest, -12.0, 12.0);
    const int firstGraded =
        gradeBeepTimes("last picture rule, the new pass's first beep", cap, {{"pass 1 beep 0", next}}, -12.0, 100.0);
    const int afterGraded =
        gradeBeepTimes("last picture rule, after the loop", cap, beepsOf(1, next, 1.01, 1), -12.0, 12.0);
    check(restGraded == static_cast<int>(rest.size()) && restGraded >= 2 && firstGraded == 1 && afterGraded == 2,
          "last picture rule: every beep of the rest of the pass and the first three of the next were graded",
          fmt("%.0f of %.0f before the loop, %.0f of 3 after", restGraded, static_cast<double>(rest.size()),
              firstGraded + afterGraded));
}

std::optional<sockaddr_un> unixAddress(const std::string& path) {
    sockaddr_un address{};
    if (path.size() >= sizeof address.sun_path) return std::nullopt;
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    return address;
}

// A socket bound and listening at `path`, non-blocking, with nothing at the path removed; -1 if not.
int bindListener(const std::string& path) {
    const std::optional<sockaddr_un> address = unixAddress(path);
    const int fd = address ? ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0) : -1;
    if (fd < 0) return -1;
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&*address), sizeof *address) != 0 || ::listen(fd, 8) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// A decoy listener of this process's: removed afterwards only while its path still names it.
struct DecoySocket {
    std::string path;
    int fd = -1;
    ino_t inode = 0;
};

void closeDecoy(DecoySocket& d) {
    if (d.fd >= 0) ::close(d.fd);
    d.fd = -1;
    struct stat st {};
    if (::lstat(d.path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode) && st.st_ino == d.inode) ::unlink(d.path.c_str());
}

// A listening socket at `path`, or nothing with `why`. Whatever is already there is left alone unless
// it is a socket that refuses a connection -- nobody listens on it, so it is an earlier run's leftover.
std::optional<DecoySocket> listenAt(const std::string& path, std::string& why) {
    const std::optional<sockaddr_un> address = unixAddress(path);
    if (!address) {
        why = path + " is too long for a socket's address";
        return std::nullopt;
    }
    struct stat st {};
    if (::lstat(path.c_str(), &st) == 0) {
        if (!S_ISSOCK(st.st_mode)) {
            why = path + " is there and is not a socket; it is left alone";
            return std::nullopt;
        }
        // Non-blocking, so a listener with a full backlog answers EAGAIN rather than holding this up.
        const int probe = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (probe < 0) {
            why = std::string("no socket to try ") + path + " with";
            return std::nullopt;
        }
        const int connected = ::connect(probe, reinterpret_cast<const sockaddr*>(&*address), sizeof *address);
        const int err = errno;
        ::close(probe);
        if (connected == 0 || err != ECONNREFUSED) {
            why = path + " is a socket in use (" + (connected == 0 ? std::string("it accepted a connection") : std::strerror(err)) +
                  "); it is left alone";
            return std::nullopt;
        }
        if (::unlink(path.c_str()) != 0) {
            why = path + " is a stale socket that could not be removed (" + std::strerror(errno) + ")";
            return std::nullopt;
        }
    } else if (errno != ENOENT) {
        why = path + " cannot be examined (" + std::strerror(errno) + ")";
        return std::nullopt;
    }
    DecoySocket made{path, bindListener(path), 0};
    if (made.fd < 0) {
        why = path + " could not be bound (" + std::strerror(errno) + ")";
        return std::nullopt;
    }
    if (::lstat(path.c_str(), &st) == 0) made.inode = st.st_ino;
    return made;
}

// Why `dir` may not hold the decoys, or nothing: a login session's runtime directory, or the caller's,
// is where a live sound server's socket is.
std::string decoyDirFault(const std::string& dir) {
    std::error_code ec;
    std::filesystem::path real = std::filesystem::weakly_canonical(dir, ec);
    if (ec) real = std::filesystem::path(dir).lexically_normal();
    const std::string text = real.string();
    if (text.empty() || text[0] != '/') return "the decoy directory " + dir + " is not absolute";
    if (text == "/run/user" || text.rfind("/run/user/", 0) == 0) {
        return "the decoy directory " + text + " is in a login session's runtime directory";
    }
    if (const char* caller = std::getenv("WALLPAPERX_CALLER_XDG_RUNTIME_DIR"); caller != nullptr && *caller != '\0') {
        std::filesystem::path callerReal = std::filesystem::weakly_canonical(caller, ec);
        if (ec) callerReal = std::filesystem::path(caller).lexically_normal();
        const std::string callerDir = callerReal.string();
        if (text == callerDir || text.rfind(callerDir + "/", 0) == 0) {
            return "the decoy directory " + text + " is the caller's own runtime directory or inside it";
        }
    }
    return {};
}

// The decoy sockets named `names` in `dir`, all or none, with `why` when none.
std::optional<std::vector<DecoySocket>> makeDecoys(const std::string& dir, std::initializer_list<const char*> names,
                                                   std::string& why) {
    why = decoyDirFault(dir);
    if (!why.empty()) return std::nullopt;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::vector<DecoySocket> made;
    for (const char* name : names) {
        std::optional<DecoySocket> one = listenAt(dir + name, why);
        if (!one) {
            for (DecoySocket& d : made) closeDecoy(d);
            return std::nullopt;
        }
        made.push_back(*one);
    }
    return made;
}

// GOAL: S15.542 -- the decoy phase never removes a socket it did not make, nor anything else: a socket
//       someone listens on is left in place and still reaches its listener, a file that is not a socket
//       is left alone, and only a socket that refuses connections (an earlier run's leftover) is
//       replaced; a decoy directory in a login session's runtime directory, or the caller's, is refused
//       before anything is made there. False if a decoy path that names a live server's socket deletes it.
// WHY-THIS-SHAPE: driven on paths in this process's own scratch, the "live server" a listener of the
//       test's own -- never a real one. The login-session refusal is driven through makeDecoys on a
//       /run/user directory no uid has, which cannot be created even with the refusal broken; the caller's
//       directory only through decoyDirFault, so a broken refusal can never write in it.
// CONTROL: control_run.sh 2026-10-11 inside .claude/tools/sandbox.sh, reverted, each a break in this file:
//       listenAt's in-use test made `if (false)` -> "FAIL: decoy rule: a socket someone is listening on is
//       left alone" (it was replaced); its not-a-socket test made `if (false)` -> "... a file that is not a
//       socket is left alone" (replaced by a socket); its stale unlink skipped -> "... an earlier run's
//       leftover, is replaced"; decoyDirFault's /run/user clause made `if (false)` -> "... in a login
//       session's runtime directory is refused" (could not be bound, nothing made); its caller clause made
//       `if (false)` -> "... in the caller's runtime directory is refused"; closeDecoy's unlink deleted ->
//       "... the isolation phase removes the decoy sockets it made" (pipewire-0 and remote still there).
void runDecoySafetyChecks() {
    const char* kGoals = "S15.542 (the decoys never remove what they did not make)";
    const char* tmp = std::getenv("TMPDIR");
    std::string base = std::string(tmp != nullptr && *tmp != '\0' ? tmp : "/tmp") + "/decoy-safety-XXXXXX";
    if (::mkdtemp(base.data()) == nullptr) {
        notVerified(kGoals, "no scratch directory could be made");
        return;
    }
    const std::string live = base + "/live", file = base + "/file", stale = base + "/stale";
    if (!unixAddress(live)) {
        notVerified(kGoals, base + " is too long to hold a socket's path");
        ::rmdir(base.c_str());
        return;
    }
    std::string why;

    const int server = bindListener(live);
    if (server < 0) {
        notVerified(kGoals, "the stand-in server's socket could not be made");
    } else {
        const std::optional<DecoySocket> taken = listenAt(live, why);
        // Still that listener's: a connection to the path is accepted on its descriptor.
        const int client = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        const std::optional<sockaddr_un> address = unixAddress(live);
        const bool connected =
            client >= 0 && ::connect(client, reinterpret_cast<const sockaddr*>(&*address), sizeof *address) == 0;
        const int accepted = ::accept4(server, nullptr, nullptr, SOCK_CLOEXEC);
        std::printf("audio_playback_test: decoy rule: a live socket: %s; it still reaches its listener: %s\n",
                    taken ? "taken over" : why.c_str(), connected && accepted >= 0 ? "yes" : "no");
        check(!taken, "decoy rule: a socket someone is listening on is left alone", taken ? "it was replaced" : "");
        check(connected && accepted >= 0, "decoy rule: and still reaches the listener it had");
        if (accepted >= 0) ::close(accepted);
        if (client >= 0) ::close(client);
        if (taken) {
            DecoySocket d = *taken;
            closeDecoy(d);
        }
        ::close(server);
        ::unlink(live.c_str());
    }

    std::ofstream(file) << "not a socket";
    {
        const std::optional<DecoySocket> taken = listenAt(file, why);
        struct stat st {};
        const bool kept = ::lstat(file.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 12;
        check(!taken && kept, "decoy rule: a file that is not a socket is left alone",
              taken ? "it was replaced by a socket" : (kept ? "" : "it is gone or changed"));
        if (taken) {
            DecoySocket d = *taken;
            closeDecoy(d);
        }
        ::unlink(file.c_str());
    }

    if (const int leftover = bindListener(stale); leftover < 0) {
        notVerified(kGoals, "no stale socket could be made to replace");
    } else {
        ::close(leftover);
        std::optional<DecoySocket> taken = listenAt(stale, why);
        check(taken.has_value(), "decoy rule: a socket nobody listens on, an earlier run's leftover, is replaced", why);
        if (taken) closeDecoy(*taken);
        ::unlink(stale.c_str());
    }

    // No uid owns this, so nothing can be made there whatever the refusal says.
    const std::string loginDir = "/run/user/4294967294/wallpaperx-decoy-check";
    const std::optional<std::vector<DecoySocket>> refused = makeDecoys(loginDir, {"/pipewire-0"}, why);
    check(!refused && why.find("login session's runtime directory") != std::string::npos,
          "decoy rule: a decoy directory in a login session's runtime directory is refused", why);
    if (refused) {
        std::vector<DecoySocket> made = *refused;
        for (DecoySocket& d : made) closeDecoy(d);
    }
    if (const char* caller = std::getenv("WALLPAPERX_CALLER_XDG_RUNTIME_DIR"); caller != nullptr && *caller != '\0') {
        const std::string fault = decoyDirFault(std::string(caller) + "/decoy");
        check(fault.find("caller's own runtime directory") != std::string::npos,
              "decoy rule: a decoy directory in the caller's runtime directory is refused", fault);
    } else {
        notVerified(kGoals, "the caller had no runtime directory, so refusing one is not asked");
    }
    check(decoyDirFault(base).empty(), "decoy rule: the test's own scratch is accepted", decoyDirFault(base));
    ::rmdir(base.c_str());
}

// GOAL: S15.515 -- an entry whose caller's environment names PipeWire servers (PIPEWIRE_REMOTE,
//       PIPEWIRE_RUNTIME_DIR) never reaches them: the sound engine, asked to play, makes no connection to
//       any socket the caller named, and finds no server in the entry's private runtime directory. False
//       if isolated_run.sh hands PIPEWIRE_REMOTE or PIPEWIRE_RUNTIME_DIR through. The engine is a native
//       PipeWire client and reads no PULSE_*, JACK_* or ALSA_CONFIG_PATH, so no decoy could see those
//       followed; only test_isolation_wrapper checks they are cleared.
// WHY-THIS-SHAPE: the caller's names point at sockets this phase listens on, so "not used" is a count of
//       connections accepted rather than a reading of the environment, which test_isolation_wrapper
//       does. The engine's own "no sound server" line proves it tried to connect, so an engine that
//       never reached connect cannot pass by being seen doing nothing.
// CONTROL: control_run.sh 2026-10-11, reverted: PIPEWIRE_RUNTIME_DIR and PIPEWIRE_REMOTE alone taken out of
//       the sound-server `unset` in renderer/tests/support/isolated_run.sh -> "FAIL: isolation rule: the sound
//       engine never connects to a sound server the caller named" (1 connection accepted) and the "found
//       none" line.
// True once the decoys were made, which the caller then sees removed.
bool runSoundIsolationChecks(const std::string& decoy) {
    const char* kGoals = "S15.515 (the caller's sound server is out of reach)";
    // GOAL: S15.540 -- this phase makes a real sound client, so it refuses to run, as a harness failure,
    //       unless XDG_RUNTIME_DIR is the one isolated_run.sh made for the entry; a bare run from a desktop
    //       terminal would otherwise play through the live PipeWire. False if the phase reaches its engine
    //       with a runtime directory the wrapper did not make.
    // WHY-THIS-SHAPE: runtimeDirFault() alone, not soundIsolationFault(): the decoy variables are set on
    //       purpose here, and a variable the wrapper failed to clear is what the phase grades. A harness
    //       failure, not a skip, so a refused run can never pass as one.
    // CONTROL: control_run.sh 2026-10-11 inside .claude/tools/sandbox.sh, reverted: isolated_run.sh made to
    //       keep the caller's XDG_RUNTIME_DIR (the sandbox's own, holding no server) -> "FAIL (harness): the
    //       isolation phase refused to make a sound client: XDG_RUNTIME_DIR=... is the caller's own runtime
    //       directory or inside it".
    if (const std::string why = runtimeDirFault(); !why.empty()) {
        harnessFailure("the isolation phase refused to make a sound client: " + why);
        return false;
    }
    std::string why;
    std::optional<std::vector<DecoySocket>> decoys = makeDecoys(decoy, {"/pipewire-0", "/remote"}, why);
    if (!decoys) {
        if (!decoyDirFault(decoy).empty()) {
            harnessFailure("the isolation phase refused its decoys: " + why);
        } else {
            notVerified(kGoals, "no decoy socket could be made: " + why);
        }
        return false;
    }
    struct CloseDecoys {
        std::vector<DecoySocket>& list;
        ~CloseDecoys() {
            for (DecoySocket& d : list) closeDecoy(d);
        }
    } closeDecoys{*decoys};
    int connections = 0;
    const auto acceptAll = [&] {
        for (const DecoySocket& l : *decoys) {
            for (int c; (c = ::accept4(l.fd, nullptr, nullptr, SOCK_CLOEXEC)) >= 0;) {
                ++connections;
                ::close(c);
            }
        }
    };
    bool lookedAndFoundNone = false;
    {
        Engine engine(kGoals, true);
        if (!engine.output) return true;
        std::unique_ptr<AudioTrack> track = engine.output->track(asset("wallpaper_beeps.mp4"), "isolation");
        track->play(anchorAt(0, probe::nowNs() + 100 * kMs));
        const auto deadline = Clock::now() + 3s;
        while (Clock::now() < deadline && !lookedAndFoundNone) {
            acceptAll();
            std::lock_guard<std::mutex> lock(engine.logMutex);
            for (const std::string& line : engine.log) {
                if (line.find("no sound server to play to") != std::string::npos) lookedAndFoundNone = true;
            }
            std::this_thread::sleep_for(20ms);
        }
        // Long enough for a client that did connect to have been accepted.
        std::this_thread::sleep_for(300ms);
        acceptAll();
        track->stop();
    }
    acceptAll();
    std::printf("audio_playback_test: isolation rule: %d connections to the caller's sound servers; engine found "
                "none: %s\n",
                connections, lookedAndFoundNone ? "yes" : "no");
    check(connections == 0, "isolation rule: the sound engine never connects to a sound server the caller named",
          fmt("%.0f connections accepted on the decoys", connections));
    check(lookedAndFoundNone, "isolation rule: the sound engine looked for a server and found none in the entry's "
                              "own runtime directory");
    return true;
}

// ---------------------------------------------------------------------------------------------- //
// AudioOutput's segments, failures and connection
// ---------------------------------------------------------------------------------------------- //

// GOAL: S15.520 -- a segment plays only its own frames: with A (the file's last 200 ms) playing and two
//       later plays B and C both published before A runs out, nothing is heard between A's end and the
//       switch. False if the callback bounds A by the newest segment's start, so it runs on into B's
//       frames.
// WHY-THIS-SHAPE: both later plays must be published before B's switch comes due, or a correct engine
//       plays B in the gap; B is 550 ms out, so the test thread has half a second, and its lateness is
//       measured -- a run that published C less than 50 ms before B's switch is not verified, never
//       graded. A, the file's last 200 ms, ends its own filling at once, so B is begun at once. The gap is
//       read by heard time, from 15 ms after A's end to 15 ms before C's switch fade.
// COVERAGE-ASSERT: A and C are both heard, so a silent track cannot pass the gap.
// CONTROL: control_run.sh 2026-10-11, reverted: render()'s `next` made `newestStart` alone, in
//       renderer/audio/audio_output.cpp -> "FAIL: segment rule: nothing is heard between a segment's end and
//       the next switch" (peak 0.3582, at these offsets). Test-side, control_run.sh, reverted: a 600 ms sleep
//       before publishing C -> "NOT VERIFIED -- S15.520 ...: the test thread published C -50 ms before B's
//       switch" (a skip, as meant). By hand, earlier: the track made on the soundless bands fixture -> "FAIL:
//       segment rule: the first segment and the last are heard".
void runQueuedSegmentChecks() {
    const char* kGoals = "S15.520 (a segment ends where the next one published starts)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    resetCapture();
    Engine engine(kGoals);
    if (!engine.output) return;
    std::unique_ptr<AudioTrack> track = engine.output->track(asset("wallpaper_tone.mp4"), "queued");
    const int64_t t0 = probe::nowNs() + 300 * kMs;
    const int64_t switchB = t0 + 600 * kMs;
    track->play(anchorAt(1800 * kMs, t0));
    sleepUntil(t0 + 50 * kMs);
    track->play(anchorAt(0, switchB));
    std::this_thread::sleep_for(20ms);
    track->play(anchorAt(500 * kMs, t0 + 640 * kMs));
    const int64_t publishedC = probe::nowNs();
    sleepUntil(t0 + 1150 * kMs);
    track->stop();
    std::this_thread::sleep_for(200ms);
    if (publishedC + 50 * kMs > switchB) {
        notVerified(kGoals, fmt("the test thread published C %.0f ms before B's switch, too late for a correct engine to "
                                "skip B",
                                (switchB - publishedC) / 1e6));
        return;
    }
    const Capture cap = takeCapture();
    const float a = peakHeard(cap, t0 + 20 * kMs, t0 + 180 * kMs);
    const float gap = peakHeard(cap, t0 + 215 * kMs, t0 + 625 * kMs);
    const float c = peakHeard(cap, t0 + 700 * kMs, t0 + 1100 * kMs);
    std::printf("audio_playback_test: segment rule: A %.4f, between A's end and the switch %.4f, C %.4f; C published "
                "%.0f ms before B's switch\n",
                a, gap, c, (switchB - publishedC) / 1e6);
    check(a > 0.3f && c > 0.3f, "segment rule: the first segment and the last are heard", fmt("A %.4f, C %.4f", a, c));
    check(gap <= 1e-3f, "segment rule: nothing is heard between a segment's end and the next switch",
          fmt("peak %.4f", gap));
}

// GOAL: S15.521 -- a play far ahead leaves the sound before it playing until then: with a tone playing,
//       a play 1.2 s off keeps the tone going to its switch. False if the far play is begun at once,
//       which ends the current sound at what it had decoded, a third of a second.
// WHY-THIS-SHAPE: the right channel of the long fixture is a steady tone, so any stretch without sound
//       is a run of zeros; 48 frames (1 ms) is allowed, a fade is not silence.
// COVERAGE-ASSERT: the window was heard nearly whole (heard frames counted), and the far play is heard.
// CONTROL: control_run.sh 2026-10-11, reverted, in renderer/audio/audio_output.cpp: service()'s `later`
//       made `false` -> "FAIL: far play rule: the sound before a far play keeps playing until it" (38805
//       frames silent); begin()'s top-up `fill(t)` deleted -> the same line (926 frames). Test-side: the
//       window moved before the play -> "FAIL: far play rule: the window before the far play was heard".
void runFuturePlayChecks() {
    const char* kGoals = "S15.521 (a far play keeps the sound before it)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    resetCapture();
    Engine engine(kGoals);
    if (!engine.output) return;
    std::unique_ptr<AudioTrack> track = engine.output->track(asset("wallpaper_beeps_long.mp4"), "future");
    const int64_t t0 = probe::nowNs() + 300 * kMs;
    track->play(anchorAt(0, t0));
    sleepUntil(t0 + 300 * kMs);
    track->play(anchorAt(5000 * kMs, t0 + 1500 * kMs));
    sleepUntil(t0 + 2200 * kMs);
    track->stop();
    std::this_thread::sleep_for(200ms);
    const Capture cap = takeCapture();
    const auto [silent, heard] = longestSilence(cap, t0 + 350 * kMs, t0 + 1450 * kMs, true);
    const float after = peakHeard(cap, t0 + 1550 * kMs, t0 + 2100 * kMs, true);
    std::printf("audio_playback_test: far play rule: longest silence before the far play %llu frames of %llu heard; "
                "after it %.4f\n",
                static_cast<unsigned long long>(silent), static_cast<unsigned long long>(heard), after);
    check(heard >= 48000ULL * 1100 / 1000 * 9 / 10 && after > 0.2f,
          "far play rule: the window before the far play was heard, and the far play after it",
          fmt("%.0f frames heard, %.4f after", static_cast<double>(heard), after));
    check(silent <= 48, "far play rule: the sound before a far play keeps playing until it",
          fmt("%.0f frames silent", static_cast<double>(silent)));
}

// GOAL: S15.522 -- sound that starts from silence on an anchor still to come ramps in from its first
//       frame: the largest sample in its first half millisecond is far below the tone's level. False if
//       the gain ramps up while the callback holds back to the start time, so the first frame comes in
//       at full level, a click.
// WHY-THIS-SHAPE: the 240-frame ramp puts the first 24 frames under a tenth of the level; a ramp spent
//       in the hold puts them at the tone's 0.35.
// CONTROL: control_run.sh 2026-10-11, reverted: render()'s `&& p.holdFrames == 0` deleted from the gain
//       target, renderer/audio/audio_output.cpp -> "FAIL: fade-in rule: a start from silence ramps in from its
//       first frame" (0.3448). Test-side: the bands fixture -> "FAIL: fade-in rule: the tone was heard".
void runFadeInChecks() {
    const char* kGoals = "S15.522 (a start from silence ramps in)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    resetCapture();
    Engine engine(kGoals);
    if (!engine.output) return;
    std::unique_ptr<AudioTrack> track = engine.output->track(asset("wallpaper_tone.mp4"), "fadein");
    track->setVolume(1.0f);
    const int64_t t0 = probe::nowNs() + 300 * kMs;
    track->play(anchorAt(0, t0));
    sleepUntil(t0 + 400 * kMs);
    track->stop();
    std::this_thread::sleep_for(200ms);
    const Capture cap = takeCapture();
    std::optional<uint64_t> first;
    for (uint64_t i = 0; i < cap.left.size() && !first; ++i) {
        if (std::fabs(cap.left[i]) > 1e-6f) first = i;
    }
    float early = 0.0f, later = 0.0f;
    if (first) {
        for (uint64_t i = *first; i < cap.left.size() && i < *first + 2400; ++i) {
            float& level = i < *first + 24 ? early : later;
            level = std::max(level, std::fabs(cap.left[i]));
        }
    }
    std::printf("audio_playback_test: fade-in rule: largest sample in the first half millisecond %.4f, then %.4f\n",
                early, later);
    check(first.has_value() && later > 0.3f, "fade-in rule: the tone was heard", fmt("level %.4f", later));
    check(first.has_value() && early <= 0.1f, "fade-in rule: a start from silence ramps in from its first frame",
          fmt("%.4f in the first 24 frames", early));
}

// GOAL: S15.523 -- one stream's error costs that track alone: the other track's stream is untouched,
//       and the failed track gets a new stream by itself a few seconds later. False if a stream error is
//       taken as the server lost (every stream destroyed), or leaves its track silent for good.
// WHY-THIS-SHAPE: the error is delivered through the very state-changed callback AudioOutput gave the
//       first stream, as PipeWire would; streams destroyed and made are counted at pw_stream_destroy and
//       pw_stream_new, which only AudioOutput calls here.
// COVERAGE-ASSERT: both tracks had a stream before the error.
// CONTROL: control_run.sh 2026-10-11, reverted, in AudioOutput::run, renderer/audio/audio_output.cpp: a
//       stream error handled by loseOutput -> "FAIL: stream error rule: one stream's error destroys that
//       stream alone" (2 destroyed) and "... gets a new stream by itself" (2 made); its retry set to the end of
//       time -> "... gets a new stream by itself" (0 made). Test-side: one listener dropped -> "FAIL: stream
//       error rule: both tracks have a stream before the error".
void runStreamErrorChecks() {
    const char* kGoals = "S15.523 (a stream error costs its track alone)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    {
        std::lock_guard<std::mutex> lock(probe::g_pwMutex);
        probe::g_listeners.clear();
    }
    Engine engine(kGoals);
    if (!engine.output) return;
    std::unique_ptr<AudioTrack> one = engine.output->track(asset("wallpaper_beeps_long.mp4"), "error-one");
    std::unique_ptr<AudioTrack> two = engine.output->track(asset("wallpaper_beeps_long.mp4"), "error-two");
    const int64_t t0 = probe::nowNs() + 300 * kMs;
    one->play(anchorAt(0, t0));
    std::this_thread::sleep_for(100ms);
    two->play(anchorAt(0, t0));
    sleepUntil(t0 + 1000 * kMs);
    std::vector<probe::StreamListener> listeners;
    {
        std::lock_guard<std::mutex> lock(probe::g_pwMutex);
        listeners = probe::g_listeners;
    }
    if (listeners.size() != 2) {
        check(false, "stream error rule: both tracks have a stream before the error",
              fmt("%.0f streams", static_cast<double>(listeners.size())));
        return;
    }
    const int destroyedBefore = probe::g_streamsDestroyed.load();
    const int madeBefore = probe::g_streamsMade.load();
    listeners[0].events->state_changed(listeners[0].data, PW_STREAM_STATE_STREAMING, PW_STREAM_STATE_ERROR, "injected");
    std::this_thread::sleep_for(1000ms);
    const int destroyed = probe::g_streamsDestroyed.load() - destroyedBefore;
    std::this_thread::sleep_for(5500ms);
    const int made = probe::g_streamsMade.load() - madeBefore;
    std::printf("audio_playback_test: stream error rule: %d destroyed in the second after the error, %d made in the "
                "6.5 s after it\n",
                destroyed, made);
    check(destroyed == 1, "stream error rule: one stream's error destroys that stream alone", fmt("%.0f destroyed", destroyed));
    check(made == 1, "stream error rule: its track gets a new stream by itself", fmt("%.0f made", made));
}

// GOAL: S15.524 -- a track whose stream could not be made plays once one can: with the server refusing
//       one pw_stream_connect, the track is heard a few seconds later. False if a failed stream leaves
//       its track silent for good.
// COVERAGE-ASSERT: the refusal was used up, so the failure path was reached.
// CONTROL: control_run.sh 2026-10-11, reverted: ensureStream's failure made closeAfterFailure(t) instead of
//       a retry, renderer/audio/audio_output.cpp -> "FAIL: stream retry rule: a track whose stream could not be
//       made plays once one can, within 9 s" (never heard). Test-side: the interposer never refusing -> "FAIL:
//       stream retry rule: the staged refusal was reached".
void runStreamRetryChecks() {
    const char* kGoals = "S15.524 (a refused stream is asked for again)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    struct Restore {
        ~Restore() { probe::g_refuseConnects.store(0); }
    } restore;
    Engine engine(kGoals);
    if (!engine.output) return;
    probe::g_refuseConnects.store(1);
    const int64_t started = probe::nowNs();
    std::unique_ptr<AudioTrack> track = engine.output->track(asset("wallpaper_beeps_long.mp4"), "retry");
    track->play(anchorAt(0, probe::nowNs()));
    const auto deadline = Clock::now() + 9s;
    while (Clock::now() < deadline && probe::g_lastLoudQueuedAt.load() <= started) std::this_thread::sleep_for(50ms);
    const int64_t loud = probe::g_lastLoudQueuedAt.load();
    std::printf("audio_playback_test: stream retry rule: refusals left %d; heard %s\n", probe::g_refuseConnects.load(),
                loud > started ? fmt("%.2f s after the play", (loud - started) / 1e9).c_str() : "never");
    check(probe::g_refuseConnects.load() == 0, "stream retry rule: the staged refusal was reached");
    check(loud > started, "stream retry rule: a track whose stream could not be made plays once one can, within 9 s");
    track->stop();
}

std::atomic<int> g_scopeThrows{0};
std::atomic<bool> g_scopeBreaks{false};
constexpr const char* kScopeChildFlag = "--scope-child";
constexpr int kScopeChildNotReached = 3;
constexpr int kScopeChildRefused = 4;

// A child whose content scope throws on its own account, from the moment its sound is being decoded,
// and whose decoding then fails: the close after that failure meets the throwing scope.
[[noreturn]] void runScopeChild() {
    wallpaperx::testing::suppressCoreDump();
    {
        Engine engine("S15.527 (child)", false, {}, [](const std::string&, const std::function<void()>& work) {
            if (g_scopeBreaks.load()) {
                g_scopeThrows.fetch_add(1);
                throw std::runtime_error("the scope broke its contract");
            }
            work();
        });
        if (!engine.output) _exit(kScopeChildRefused);
        std::unique_ptr<AudioTrack> track = engine.output->track(asset("wallpaper_beeps_long.mp4"), "scope");
        track->play(anchorAt(0, probe::nowNs()));
        const auto deadline = Clock::now() + 8s;
        while (probe::g_swrCalls.load() == 0 && Clock::now() < deadline) std::this_thread::sleep_for(10ms);
        g_scopeBreaks.store(true);
        probe::g_swrMode.store(probe::kSwrFail);
        std::this_thread::sleep_for(1500ms);
        track.reset();
    }
    _exit(g_scopeThrows.load() >= 2 ? 0 : kScopeChildNotReached);
}

// GOAL: S15.527 -- a content scope that throws on its own account costs the sound, never the process:
//       the decoder is still closed and the process lives. False if the close inside the scope lets the
//       scope's exception out of a noexcept function, which ends the daemon with std::terminate.
// WHY-THIS-SHAPE: in a child, so the termination being guarded against is observable as SIGABRT.
// COVERAGE-ASSERT: the child exits 0 only after the scope threw at least twice (the read and the close).
// CONTROL: control_run.sh 2026-10-11, reverted: closeDecoder's try/catch removed, renderer/audio/audio_output.cpp
//       -> "FAIL: throwing scope rule: a scope that throws costs the sound, not the process" (died of signal 6).
//       Test-side: the scope never set to throw -> "FAIL (harness): ... scope never threw twice".
void runThrowingScopeChecks() {
    const char* kGoals = "S15.527 (a throwing scope costs the sound, not the process)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    const wallpaperx::testing::SelfExecResult r = wallpaperx::testing::runSelf({kScopeChildFlag}, std::chrono::seconds(30));
    std::printf("audio_playback_test: throwing scope rule: the child %s\n", r.fault.empty() ? r.how().c_str() : r.fault.c_str());
    if (!r.fault.empty() || !r.reaped) {
        harnessFailure("the throwing-scope child could not be run to its end: " + (r.fault.empty() ? r.how() : r.fault));
        return;
    }
    if (r.exitedWith(kScopeChildNotReached) || r.exitedWith(kScopeChildRefused)) {
        harnessFailure(r.exitedWith(kScopeChildRefused)
                           ? "the throwing-scope child refused to make a sound client"
                           : "the throwing-scope child's scope never threw twice, so the close after a failure was not reached");
        return;
    }
    check(r.exitedWith(0), "throwing scope rule: a scope that throws costs the sound, not the process", r.how());
}

// GOAL: S15.528 -- an exception of any type from one track's pass costs that pass, and the track plays
//       once it can: with the log throwing an int on the "no sound server" line, the track is heard once
//       a server is there. False if only std::exception is caught per track, so the int escapes the pass
//       with the track's request marked as acted on, and it is never retried.
// COVERAGE-ASSERT: the log did throw.
// CONTROL: control_run.sh 2026-10-11, reverted: run()'s per-track `catch (...)` removed,
//       renderer/audio/audio_output.cpp -> "FAIL: pass failure rule: the track whose pass was cut short plays
//       once the server is there, within 8 s" (never heard). Test-side: the log throwing on a line that never
//       comes -> "FAIL: pass failure rule: the log threw on the no-server line".
void runPassFailureChecks() {
    const char* kGoals = "S15.528 (a non-standard exception is retried)";
    std::atomic<int> thrown{0};
    Engine engine(kGoals, false, [&](const std::string& line) {
        if (line.find("no sound server") != std::string::npos && thrown.fetch_add(1) == 0) throw 7;
    });
    if (!engine.output) return;
    resetCapture();
    std::unique_ptr<AudioTrack> track = engine.output->track(asset("wallpaper_beeps_long.mp4"), "pass");
    track->play(anchorAt(0, probe::nowNs()));
    std::this_thread::sleep_for(1000ms);
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    const int64_t started = probe::nowNs();
    const auto deadline = Clock::now() + 8s;
    while (Clock::now() < deadline && probe::g_lastLoudQueuedAt.load() <= started) std::this_thread::sleep_for(50ms);
    const int64_t loud = probe::g_lastLoudQueuedAt.load();
    std::printf("audio_playback_test: pass failure rule: the log threw %d times; heard %s\n", thrown.load(),
                loud > started ? fmt("%.2f s after the server came", (loud - started) / 1e9).c_str() : "never");
    check(thrown.load() >= 1, "pass failure rule: the log threw on the no-server line");
    check(loud > started, "pass failure rule: the track whose pass was cut short plays once the server is there, within 8 s");
    track.reset();
}

// GOAL: S15.529 -- a failure outside any one track's work is said, once: with the log throwing on the
//       "went away" line, one "could not be served" line follows. False if the outer handler swallows
//       it silently.
// COVERAGE-ASSERT: the "went away" line was reached.
// CONTROL: control_run.sh 2026-10-11, reverted: the outer handler's "could not be served" line deleted from
//       run(), renderer/audio/audio_output.cpp -> "FAIL: pass failure rule: a pass cut short outside any track's
//       work is said in one line" (0 lines). Test-side: the log throwing on a line that never comes -> "FAIL:
//       pass failure rule: the server's loss reached the log".
void runPassFailureLoggedChecks() {
    const char* kGoals = "S15.529 (a pass cut short is said once)";
    auto sound = std::make_unique<SoundServer>(kGoals);
    if (!sound->ready) return;
    std::atomic<int> wentAway{0};
    Engine engine(kGoals, false, [&](const std::string& line) {
        if (line.find("went away") != std::string::npos) {
            wentAway.fetch_add(1);
            throw std::runtime_error("the log broke");
        }
    });
    if (!engine.output) return;
    std::unique_ptr<AudioTrack> track = engine.output->track(asset("wallpaper_beeps_long.mp4"), "passlog");
    track->play(anchorAt(0, probe::nowNs()));
    std::this_thread::sleep_for(1500ms);
    sound.reset();
    std::this_thread::sleep_for(1000ms);
    const int said = engine.linesWith("could not be served");
    std::printf("audio_playback_test: pass failure rule: 'went away' %d, 'could not be served' %d\n", wentAway.load(), said);
    check(wentAway.load() >= 1, "pass failure rule: the server's loss reached the log");
    check(said == 1, "pass failure rule: a pass cut short outside any track's work is said in one line",
          fmt("%.0f lines", said));
    track.reset();
}

// GOAL: S15.531 -- with no server and no socket, the engine asks once and then waits for the socket to
//       appear: one connection attempt in 6.5 s. False for an engine that retries on a timer, which is
//       the polling the project forbids (two attempts in that time at its five seconds).
// WHY-THIS-SHAPE: attempts are counted at pw_context_connect on AudioOutput's own contexts, told from
//       the stand-in's by their application name; no stand-in runs here anyway.
// CONTROL: control_run.sh 2026-10-11, reverted: the inotify watch never made (`m_serverWatch = -1;`),
//       renderer/audio/audio_output.cpp -> "FAIL: no polling rule: a missing server is asked once, not again on
//       a timer" (2 attempts). isolated_run.sh not exporting WALLPAPERX_CALLER_XDG_RUNTIME_DIR -> "FAIL
//       (harness): no sound client was made for S15.531 ...: not run through isolated_run.sh".
void runNoPollingChecks() {
    const char* kGoals = "S15.531 (no server, no polling)";
    if (!soundClientAllowed(kGoals)) return;
    struct stat st {};
    if (::lstat(runtimeSocket().c_str(), &st) == 0) {
        notVerified(kGoals, runtimeSocket() + " is there, so the engine has a socket to ask on a timer");
        return;
    }
    const int before = probe::g_engineConnectAttempts.load();
    {
        Engine engine(kGoals);
        if (!engine.output) return;
        std::unique_ptr<AudioTrack> track = engine.output->track(asset("wallpaper_tone.mp4"), "nopoll");
        track->play(anchorAt(0, probe::nowNs()));
        std::this_thread::sleep_for(6500ms);
    }
    const int attempts = probe::g_engineConnectAttempts.load() - before;
    std::printf("audio_playback_test: no polling rule: %d connection attempts in 6.5 s with no server\n", attempts);
    check(attempts == 1, "no polling rule: a missing server is asked once, not again on a timer", fmt("%.0f attempts", attempts));
}

// GOAL: S15.533 -- every connection attempt reuses the one PipeWire loop and context: a socket that
//       refuses, then the fallback timer, make three attempts on one context. False for an engine that
//       tears the context down at each failure.
// WHY-THIS-SHAPE: a regular file named pipewire-0 refuses connections and its creation is the socket
//       event, so the attempts are the first, the event's and the timer's, with no server anywhere.
// COVERAGE-ASSERT: at least three attempts.
// CONTROL: control_run.sh 2026-10-11, reverted: connect()'s failure path calling shutdown(),
//       renderer/audio/audio_output.cpp -> "FAIL: one context rule: every attempt reuses the one context" (3
//       contexts).
void runOneContextChecks() {
    const char* kGoals = "S15.533 (one context for every attempt)";
    if (!soundClientAllowed(kGoals)) return;
    const std::string fake = runtimeSocket();
    struct stat st {};
    if (::lstat(fake.c_str(), &st) == 0) {
        notVerified(kGoals, fake + " is already there");
        return;
    }
    const int contextsBefore = probe::g_engineContextsMade.load();
    const int attemptsBefore = probe::g_engineConnectAttempts.load();
    {
        Engine engine(kGoals);
        if (!engine.output) return;
        std::unique_ptr<AudioTrack> track = engine.output->track(asset("wallpaper_tone.mp4"), "onecontext");
        track->play(anchorAt(0, probe::nowNs()));
        std::this_thread::sleep_for(500ms);
        ::close(::open(fake.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0600));
        std::this_thread::sleep_for(6500ms);
        track->stop();
    }
    ::unlink(fake.c_str());
    const int contexts = probe::g_engineContextsMade.load() - contextsBefore;
    const int attempts = probe::g_engineConnectAttempts.load() - attemptsBefore;
    std::printf("audio_playback_test: one context rule: %d contexts for %d connection attempts\n", contexts, attempts);
    check(attempts >= 3, "one context rule: the server was asked three times", fmt("%.0f attempts", attempts));
    check(contexts == 1, "one context rule: every attempt reuses the one context", fmt("%.0f contexts", contexts));
}

// GOAL: S15.532 -- each track's stream carries the name it was made with, and the daemon names a
//       screen's stream after its connector ("Wallpaper on DP-1"). False if every stream shows the same
//       name, or the daemon names the monitor's number where it has a connector.
// CONTROL: control_run.sh 2026-10-11, reverted: ensureStream's media.name made "Wallpaper",
//       renderer/audio/audio_output.cpp -> both lines red ('Wallpaper' three times); heardTrackLocked's
//       connector lookup made `if (false && ...)`, backend/daemon/wallpaper_manager.cpp -> "FAIL: stream name
//       rule: the daemon names a stream after its screen" ('Wallpaper on monitor 0').
void runStreamNameChecks() {
    const char* kGoals = "S15.532 (stream names)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    {
        std::lock_guard<std::mutex> lock(probe::g_pwMutex);
        probe::g_mediaNames.clear();
    }
    {
        Engine engine(kGoals);
        if (!engine.output) return;
        std::unique_ptr<AudioTrack> one = engine.output->track(asset("wallpaper_tone.mp4"), "name-one", "Track one");
        std::unique_ptr<AudioTrack> two = engine.output->track(asset("wallpaper_tone.mp4"), "name-two", "Track two");
        one->play(anchorAt(0, probe::nowNs()));
        two->play(anchorAt(0, probe::nowNs()));
        std::this_thread::sleep_for(800ms);
    }
    {
        Daemon d(smallConfig());
        if (!startHeard(d, asset("wallpaper_tone.mp4"), kGoals)) return;
    }
    std::vector<std::string> names;
    {
        std::lock_guard<std::mutex> lock(probe::g_pwMutex);
        names = probe::g_mediaNames;
    }
    std::string all;
    for (const std::string& n : names) all += "'" + n + "' ";
    std::printf("audio_playback_test: stream name rule: %s\n", all.c_str());
    check(names.size() == 3 && names[0] == "Track one" && names[1] == "Track two",
          "stream name rule: each track's stream carries its own name", all);
    check(names.size() == 3 && names[2] == "Wallpaper on DP-1", "stream name rule: the daemon names a stream after its screen",
          all);
}

// ---------------------------------------------------------------------------------------------- //
// The daemon's steering, further cases
// ---------------------------------------------------------------------------------------------- //

// GOAL: S15.535 -- a video whose nominal rate is above the 120-a-second picture cap plays silent (its
//       pictures run slower than its sound would), while one at exactly 120 a second is heard. False
//       if the capped video's sound plays unstretched against slowed pictures, or the cap's own rate is
//       silenced.
// WHY-THIS-SHAPE: the 120 file is the control for the 240 one -- heard on the same harness -- so the
//       240's silence is not a harness that can sound nothing. The 240's silence is watched for
//       kSoundArrivalDeadline after its third picture, the same time startHeard allows the 120's sound,
//       so a capped video whose sound is merely late cannot pass as silent.
// CONTROL: control_run.sh 2026-10-11, reverted, in backend/daemon/wallpaper_manager.cpp: soundWanted's
//       `!session.slowedByCap` made true -> "FAIL: rate cap rule: a heard video at 240 pictures a second plays
//       silent, with no stream" (1 streams, heard); slowedByCap's `<` made `<=` (it reads
//       averageFrameInterval() now) -> "FAIL: S15.535 ...: a heard video's sound reaches PipeWire within 5 s
//       of its third picture" (the 120 file).
void runRateCapChecks() {
    const char* kGoals = "S15.535 (a video above the picture cap plays silent)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    {
        Daemon d(smallConfig());
        resetCapture();
        const int made = probe::g_streamsMade.load();
        const int64_t started = probe::nowNs();
        d.play(asset("wallpaper_tone_rate240.mp4"), ScreenFocus::Desktop);
        if (!d.waitForCommits(3, 15s)) {
            notVerified(kGoals, "the 240-a-second video drew no pictures within 15 s");
            return;
        }
        std::this_thread::sleep_for(kSoundArrivalDeadline);
        const bool loud = probe::g_lastLoudQueuedAt.load() > started;
        const int streams = probe::g_streamsMade.load() - made;
        std::printf("audio_playback_test: rate cap rule: 240 a second: %d streams, %s, %zu pictures\n", streams,
                    loud ? "heard" : "silent", d.commitCount());
        check(!loud && streams == 0, "rate cap rule: a heard video at 240 pictures a second plays silent, with no stream",
              fmt("%.0f streams", streams) + (loud ? ", heard" : ""));
    }
    Daemon d(smallConfig());
    if (startHeard(d, asset("wallpaper_tone_rate120.mp4"), kGoals)) {
        std::printf("audio_playback_test: rate cap rule: the 120-a-second video is heard\n");
    }
}

// GOAL: S15.536 -- a resume starts the sound where its pictures resume: the first sound seek after a
//       1.5 s pause targets the file's time at the pause, not 1.5 s past it. False if the sound is
//       anchored on the schedule left standing through the pause, which puts it the pause's length ahead
//       (then corrected a round later).
// WHY-THIS-SHAPE: the sound's place at the pause is the last seek before it plus the time played since
//       -- the sound plays at real time between seeks -- so the check is a seek target, which no
//       interleaving moves by more than a picture or two; 300 ms is far inside the 1.5 s at stake.
//       Counting seeks would assert on the scheduler: a late round can earn a legitimate re-anchor.
// COVERAGE-ASSERT: a seek before the pause, and one after the resume.
// CONTROL: control_run.sh 2026-10-11, reverted: steerSoundLocked's overdue-start branch disabled, in
//       backend/daemon/wallpaper_manager.cpp -> "FAIL: resume rule: a resume seeks the sound to where it was
//       paused, not past it by the pause" (+1500.1 ms). Test-side: the seeks before the pause taken from the
//       pause on -> "FAIL: resume rule: the sound was sought at its start and again after the resume".
void runResumeChecks() {
    const char* kGoals = "S15.536 (a resume seeks the sound to its pictures)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    Daemon d(smallConfig());
    const int64_t started = probe::nowNs();
    if (!startHeard(d, asset("wallpaper_beeps_long.mp4"), kGoals)) return;
    std::this_thread::sleep_for(1500ms);
    const int64_t pausedAt = probe::nowNs();
    const std::vector<probe::SoundSeek> before = soundSeeksSince(started);
    d.manager.setMonitorPause(0, "always_paused");
    d.worker.wake();
    std::this_thread::sleep_for(1500ms);
    const int64_t resumedAt = probe::nowNs();
    d.manager.setMonitorPause(0, "play");
    d.worker.wake();
    std::this_thread::sleep_for(1000ms);
    const std::vector<probe::SoundSeek> after = soundSeeksSince(resumedAt);
    if (before.empty() || after.empty()) {
        check(false, "resume rule: the sound was sought at its start and again after the resume",
              fmt("%.0f seeks before the pause, %.0f after the resume", static_cast<double>(before.size()),
                  static_cast<double>(after.size())));
        return;
    }
    const int64_t expected = before.back().targetNs + (pausedAt - before.back().at);
    const double error = (after.front().targetNs - expected) / 1e6;
    std::printf("audio_playback_test: resume rule: %zu seeks after the resume, the first %+.1f ms from the sound's place "
                "at the pause\n",
                after.size(), error);
    check(std::fabs(error) <= 300.0, "resume rule: a resume seeks the sound to where it was paused, not past it by the pause",
          fmt("%+.1f ms", error));
}

// GOAL: S15.537 -- a heard file with one picture plays its sound by itself and loops it: heard in its
//       first pass and again after its 3 s of sound have ended; and a heard one-picture file with no
//       sound is still released by the idle timer. False if a parked session's sound never starts or
//       stops at its end, or a soundless one keeps its session for good.
// WHY-THIS-SHAPE: both windows are measured from the sound's onset -- its first frame heard above 0.3 --
//       not from the call to play, which may come up to 15 s before the first picture: "first pass" is 0.2
//       to 2.5 s after the onset, "looped" 3.5 to 4.5 s, half a second past the 3 s sound's end whatever
//       the start-up took. A record that ends short of that is not verified. "Loops it" is graded only once
//       the first pass was heard.
// COVERAGE-ASSERT: the soundless session held its file before the release.
// CONTROL: control_run.sh 2026-10-11, reverted: advanceSessionLocked's Parked branch disabled,
//       backend/daemon/wallpaper_manager.cpp -> "FAIL: parked rule: a heard file with one picture plays its
//       sound" (nothing heard within 5 s), and the soundless release line (1 descriptor); fill()'s loop
//       return disabled, renderer/audio/audio_output.cpp -> "FAIL: parked rule: and loops it past the
//       sound's end" alone (peak 0.0000); begin()'s notice of the
//       open made a bare `t.content.store(...)`, renderer/audio/audio_output.cpp -- no round then reads the
//       track's None -> "FAIL: parked rule: a heard one-picture file with no sound is still released"
//       (1 descriptor). Test-side, by hand, earlier: counting held at 3 s -> the "held" line.
void runParkedChecks() {
    const char* kGoals = "S15.537 (a one-picture file's sound)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    {
        Daemon d(smallConfig());
        resetCapture();
        const int64_t started = probe::nowNs();
        d.play(asset("wallpaper_one_picture_tone.mp4"), ScreenFocus::Desktop);
        if (!d.waitForCommits(1, 15s)) {
            notVerified(kGoals, "the one-picture video drew no picture within 15 s");
            return;
        }
        if (!waitFor(kSoundArrivalDeadline, [&] { return probe::g_lastLoudQueuedAt.load() > started; })) {
            check(false, "parked rule: a heard file with one picture plays its sound", "nothing heard within 5 s of its picture");
        } else {
            // What is queued is heard a stream latency later; 5 s past the first loud buffer covers 4.5 s past the onset.
            sleepUntil(probe::g_lastLoudQueuedAt.load() + 5000 * kMs);
            const Capture cap = takeCapture();
            const std::optional<int64_t> onset = firstSound(cap, started, 0.3f);
            const int64_t recordEnd = cap.buffers.empty() ? 0 : cap.heardAt(cap.buffers.back(), cap.buffers.back().first +
                                                                                                    cap.buffers.back().frames);
            if (!onset) {
                check(false, "parked rule: a heard file with one picture plays its sound", "no frame above 0.3 in the record");
            } else if (recordEnd < *onset + 4500 * kMs) {
                notVerified(kGoals, fmt("the record ends %.2f s after the sound's onset, short of the loop window",
                                        (recordEnd - *onset) / 1e9));
            } else {
                const float early = peakHeard(cap, *onset + 200 * kMs, *onset + 2500 * kMs);
                const float looped = peakHeard(cap, *onset + 3500 * kMs, *onset + 4500 * kMs);
                std::printf("audio_playback_test: parked rule: one picture: onset %.2f s after the play, %.4f in its first "
                            "pass, %.4f after its 3 s ended\n",
                            (*onset - started) / 1e9, early, looped);
                check(early > 0.3f, "parked rule: a heard file with one picture plays its sound", fmt("peak %.4f", early));
                if (early > 0.3f) check(looped > 0.3f, "parked rule: and loops it past the sound's end", fmt("peak %.4f", looped));
            }
        }
    }
    WallpaperManager::Config config = smallConfig();
    config.idleSessionRelease = 1500ms;
    Daemon d(config);
    const std::string file = asset("wallpaper_one_picture.mp4");
    d.play(file, ScreenFocus::Desktop);
    if (!d.waitForCommits(1, 15s)) {
        notVerified(kGoals, "the soundless one-picture video drew no picture within 15 s");
        return;
    }
    std::this_thread::sleep_for(300ms);
    const int held = descriptorsOn(file);
    std::this_thread::sleep_for(4200ms);
    const int open = descriptorsOn(file);
    std::printf("audio_playback_test: parked rule: no sound: %d descriptors held, %d after the idle release\n", held, open);
    check(held >= 1, "parked rule: the soundless one-picture session held its file", fmt("%.0f", held));
    check(open == 0, "parked rule: a heard one-picture file with no sound is still released", fmt("%.0f descriptors", open));
}

// The daemon's catch-up allowance for the long fixture's 30 pictures a second: a sound ahead of its
// pictures by up to 12 pictures' worth of (1/30 - 1/120) s is caught up, beyond that re-anchored.
constexpr int64_t kLongFixtureInterval = 1'000'000'000 / 30;
constexpr int64_t kCatchUpAllowance = 12 * (kLongFixtureInterval - 1'000'000'000 / 120);

// Plays the long fixture heard, does `disturb` 1.5 s in, and grades the next 1.4 s: heard throughout,
// and, while no round in it waited longer than the catch-up allowance plus a picture, no sound seek
// and no silence in the right channel's tone. False when nothing was graded.
bool runSoundUndisturbed(const char* rule, const char* goals, const std::function<bool(Daemon&)>& disturb) {
    SoundServer sound(goals);
    if (!sound.ready) return false;
    Daemon d(smallConfig());
    if (!startHeard(d, asset("wallpaper_beeps_long.mp4"), goals)) return false;
    std::this_thread::sleep_for(1500ms);
    const int64_t at = probe::nowNs();
    if (!disturb(d)) return false;
    std::this_thread::sleep_for(1500ms);
    const size_t seeks = soundSeeksSince(at).size();
    const Capture cap = takeCapture();
    const std::vector<Commit> commits = d.takeCommits();
    const int64_t end = at + 1400 * kMs;
    // The window's edges count as commits, so a stall across either edge, or one with no commit at all, is a long wait.
    int64_t previous = at, longest = 0;
    for (const Commit& c : commits) {
        if (c.at < at || c.at >= end) continue;
        longest = std::max(longest, c.at - previous);
        previous = c.at;
    }
    longest = std::max(longest, end - previous);
    const auto [silent, heard] = longestSilence(cap, at, end, true);
    std::printf("audio_playback_test: %s: %zu sound seeks; longest silence %llu frames of %llu heard; longest wait "
                "between commits %.1f ms\n",
                rule, seeks, static_cast<unsigned long long>(silent), static_cast<unsigned long long>(heard), longest / 1e6);
    check(heard >= 48000ULL * 1400 / 1000 * 9 / 10, std::string(rule) + ": the sound was heard through the window",
          fmt("%.0f frames", static_cast<double>(heard)));
    if (longest > kCatchUpAllowance + kLongFixtureInterval) {
        notVerified(goals, fmt("a round waited %.0f ms between commits, past the %.0f ms catch-up allowance plus a "
                               "picture, so a re-anchor would be correct",
                               longest / 1e6, (kCatchUpAllowance + kLongFixtureInterval) / 1e6));
        return true;
    }
    check(silent <= 48, std::string(rule) + ": the sound plays on", fmt("%.0f frames silent", static_cast<double>(silent)));
    check(seeks == 0, std::string(rule) + ": the sound is not moved", fmt("%.0f seeks", static_cast<double>(seeks)));
    return true;
}

// GOAL: S15.538 -- a picture held up once, by up to a dozen pictures at the rate cap, is caught up to
//       its playing sound: the sound is not moved and plays on. False if a late picture re-anchors the
//       sound, which jumps back by the delay.
// WHY-THIS-SHAPE: the picture decoder's next av_read_frame sleeps 70 ms, once. Zero seeks are asserted
//       because the only seek a playing sound gets is a re-anchor, and after a stall inside the catch-up
//       allowance a re-anchor is the defect; seeks and silence are graded only when no round in the
//       window waited past that allowance plus a picture (a scheduler stall, not the staged 70 ms) --
//       otherwise not verified, as S15.536 notes a late round can earn a legitimate re-anchor.
// COVERAGE-ASSERT: the stall was consumed by a picture read.
// CONTROL: control_run.sh 2026-10-11, reverted: steerSoundLocked's catch-up branch disabled,
//       backend/daemon/wallpaper_manager.cpp -> "FAIL: late picture rule: the sound is not moved"
//       (1 seek). Test-side, by hand, earlier: the stall never applied -> "the staged picture stall
//       was reached"; the window moved before the sound -> "the sound was heard through the window".
void runLatePictureChecks() {
    const bool graded = runSoundUndisturbed("late picture rule", "S15.538 (a late picture does not move the sound)", [](Daemon&) {
        probe::g_stallPictureMs.store(70);
        return true;
    });
    const int left = probe::g_stallPictureMs.exchange(0);
    if (graded) check(left == 0, "late picture rule: the staged picture stall was reached", fmt("%.0f ms left unused", left));
}

// GOAL: S15.539 -- a render owed for the same video (a resize) leaves its sound playing: no seek and
//       no silence. False if a round that finds a render owed stops the sound, which the next playing
//       round starts again.
// WHY-THIS-SHAPE: another monitor's job is queued first, so one clock round runs while this monitor's
//       render is owed, which is the round that would stop the sound. Seeks and silence are graded under
//       the same catch-up-allowance guard as S15.538's.
// COVERAGE-ASSERT: the size change posted a render.
// CONTROL: control_run.sh 2026-10-11, reverted: advanceSessionLocked stopping the sound on any stopped
//       round, backend/daemon/wallpaper_manager.cpp -> "FAIL: render owed rule: the sound is not moved"
//       (1 seek), and "the sound plays on" (1494 frames silent). Test-side, by hand, earlier: no size change
//       -> "FAIL: render owed rule: the size
//       change posted a render".
void runRenderOwedChecks() {
    runSoundUndisturbed("render owed rule", "S15.539 (a render owed keeps the sound)", [](Daemon& d) {
        std::vector<int> redraw;
        d.manager.setOutputSize("DP-1", 96, 72, redraw);
        d.worker.post(7);
        for (const int id : redraw) d.worker.post(id);
        check(!redraw.empty(), "render owed rule: the size change posted a render");
        return !redraw.empty();
    });
}

// Puts back on scope exit what a phase broke on purpose.
struct ProbeReset {
    ~ProbeReset() {
        probe::g_swrMode.store(probe::kSwrPass);
        probe::g_armReadFailure.store(false);
        probe::g_readsToFail.store(0);
        probe::g_readFailuresArmed.store(1);
        for (auto& f : probe::g_failReadFds) f.store(-1);
        probe::g_failEventPoll.store(false);
        probe::g_unknownAverageRate.store(false);
        std::lock_guard<std::mutex> lock(probe::g_openMutex);
        probe::g_openWatch.clear();
    }
};

// GOAL: S15.543 -- a heard one-picture file whose sound fails on every decode is reopened for its sound at
//       most kPlaybackRetries times (two opens each: pictures and sound), then stays released: no open for
//       3.5 s and no descriptor left. False if the track's failure reaches no round (the file is held for
//       good), or the reopen budget is not spent (opened, released and opened for ever).
// WHY-THIS-SHAPE: the decode is failed at swr_convert, which only the sound decoder calls, once the sound
//       is heard; 1.5 s idle release. Opens are counted at avformat_open_input, and the retries are waited
//       out by event: each open restarts a 3.5 s quiet window (over twice a release cycle), and the wait
//       ends at the first window with no open -- or at 4 opens past the bound, so the defect of reopening
//       for ever ends the wait too. A fixed sleep would grade a slow machine's retries as "no further open".
// COVERAGE-ASSERT: both decoders held the file, a conversion really failed, and the wait ended quiet.
// CONTROL: control_run.sh 2026-10-12, reverted, renderer/audio/audio_output.cpp: closeAfterFailure's
//       `setContent(t, ...Failed)` made a bare `t.content.store(...)` -> "FAIL: parked failure rule: a
//       one-picture file whose sound stopped on a failed decode ends released" (1 descriptor, 0 opens); in
//       backend/daemon/wallpaper_manager.cpp reopenVideosLocked, `spend(Fault::Daemon)` made
//       `(spend(...) || true)` -> "FAIL: parked failure rule: the reopens end within the retry budget" (12
//       opens, bound 6, still opening) and "... ends released" (1 descriptor).
void runParkedSoundFailsChecks() {
    const char* kGoals = "S15.543 (a parked file whose sound fails is released)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    ProbeReset reset;
    WallpaperManager::Config config = smallConfig();
    config.idleSessionRelease = 1500ms;
    Daemon d(config);
    const std::string file = asset("wallpaper_one_picture_tone.mp4");
    {
        std::lock_guard<std::mutex> lock(probe::g_openMutex);
        probe::g_openWatch = "wallpaper_one_picture_tone.mp4";
    }
    const int64_t started = probe::nowNs();
    d.play(file, ScreenFocus::Desktop);
    if (!d.waitForCommits(1, 15s) || !waitFor(kSoundArrivalDeadline, [&] { return probe::g_lastLoudQueuedAt.load() > started; })) {
        notVerified(kGoals, "the one-picture tone file was not drawn and heard, so it has no sound to lose");
        return;
    }
    std::this_thread::sleep_for(500ms);
    const int held = descriptorsOn(file);
    probe::g_swrFailed.store(0);
    const int opensBefore = probe::g_opens.load();
    probe::g_swrMode.store(probe::kSwrFail);
    const int bound = 2 * static_cast<int>(WallpaperManager::kPlaybackRetries);
    bool quiet = false;
    while (probe::g_opens.load() - opensBefore <= bound + 4) {
        const int seen = probe::g_opens.load();
        if (!waitFor(3500ms, [&] { return probe::g_opens.load() != seen; })) {
            quiet = true;
            break;
        }
    }
    const int opens = probe::g_opens.load() - opensBefore;
    const int failed = probe::g_swrFailed.load();
    const int open = descriptorsOn(file);
    std::printf("audio_playback_test: parked failure rule: %d descriptors while heard, %d failed conversions, %d opens "
                "after the failure began, then %s; %d descriptors at the end\n",
                held, failed, opens, quiet ? "3.5 s with none" : "still opening", open);
    check(held >= 2 && failed > 0, "parked failure rule: both decoders held the file and the sound's decode failed",
          fmt("%.0f descriptors, %.0f failed conversions", held, failed));
    check(quiet && opens <= bound, "parked failure rule: the reopens end within the retry budget",
          fmt("%.0f opens, bound %.0f", opens, bound));
    check(open == 0, "parked failure rule: a one-picture file whose sound stopped on a failed decode ends released",
          fmt("%.0f descriptors", open));
}

// GOAL: S15.544 -- a one-picture file with sound, released while paused, is opened again and heard
//       within 4 s of play resuming, from the picture on screen. False if a released parked session is
//       reopened only by a new wallpaper, or reopened without its picture decoded, which loses its sound.
// WHY-THIS-SHAPE: a one-second paused release, the release seen as zero descriptors before the resume
//       (not verified otherwise: nothing would need reopening). Heard means a loud buffer queued after it.
// CONTROL: control_run.sh 2026-10-11, reverted, backend/daemon/wallpaper_manager.cpp: reopenVideosLocked's
//       Parked clause made `false && ...` -> "FAIL: parked reopen rule: a released one-picture file's sound is
//       heard again within 4 s of play resuming" (never); advanceSessionLocked's `parkedCatchingUp` made
//       `false && ...` -> the same line (never).
void runParkedReopenChecks() {
    const char* kGoals = "S15.544 (a released one-picture file's sound comes back)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    WallpaperManager::Config config = smallConfig();
    config.pausedSessionRelease = 1000ms;
    Daemon d(config);
    const std::string file = asset("wallpaper_one_picture_tone.mp4");
    const int64_t started = probe::nowNs();
    d.play(file, ScreenFocus::Desktop);
    if (!d.waitForCommits(1, 15s) || !waitFor(kSoundArrivalDeadline, [&] { return probe::g_lastLoudQueuedAt.load() > started; })) {
        notVerified(kGoals, "the one-picture tone file was not drawn and heard");
        return;
    }
    d.manager.setMonitorPause(0, "always_paused");
    d.worker.wake();
    if (!waitFor(4000ms, [&] { return descriptorsOn(file) == 0; })) {
        notVerified(kGoals, fmt("the paused session was not released within 4 s (%.0f descriptors), so nothing needs "
                                "reopening",
                                descriptorsOn(file)));
        return;
    }
    const int64_t resumed = probe::nowNs();
    const int64_t loudBefore = probe::g_lastLoudQueuedAt.load();
    d.manager.setMonitorPause(0, "play");
    d.worker.wake();
    const bool heard = waitFor(4000ms, [&] { return probe::g_lastLoudQueuedAt.load() > resumed; });
    const int64_t heardAt = probe::g_lastLoudQueuedAt.load();
    std::printf("audio_playback_test: parked reopen rule: last loud buffer %.2f s before the resume; heard again %s\n",
                (resumed - loudBefore) / 1e9, heard ? fmt("%.3f s after the resume", (heardAt - resumed) / 1e9).c_str() : "never");
    check(heard, "parked reopen rule: a released one-picture file's sound is heard again within 4 s of play resuming");
}

// GOAL: S15.545 -- a heard one-picture file found to have no sound, once released, is not opened again
//       while nothing changes: no open of the file in the 4 s after its release. False if the reopen for
//       sound does not remember the file had none, so it is opened, released and opened for ever.
// WHY-THIS-SHAPE: opens counted at avformat_open_input by file name; any open in the window is wrong, so
//       the count is exact under every interleaving. The window starts once the file is seen with no
//       descriptor, or after 4 s regardless: a file reopened a millisecond after each release is never
//       seen at zero, and waiting for that would skip the very defect graded. Not verified only when
//       neither a release nor an open was seen -- a session never released has nothing to reopen.
// CONTROL: control_run.sh 2026-10-12, reverted: reopenVideosLocked's `state.noSound.forSeq !=
//       state.wallpaperSeq` test made `true`, backend/daemon/wallpaper_manager.cpp -> "FAIL: parked silent
//       rule: a released one-picture file with no sound is not opened again" (6 opens; no release seen).
void runParkedSilentStaysChecks() {
    const char* kGoals = "S15.545 (a soundless one-picture file stays released)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    ProbeReset reset;
    WallpaperManager::Config config = smallConfig();
    config.idleSessionRelease = 1000ms;
    Daemon d(config);
    const std::string file = asset("wallpaper_one_picture.mp4");
    {
        std::lock_guard<std::mutex> lock(probe::g_openMutex);
        probe::g_openWatch = "wallpaper_one_picture.mp4";
    }
    d.play(file, ScreenFocus::Desktop);
    if (!d.waitForCommits(1, 15s)) {
        notVerified(kGoals, "the soundless one-picture file drew no picture within 15 s");
        return;
    }
    const bool released = waitFor(4000ms, [&] { return descriptorsOn(file) == 0; });
    const int before = probe::g_opens.load();
    std::this_thread::sleep_for(4000ms);
    const int opens = probe::g_opens.load() - before;
    std::printf("audio_playback_test: parked silent rule: released seen: %s; %d opens in the next 4 s\n",
                released ? "yes" : "no", opens);
    if (!released && opens == 0) {
        notVerified(kGoals, "the soundless one-picture file was neither released nor opened again (S15.537 grades the "
                            "release)");
        return;
    }
    check(opens == 0, "parked silent rule: a released one-picture file with no sound is not opened again",
          fmt("%.0f opens", opens));
}

// GOAL: S15.551 -- a heard one-picture file whose picture nobody collects goes silent once the screen
//       counts as unwatched, with nothing else waking the daemon: no buffer queued from 1 s past
//       `unwatchedAfter` (2.5 s) to 3 s past it. False if the parked session waits on its sound with no
//       deadline, so the sound loops on for a screen nobody is looking at.
// WHY-THIS-SHAPE: after play() the test sends the daemon nothing, and the idle release is a minute, so
//       only a round woken by the unwatched moment itself can stop the sound. "No buffer" rather than
//       "silent", so a stream left running on silence fails too.
// COVERAGE-ASSERT: the sound was heard while watched.
// CONTROL: control_run.sh 2026-10-12, reverted: advanceSessionLocked's parked branch `return unwatchedAt;`
//       made `return std::nullopt;`, backend/daemon/wallpaper_manager.cpp -> "FAIL: parked unwatched rule:
//       the sound stops once the screen is unwatched" (94 buffers, peak 0.3539).
void runParkedUnwatchedChecks() {
    const char* kGoals = "S15.551 (a parked file's sound stops when unwatched)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    WallpaperManager::Config config = smallConfig();
    config.unwatchedAfter = 2500ms;
    Daemon d(config);
    resetCapture();
    const int64_t started = probe::nowNs();
    d.play(asset("wallpaper_one_picture_tone.mp4"), ScreenFocus::Desktop);
    if (!d.waitForCommits(1, 15s)) {
        notVerified(kGoals, "the one-picture tone file drew no picture within 15 s");
        return;
    }
    const int64_t committed = d.takeCommits().front().at;
    const bool heard = waitFor(2000ms, [&] { return probe::g_lastLoudQueuedAt.load() > started; });
    sleepUntil(committed + 5500 * kMs);
    const Capture cap = takeCapture();
    const Window after = windowOf(cap, committed + 3500 * kMs, committed + 5500 * kMs);
    std::printf("audio_playback_test: parked unwatched rule: heard %s; %zu buffers, peak %.4f from 1 s past unwatched\n",
                heard ? "yes" : "no", after.buffers, after.peak);
    check(heard, "parked unwatched rule: the sound is heard while watched");
    check(after.buffers == 0, "parked unwatched rule: the sound stops once the screen is unwatched",
          fmt("%.0f buffers", static_cast<double>(after.buffers)));
}

// GOAL: S15.552 -- a playing video left owing a render plays its sound on, but not past the moment the
//       screen counts as unwatched: no buffer from 1 s to 3 s past `unwatchedAfter` (3 s). False if the
//       render-owed round waits only for the idle release, so the sound plays on unwatched until then.
// WHY-THIS-SHAPE: the resize's render is never posted, so the render stays owed and no picture deadline
//       wakes a round; the idle release is a minute. Graded only if the render was owed within 2 s of the
//       first picture and no picture was committed after it -- otherwise the pictures' own rounds could be
//       what stops the sound, and the phase is not verified.
// COVERAGE-ASSERT: the sound was heard while the render was owed.
// CONTROL: control_run.sh 2026-10-12, reverted: advanceSessionLocked's render-owed return made
//       `return releaseAt;`, backend/daemon/wallpaper_manager.cpp -> "FAIL: render owed unwatched rule: the
//       sound stops once the screen is unwatched" (94 buffers).
void runRenderOwedUnwatchedChecks() {
    const char* kGoals = "S15.552 (a render-owed video's sound stops when unwatched)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    WallpaperManager::Config config = smallConfig();
    config.unwatchedAfter = 3000ms;
    Daemon d(config);
    if (!startHeard(d, asset("wallpaper_tone.mp4"), kGoals)) return;
    const int64_t committed = d.takeCommits().front().at;
    std::vector<int> redraw;
    d.manager.setOutputSize("DP-1", 96, 72, redraw);
    const int64_t owedAt = probe::nowNs();
    if (owedAt > committed + 2000 * kMs || redraw.empty()) {
        notVerified(kGoals, fmt("the render was owed %.2f s after the first picture (wanted within 2 s), %.0f renders "
                                "posted",
                                (owedAt - committed) / 1e9, static_cast<double>(redraw.size())));
        return;
    }
    sleepUntil(committed + 6000 * kMs);
    const Capture cap = takeCapture();
    size_t lateCommits = 0;
    for (const Commit& c : d.takeCommits()) lateCommits += c.at > owedAt + 200 * kMs ? 1 : 0;
    const float owedHeard = peakHeard(cap, owedAt + 200 * kMs, committed + 2800 * kMs);
    const Window after = windowOf(cap, committed + 4000 * kMs, committed + 6000 * kMs);
    std::printf("audio_playback_test: render owed unwatched rule: owed %.2f s in; %zu pictures after; peak %.4f while "
                "owed; %zu buffers, peak %.4f from 1 s past unwatched\n",
                (owedAt - committed) / 1e9, lateCommits, owedHeard, after.buffers, after.peak);
    if (lateCommits != 0) {
        notVerified(kGoals, fmt("%.0f pictures were committed after the render was owed, so it was not owed throughout",
                                static_cast<double>(lateCommits)));
        return;
    }
    check(owedHeard > 0.25f, "render owed unwatched rule: the sound is heard while the render is owed",
          fmt("peak %.4f", owedHeard));
    check(after.buffers == 0, "render owed unwatched rule: the sound stops once the screen is unwatched",
          fmt("%.0f buffers", static_cast<double>(after.buffers)));
}

// GOAL: S15.553 -- a sound that failed once is not taken for a file without sound: a heard one-picture
//       file whose sound decode fails once, or whose sound fails to open once, is opened again after its
//       release and heard. False if a failure is recorded as "no sound", which never reopens.
// WHY-THIS-SHAPE: the decode is failed at swr_convert until one call fails; the open at
//       av_find_best_stream for audio (both the sound decoder's alone), lifted as soon as one refusal is
//       seen -- a second before the 1 s idle release can reopen. Opens are counted at avformat_open_input
//       from the moment the failure is lifted, so the play's own opens cannot pass "opened again". Each
//       case has its own sound server: under a shared one the second daemon's stream was never linked.
// COVERAGE-ASSERT: the staged failure happened in each case.
// CONTROL: control_run.sh 2026-10-12, reverted, backend/daemon/wallpaper_manager.cpp: the parked branch's
//       record forced to `Why::Silent` -> "FAIL: failed sound rule, decode: the file is opened again" (no open
//       within 5 s) and the same for the open; renderer/audio/audio_output.cpp begin()'s catch storing
//       `Content::None` -> "FAIL: failed sound rule, open: the file is opened again", the decode case green.
void parkedFailedReopen(const char* goals, const std::string& which) {
    SoundServer sound(goals);
    if (!sound.ready) return;
    ProbeReset reset;
    struct Restore {
        ~Restore() { probe::g_refuseAudioStream.store(false); }
    } restore;
    WallpaperManager::Config config = smallConfig();
    config.idleSessionRelease = 1000ms;
    Daemon d(config);
    const std::string file = asset("wallpaper_one_picture_tone.mp4");
    {
        std::lock_guard<std::mutex> lock(probe::g_openMutex);
        probe::g_openWatch = "wallpaper_one_picture_tone.mp4";
    }
    const std::string rule = "failed sound rule, " + which;
    const bool decode = which == "decode";
    const int refusedBefore = probe::g_audioStreamsRefused.load();
    if (!decode) probe::g_refuseAudioStream.store(true);
    const int64_t started = probe::nowNs();
    d.play(file, ScreenFocus::Desktop);
    if (!d.waitForCommits(1, 15s)) {
        notVerified(goals, "the one-picture tone file drew no picture within 15 s");
        return;
    }
    bool failed = false;
    if (decode) {
        if (!waitFor(kSoundArrivalDeadline, [&] { return probe::g_lastLoudQueuedAt.load() > started; })) {
            notVerified(goals, "the one-picture tone file was not heard, so its decode cannot be failed once");
            return;
        }
        std::this_thread::sleep_for(300ms);
        probe::g_swrFailed.store(0);
        probe::g_swrMode.store(probe::kSwrFail);
        failed = waitFor(2000ms, [] { return probe::g_swrFailed.load() > 0; });
        probe::g_swrMode.store(probe::kSwrPass);
    } else {
        failed = waitFor(kSoundArrivalDeadline, [&] { return probe::g_audioStreamsRefused.load() > refusedBefore; });
        probe::g_refuseAudioStream.store(false);
    }
    const int opensBefore = probe::g_opens.load();
    const bool reopened = failed && waitFor(5000ms, [&] { return probe::g_opens.load() > opensBefore; });
    const int64_t reopenedAt = probe::nowNs();
    const bool heard = reopened && waitFor(4000ms, [&] { return probe::g_lastLoudQueuedAt.load() > reopenedAt; });
    std::printf("audio_playback_test: %s: failed %s; opened again %s; heard again %s\n", rule.c_str(),
                failed ? "yes" : "no", reopened ? "yes" : "no", heard ? "yes" : "no");
    check(failed, rule + ": the staged failure happened");
    if (!failed) return;
    check(reopened, rule + ": the file is opened again", "no open within 5 s of the failure");
    check(heard, rule + ": and its sound is heard again", reopened ? "nothing loud within 4 s of the open" : "");
}

void runParkedFailedReopenChecks() {
    const char* kGoals = "S15.553 (a failed sound is opened again)";
    parkedFailedReopen(kGoals, "decode");
    parkedFailedReopen(kGoals, "open");
}

// GOAL: S15.554 -- volume 0 counts as not heard for a one-picture file: it is released by the idle timer,
//       decodes nothing and is not opened again while at 0, and is heard again within 4 s of volume 1.
//       False if volume 0 keeps the sound decoding at zero gain, so the file is never released, or if the
//       reopen for sound ignores the volume and opens it at 0.
// WHY-THIS-SHAPE: 1 s idle release; the release is waited for, then 2.5 s at 0 graded for conversions
//       (swr_convert) and opens (avformat_open_input). The opens line is what catches a reopen that
//       ignores the volume: the reopened session releases again, so "released" alone can still be seen.
// COVERAGE-ASSERT: the file was heard at volume 1 before the change.
// CONTROL: control_run.sh 2026-10-12, reverted, backend/daemon/wallpaper_manager.cpp: `&& volume > 0.0f`
//       taken out of SoundWanted::audible() (wallpaper_manager.h) -> "FAIL: volume zero rule: a parked file
//       at volume 0 is released" (2 descriptors) and "... decodes nothing" (117 conversions); reopenVideosLocked's
//       SoundWanted built without its `wallpaperVolume(...)` argument -> "FAIL: volume zero rule: and is not
//       opened again at volume 0" (2 opens) alone.
void runParkedVolumeZeroChecks() {
    const char* kGoals = "S15.554 (volume 0 releases a one-picture file)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    WallpaperManager::Config config = smallConfig();
    config.idleSessionRelease = 1000ms;
    Daemon d(config);
    const std::string file = asset("wallpaper_one_picture_tone.mp4");
    const int64_t started = probe::nowNs();
    d.play(file, ScreenFocus::Desktop);
    if (!d.waitForCommits(1, 15s) || !waitFor(kSoundArrivalDeadline, [&] { return probe::g_lastLoudQueuedAt.load() > started; })) {
        notVerified(kGoals, "the one-picture tone file was not drawn and heard");
        return;
    }
    ProbeReset reset;
    {
        std::lock_guard<std::mutex> lock(probe::g_openMutex);
        probe::g_openWatch = "wallpaper_one_picture_tone.mp4";
    }
    d.properties({{"volume", 0.0}});
    const bool released = waitFor(4000ms, [&] { return descriptorsOn(file) == 0; });
    const int heldAtZero = descriptorsOn(file);
    const uint64_t swrBefore = probe::g_swrCalls.load();
    const int opensBefore = probe::g_opens.load();
    std::this_thread::sleep_for(2500ms);
    const uint64_t decoded = probe::g_swrCalls.load() - swrBefore;
    const int opens = probe::g_opens.load() - opensBefore;
    const int64_t raised = probe::nowNs();
    d.properties({{"volume", 1.0}});
    const bool heard = waitFor(4000ms, [&] { return probe::g_lastLoudQueuedAt.load() > raised; });
    std::printf("audio_playback_test: volume zero rule: released %s; %llu conversions and %d opens in 2.5 s at 0; heard "
                "at 1 %s\n",
                released ? "yes" : "no", static_cast<unsigned long long>(decoded), opens, heard ? "yes" : "no");
    check(released, "volume zero rule: a parked file at volume 0 is released", fmt("%.0f descriptors", heldAtZero));
    check(decoded == 0, "volume zero rule: and decodes nothing", fmt("%.0f conversions", static_cast<double>(decoded)));
    check(opens == 0, "volume zero rule: and is not opened again at volume 0", fmt("%.0f opens", opens));
    check(heard, "volume zero rule: raised to 1, it is heard again within 4 s");
}

// `failures` reads of `file` fail, from the first read after the first av_seek_frame from now -- the
// loop's -- and the picture decoder recovers by re-opening its container (`reopens`) or by another
// seek; graded on the log over the next 6.5 s.
void pictureReadRecovered(const char* goals, const std::string& file, const std::string& which, int failures,
                          bool reopens) {
    Daemon d(smallConfig());
    probe::g_readsFailed.store(0);
    probe::g_readFailuresArmed.store(failures);
    d.play(file, std::nullopt);
    if (!d.waitForCommits(5, 15s)) {
        notVerified(goals, "the " + which + " file drew no pictures within 15 s");
        return;
    }
    const std::optional<std::vector<int>> fds = descriptorList(file);
    if (!fds || fds->empty() || fds->size() > 4) {
        notVerified(goals, "the " + which + " file's descriptors could not be told apart");
        return;
    }
    for (size_t i = 0; i < fds->size(); ++i) probe::g_failReadFds[i].store((*fds)[i]);
    {
        std::lock_guard<std::mutex> lock(probe::g_openMutex);
        probe::g_openWatch = std::filesystem::path(file).filename().string();
    }
    const int opensBefore = probe::g_opens.load();
    wallpaperx::testing::StreamCapture log(STDERR_FILENO);
    const size_t before = d.commitCount();
    probe::g_armReadFailure.store(true);
    std::this_thread::sleep_for(6500ms);
    for (auto& f : probe::g_failReadFds) f.store(-1);
    log.restore();
    const int opened = probe::g_opens.load() - opensBefore;
    const size_t pictures = d.commitCount() - before;
    const int stopped = log.linesWith({"stopped playing"});
    std::printf("audio_playback_test: recovered read rule, %s: %d reads failed, %d container opens, %zu pictures in "
                "6.5 s, %d stop lines\n",
                which.c_str(), probe::g_readsFailed.load(), opened, pictures, stopped);
    check(probe::g_readsFailed.load() == failures, "recovered read rule, " + which + ": the staged reads failed after a seek",
          fmt("%.0f of %.0f failed", probe::g_readsFailed.load(), failures));
    // A stop fails the case whatever path ran; the daemon re-opens a stopped file itself, so the opens
    // say which path the recovery took only when nothing stopped.
    if (stopped == 0 && (opened > 0) != reopens) {
        notVerified(goals, "the '" + which + "' case recovered " + (opened > 0 ? "by re-opening" : "without re-opening") +
                               " its container, so it no longer reaches the clear it stands for");
        return;
    }
    check(stopped == 0, "recovered read rule, " + which + ": the video plays on past the later ends of its file",
          fmt("%.0f 'stopped playing' lines, %.0f pictures in 6.5 s", stopped, static_cast<double>(pictures)));
}

// GOAL: S15.546 -- a failed read the picture decoder recovered from is not reported again at a later,
//       normal end of the file: a looping video with reads failed after its loop's seek plays on.
//       False if the error ffmpeg stored for that read outlives the recovery and turns the next end of
//       file into "cannot read the wallpaper".
// WHY-THIS-SHAPE: a B-frames file over twice ffmpeg's 32 KiB buffer, so a read after the seek reaches the
//       descriptor, and whose pass ends on a failing read, before the next loop's seek could clear a
//       stale error. rewind() tries packet 1 by decode time, then by presentation time, then by byte
//       position, and re-opens the container only when all three fail: three failed reads force the
//       re-open (openContainer's clear), one is recovered by the presentation-time seek (seekFrame's
//       clear). Which path ran is counted at avformat_open_input; another is not verified. A file without
//       B-frames is not used: it reaches no failing read before the next seek clears the error, so
//       neither clear's removal shows there. Graded on the log, not lastError: the reopen budget redraws
//       the failed video and clears lastError, so a check of it passes on the defect.
// COVERAGE-ASSERT: exactly the staged reads failed; the re-open happened, or did not, as each case needs.
// CONTROL: control_run.sh 2026-10-11, reverted, renderer/video/ffmpeg_file.cpp: seekFrame's clear deleted ->
//       "FAIL: recovered read rule, sought again: the video plays on past the later ends of its file" (1
//       'stopped playing' line); both clears deleted -> that and "... re-opened: ..." (1 line each).
//       openContainer's `io->error = 0;` alone deleted stays green: the decoder seeks after every re-open
//       before its next failing read, and that seek's clear covers it -- no observable effect on its own.
void runPictureReadRecoveredChecks() {
    const char* kGoals = "S15.546 (a recovered read error is not reported later)";
    ProbeReset reset;
    pictureReadRecovered(kGoals, asset("wallpaper_reads_bframes.mp4"), "re-opened", 3, true);
    pictureReadRecovered(kGoals, asset("wallpaper_reads_bframes.mp4"), "sought again", 1, false);
}

// GOAL: S15.547 -- a play put off because it was far ahead still begins when the sound before it has
//       reached the end of its file: with a 16384-frame WAV playing, a play 1.5 s later is heard. False
//       if a put-off play is given a wake time only while the sound before it is still being decoded.
// WHY-THIS-SHAPE: 16384 frames is exactly one fill, so the first fill takes the whole file without
//       meeting its end, and the end is met on a later fill -- after the second play was put off. If the
//       test thread is late the second play is begun at once instead, which passes on a correct engine.
// COVERAGE-ASSERT: the first play is heard.
// CONTROL: control_run.sh 2026-10-11, reverted: `t.filling &&` put back in nextPassAt's test,
//       renderer/audio/audio_output.cpp -> "FAIL: put-off play rule: a play put off past the end of the sound
//       before it is heard" (peak 0.0000).
void runDeferredPlayChecks() {
    const char* kGoals = "S15.547 (a put-off play outlives the end of the sound before it)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    resetCapture();
    Engine engine(kGoals);
    if (!engine.output) return;
    std::unique_ptr<AudioTrack> track = engine.output->track(asset("sound_16384_frames.wav"), "deferred");
    const int64_t t0 = probe::nowNs() + 200 * kMs;
    track->play(anchorAt(0, t0));
    std::this_thread::sleep_for(100ms);
    track->play(anchorAt(0, t0 + 1500 * kMs));
    sleepUntil(t0 + 2000 * kMs);
    track->stop();
    std::this_thread::sleep_for(200ms);
    const Capture cap = takeCapture();
    const float first = peakHeard(cap, t0 + 20 * kMs, t0 + 300 * kMs);
    const float second = peakHeard(cap, t0 + 1550 * kMs, t0 + 1800 * kMs);
    std::printf("audio_playback_test: put-off play rule: first %.4f, the put-off play %.4f\n", first, second);
    check(first > 0.1f, "put-off play rule: the first play is heard", fmt("peak %.4f", first));
    check(second > 0.1f, "put-off play rule: a play put off past the end of the sound before it is heard",
          fmt("peak %.4f", second));
}

// GOAL: S15.548 -- a looping sound begun again long after its anchor -- its stream failed and was made
//       again -- starts over at its `from`: the first seek after the new stream targets at most 100 ms
//       into the file. False if the old anchor's time is kept, which seeks past the 2 s file's end.
// WHY-THIS-SHAPE: the stream error is delivered through the stream's own state callback, as in S15.523;
//       the seek graded is the first after the new stream is made, which begin() does just before its
//       seek, so a loop's own wrap-round seek before the error cannot be the one graded.
// COVERAGE-ASSERT: a new stream was made after the error, and a seek followed it.
// CONTROL: control_run.sh 2026-10-11, reverted: begin()'s `if (anchor.loop) at = std::max(...)` deleted,
//       renderer/audio/audio_output.cpp -> "FAIL: loop re-begin rule: a loop begun again starts at its from"
//       (the first seek targets 7926.2 ms into a 2 s file).
void runLoopRebeginChecks() {
    const char* kGoals = "S15.548 (a re-begun loop starts at its from)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    {
        std::lock_guard<std::mutex> lock(probe::g_pwMutex);
        probe::g_listeners.clear();
    }
    Engine engine(kGoals);
    if (!engine.output) return;
    std::unique_ptr<AudioTrack> track = engine.output->track(asset("wallpaper_tone.mp4"), "rebegin");
    track->play({0ns, Clock::now(), true});
    std::this_thread::sleep_for(3000ms);
    probe::StreamListener listener;
    {
        std::lock_guard<std::mutex> lock(probe::g_pwMutex);
        if (probe::g_listeners.size() == 1) listener = probe::g_listeners.front();
    }
    if (listener.events == nullptr) {
        check(false, "loop re-begin rule: the looping track had one stream before the error");
        return;
    }
    const int madeBefore = probe::g_streamsMade.load();
    listener.events->state_changed(listener.data, PW_STREAM_STATE_STREAMING, PW_STREAM_STATE_ERROR, "injected");
    const bool remade = waitFor(8000ms, [&] { return probe::g_streamsMade.load() > madeBefore; });
    const int64_t madeAt = probe::g_lastStreamMadeAt.load();
    std::vector<probe::SoundSeek> seeks;
    const bool sought = remade && waitFor(1000ms, [&] {
                            seeks = soundSeeksSince(madeAt);
                            return !seeks.empty();
                        });
    track->stop();
    if (!remade || !sought) {
        check(false, "loop re-begin rule: the loop was begun again on a new stream, with a seek",
              remade ? "no seek after the new stream" : "no new stream within 8 s");
        return;
    }
    std::printf("audio_playback_test: loop re-begin rule: the first seek after the new stream targets %.1f ms\n",
                seeks.front().targetNs / 1e6);
    check(seeks.front().targetNs <= 100 * kMs, "loop re-begin rule: a loop begun again starts at its from",
          fmt("the first seek targets %.1f ms into a 2 s file", seeks.front().targetNs / 1e6));
}

// GOAL: S15.549 -- an AudioOutput whose thread stops on an error answers for every track it held: a
//       track playing then reads Failed, the stop is noticed, and a track made afterwards reads Failed at
//       once. False if a stopped output leaves its tracks Sound or Unknown, which holds a parked session
//       waiting for a change that never comes, or calls them None, which marks the file as having no sound.
// WHY-THIS-SHAPE: the output's own poll() is failed once (the next poll whose first descriptor is an
//       eventfd), so run() ends as it would on a real error. run() sets each track's content and then
//       notifies, one track at a time, so each wait is for the whole outcome -- every content and the
//       notice count -- never its first sign; each part is then checked on its own so a failure names it.
//       A track made just as it stops may be refused by track() or ended by run(): both read Failed.
// COVERAGE-ASSERT: the first track's open was noticed exactly once before the stop.
// CONTROL: control_run.sh 2026-10-12, reverted, renderer/audio/audio_output.cpp: run()'s exit loop without
//       its setContent -> "FAIL: stopped output rule: a track playing when the output stopped reads Failed",
//       "... a track made as it stopped ..." and "... the stop was noticed" (1 notice before, 1 after); that
//       setContent's `Failed` made `None` -> the first two lines (both read None); track()'s refusal storing
//       `None` -> "... a track made after it stopped reads Failed" alone; run()'s exit without
//       `m_stopping.store(true...)` -> the same line alone (it stayed Unknown).
void runStoppedOutputChecks() {
    const char* kGoals = "S15.549 (a stopped output's tracks have no sound)";
    if (!soundClientAllowed(kGoals)) return;
    ProbeReset reset;
    using Content = AudioTrack::Content;
    std::atomic<int> notices{0};
    auto output = std::make_unique<AudioOutput>([](const std::string&) {}, AudioOutput::ContentScope{},
                                                [&notices] { notices.fetch_add(1); });
    std::unique_ptr<AudioTrack> first = output->track(asset("wallpaper_tone.mp4"), "stopped-a");
    first->play(anchorAt(0, probe::nowNs()));
    waitFor(5000ms, [&] { return first->content() != Content::Unknown && notices.load() >= 1; });
    const bool opened = first->content() == Content::Sound;
    const int openNotices = notices.load();
    probe::g_failEventPoll.store(true);
    std::unique_ptr<AudioTrack> second = output->track(asset("wallpaper_tone.mp4"), "stopped-b");
    waitFor(5000ms, [&] {
        return first->content() == Content::Failed && second->content() == Content::Failed && notices.load() > openNotices;
    });
    std::unique_ptr<AudioTrack> third = output->track(asset("wallpaper_tone.mp4"), "stopped-c");
    const int stopNotices = notices.load();
    std::printf("audio_playback_test: stopped output rule: opened %s with %d notices; after the stop: first %d, second "
                "%d, third %d, %d notices\n",
                opened ? "yes" : "no", openNotices, static_cast<int>(first->content()), static_cast<int>(second->content()),
                static_cast<int>(third->content()), stopNotices);
    check(opened && openNotices == 1, "stopped output rule: the first track's open was noticed once",
          fmt("%.0f notices", static_cast<double>(openNotices)));
    check(first->content() == Content::Failed, "stopped output rule: a track playing when the output stopped reads Failed");
    check(second->content() == Content::Failed, "stopped output rule: a track made as it stopped reads Failed");
    check(third->content() == Content::Failed, "stopped output rule: a track made after it stopped reads Failed");
    check(stopNotices > openNotices, "stopped output rule: the stop was noticed",
          fmt("%.0f notices before, %.0f after", static_cast<double>(openNotices), static_cast<double>(stopNotices)));
    first.reset();
    second.reset();
    third.reset();
    output.reset();
}

// GOAL: S15.550 -- a file whose container gives no average picture rate is not judged against the
//       120-a-second cap by ffmpeg's guessed base rate: with the average hidden and the guess 1000 a
//       second, the tone file is still heard. False if the cap falls back to the guess.
// WHY-THIS-SHAPE: no ffmpeg build writes such a file on request, so avformat_find_stream_info's answer
//       is changed for video streams, the variable-rate case the guess misreads.
// COVERAGE-ASSERT: the rate was hidden on at least one stream.
// CONTROL: control_run.sh 2026-10-11, reverted: averageFrameInterval falling back to `r_frame_rate`,
//       renderer/video/video_decoder.cpp -> "FAIL: S15.550 ...: a heard video's sound reaches PipeWire within
//       5 s of its third picture".
void runUnknownRateChecks() {
    const char* kGoals = "S15.550 (a file with no average rate plays its sound)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    ProbeReset reset;
    const int hiddenBefore = probe::g_averageRatesHidden.load();
    probe::g_unknownAverageRate.store(true);
    Daemon d(smallConfig());
    const bool heard = startHeard(d, asset("wallpaper_tone.mp4"), kGoals);
    const int hidden = probe::g_averageRatesHidden.load() - hiddenBefore;
    check(hidden >= 1, "unknown rate rule: the average rate was hidden", fmt("%.0f streams", hidden));
    if (heard) std::printf("audio_playback_test: unknown rate rule: heard with the average rate hidden\n");
}

// ---------------------------------------------------------------------------------------------- //
// AudioDecoder alone
// ---------------------------------------------------------------------------------------------- //

// GOAL: S15.507 -- AudioDecoder::seek(t) then read() gives the file's sound from exactly t: the first
//       frame of a beep at t is within a few frames of the start, at the file's start and mid-file, and
//       a seek between beeps puts the next beep exactly as far in as it lies. False for a seek with no
//       preroll (AAC's first frame lost at the start) or one that keeps the frames before t.
// WHY-THIS-SHAPE: AAC in MP4, the container and codec the coder measured to within a frame; the
//       allowance is +3 frames above because a 1 kHz beep crosses the onset level a frame in.
// CONTROL: control_run.sh 2026-10-11, reverted, in renderer/audio/audio_decoder.cpp: kSeekPreroll 0 ->
//       "FAIL: decoder rule: a seek to the start puts the beep where it lies" (frame 1024); the
//       `if (gap < 0) m_dropFrames = ...` deleted -> "a seek to a beep mid-file" (4994) and "a seek between
//       beeps" (17730, wanted 12000); openCodec's no-stream `return false` deleted -> "a file with no sound threw".
//       The `first >= 0` requirement: control_run.sh 2026-10-11, reverted, read() made to return silence of the
//       same length -> "FAIL: decoder rule: a seek to the start puts the beep where it lies" (frame -1, wanted
//       0), and the mid-file and between-beeps lines.
void runDecoderChecks() {
    const char* kGoals = "S15.507 (AudioDecoder seeks)";
    std::unique_ptr<AudioDecoder> decoder;
    try {
        decoder = AudioDecoder::open(asset("wallpaper_beeps.mp4"));
    } catch (const std::exception& e) {
        check(false, std::string("decoder rule: the beeps fixture opens: ") + e.what());
        return;
    }
    if (!decoder) {
        check(false, "decoder rule: the beeps fixture's sound is found");
        return;
    }
    struct Case {
        const char* what;
        double seekSeconds;
        int64_t expectFrame;
    };
    const Case cases[] = {
        {"a seek to the start", 0.0, 0},
        {"a seek to a beep mid-file", 1.0, 0},
        {"a seek between beeps", 1.25, 12000},
    };
    std::vector<float> buffer(size_t{kRate} * 2);
    for (const Case& c : cases) {
        try {
            decoder->seek(std::chrono::nanoseconds(static_cast<int64_t>(c.seekSeconds * 1e9)));
            const size_t got = decoder->read(buffer.data(), kRate / 2 + 2400);
            int64_t first = -1;
            for (size_t i = 0; i < got; ++i) {
                if (std::fabs(buffer[2 * i]) > 0.05f) {
                    first = static_cast<int64_t>(i);
                    break;
                }
            }
            std::printf("audio_playback_test: decoder rule: %s (%.2f s): %zu frames read, first beep frame %lld, wanted "
                        "%lld\n",
                        c.what, c.seekSeconds, got, static_cast<long long>(first), static_cast<long long>(c.expectFrame));
            check(got > 0, std::string("decoder rule: ") + c.what + " reads sound");
            // -1 is "no beep found", and lies inside the window when the beep is wanted at frame 0.
            check(first >= 0 && first >= c.expectFrame - 2 && first <= c.expectFrame + 3,
                  std::string("decoder rule: ") + c.what + " puts the beep where it lies",
                  fmt("frame %.0f, wanted %.0f", static_cast<double>(first), static_cast<double>(c.expectFrame)));
        } catch (const std::exception& e) {
            check(false, std::string("decoder rule: ") + c.what + " threw: " + e.what());
        }
    }
    try {
        check(AudioDecoder::open(asset("wallpaper_bands.mp4")) == nullptr,
              "decoder rule: a file with no sound opens as no decoder");
    } catch (const std::exception& e) {
        check(false, std::string("decoder rule: a file with no sound threw: ") + e.what());
    }
    (void)kGoals;
}

// GOAL: S15.525 -- a read of the file that fails part way is a failure of the sound, not its end:
//       AudioDecoder::read throws WallpaperContentError naming the read. False if the decoder takes a
//       failed read for the end of the file and plays silence, as a demuxer's own error at the end is.
// WHY-THIS-SHAPE: the decoder's own descriptor is replaced, under it, by one on a directory, so its next
//       read fails (EISDIR) where a file read would fail with EIO -- a real failing read, with nothing
//       interposed. Twenty seconds asked of a twelve-second file, so a decoder that took the failure for
//       the end returns short and silent rather than throwing.
// COVERAGE-ASSERT: exactly one descriptor on the file (the decoder's) before it is replaced.
// CONTROL: control_run.sh 2026-10-11, reverted: decodeMore's test made `err != AVERROR_EOF &&
//       !avio_feof(...)` alone, renderer/audio/audio_decoder.cpp -> "FAIL: read error rule: a failed read
//       throws a content error naming the read" (1.86 s read, then no throw; re-run with the descriptors
//       listed from /proc/self/fd by descriptorList, not a fixed range). The one-descriptor guard has no
//       control: it reports not verified, and no harness input this file holds opens a second descriptor.
void runDecoderReadErrorChecks() {
    const char* kGoals = "S15.525 (a failed read is not the end)";
    const std::string file = asset("wallpaper_beeps_long.mp4");
    try {
        std::unique_ptr<AudioDecoder> decoder = AudioDecoder::open(file);
        if (!decoder) {
            check(false, "read error rule: the long fixture's sound is found");
            return;
        }
        std::vector<float> buffer(size_t{kRate} * 2);
        decoder->seek(std::chrono::nanoseconds(0));
        const size_t before = decoder->read(buffer.data(), 4800);
        const std::optional<std::vector<int>> fds = descriptorList(file);
        if (before != 4800 || !fds || fds->size() != 1) {
            notVerified(kGoals, fmt("the decoder read %.0f frames and holds %.0f descriptors on the file (-1: unlisted), "
                                    "so its own is not known",
                                    static_cast<double>(before), fds ? static_cast<double>(fds->size()) : -1.0));
            return;
        }
        const int directory = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory < 0 || ::dup3(directory, fds->front(), O_CLOEXEC) < 0) {
            if (directory >= 0) ::close(directory);
            notVerified(kGoals, "the decoder's descriptor could not be replaced");
            return;
        }
        ::close(directory);
        size_t total = 0;
        std::string thrown;
        try {
            for (int i = 0; i < 20; ++i) total += decoder->read(buffer.data(), kRate);
        } catch (const wallpaperx::WallpaperContentError& e) {
            thrown = e.what();
        }
        std::printf("audio_playback_test: read error rule: %.2f s read after the descriptor failed; thrown: %s\n",
                    total / double(kRate), thrown.empty() ? "nothing" : thrown.c_str());
        check(!thrown.empty() && thrown.find("cannot read the wallpaper's audio") != std::string::npos,
              "read error rule: a failed read throws a content error naming the read, not an end of the sound",
              fmt("%.2f s read, then ", total / double(kRate)) + (thrown.empty() ? "no throw" : thrown));
    } catch (const std::exception& e) {
        check(false, std::string("read error rule: unexpected ") + e.what());
    }
}

// GOAL: S15.526 -- a seek to the start then read() works in every container the decoder is likely to
//       meet: AVI (MP3), FLV, Matroska and MPEG-TS (AAC), WebM (Opus, Vorbis) and Ogg (Vorbis) each give
//       their tone, without a throw. False for a seek whose latest allowed point is the preroll
//       before the target, which AVI's and FLV's demuxers refuse at their start (nothing is indexed
//       before it).
// CONTROL: control_run.sh 2026-10-11, reverted: seek()'s latest point made `target`,
//       renderer/audio/audio_decoder.cpp -> "FAIL: seek to start rule: a seek to the start of
//       wallpaper_seek_mp3.avi does not fail" and the same for wallpaper_seek_aac.flv ("Operation not
//       permitted"); every other container stayed green.
void runDecoderSeekStartChecks() {
    for (const char* name : {"wallpaper_seek_mp3.avi", "wallpaper_seek_aac.flv", "wallpaper_seek_aac.mkv",
                             "wallpaper_seek_aac.ts", "wallpaper_seek_opus.webm", "wallpaper_seek_vorbis.webm",
                             "wallpaper_seek_vorbis.ogv"}) {
        try {
            const std::unique_ptr<AudioDecoder> decoder = AudioDecoder::open(asset(name));
            if (!decoder) {
                check(false, std::string("seek to start rule: ") + name + "'s sound is found");
                continue;
            }
            decoder->seek(std::chrono::nanoseconds(0));
            // Two seconds: an MPEG-TS stream starts about 1.4 s in, and a seek before a stream's start
            // reads silence up to it.
            std::vector<float> buffer(size_t{kRate} * 2 * 2);
            const size_t got = decoder->read(buffer.data(), size_t{kRate} * 2);
            float peak = 0.0f;
            for (size_t i = 0; i < got * 2; ++i) peak = std::max(peak, std::fabs(buffer[i]));
            std::printf("audio_playback_test: seek to start rule: %s: %zu frames, peak %.3f\n", name, got, peak);
            check(got >= 4800 && peak > 0.2f, std::string("seek to start rule: ") + name + " plays its tone after a seek to 0",
                  fmt("%.0f frames, peak %.3f", static_cast<double>(got), peak));
        } catch (const std::exception& e) {
            check(false, std::string("seek to start rule: a seek to the start of ") + name + " does not fail", e.what());
        }
    }
}

// GOAL: S15.534 -- the sound decoder refuses a playlist (an ffconcat script) in the picture decoder's
//       words, "a list of other files". False if the two decoders' refusals differ, so one file is
//       reported two ways.
// CONTROL: control_run.sh 2026-10-11, reverted: AudioDecoder::openContainer given its own open sequence
//       back, renderer/audio/audio_decoder.cpp -> "FAIL: playlist rule: the sound decoder refuses a playlist as
//       the picture decoder does" ("cannot open the wallpaper (Operation not permitted)").
void runDecoderPlaylistChecks() {
    const char* tmp = std::getenv("TMPDIR");
    const std::string list = std::string(tmp != nullptr && *tmp != '\0' ? tmp : "/tmp") + "/audio-playlist-" +
                             std::to_string(::getpid()) + ".ffconcat";
    std::ofstream(list) << "ffconcat version 1.0\nfile '" << asset("wallpaper_beeps.mp4") << "'\n";
    std::string said;
    bool opened = false;
    try {
        opened = AudioDecoder::open(list) != nullptr;
    } catch (const wallpaperx::WallpaperContentError& e) {
        said = e.what();
    } catch (const std::exception& e) {
        said = std::string("(not a content error) ") + e.what();
    }
    ::unlink(list.c_str());
    std::printf("audio_playback_test: playlist rule: %s\n", opened ? "opened" : said.c_str());
    check(!opened && said.find("a list of other files") != std::string::npos,
          "playlist rule: the sound decoder refuses a playlist as the picture decoder does", said);
}

// ---------------------------------------------------------------------------------------------- //
// Fatal signals on the sound thread (children of this binary)
// ---------------------------------------------------------------------------------------------- //

constexpr const char* kSoundChildFlag = "--sound-child";
constexpr int kSoundChildSetupFailed = 120;
constexpr int kSoundChildNoFault = 119;
constexpr int kSoundChildNoSound = 118;

// A child that starts as the daemon does -- restoreSaved(), recordStartingVideos() -- with the screen
// heard, and dies where `mode` puts a fault:
//   fault-read            swr_convert faults: a read of the file's sound, inside the content scope
//   fail-read             swr_convert fails, and the decoder's close after that failure faults
//   fault-connect         pw_stream_connect faults: making the stream, outside the scope
//   fault-routine-close   the decoder's close when a released session's track goes faults, inside it
[[noreturn]] void runSoundChild(int argc, char** argv) {
    wallpaperx::testing::suppressCoreDump();
    wallpaperx::installFatalSignalHandlers();
    if (argc < 5) _exit(kSoundChildSetupFailed);
    if (const std::string why = soundIsolationFault(); !why.empty()) {
        std::fprintf(stderr, "audio_playback_test child: no sound client is made here: %s\n", why.c_str());
        _exit(kSoundChildSetupFailed);
    }
    const std::string mode = argv[2];
    if (mode == "fault-read") probe::g_swrMode.store(probe::kSwrFault);
    if (mode == "fail-read") probe::g_swrMode.store(probe::kSwrFailThenFaultFree);
    if (mode == "fault-connect") probe::g_faultInConnect.store(true);
    WallpaperManager::Config config = smallConfig();
    config.stateDir = argv[3];
    config.configFile = argv[4];
    config.idleSessionRelease = 1s;
    try {
        auto manager = std::make_unique<WallpaperManager>(config);
        const std::vector<int> posted = manager->restoreSaved();
        manager->recordStartingVideos();
        RenderWorker worker(*manager, [](int, uint64_t) {});
        std::vector<int> redraw;
        manager->setMonitorOutput(0, "DP-1", redraw);
        manager->setOutputFocus(":1.5", "DP-1", ScreenFocus::Desktop, false);
        for (const int id : posted) worker.post(id);
        for (const int id : redraw) worker.post(id);
        if (mode == "fault-routine-close") {
            const auto deadline = Clock::now() + 10s;
            while (probe::g_swrCalls.load() == 0) {
                if (Clock::now() >= deadline) _exit(kSoundChildNoSound);
                std::this_thread::sleep_for(10ms);
            }
            manager->setOutputVisible("DP-1", false);
            worker.wake();
            // Past any play() the last playing round sent, and well before the release a second later.
            std::this_thread::sleep_for(150ms);
            probe::g_faultInSwrFree.store(true);
        }
        std::this_thread::sleep_for(15s);
        std::fprintf(stderr, "audio_playback_test child (%s): no fault within 15 s\n", mode.c_str());
        _exit(kSoundChildNoFault);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "audio_playback_test child (%s): %s\n", mode.c_str(), e.what());
    }
    _exit(kSoundChildSetupFailed);
}

struct CrashScene {
    explicit CrashScene(const char* tag) : scratch(tag) {}
    bool prepare(const std::string& video) {
        if (!scratch.ok()) return false;
        stateDir = scratch.path() / "state";
        configFile = scratch.path() / "config.json";
        path = video;
        nlohmann::json monitors = nlohmann::json::array();
        monitors.push_back({{"monitor_id", 0}, {"type", "video"}, {"path", video}});
        std::ofstream(configFile) << nlohmann::json{{"version", 1}, {"monitors", monitors}}.dump();
        return std::filesystem::exists(configFile);
    }
    // Runs one child; false, with a harness fault said, unless it died of SIGSEGV as staged.
    bool die(const std::string& mode) {
        const wallpaperx::testing::SelfExecResult r = wallpaperx::testing::runSelf(
            {kSoundChildFlag, mode, stateDir.string(), configFile.string()}, std::chrono::seconds(40));
        if (!r.fault.empty() || !r.diedOf(SIGSEGV)) {
            std::string how = r.fault.empty() ? r.how() : r.fault;
            if (r.exitedWith(kSoundChildNoSound)) how = "its sound was never decoded";
            if (r.exitedWith(kSoundChildNoFault)) how = "nothing faulted within 15 s";
            harnessFailure("a '" + mode + "' sound child did not die of the fault staged on its sound thread (" + how +
                           "), so its record grades nothing");
            return false;
        }
        std::printf("audio_playback_test: a '%s' sound child died of SIGSEGV; the record %s\n", mode.c_str(),
                    describeDied().c_str());
        return true;
    }
    std::string describeDied() const {
        const std::optional<wallpaperx::PlayingHistory> h = wallpaperx::readPlayingRecord(stateDir);
        if (!h) return "no readable record";
        if (!h->died) return "names no video";
        return "names monitor " + std::to_string(h->died->monitorId) + " '" + h->died->path + "' signal " +
               std::to_string(h->signal.value_or(-1));
    }
    bool namesTheVideo() const {
        const std::optional<wallpaperx::PlayingHistory> h = wallpaperx::readPlayingRecord(stateDir);
        return h && h->died && h->died->monitorId == 0 && h->died->path == path && h->signal && *h->signal == SIGSEGV;
    }
    bool namesNothing() const {
        const std::optional<wallpaperx::PlayingHistory> h = wallpaperx::readPlayingRecord(stateDir);
        return h && !h->died && !h->signal;
    }

    wallpaperx::testing::ScratchDir scratch;
    std::filesystem::path stateDir, configFile;
    std::string path;
};

// GOAL: S15.508 -- a fatal signal on the sound thread while it reads a heard video's sound writes that
//       video's monitor and path, and the signal, into the playing record, as one on the render thread
//       does. False if the sound thread's work runs under no guard, so a file whose sound crashes FFmpeg
//       kills the daemon at every start and is never refused.
// WHY-THIS-SHAPE: a real SIGSEGV inside swr_convert, which only the sound decoder calls, in a child
//       that starts as the daemon does; a guard planted by the test would grade the test.
// CONTROL: control_run.sh 2026-10-11, reverted: the PlayingBlame in m_audio's content scope
//       (backend/daemon/wallpaper_manager.cpp, the constructor) replaced by `(void)blame;` -> "FAIL: sound
//       fault rule: a fault reading a heard video's sound names that video and the signal" (names no video).
void runSoundFaultNamedChecks() {
    const char* kGoals = "S15.508 (a sound-thread death names its video)";
    SoundServer sound(kGoals, false);
    if (!sound.ready) return;
    CrashScene scene("soundfault");
    if (!scene.prepare(asset("wallpaper_beeps.mp4"))) {
        notVerified(kGoals, "no scratch state directory could be made");
        return;
    }
    if (!scene.die("fault-read")) return;
    check(scene.namesTheVideo(), "sound fault rule: a fault reading a heard video's sound names that video and the signal",
          "the record " + scene.describeDied());
}

// GOAL: S15.509 -- two such deaths and the third start refuses the video, as for picture deaths: the
//       existing count fed by the sound thread. False if a sound death is not counted.
// CONTROL: the same break as S15.508's -> "FAIL: sound fault rule: a video whose sound killed the daemon
//       twice is refused at the next start" (status ''). The "counts both deaths" line is gated on the
//       refusal, which itself needs two, so it has no control of its own.
void runSoundFaultQuarantineChecks() {
    const char* kGoals = "S15.509 (two sound deaths refuse the video)";
    SoundServer sound(kGoals, false);
    if (!sound.ready) return;
    CrashScene scene("soundquarantine");
    if (!scene.prepare(asset("wallpaper_beeps.mp4"))) {
        notVerified(kGoals, "no scratch state directory could be made");
        return;
    }
    if (!scene.die("fault-read") || !scene.die("fault-read")) return;
    try {
        WallpaperManager::Config config = smallConfig();
        config.stateDir = scene.stateDir;
        config.configFile = scene.configFile;
        WallpaperManager manager(config);
        manager.restoreSaved();
        const WallpaperManager::Snapshot snap = manager.snapshot();
        const auto it = snap.monitors.find(0);
        const bool refused = it != snap.monitors.end() && !it->second.lastError.empty() && !it->second.renderOutstanding;
        check(refused, "sound fault rule: a video whose sound killed the daemon twice is refused at the next start",
              it == snap.monitors.end() ? "monitor 0 was not restored" : "status: '" + it->second.lastError + "'");
        if (refused) {
            check(it->second.lastError.find("2nd time") != std::string::npos,
                  "sound fault rule: the refusal counts both deaths", it->second.lastError);
        }
    } catch (const std::exception& e) {
        harnessFailure(std::string("the start after two sound deaths threw: ") + e.what());
    }
}

// GOAL: S15.510, first clause -- a fatal signal on the sound thread in making the PipeWire stream, which is the
//       server's work and not the file's, names nothing. False if server calls run under the video's
//       guard, so a sound server's crash is counted against a wallpaper.
// CONTROL: control_run.sh 2026-10-11, reverted: begin()'s ensureStream run inside onContent,
//       renderer/audio/audio_output.cpp -> "FAIL: sound fault rule: a fault in making the stream names nothing"
//       (names monitor 0 ... signal 11).
void runSoundFaultOutsideChecks() {
    const char* kGoals = "S15.510 (a sound-thread death making the stream names nothing)";
    SoundServer sound(kGoals, false);
    if (!sound.ready) return;
    CrashScene scene("soundoutside");
    if (!scene.prepare(asset("wallpaper_beeps.mp4"))) {
        notVerified(kGoals, "no scratch state directory could be made");
        return;
    }
    if (!scene.die("fault-connect")) return;
    check(scene.namesNothing(), "sound fault rule: a fault in making the stream names nothing", "the record " + scene.describeDied());
}

// GOAL: S15.510, second clause -- a fatal signal while a released session's track closes its decoder
//       names that video: closing a decoder runs the file's own code (its demuxer's and codec's
//       teardown), so it is the file's content too. False if the routine close runs outside the video's
//       guard, so a file whose teardown crashes kills the daemon at every release and is never refused.
// WHY-THIS-SHAPE: the fault is staged in swr_free, the decoder's first teardown call, armed after the
//       screen is hidden and before the one-second release closes the track.
// CONTROL: control_run.sh 2026-10-11, reverted: destroyStream's closeDecoder(t) made a bare
//       `t.decoder.reset()`, renderer/audio/audio_output.cpp -> "FAIL: sound fault rule: a fault closing a
//       released track's decoder names the video" (names no video).
void runSoundFaultRoutineCloseChecks() {
    const char* kGoals = "S15.510 (a death closing a released track's decoder is blamed)";
    SoundServer sound(kGoals, false);
    if (!sound.ready) return;
    CrashScene scene("soundroutine");
    if (!scene.prepare(asset("wallpaper_beeps.mp4"))) {
        notVerified(kGoals, "no scratch state directory could be made");
        return;
    }
    if (!scene.die("fault-routine-close")) return;
    check(scene.namesTheVideo(), "sound fault rule: a fault closing a released track's decoder names the video",
          "the record " + scene.describeDied());
}

// GOAL: S15.511 -- a death while the decoder is closed after a failed read is blamed on the video: the
//       close after a failure is the file's content too. False if that close runs outside the guard.
// CONTROL: control_run.sh 2026-10-11, reverted: closeAfterFailure's closeDecoder(t) made a bare
//       `t.decoder.reset()` in renderer/audio/audio_output.cpp -> "FAIL: sound fault rule: a fault closing the
//       decoder after a failed read names the video" (names no video).
void runSoundFaultClosingChecks() {
    const char* kGoals = "S15.511 (a death closing after a failed read is blamed)";
    SoundServer sound(kGoals, false);
    if (!sound.ready) return;
    CrashScene scene("soundclose");
    if (!scene.prepare(asset("wallpaper_beeps.mp4"))) {
        notVerified(kGoals, "no scratch state directory could be made");
        return;
    }
    if (!scene.die("fail-read")) return;
    check(scene.namesTheVideo(), "sound fault rule: a fault closing the decoder after a failed read names the video",
          "the record " + scene.describeDied());
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], kSoundChildFlag) == 0) runSoundChild(argc, argv);
    if (argc > 1 && std::strcmp(argv[1], kScopeChildFlag) == 0) runScopeChild();
    wallpaperx::testing::g_testName = "audio_playback_test";

    const char* fixtures[] = {"wallpaper_beeps.mp4", "wallpaper_tone.mp4", "wallpaper_beeps_short_audio.mp4",
                              "wallpaper_beeps_long.mp4", "wallpaper_tone_slow.mp4", "wallpaper_beeps_slow.mp4",
                              "wallpaper_bands.mp4", "wallpaper_one_picture.mp4", "wallpaper_tone_rate240.mp4",
                              "wallpaper_tone_rate120.mp4", "wallpaper_one_picture_tone.mp4", "wallpaper_seek_mp3.avi",
                              "wallpaper_seek_aac.flv", "wallpaper_seek_aac.mkv", "wallpaper_seek_aac.ts",
                              "wallpaper_seek_opus.webm", "wallpaper_seek_vorbis.webm", "wallpaper_seek_vorbis.ogv",
                              "wallpaper_reads_bframes.mp4", "sound_16384_frames.wav"};
    for (const char* f : fixtures) {
        if (!std::filesystem::exists(asset(f))) {
            notVerified("every goal in this file",
                        "the fixture " + asset(f) + " was not generated, which means this build found no ffmpeg command line");
            return wallpaperx::testing::reportNotVerifiedOrPassed();
        }
    }

    // The audio_isolation entry: a caller whose sound-server variables name decoys. Only that phase,
    // which checks the runtime directory alone; every other one also refuses on the variables.
    if (const char* decoy = std::getenv("WALLPAPERX_TEST_SOUND_DECOY"); decoy != nullptr && *decoy != '\0') {
        try {
            runDecoySafetyChecks();
            const bool madeDecoys = runSoundIsolationChecks(decoy);
            // GOAL: S15.542, last clause -- the decoys this phase made are gone once it ends.
            // CONTROL: see S15.542 above runDecoySafetyChecks (closeDecoy's unlink deleted).
            struct stat st {};
            for (const char* name : {"/pipewire-0", "/remote"}) {
                if (!madeDecoys) break;
                const std::string path = std::string(decoy) + name;
                check(::lstat(path.c_str(), &st) != 0, "decoy rule: the isolation phase removes the decoy sockets it made",
                      path + " is still there");
            }
        } catch (const std::exception& e) {
            harnessFailure(std::string("the isolation phase ended in an exception: ") + e.what());
        }
        if (wallpaperx::testing::reportCheckFailures()) return 1;
        if (wallpaperx::testing::reportHarnessFaults("in this test file, not in the daemon")) return 1;
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }

    const std::pair<const char*, void (*)()> phases[] = {
        {"decoder", &runDecoderChecks},
        {"decoderReadError", &runDecoderReadErrorChecks},
        {"decoderSeekStart", &runDecoderSeekStartChecks},
        {"decoderPlaylist", &runDecoderPlaylistChecks},
        {"engineLoop", &runEngineLoopChecks},
        {"engineShortAudio", &runEngineShortAudioChecks},
        {"engineLateStart", &runEngineLateStartChecks},
        {"engineRamp", &runEngineRampChecks},
        {"engineUnderrun", &runEngineUnderrunChecks},
        {"engineLongUnderrun", &runEngineLongUnderrunChecks},
        {"engineQueuedSegments", &runQueuedSegmentChecks},
        {"engineFuturePlay", &runFuturePlayChecks},
        {"engineFadeIn", &runFadeInChecks},
        {"engineStreamError", &runStreamErrorChecks},
        {"engineStreamRetry", &runStreamRetryChecks},
        {"engineThrowingScope", &runThrowingScopeChecks},
        {"enginePassFailure", &runPassFailureChecks},
        {"enginePassFailureLogged", &runPassFailureLoggedChecks},
        {"engineNoPolling", &runNoPollingChecks},
        {"engineOneContext", &runOneContextChecks},
        {"streamNames", &runStreamNameChecks},
        {"heard", &runHeardChecks},
        {"stop", &runStopChecks},
        {"speed", &runSpeedChecks},
        {"volume", &runVolumeChecks},
        {"sync", &runSyncChecks},
        {"loop", &runLoopChecks},
        {"release", &runReleaseChecks},
        {"noSound", &runNoSoundChecks},
        {"badSound", &runBadSoundChecks},
        {"caughtUp", &runCaughtUpChecks},
        {"busWake", &runBusWakeChecks},
        {"lastPicture", &runLastPictureChecks},
        {"reconnect",[] { runReconnect(false); }},
        {"reconnectPlaysOn", [] { runReconnect(true); }},
        {"rateCap", &runRateCapChecks},
        {"resume", &runResumeChecks},
        {"parked", &runParkedChecks},
        {"latePicture", &runLatePictureChecks},
        {"renderOwed", &runRenderOwedChecks},
        {"parkedSoundFails", &runParkedSoundFailsChecks},
        {"parkedReopen", &runParkedReopenChecks},
        {"parkedSilentStays", &runParkedSilentStaysChecks},
        {"parkedUnwatched", &runParkedUnwatchedChecks},
        {"renderOwedUnwatched", &runRenderOwedUnwatchedChecks},
        {"parkedFailedReopen", &runParkedFailedReopenChecks},
        {"parkedVolumeZero", &runParkedVolumeZeroChecks},
        {"pictureReadRecovered", &runPictureReadRecoveredChecks},
        {"engineDeferredPlay", &runDeferredPlayChecks},
        {"engineLoopRebegin", &runLoopRebeginChecks},
        {"engineStoppedOutput", &runStoppedOutputChecks},
        {"unknownRate", &runUnknownRateChecks},
        {"soundFaultNamed", &runSoundFaultNamedChecks},
        {"soundFaultQuarantine", &runSoundFaultQuarantineChecks},
        {"soundFaultOutside", &runSoundFaultOutsideChecks},
        {"soundFaultRoutineClose", &runSoundFaultRoutineCloseChecks},
        {"soundFaultClosing", &runSoundFaultClosingChecks},
    };
    for (const auto& [name, run] : phases) {
        if (!phaseSelected(name)) continue;
        try {
            run();
        } catch (const std::exception& e) {
            harnessFailure(std::string("phase ") + name + " ended in an exception: " + e.what());
        }
    }
    {
        std::lock_guard<std::mutex> lock(probe::g_recordMutex);
        if (probe::g_overflow) harnessFailure("the buffer record overflowed, so some phase graded a truncated record");
    }

    if (wallpaperx::testing::reportCheckFailures()) return 1;
    if (wallpaperx::testing::reportHarnessFaults("in this test file, not in the daemon")) return 1;
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
