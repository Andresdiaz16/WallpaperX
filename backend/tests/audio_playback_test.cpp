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
// (timingOfNextBuffer in renderer/audio/audio_output.cpp), so every timing check here assumes
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
#include <vulkan/vulkan.h>

#include <sdbus-c++/sdbus-c++.h>

extern "C" {
#include <libavcodec/avcodec.h>
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
// The next av_read_frame of a picture decoder reading a file whose name holds g_stallFile sleeps
// g_stallFileMs, once, and says when it slept. With g_slowFile set, the slow reads above are that
// file's alone.
std::mutex g_stallFileMutex;
std::string g_stallFile;
std::string g_slowFile;
std::atomic<int> g_stallFileMs{0};
std::atomic<int64_t> g_fileStallStartedAt{0};
std::atomic<int64_t> g_fileStallEndedAt{0};

// Every picture a picture decoder handed on, in order: when, from which file, and its presentation
// time. A picture is handed on where VideoDecoder moves an admitted frame out of its decoding frame
// (av_frame_move_ref), the frame avcodec_receive_frame last filled on that thread for a video codec;
// the file is the one whose packets that codec was sent.
struct PictureRecord {
    int64_t at = 0;
    std::string file;
    std::optional<int64_t> presentationNs;
};
std::mutex g_pictureMutex;
std::vector<PictureRecord> g_pictures;
std::map<const AVPacket*, std::string> g_packetFile;
std::map<const AVCodecContext*, std::string> g_contextFile;
thread_local AVCodecContext* t_videoContext = nullptr;
thread_local const AVFrame* t_videoFrame = nullptr;

// Non-zero: every stream of the engine's is given this rate correction (pw_stream_set_rate), so the
// server takes its frames at 48000 / rate a second, as a sound card off its nominal rate would.
std::atomic<double> g_streamRate{0.0};

// Set: pw_stream_get_time_n reports `now` as 0, as for a stream the server has not timed yet, so no
// stream has a clock; and how many of production's reports were hidden. record() reads the time too,
// under t_inRecord, which is not counted.
std::atomic<bool> g_hideStreamTime{false};
std::atomic<int> g_streamTimesHidden{0};
// Set: the next sound seek to under 50 ms of the file (a hand-over to a new pass) sets g_hideStreamTime.
std::atomic<bool> g_hideAtHandOver{false};
thread_local bool t_inRecord = false;

// Every picture a picture decoder returns for a file whose name holds g_costFile costs g_costMs more,
// spent where avcodec_receive_frame returns it, so a picture skipped undecoded costs nothing. While
// g_costCycle holds entries, each such picture takes the next one instead, round and round.
std::mutex g_costMutex;
std::string g_costFile;
std::vector<int> g_costCycle;
size_t g_costCycleAt = 0;
std::atomic<int> g_costMs{0};

// The picture of a file whose name holds g_holdFile, at presentation time g_holdAtNs, is held g_holdMs
// once as the decoder returns it, and when that hold ran is recorded; unlike a stalled read, which a
// decoder with frame threads may have made pictures earlier, this lands on that picture's hand-off.
std::mutex g_holdMutex;
std::string g_holdFile;
std::atomic<int64_t> g_holdAtNs{-1};
std::atomic<int> g_holdMs{0};
std::atomic<int64_t> g_holdStartedAt{0};
std::atomic<int64_t> g_holdEndedAt{0};
// Set: the hold fires every time that picture is returned, not once (each pass's first picture, say).
std::atomic<bool> g_holdRepeats{false};

// Draws of three vertices and one instance: FrameConverter's conversion pass, and nothing else this
// binary runs.
std::atomic<int> g_conversions{0};
// When each vkCmdBlitImage was recorded: VideoRenderer showing a converted picture, the one blit here.
std::mutex g_blitMutex;
std::vector<int64_t> g_blits;
// The next vkCmdBlitImage sleeps this long first, once: a draw held up outside any decode.
std::atomic<int> g_blitHoldMs{0};

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
    probe::t_inRecord = true;
    probe::record(stream, buffer);
    probe::t_inRecord = false;
    // From the stream's own process callback, where pw_stream_set_rate may be called. Every cycle: it only
    // stores the value, and a new stream gets it on its first buffer.
    if (const double rate = probe::g_streamRate.load(); rate != 0.0) pw_stream_set_rate(stream, rate);
    return real(stream, buffer);
}

extern "C" int pw_stream_get_time_n(pw_stream* stream, pw_time* time, size_t size) {
    using Fn = int (*)(pw_stream*, pw_time*, size_t);
    static const Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "pw_stream_get_time_n"));
    const int r = real(stream, time, size);
    if (r == 0 && time != nullptr && probe::g_hideStreamTime.load()) {
        time->now = 0;
        if (!probe::t_inRecord) probe::g_streamTimesHidden.fetch_add(1);
    }
    return r;
}

extern "C" void vkCmdDraw(VkCommandBuffer cmd, uint32_t vertices, uint32_t instances, uint32_t firstVertex,
                          uint32_t firstInstance) {
    using Fn = void (*)(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t);
    static const Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "vkCmdDraw"));
    if (vertices == 3 && instances == 1) probe::g_conversions.fetch_add(1);
    real(cmd, vertices, instances, firstVertex, firstInstance);
}

extern "C" void vkCmdBlitImage(VkCommandBuffer cmd, VkImage source, VkImageLayout sourceLayout, VkImage target,
                               VkImageLayout targetLayout, uint32_t regions, const VkImageBlit* region, VkFilter filter) {
    using Fn = void (*)(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout, uint32_t, const VkImageBlit*, VkFilter);
    static const Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "vkCmdBlitImage"));
    {
        std::lock_guard<std::mutex> lock(probe::g_blitMutex);
        probe::g_blits.push_back(probe::nowNs());
    }
    if (const int ms = probe::g_blitHoldMs.exchange(0); ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    real(cmd, source, sourceLayout, target, targetLayout, regions, region, filter);
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
        if (target < 50'000'000 && probe::g_hideAtHandOver.exchange(false)) probe::g_hideStreamTime.store(true);
        std::lock_guard<std::mutex> lock(probe::g_seekMutex);
        probe::g_soundSeeks.push_back({probe::nowNs(), target});
    }
    return real(s, index, min, ts, max, flags);
}

// Only a picture decoder reads a container whose video stream it has not discarded.
extern "C" int av_read_frame(AVFormatContext* s, AVPacket* pkt) {
    using Fn = int (*)(AVFormatContext*, AVPacket*);
    static const Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "av_read_frame"));
    bool pictures = false;
    for (unsigned i = 0; s != nullptr && i < s->nb_streams; ++i) {
        const AVStream& st = *s->streams[i];
        if (st.codecpar->codec_type == AVMEDIA_TYPE_VIDEO && st.discard != AVDISCARD_ALL) pictures = true;
    }
    if (pictures) {
        if (const int ms = probe::g_stallPictureMs.exchange(0); ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        }
        bool slowed = probe::g_slowPictureReads.load() > 0;
        if (slowed) {
            std::lock_guard<std::mutex> lock(probe::g_stallFileMutex);
            slowed = probe::g_slowFile.empty() || (s->url != nullptr && std::strstr(s->url, probe::g_slowFile.c_str()) != nullptr);
        }
        if (slowed) {
            if (probe::g_slowPictureReads.fetch_sub(1) > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(probe::g_slowPictureMs.load()));
            } else {
                probe::g_slowPictureReads.store(0);
            }
        }
        if (probe::g_stallFileMs.load() > 0 && s->url != nullptr) {
            int ms = 0;
            {
                std::lock_guard<std::mutex> lock(probe::g_stallFileMutex);
                if (!probe::g_stallFile.empty() && std::strstr(s->url, probe::g_stallFile.c_str()) != nullptr) {
                    ms = probe::g_stallFileMs.exchange(0);
                }
            }
            if (ms > 0) {
                probe::g_fileStallStartedAt.store(probe::nowNs());
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                probe::g_fileStallEndedAt.store(probe::nowNs());
            }
        }
    }
    const int r = real(s, pkt);
    if (pictures && r >= 0 && pkt != nullptr && s->url != nullptr) {
        std::lock_guard<std::mutex> lock(probe::g_pictureMutex);
        probe::g_packetFile[pkt] = s->url;
    }
    return r;
}

extern "C" int avcodec_send_packet(AVCodecContext* avctx, const AVPacket* avpkt) {
    using Fn = int (*)(AVCodecContext*, const AVPacket*);
    static const Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "avcodec_send_packet"));
    if (avctx != nullptr && avpkt != nullptr && avctx->codec_type == AVMEDIA_TYPE_VIDEO) {
        std::lock_guard<std::mutex> lock(probe::g_pictureMutex);
        if (const auto it = probe::g_packetFile.find(avpkt); it != probe::g_packetFile.end()) {
            probe::g_contextFile[avctx] = it->second;
        }
    }
    return real(avctx, avpkt);
}

extern "C" int avcodec_receive_frame(AVCodecContext* avctx, AVFrame* frame) {
    using Fn = int (*)(AVCodecContext*, AVFrame*);
    static const Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "avcodec_receive_frame"));
    // Cleared first: libavcodec's own moves inside the call must not count as a picture handed on.
    probe::t_videoFrame = nullptr;
    const int r = real(avctx, frame);
    if (r == 0 && avctx != nullptr && avctx->codec_type == AVMEDIA_TYPE_VIDEO) {
        probe::t_videoContext = avctx;
        probe::t_videoFrame = frame;
        if (probe::g_costMs.load() > 0) {
            bool costed = false;
            {
                std::lock_guard<std::mutex> lock(probe::g_pictureMutex);
                const auto it = probe::g_contextFile.find(avctx);
                std::lock_guard<std::mutex> costLock(probe::g_costMutex);
                costed = it != probe::g_contextFile.end() && !probe::g_costFile.empty() &&
                         it->second.find(probe::g_costFile) != std::string::npos;
            }
            if (costed) {
                int ms = probe::g_costMs.load();
                {
                    std::lock_guard<std::mutex> lock(probe::g_costMutex);
                    if (!probe::g_costCycle.empty()) {
                        ms = probe::g_costCycle[probe::g_costCycleAt++ % probe::g_costCycle.size()];
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            }
        }
        if (probe::g_holdMs.load() > 0 && frame != nullptr && frame->best_effort_timestamp != AV_NOPTS_VALUE &&
            avctx->pkt_timebase.num > 0) {
            const int64_t at = av_rescale_q(frame->best_effort_timestamp, avctx->pkt_timebase, AVRational{1, 1'000'000'000});
            bool held = std::llabs(at - probe::g_holdAtNs.load()) < 2'000'000;
            if (held) {
                std::lock_guard<std::mutex> lock(probe::g_pictureMutex);
                const auto it = probe::g_contextFile.find(avctx);
                std::lock_guard<std::mutex> holdLock(probe::g_holdMutex);
                held = it != probe::g_contextFile.end() && !probe::g_holdFile.empty() &&
                       it->second.find(probe::g_holdFile) != std::string::npos;
            }
            if (const int ms = !held ? 0 : probe::g_holdRepeats.load() ? probe::g_holdMs.load() : probe::g_holdMs.exchange(0);
                ms > 0) {
                probe::g_holdStartedAt.store(probe::nowNs());
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                probe::g_holdEndedAt.store(probe::nowNs());
            }
        }
    }
    return r;
}

extern "C" void av_frame_move_ref(AVFrame* dst, AVFrame* src) {
    using Fn = void (*)(AVFrame*, AVFrame*);
    static const Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "av_frame_move_ref"));
    if (src != nullptr && src == probe::t_videoFrame) {
        probe::t_videoFrame = nullptr;
        const AVCodecContext& ctx = *probe::t_videoContext;
        probe::PictureRecord r;
        r.at = probe::nowNs();
        if (src->best_effort_timestamp != AV_NOPTS_VALUE && ctx.pkt_timebase.num > 0) {
            r.presentationNs = av_rescale_q(src->best_effort_timestamp, ctx.pkt_timebase, AVRational{1, 1'000'000'000});
        }
        std::lock_guard<std::mutex> lock(probe::g_pictureMutex);
        if (const auto it = probe::g_contextFile.find(&ctx); it != probe::g_contextFile.end()) r.file = it->second;
        probe::g_pictures.push_back(std::move(r));
    }
    real(dst, src);
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
// One sound-server cycle (1024 frames) plus kSoundDrift (20 ms, backend/daemon/wallpaper_manager.cpp, copied
// by hand): a server later than this between two buffers can leave a heard sound behind enough to be moved.
constexpr int64_t kServerLateBound = 1024 * 1'000'000'000LL / kRate + 20 * kMs;

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
    {
        std::lock_guard<std::mutex> lock(probe::g_pictureMutex);
        probe::g_pictures.clear();
    }
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

// Sound seeks made in [from, to).
std::vector<probe::SoundSeek> soundSeeksSince(int64_t from, int64_t to = INT64_MAX) {
    std::lock_guard<std::mutex> lock(probe::g_seekMutex);
    std::vector<probe::SoundSeek> out;
    for (const probe::SoundSeek& s : probe::g_soundSeeks) {
        if (s.at >= from && s.at < to) out.push_back(s);
    }
    return out;
}

// The longest the sound server went between two buffers of the engine's, over every gap that overlaps
// [from, to): the gaps across both edges count, and so does the time from the last buffer to `to` when
// none came after it, so a stall that straddles an edge is seen whole. A cycle is 1024 frames, 21.3 ms.
int64_t longestBufferGap(const Capture& cap, int64_t from, int64_t to) {
    std::optional<int64_t> previous;
    int64_t longest = 0;
    for (const probe::BufferRecord& b : cap.buffers) {
        if (b.queuedAt < from) {
            previous = b.queuedAt;
            continue;
        }
        if (previous) longest = std::max(longest, b.queuedAt - *previous);
        previous = b.queuedAt;
        if (b.queuedAt >= to) return longest;
    }
    if (previous) longest = std::max(longest, to - *previous);
    return longest;
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

    // The running server's process, or -1.
    pid_t pid() const { return m_pid; }

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
    int monitor = 0;
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
        : manager(std::move(config)), worker(manager, [this](int monitor, uint64_t generation) {
              const Commit c{generation, probe::nowNs(), monitor};
              {
                  std::lock_guard<std::mutex> lock(commitMutex);
                  commits.push_back(c);
              }
              if (onCommit) onCommit(c);
          }) {}
    ~Daemon() { worker.stop(); }

    // Monitor 0's commits; every monitor's with `all`.
    std::vector<Commit> takeCommits(bool all = false) {
        std::lock_guard<std::mutex> lock(commitMutex);
        if (all) return commits;
        std::vector<Commit> out;
        for (const Commit& c : commits) {
            if (c.monitor == 0) out.push_back(c);
        }
        return out;
    }
    size_t commitCount() {
        std::lock_guard<std::mutex> lock(commitMutex);
        return static_cast<size_t>(std::count_if(commits.begin(), commits.end(), [](const Commit& c) { return c.monitor == 0; }));
    }
    // A second screen, DP-2, playing `file` with no focus report, so it is never heard.
    void playOther(const std::string& file) {
        std::vector<int> redraw;
        manager.setMonitorOutput(1, "DP-2", redraw);
        manager.requestWallpaper("video", file, 1);
        worker.post(1);
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
    // Called on the render thread after each commit is recorded. Set it before the first play():
    // the post that starts the playing orders the write before the thread's reads.
    std::function<void(const Commit&)> onCommit;
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
// each point, the largest distance of any point from it (printed, never graded), and the slope.
struct Line {
    std::vector<int64_t> fitted;
    int64_t worst = 0;
    double slope = 0;
};
Line fitLine(const std::vector<std::pair<int64_t, int64_t>>& points) {
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
    return {fitted, worst, slope};
}

// A commit and the picture it showed, by that picture's presentation time in the file.
struct Shown {
    Commit commit;
    std::optional<int64_t> presentation;
};

// Every picture `file`'s decoder handed on, in order, dropped ones included.
std::vector<probe::PictureRecord> picturesOf(const std::string& file) {
    const std::string name = std::filesystem::path(file).filename().string();
    std::vector<probe::PictureRecord> out;
    std::lock_guard<std::mutex> lock(probe::g_pictureMutex);
    for (const probe::PictureRecord& p : probe::g_pictures) {
        if (p.file.find(name) != std::string::npos) out.push_back(p);
    }
    return out;
}

// One picture's interval in `file`: the smallest step between two presentation times its decoder
// handed on. Pictures dropped or skipped undecoded only lengthen steps, so this holds for a decoder
// that skips, where the median step of the commits is several pictures. 0 with fewer than two.
int64_t fileInterval(const std::string& file) {
    std::vector<int64_t> times;
    for (const probe::PictureRecord& p : picturesOf(file)) {
        if (p.presentationNs) times.push_back(*p.presentationNs);
    }
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());
    int64_t best = 0;
    for (size_t i = 1; i < times.size(); ++i) {
        if (const int64_t step = times[i] - times[i - 1]; step > 0 && (best == 0 || step < best)) best = step;
    }
    return best;
}

// The picture each of `commits` showed: the last one the picture decoder of `file` handed on before
// it. A round draws the picture decoded last and the render thread announces the commit before the
// next round decodes another (advanceSessionLocked, RenderWorker::runClock); the record is made on
// that same thread. Nothing, for a commit no picture of `file` came before.
std::vector<Shown> shownPictures(const std::vector<Commit>& commits, const std::string& file) {
    const std::vector<probe::PictureRecord> pictures = picturesOf(file);
    std::vector<Shown> out;
    size_t next = 0;
    std::optional<int64_t> last;
    bool any = false;
    for (const Commit& c : commits) {
        while (next < pictures.size() && pictures[next].at <= c.at) {
            last = pictures[next++].presentationNs;
            any = true;
        }
        out.push_back({c, any ? last : std::nullopt});
    }
    return out;
}

// The presentation time of the picture `file`'s decoder handed on last.
std::optional<int64_t> lastPictureOf(const std::string& file) {
    const std::vector<probe::PictureRecord> pictures = picturesOf(file);
    if (pictures.empty()) return std::nullopt;
    return pictures.back().presentationNs;
}

// One picture's interval in `shown`: the median step of two commits in a row whose pictures go forward,
// which a minority of dropped pictures does not move. 0 when no such step was committed.
int64_t pictureInterval(const std::vector<Shown>& shown) {
    std::vector<int64_t> steps;
    for (size_t i = 1; i < shown.size(); ++i) {
        if (shown[i].presentation && shown[i - 1].presentation && *shown[i].presentation > *shown[i - 1].presentation) {
            steps.push_back(*shown[i].presentation - *shown[i - 1].presentation);
        }
    }
    if (steps.empty()) return 0;
    std::nth_element(steps.begin(), steps.begin() + static_cast<std::ptrdiff_t>(steps.size() / 2), steps.end());
    return steps[steps.size() / 2];
}

// How many pictures a pass skipped between two shown in a row, `before` then `after`.
int skippedBetween(int64_t before, int64_t after, int64_t interval) {
    if (after <= before || interval <= 0) return 0;
    return static_cast<int>(std::llround(static_cast<double>(after - before) / static_cast<double>(interval))) - 1;
}

// Where each shown picture falls in the run: its presentation time, run on across loops, with
// `passes[i]` the pass it belongs to. Nothing, with `why`, when a commit's picture is not known.
struct FilePlace {
    std::vector<int64_t> at;
    std::vector<int> passes;
    int dropped = 0;  // pictures the presentation times skip over within a pass: dropped, or never decoded
};
std::optional<FilePlace> filePlaces(const std::vector<Shown>& shown, const std::string& file, std::string& why) {
    if (shown.size() < 2) {
        why = fmt("only %.0f pictures were committed", static_cast<double>(shown.size()));
        return std::nullopt;
    }
    for (size_t i = 0; i < shown.size(); ++i) {
        if (!shown[i].presentation) {
            why = fmt("which picture commit %.0f showed is not known: no picture of its file was seen handed on before it",
                      static_cast<double>(i));
            return std::nullopt;
        }
    }
    // GOAL: S15.570 -- the skip count and the pass length hold for a file whose decoder skips pictures:
    //       never negative, and each pass one file long. False when the interval is the commits' median
    //       step, which on a skipping file is several pictures, so a one-picture step counts -1.
    // CONTROL: in phase graderSelfTest; see S15.568's block there.
    const int64_t interval = fileInterval(file);
    if (interval == 0) {
        why = "the file's decoder handed on fewer than two distinct pictures";
        return std::nullopt;
    }
    // A pass is as long as the file's pictures, from the decoder's own record, which holds the pictures
    // dropped too: the latest committed one falls short whenever the file's last picture was dropped.
    std::optional<int64_t> last;
    for (const probe::PictureRecord& p : picturesOf(file)) {
        if (p.presentationNs && (!last || *p.presentationNs > *last)) last = p.presentationNs;
    }
    if (!last) {
        why = "the file's decoder handed on no picture with a presentation time";
        return std::nullopt;
    }
    const int64_t passLength = *last + interval;
    FilePlace out;
    int pass = 0;
    for (size_t i = 0; i < shown.size(); ++i) {
        const int64_t p = *shown[i].presentation;
        if (i > 0) {
            // Only a time that goes back starts a pass: the same picture committed again stays in its own.
            const int64_t before = *shown[i - 1].presentation;
            if (p < before) {
                ++pass;
            } else {
                out.dropped += skippedBetween(before, p, interval);
            }
        }
        out.at.push_back(pass * passLength + p);
        out.passes.push_back(pass);
    }
    return out;
}

// Each committed picture that starts a beep -- one at a multiple of half a second in the file, and
// scheduled after `from` -- set against the beep heard nearest its scheduled time: its place on the
// robust line through every commit against where its picture falls in the run. A picture committed
// again is graded once. Returns how many were graded, -1 when the run cannot be graded; `line` is that
// line's, when graded.
int gradeBeepsAgainstPictures(const std::string& rule, const Capture& cap, const std::vector<Shown>& shown,
                              const std::string& file, int64_t from, double toleranceMs, int& passStartsGraded,
                              Line* line = nullptr) {
    passStartsGraded = 0;
    std::string why;
    const std::optional<FilePlace> places = filePlaces(shown, file, why);
    if (!places) {
        notVerified(rule.c_str(), why);
        return -1;
    }
    std::vector<std::pair<int64_t, int64_t>> points;
    for (size_t i = 0; i < shown.size(); ++i) points.push_back({places->at[i], shown[i].commit.at});
    const Line fitted = fitLine(points);
    if (line != nullptr) *line = fitted;
    std::printf("audio_playback_test: %s: %zu commits over %d passes, %d pictures not shown, at most %.1f ms from the line "
                "through them\n",
                rule.c_str(), shown.size(), places->passes.back() + 1, places->dropped, fitted.worst / 1e6);
    const int64_t recordEnd =
        cap.buffers.empty() ? 0 : cap.heardAt(cap.buffers.back(), cap.buffers.back().first + cap.buffers.back().frames);
    const std::vector<int64_t> heard = onsets(cap, 0.1f, 480);
    int graded = 0;
    for (size_t i = 0; i < shown.size(); ++i) {
        const int64_t p = *shown[i].presentation;
        if (i > 0 && p == *shown[i - 1].presentation && places->passes[i] == places->passes[i - 1]) continue;
        if (std::llabs(p - (p + 250 * kMs) / (500 * kMs) * (500 * kMs)) > 2 * kMs) continue;
        const int64_t scheduled = fitted.fitted[i];
        if (scheduled < from || scheduled + 250 * kMs > recordEnd) continue;
        std::optional<int64_t> nearest;
        for (const int64_t h : heard) {
            if (!nearest || std::llabs(h - scheduled) < std::llabs(*nearest - scheduled)) nearest = h;
        }
        const double error = nearest ? (*nearest - scheduled) / 1e6 : 1e9;
        std::printf("audio_playback_test: %s: the beep at %.1f s of pass %d heard %+.2f ms from its picture's scheduled "
                    "time\n",
                    rule.c_str(), p / 1e9, places->passes[i], error);
        check(std::fabs(error) <= toleranceMs, rule + ": each beep is heard with the picture at its timestamp",
              fmt("the beep at %.1f s of pass %.0f: %+.2f ms", p / 1e9, places->passes[i], error));
        ++graded;
        if (p < 2 * kMs && i != 0) ++passStartsGraded;
    }
    return graded;
}

// GOAL: S15.497 -- with the beeps fixture, each beep is heard within 12 ms of the scheduled time of the
//       picture showing that beep's timestamp, across loops. False if the pictures follow a clock that
//       misplaces the sound, or the steering puts them anywhere but on the sound.
// WHY-THIS-SHAPE: which picture a commit showed is read from the decoder's own output (shownPictures),
//       not from its generation: production drops pictures the sound has passed, so the n-th commit
//       need not show the n-th picture. A dropped beep picture is simply not graded. Each picture's time
//       is its place on a robust (Theil-Sen) line through every commit against where it falls in the
//       file, so a late wake of the render thread moves no picture's time; how far commits sit from it
//       is printed, not graded.
// COVERAGE-ASSERT: at least eight beeps graded, one at a pass start after a loop; every commit's
//       picture known, or not verified.
// CONTROL: control_run.sh 2026-10-12, reverted: AudioTrack::clock() reporting the file 33 ms ahead
//       (renderer/audio/audio_output.cpp) -> "FAIL: sync rule: each beep is heard with the picture at its
//       timestamp" (+20.7 to +44.9 ms). ` - waiting.left` deleted from both anchors in steerSoundLocked
//       (backend/daemon/wallpaper_manager.cpp) -> "FAIL: sync rule: each beep is heard with the picture at its
//       timestamp" (-31.7 to -33.3 ms, the third pass).
void runSyncChecks() {
    const char* kGoals = "S15.497 (sound with its pictures)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    Daemon d(smallConfig());
    const std::string file = asset("wallpaper_beeps.mp4");
    if (!startHeard(d, file, kGoals)) return;
    std::this_thread::sleep_for(5500ms);
    const std::vector<Commit> commits = d.takeCommits();
    const Capture cap = takeCapture();
    const std::optional<int64_t> first = firstSound(cap, 0, 1e-3f);
    if (!first) {
        check(false, "sync rule: the sound heard at the start is still in the record");
        return;
    }
    int passStarts = 0;
    const int graded =
        gradeBeepsAgainstPictures("sync rule", cap, shownPictures(commits, file), file, *first + 60 * kMs, 12.0, passStarts);
    if (graded < 0) return;
    check(graded >= 8 && passStarts >= 1, "sync rule: at least eight beeps were graded, one at a pass start after a loop",
          fmt("%.0f graded, %.0f at a pass start", graded, passStarts));
}

// GOAL: S15.498 -- a continuous tone plays through every loop with no silence longer than 10 ms, and
//       with no dip in level except the one switch fade at each loop: the sound is not moved while it
//       agrees with the pictures. False for a new pass's sound anchored anywhere but where the last
//       pass's sound ends (silence at the loop) or a sound re-anchored every round (a dip at every picture).
// WHY-THIS-SHAPE: a dip is 24+ frames under 0.05 between louder frames -- a 440 Hz tone at 0.5 crosses
//       zero in two frames, and a 5 ms fade spends about 24 frames down there. One fade per loop is
//       allowed, plus two for a re-anchor a stall could earn. Loops are counted from the pictures the
//       commits showed (a pass starts where the presentation time goes back), not from a timing line or
//       a count of commits, which dropped pictures would throw off; a run whose pictures are not known
//       leaves the dip half not verified. The silence rule needs no pictures and is always graded.
// COVERAGE-ASSERT: at least six seconds of sound analysed; with the pictures known, two loops crossed.
// CONTROL: control_run.sh 2026-10-12, reverted, in steerSoundLocked, backend/daemon/wallpaper_manager.cpp:
//       the hand-over to a new pass anchored at `planned.timeline + kSoundDrift` -> "FAIL: loop rule: no
//       silence longer than 10 ms inside the tone"; the behind-test `if (behind > kSoundDrift)` made
//       `if (true)` (the sound moved every round) -> "FAIL: loop rule: no dip in the tone but one fade per
//       loop". Test-side, control_run.sh, reverted: a new pass counted only where the presentation time
//       goes back by more than the file -> "FAIL: loop rule: the analysed sound crossed two loops" (0 loops).
void runLoopChecks() {
    const char* kGoals = "S15.498 (a loop plays through)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    Daemon d(smallConfig());
    const std::string file = asset("wallpaper_tone.mp4");
    if (!startHeard(d, file, kGoals)) return;
    std::this_thread::sleep_for(7000ms);
    const std::vector<Commit> commits = d.takeCommits();
    const Capture cap = takeCapture();
    const Continuity c = continuity(cap, 480, 0.05f, 24);
    std::string why;
    const std::optional<FilePlace> places = filePlaces(shownPictures(commits, file), file, why);
    const bool counted = places.has_value();
    const int loops = counted ? places->passes.back() - places->passes.front() : 0;
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
// CONTROL: control_run.sh 2026-10-11, reverted, in renderer/audio/audio_output.cpp: loseOutput's `dropStream(*t,
//       now)` given a retry at the end of time -> "FAIL: reconnect rule: the sound comes back by itself ..." (not
//       back); its m_log deleted -> "... losing the sound server is said in one line" (0); connect()'s "can be
//       reached again" line deleted -> "... its return is said in one line" (0). "The pictures go on" has no control:
//       no sound-thread break short of a crash stops them. S15.513: destroyStream's `t.playhead = {};` deleted stays
//       green in this phase (control_run.sh 2026-10-12, reverted): the daemon moves a sound left behind its pictures
//       with a new play(), which covers for the stale playhead. Phase engineReconnect grades that half on AudioOutput
//       alone. The beep line here shares S15.497's check: AudioTrack::clock() reporting the file 33 ms ahead ->
//       "FAIL: reconnect rule: each beep is heard with the picture at its timestamp" (+32.97 ms).
// Phase `reconnect` grades S15.501 and phase `reconnectPlaysOn` S15.513, each on its own run of the
// same scenario, so a break of one leaves the other's controls gradable.
void runReconnect(bool playsOn) {
    const char* kGoals = playsOn ? "S15.513 (sound plays on after the server returns)" : "S15.501 (the sound server lost and back)";
    auto sound = std::make_unique<SoundServer>(kGoals);
    if (!sound->ready) return;
    Daemon d(smallConfig());
    const std::string file = asset("wallpaper_beeps_long.mp4");
    if (!startHeard(d, file, kGoals)) return;
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
    const int graded =
        gradeBeepsAgainstPictures("reconnect rule", cap, shownPictures(commits, file), file, backAt + 100 * kMs, 12.0, passStarts);
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

// GOAL: S15.513, the engine's half -- a track whose sound server is lost and comes back plays on from its
//       anchor with no new play(): once back, the right channel's tone is not silent again. False if the
//       new stream inherits the old one's playhead, which holds the request serial the track is begun
//       again under, so the callback never switches to the new segment and falls silent.
// WHY-THIS-SHAPE: AudioOutput alone. Phase reconnectPlaysOn drives the same loss through the daemon, which
//       moves a sound that fell behind its pictures with a new play() -- a new serial -- and so covers for
//       this defect: that phase stays green with the playhead kept.
// CONTROL: control_run.sh 2026-10-12, reverted: destroyStream's `t.playhead = {};` deleted,
//       renderer/audio/audio_output.cpp -> "FAIL: engine reconnect rule: once back, the track plays on".
void runEngineReconnectChecks() {
    const char* kGoals = "S15.513 (a track plays on after its server returns)";
    auto sound = std::make_unique<SoundServer>(kGoals);
    if (!sound->ready) return;
    resetCapture();
    Engine engine(kGoals);
    if (!engine.output) return;
    std::unique_ptr<AudioTrack> track = engine.output->track(asset("wallpaper_beeps_long.mp4"), "engine");
    const int64_t t0 = probe::nowNs() + 300 * kMs;
    track->play(anchorAt(0, t0));
    sleepUntil(t0 + 1500 * kMs);
    sound.reset();
    std::this_thread::sleep_for(1000ms);
    sound = std::make_unique<SoundServer>(kGoals);
    if (!sound->ready) return;
    const int64_t restartedAt = probe::nowNs();
    int64_t backAt = 0;
    if (waitFor(8000ms, [&] { return probe::g_lastLoudQueuedAt.load() > restartedAt; })) backAt = probe::g_lastLoudQueuedAt.load();
    std::this_thread::sleep_for(2000ms);
    track->stop();
    std::this_thread::sleep_for(200ms);
    const Capture cap = takeCapture();
    if (backAt == 0) {
        check(false, "engine reconnect rule: the track's sound comes back within 8 s of its server");
        return;
    }
    Continuity tone = continuity(cap, 480, 0.05f, 24, true);
    tone.gaps.push_back(tone.trailing);
    uint64_t silentAfter = 0;
    for (const auto& [at, frames] : tone.gaps) {
        if (at > backAt && at < backAt + 1800 * kMs) silentAfter = std::max(silentAfter, frames);
    }
    std::printf("audio_playback_test: engine reconnect rule: back %.2f s after the server; silent again for %.1f ms after\n",
                (backAt - restartedAt) / 1e9, silentAfter * 1000.0 / kRate);
    check(silentAfter == 0, "engine reconnect rule: once back, the track plays on",
          fmt("it fell silent again for %.0f ms within the 1.8 s after its return", silentAfter * 1000.0 / kRate));
}

// GOAL: S15.519 -- sound turned on while the file's last picture is on screen, on a screen whose track
//       already exists (heard, then turned off), is heard within 150 ms, in step with that picture (each
//       beep left in the pass within 12 ms of the last picture's scheduled time plus its distance from
//       it), and is handed to the new pass on time: that pass's first beep within 5 ms of its first
//       scheduled time, its beeps at 0.5 and 1 s within 12 ms. False if such sound waits for the loop,
//       plays off the picture schedule, is never handed over, or is handed over late.
// WHY-THIS-SHAPE: one picture every two seconds and the call 100 ms after the last one's commit, so only
//       the call's own round can start the sound inside the window graded "heard", and no commit may land
//       in it. Beeps, not a steady tone, which sounds the same at any offset. The track is made first:
//       for a new one, the notice of its file's opening happens to wake an early round that hands over in
//       time, so only an existing track shows a hand-over left to the pass's own round (its head then cut
//       by the late-start skip). +-5 ms covers the onset threshold meeting the 5 ms ramp (about +1 ms).
//       Graded against the robust line through the six commits; their distance from it is printed. Not
//       skipped when the hand-over's seek came late: a round woken over 80 ms late looks exactly like the
//       defect graded here, a hand-over left to the pass's own round, so a skip would hide it. The seek's
//       lead on the pass is printed instead.
// COVERAGE-ASSERT: the sound was heard before it was turned off, so the track existed; every expected beep
//       graded, at least two before the loop and three after it.
// CONTROL: control_run.sh 2026-10-12, reverted, in steerSoundLocked, backend/daemon/wallpaper_manager.cpp:
//       `} else if (waiting.reaches && planned.timeline - now > kHandOverLead) {` made `false && ...` ->
//       "FAIL: last picture rule: sound turned on during the last picture is heard within 150 ms"; 33 ms
//       added to that branch's anchor time -> "FAIL: last picture rule, rest of the pass: each beep is heard
//       on time"; the hand-over condition made `false && next.startsPass && ...` -> "FAIL: last picture rule,
//       after the loop: every expected beep is heard within 100 ms of its time"; that branch's
//       `return {.roundAt = planned.timeline - kHandOverLead};` made `return {};` -> "FAIL: last picture
//       rule, the new pass's first beep: each beep is heard on time".
void runLastPictureChecks() {
    const char* kGoals = "S15.519 (sound started on a pass's last picture)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    Daemon d(smallConfig());
    resetCapture();
    const int64_t started = probe::nowNs();
    d.play(asset("wallpaper_beeps_slow.mp4"), ScreenFocus::Desktop);
    if (!d.waitForCommits(1, 15s) ||
        !waitFor(kSoundArrivalDeadline, [&] { return probe::g_lastLoudQueuedAt.load() > started; })) {
        notVerified(kGoals, "the slow video was not heard at its start, so its screen's track is not known to exist");
        return;
    }
    d.manager.setSound(false);
    d.worker.wake();
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
    const Line line = fitLine(points);
    std::printf("audio_playback_test: last picture rule: the six commits sit at most %.1f ms from their line\n",
                line.worst / 1e6);
    const int64_t last = line.fitted[4], next = line.fitted[5];
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
    std::optional<int64_t> handedAt;
    for (const probe::SoundSeek& s : soundSeeksSince(onAt)) {
        if (s.targetNs < 50 * kMs && s.at < next + 100 * kMs) handedAt = s.at;
    }
    std::printf("audio_playback_test: last picture rule: the hand-over's seek came %.1f ms before the new pass\n",
                handedAt ? (next - *handedAt) / 1e6 : -1.0);
    const int firstGraded =
        gradeBeepTimes("last picture rule, the new pass's first beep", cap, {{"pass 1 beep 0", next}}, -5.0, 5.0);
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

// Pictures drawn late after `from`: of `shown`'s commits in [from, to], those that came more than `slack`
// after the robust line through the commits before `from` (their picture's presentation against their
// commit) puts them. Both sides stop at a pass boundary. Nothing, with `why`, when fewer than ten commits
// before `from` place the line.
struct Lateness {
    int late = 0;
    int graded = 0;
    // The latest and the second latest any graded commit came against the line, either negative.
    int64_t worst = 0, second = 0;
};
std::optional<Lateness> lateAfter(const std::vector<Shown>& shown, int64_t from, int64_t to, int64_t slack, std::string& why) {
    size_t first = shown.size();
    for (size_t i = 0; i < shown.size(); ++i) {
        if (shown[i].commit.at >= from) {
            first = i;
            break;
        }
    }
    std::vector<std::pair<int64_t, int64_t>> points;
    for (size_t i = first; i-- > 0;) {
        if (!shown[i].presentation) break;
        if (!points.empty() && *shown[i].presentation >= points.back().first) break;
        points.push_back({*shown[i].presentation, shown[i].commit.at});
    }
    if (points.size() < 10) {
        why = fmt("only %.0f commits of the pass before the stall place the line", static_cast<double>(points.size()));
        return std::nullopt;
    }
    std::reverse(points.begin(), points.end());
    const Line line = fitLine(points);
    const int64_t base = points.front().second;
    // fitLine's own intercept, recovered from a fitted point and its slope.
    const double intercept = static_cast<double>(line.fitted.front() - base) - line.slope * points.front().first;
    Lateness out;
    out.worst = out.second = INT64_MIN;
    std::optional<int64_t> previous;
    for (size_t i = first; i < shown.size() && shown[i].commit.at <= to; ++i) {
        if (!shown[i].presentation || (previous && *shown[i].presentation < *previous)) break;
        previous = shown[i].presentation;
        const int64_t onLine = base + static_cast<int64_t>(std::llround(intercept + line.slope * *shown[i].presentation));
        const int64_t lateBy = shown[i].commit.at - onLine;
        out.second = std::max(out.second, std::min(out.worst, lateBy));
        out.worst = std::max(out.worst, lateBy);
        ++out.graded;
        if (lateBy > slack) ++out.late;
    }
    return out;
}

// What runSoundUndisturbed saw: whether it graded, when `disturb` ran, every commit and the picture each
// showed.
struct Undisturbed {
    bool graded = false;
    int64_t at = 0;
    std::vector<Commit> commits;
    std::vector<Shown> shown;
};

// Plays the long fixture heard, does `disturb` 1.5 s in, and grades the next 1.4 s: heard throughout,
// no sound seek and no silence in the right channel's tone -- unless the sound server itself fell
// behind there, which is the one thing that earns a heard sound a seek it is not otherwise owed.
Undisturbed runSoundUndisturbed(const char* rule, const char* goals, const std::function<bool(Daemon&)>& disturb) {
    Undisturbed out;
    SoundServer sound(goals);
    if (!sound.ready) return out;
    Daemon d(smallConfig());
    const std::string file = asset("wallpaper_beeps_long.mp4");
    if (!startHeard(d, file, goals)) return out;
    std::this_thread::sleep_for(1500ms);
    out.at = probe::nowNs();
    if (!disturb(d)) return out;
    std::this_thread::sleep_for(1500ms);
    // The seeks and the server's gaps over the same span: from the disturbance to this reading.
    const int64_t seekEnd = probe::nowNs();
    const size_t seeks = soundSeeksSince(out.at, seekEnd).size();
    const Capture cap = takeCapture();
    out.commits = d.takeCommits();
    out.shown = shownPictures(out.commits, file);
    const int64_t end = out.at + 1400 * kMs;
    const auto [silent, heard] = longestSilence(cap, out.at, end, true);
    const int64_t serverGap = longestBufferGap(cap, out.at, seekEnd);
    std::printf("audio_playback_test: %s: %zu sound seeks; longest silence %llu frames of %llu heard; the sound server "
                "went at most %.1f ms between cycles\n",
                rule, seeks, static_cast<unsigned long long>(silent), static_cast<unsigned long long>(heard), serverGap / 1e6);
    check(heard >= 48000ULL * 1400 / 1000 * 9 / 10, std::string(rule) + ": the sound was heard through the window",
          fmt("%.0f frames", static_cast<double>(heard)));
    // Not graded when the server ran a cycle over 20 ms late: that can leave the sound more than
    // kSoundDrift behind its pictures, which earns it a seek.
    if (serverGap > kServerLateBound) {
        notVerified(goals, fmt("the sound server went %.1f ms between cycles, more than a cycle plus the 20 ms the pictures "
                               "wait for a sound behind them, so moving the sound would be correct",
                               serverGap / 1e6));
        return out;
    }
    check(silent <= 48, std::string(rule) + ": the sound plays on", fmt("%.0f frames silent", static_cast<double>(silent)));
    check(seeks == 0, std::string(rule) + ": the sound is not moved", fmt("%.0f seeks", static_cast<double>(seeks)));
    out.graded = true;
    return out;
}

// GOAL: S15.538 -- a picture held up once, by a 110 ms stall a file that decodes quickly closes by dropping
//       inside kCatchUpHold, does not move a playing sound, which plays on with no silence. False if a late
//       picture re-anchors the sound, which then jumps back by the delay.
// WHY-THIS-SHAPE: the picture decoder's next av_read_frame sleeps 110 ms, once, leaving the sound about
//       77 ms (110 less one 33 ms interval) ahead. With a clock, a sound ahead is moved only once a hold
//       has run out, and this file drops the two pictures in two decodes of a millisecond, so however far
//       a scheduler stretches the stall, no margin to a threshold is graded. Zero seeks because the only
//       seek a playing sound gets is a re-anchor. A hold that runs out: leadLimit and holdRunOut.
// COVERAGE-ASSERT: the stall was consumed by a picture read, and held the pictures up 100 ms or more.
// CONTROL: control_batch.sh 2026-10-13, reverted, backend/daemon/wallpaper_manager.cpp steerSoundLocked: the
//       move rule `(!chased && overdue - behind > kSoundDrift)` made `(overdue - behind > kSoundDrift)` (a
//       late picture re-anchors at once) -> "FAIL: late picture rule: the sound is not moved" (1 seek; the drop
//       and lateness lines below go red with it, 0 skipped and 38 of 38 late). "The sound plays on" has no
//       production control: a stop added to the drop loop is played again within the round.
//       Test-side, control_run.sh 2026-10-12, reverted: the sound decoder stalled 1 s in the window -> "FAIL:
//       late picture rule: the sound plays on" (41984 frames silent); the av_read_frame stand-in never
//       consuming the stall -> "the staged picture stall was reached" (110 ms unused); a 30 ms stall -> "the
//       stall held the pictures up"; the window moved 3 s before the disturbance -> "the sound was heard
//       through the window". A stall never armed (0 ms) leaves "was reached" green: only "held up" sees it.
// GOAL: S15.557 -- the whole lead that stall leaves is closed by dropping, not by drawing: at least as many
//       pictures skipped after it as whole intervals the staged 110 ms leaves past one (two), and at most one
//       commit after the stall more than an interval past the line the commits before it set. False for
//       pictures caught up by drawing each one, or by dropping one and drawing the rest late.
// WHY-THIS-SHAPE: skips are read from the presentation times shown (shownPictures) at the file's own
//       interval; commit gaps cannot tell a drop from a late draw. The skips owed come from the staged stall,
//       not the measured wait, which the scheduler lengthens. Lateness is graded against a line through the
//       pre-stall commits, as the sound is not moved and the pictures stay on it; one late commit is allowed
//       (the picture the stall held up), and a correct draw is at most an interval and a decode late. A
//       burst test on commit spacing cannot be used: the first picture after the drops is drawn as the
//       stall ends and the next on time, about 20 ms apart, against an interval/2 threshold of 16.7 ms.
// CONTROL: control_batch.sh 2026-10-13, reverted, advanceSessionLocked, backend/daemon/wallpaper_manager.cpp: the
//       drop loop disabled (`!pace.timesPicture ||` made `true ||`) -> "FAIL: late picture rule: the whole lead
//       the stall left is closed by dropping" (0 skipped, 2 owed) and "... no picture after the stall is drawn
//       late, but the one it held up" (2 of 41 over an interval late); the loop left after its first drop
//       (`if (dropped) break;` at its head) -> "... closed by dropping" (1 skipped), its late check green (one
//       commit 43.5 ms late). Green: 2 skipped, the latest commit +10.2 ms against the line.
void runLatePictureChecks() {
    constexpr int kStallMs = 110;
    const Undisturbed run =
        runSoundUndisturbed("late picture rule", "S15.538, S15.557 (a late picture does not move the sound)", [](Daemon&) {
            probe::g_stallPictureMs.store(kStallMs);
            return true;
        });
    const int left = probe::g_stallPictureMs.exchange(0);
    if (!run.graded) return;
    check(left == 0, "late picture rule: the staged picture stall was reached", fmt("%.0f ms left unused", left));
    if (left != 0) return;
    const int64_t end = run.at + 1400 * kMs;
    int64_t longest = 0, stallEnd = 0;
    for (size_t i = 1; i < run.commits.size(); ++i) {
        if (run.commits[i].at < run.at || run.commits[i].at > end) continue;
        const int64_t gap = run.commits[i].at - std::max(run.commits[i - 1].at, run.at);
        if (gap > longest) {
            longest = gap;
            stallEnd = run.commits[i].at;
        }
    }
    const int64_t interval = fileInterval(asset("wallpaper_beeps_long.mp4"));
    int after = 0, dropped = 0;
    for (size_t i = 1; i < run.shown.size(); ++i) {
        const Shown &a = run.shown[i - 1], &b = run.shown[i];
        if (b.commit.at < stallEnd || b.commit.at > end) continue;
        ++after;
        if (a.presentation && b.presentation) dropped += skippedBetween(*a.presentation, *b.presentation, interval);
    }
    const int owed = interval > 0 ? static_cast<int>((kStallMs * kMs - interval) / interval) : 0;
    std::printf("audio_playback_test: late picture rule: the pictures waited %.1f ms; %d commits after, %d pictures skipped "
                "at %.2f ms a picture, %d owed\n",
                longest / 1e6, after, dropped, interval / 1e6, owed);
    check(longest >= 100 * kMs && after >= 10, "late picture rule: the stall held the pictures up, and they went on after it",
          fmt("a %.1f ms wait, %.0f commits after", longest / 1e6, after));
    if (longest < 100 * kMs || interval == 0) return;
    check(dropped >= owed, "late picture rule: the whole lead the stall left is closed by dropping",
          fmt("%.0f pictures skipped, %.0f owed", dropped, owed));
    std::string why;
    const std::optional<Lateness> late = lateAfter(run.shown, stallEnd - 1, end, interval, why);
    if (!late) {
        notVerified("S15.557 (no picture after the stall is drawn late)", why);
        return;
    }
    std::printf("audio_playback_test: late picture rule: %d of %d commits after the stall over an interval late, the "
                "latest %+.1f ms against the line before it, the next %+.1f ms\n",
                late->late, late->graded, late->worst / 1e6, late->second / 1e6);
    check(late->late <= 1, "late picture rule: no picture after the stall is drawn late, but the one it held up",
          fmt("%.0f of %.0f commits over an interval late", late->late, late->graded));
}

// GOAL: S15.539 -- a render owed for the same video (a resize) leaves its sound playing: no seek and
//       no silence. False if a round that finds a render owed stops the sound, which the next playing
//       round starts again.
// WHY-THIS-SHAPE: another monitor's job is queued first, so one clock round runs while this monitor's
//       render is owed, which is the round that would stop the sound. Seeks and silence are graded under
//       the same sound-server guard as S15.538's.
// COVERAGE-ASSERT: the size change posted a render.
// CONTROL: control_run.sh 2026-10-12, reverted: advanceSessionLocked stopping the sound on any stopped
//       round (`soundPlays` made false), backend/daemon/wallpaper_manager.cpp -> "FAIL: render owed rule: the
//       sound is not moved" (1 seek) and "FAIL: render owed rule: the sound plays on" (1608 frames silent).
//       Test-side, control_run.sh 2026-10-12, reverted: the size change taken out -> "FAIL: render owed
//       rule: the size change posted a render".
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

// Puts the staged stalls and the simulated card rate back on scope exit.
struct StallReset {
    ~StallReset() {
        probe::g_stallPictureMs.store(0);
        probe::g_slowPictureReads.store(0);
        probe::g_stallFileMs.store(0);
        probe::g_streamRate.store(0.0);
        probe::g_hideStreamTime.store(false);
        probe::g_costMs.store(0);
        probe::g_holdMs.store(0);
        probe::g_holdRepeats.store(false);
        probe::g_blitHoldMs.store(0);
        probe::g_hideAtHandOver.store(false);
        {
            std::lock_guard<std::mutex> lock(probe::g_costMutex);
            probe::g_costFile.clear();
            probe::g_costCycle.clear();
            probe::g_costCycleAt = 0;
        }
        std::lock_guard<std::mutex> lock(probe::g_stallFileMutex);
        probe::g_slowFile.clear();
        probe::g_stallFile.clear();
    }
};

// GOAL: S15.555 -- a heard screen's pictures follow its sound card: with the card 0.5 % slow or 0.5 % fast,
//       each beep over ten seconds is heard within 6 ms of its picture's scheduled time, and the pictures
//       come at the card's pace (1/30 s of the card's time apart: 33.50 or 33.17 ms). False if the
//       pictures keep the system clock's time, which leaves the beeps drifting 5 ms a second from them.
// WHY-THIS-SHAPE: the null sink runs on the monotonic clock and never drifts, so the drift is simulated:
//       pw_stream_set_rate on the engine's stream, from its own process callback, has the server take its
//       frames at 48000 / rate a second, as an off-rate card would. A run whose server did not take frames
//       at that rate is not verified, never passed. The pace is the slope of the robust line through each
//       picture's place in the file and its commit, against the rate the record shows the server took.
// COVERAGE-ASSERT: the measured rate within 0.1 % of the simulated one; at least 15 beeps graded.
// CONTROL: control_run.sh 2026-10-12, reverted: `waiting.from += behind;` deleted from steerSoundLocked,
//       backend/daemon/wallpaper_manager.cpp -> phase driftFastCard "FAIL: drift rule, card fast: each beep is
//       heard with the picture at its timestamp" and "the pictures come at the sound card's pace"; phase
//       driftSlowCard the same two lines.
void runDrift(const std::string& rule, const char* goals, double rate) {
    SoundServer sound(goals);
    if (!sound.ready) return;
    StallReset reset;
    probe::g_streamRate.store(rate);
    Daemon d(smallConfig());
    const std::string file = asset("wallpaper_beeps_long.mp4");
    if (!startHeard(d, file, goals)) return;
    // The file is twelve seconds: the run ends before its loop.
    std::this_thread::sleep_for(10500ms);
    const std::vector<Commit> commits = d.takeCommits();
    const Capture cap = takeCapture();
    if (cap.buffers.size() < 200) {
        notVerified(goals, fmt("only %.0f buffers were queued, too few to measure the rate", static_cast<double>(cap.buffers.size())));
        return;
    }
    const probe::BufferRecord &a = cap.buffers[20], &b = cap.buffers.back();
    const double taken = static_cast<double>(b.first - a.first) * 1e9 / static_cast<double>(b.queuedAt - a.queuedAt);
    const double asked = kRate / rate;
    std::printf("audio_playback_test: %s: the server took %.1f frames a second (asked %.1f) over %.1f s\n", rule.c_str(),
                taken, asked, (b.queuedAt - a.queuedAt) / 1e9);
    if (std::fabs(taken / asked - 1.0) > 0.001) {
        notVerified(goals, fmt("the private server took %.1f frames a second, not the %.1f the rate correction asks for, so "
                               "no drift was simulated",
                               taken, asked));
        return;
    }
    const std::optional<int64_t> first = firstSound(cap, 0, 1e-3f);
    if (!first) {
        check(false, rule + ": the sound heard at the start is still in the record");
        return;
    }
    int passStarts = 0;
    Line line;
    const int graded =
        gradeBeepsAgainstPictures(rule, cap, shownPictures(commits, file), file, *first + 1000 * kMs, 6.0, passStarts, &line);
    if (graded < 0) return;
    check(graded >= 15, rule + ": at least 15 beeps were graded", fmt("%.0f", graded));
    // Seconds of commits per second of the file: the card's seconds, in the monotonic clock's.
    const double pace = kRate / taken;
    std::printf("audio_playback_test: %s: %.4f ms a picture; the card's pace is %.4f\n", rule.c_str(), line.slope * 1000 / 30,
                pace * 1000 / 30);
    check(std::fabs(line.slope / pace - 1.0) <= 0.001, rule + ": the pictures come at the sound card's pace",
          fmt("%.4f ms a picture, the card's pace %.4f", line.slope * 1000 / 30, pace * 1000 / 30));
}

void runDriftSlowCardChecks() { runDrift("drift rule, card slow", "S15.555 (pictures follow a slow card)", 1.005); }
void runDriftFastCardChecks() { runDrift("drift rule, card fast", "S15.555 (pictures follow a fast card)", 0.995); }

// GOAL: S15.556 -- when the sound stops being played out (the private server stopped 400 ms while heard),
//       the pictures do not wait for it -- no gap over 200 ms between commits from 100 ms before the stop to
//       600 ms after it ends, the window's edges counted -- and the sound, behind its pictures once back, is
//       moved to them: one to three sound seeks after the stop, and the beeps from 1 s after the return
//       within 6 ms of their pictures. False if the pictures follow a stalled sound, or a sound left behind
//       is never moved.
// WHY-THIS-SHAPE: SIGSTOP on the server this phase started stalls the sound without touching the engine.
//       Pictures that follow the stalled sound wait about as long as it stopped, so the bound is half the
//       400 ms stop; one interval plus slack (50 ms) would grade the render thread's wake-ups too. Both
//       edges of the window are gaps, so a screen frozen past either end still shows. How many pictures
//       came during the stop is printed, not graded: the clock runs on from its last cycle, so no break
//       short of freezing it makes the pictures wait inside the stop. Seeks are bounded, not counted to
//       one: a re-anchored segment starting a cycle late may earn a second.
// COVERAGE-ASSERT: the record shows the server went over 300 ms without a cycle; at least three beeps
//       graded after the return.
// CONTROL: control_run.sh 2026-10-12, reverted: steerSoundLocked's `behind > kSoundDrift ||` made `false ||`,
//       backend/daemon/wallpaper_manager.cpp -> "FAIL: server stop rule: the pictures do not wait for a stopped sound
//       server" (332.0 ms; 33.3 ms green) and "FAIL: server stop rule: a sound behind its pictures once back is moved
//       to them" (0).
void runServerStopChecks() {
    const char* kGoals = "S15.556 (pictures do not wait for a stalled sound)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    Daemon d(smallConfig());
    const std::string file = asset("wallpaper_beeps_long.mp4");
    if (!startHeard(d, file, kGoals)) return;
    std::this_thread::sleep_for(1500ms);
    const pid_t server = sound.server.pid();
    if (server <= 0) {
        harnessFailure("server stop rule: the private server this phase started has no process to stop");
        return;
    }
    const int64_t stopAt = probe::nowNs();
    int64_t contAt = 0;
    {
        struct Resume {
            pid_t pid;
            ~Resume() { ::kill(pid, SIGCONT); }
        } resume{server};
        ::kill(server, SIGSTOP);
        std::this_thread::sleep_for(400ms);
    }
    contAt = probe::nowNs();
    std::this_thread::sleep_for(3000ms);
    const std::vector<Commit> commits = d.takeCommits();
    const Capture cap = takeCapture();
    const size_t seeks = soundSeeksSince(stopAt).size();
    const int64_t serverGap = longestBufferGap(cap, stopAt - 100 * kMs, contAt + 100 * kMs);
    const int64_t from = stopAt - 100 * kMs, to = contAt + 600 * kMs;
    int64_t longest = 0, previous = from;
    int duringStop = 0;
    for (const Commit& c : commits) {
        if (c.at <= from || c.at > to) continue;
        longest = std::max(longest, c.at - previous);
        previous = c.at;
        if (c.at >= stopAt && c.at <= contAt) ++duringStop;
    }
    longest = std::max(longest, to - previous);
    std::printf("audio_playback_test: server stop rule: the server went %.1f ms without a cycle; the pictures at most %.1f "
                "ms apart, %d of them during the stop; %zu sound seeks\n",
                serverGap / 1e6, longest / 1e6, duringStop, seeks);
    if (serverGap < 300 * kMs) {
        notVerified(kGoals, fmt("the stopped server went only %.1f ms without a cycle", serverGap / 1e6));
        return;
    }
    check(longest <= 200 * kMs, "server stop rule: the pictures do not wait for a stopped sound server",
          fmt("%.1f ms between commits", longest / 1e6));
    check(seeks >= 1 && seeks <= 3, "server stop rule: a sound behind its pictures once back is moved to them",
          fmt("%.0f sound seeks", static_cast<double>(seeks)));
    int passStarts = 0;
    const int graded = gradeBeepsAgainstPictures("server stop rule", cap, shownPictures(commits, file), file,
                                                 contAt + 1000 * kMs, 6.0, passStarts);
    if (graded >= 0) check(graded >= 3, "server stop rule: at least three beeps after the return were graded", fmt("%.0f", graded));
}

// GOAL: S15.558 -- the sound's clock speaks for the latest play only: a heard file of one picture every two
//       seconds, with the render thread woken every 30 ms across a loop, moves its sound once there (the
//       hand-over), brings neither picture either side of the loop over 20 ms before the robust line through
//       every commit (against the picture's place in the file) puts it, and has the new pass's first beep
//       heard within 5 ms of the line's time for its picture. False if the last pass's sound, still playing
//       after the hand-over to the next, times the new pass's pictures: they then come at once, or the
//       sound is moved again to meet them.
// WHY-THIS-SHAPE: after the hand-over the track holds the new pass's play while the old segment still
//       plays out; only a round between the two reads that clock, and with two-second pictures none runs
//       then unless something wakes the thread, as other screens' pictures would. The defect makes pictures
//       early, which is graded at 20 ms; lateness is graded too, at the same 20 ms the pictures' two-second
//       spacing held before this rework, since a picture held back across the loop stutters as visibly. The
//       beep is graded against the line, not a commit. The seek count is not graded when the sound server
//       ran a cycle over 20 ms late in its window, which earns a seek of its own (as S15.538).
// COVERAGE-ASSERT: the new pass's first picture found by its presentation time with a picture on each
//       side, every commit's picture known.
// CONTROL: control_batch.sh 2026-10-13, reverted: `serial != s.requestSerial ||` deleted from AudioTrack::clock(),
//       renderer/audio/audio_output.cpp -> "FAIL: slow loop rule: the sound is moved once across the loop, to hand it
//       over" (239 seeks) beside the early and beep lines. In advanceSessionLocked, backend/daemon/wallpaper_manager.cpp:
//       a pass's first picture's wait cut 300 ms -> "FAIL: slow loop rule: no picture across the loop comes early"
//       (-299.86 ms); a pass's first picture held 150 ms past its due time before its draw -> "FAIL: slow loop rule:
//       no picture across the loop comes late" (+149.96 ms; green within 0.1 ms).
// GOAL: S15.565 -- a hand-over made in the new pass's own wait (the first loop, before any pass is learned)
//       is asked for kHandOverLead (100 ms) before that pass, not at once: the seek to the pass's start
//       comes no more than 200 ms before the line's time for its first picture. False if the play is made
//       as soon as that picture is decoded, two seconds ahead, where it can overwrite a play the sound
//       output has not taken up yet.
// WHY-THIS-SHAPE: the overwrite itself is a race no hook here can stage, so what is graded is when the
//       request is made, which is what the daemon decides. The output starts a far-off request about
//       341 ms early, so an ungated play shows as a seek that far ahead.
// CONTROL: control_run.sh 2026-10-12, reverted: the `if (planned.timeline - now > kHandOverLead) return {.roundAt
//       ...};` line in steerSoundLocked's startsPass branch deleted, backend/daemon/wallpaper_manager.cpp -> "FAIL:
//       slow loop rule: the first loop's hand-over waits for a round kHandOverLead ahead" (340.6 ms; 100.0 green).
void runSlowLoopWakeChecks() {
    const char* kGoals = "S15.558, S15.565 (the clock is the latest play's)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    Daemon d(smallConfig());
    const std::string file = asset("wallpaper_beeps_slow.mp4");
    if (!startHeard(d, file, kGoals)) return;
    if (!d.waitForCommits(5, 15s)) {
        notVerified(kGoals, "the slow video did not reach its last picture within 15 s");
        return;
    }
    const auto deadline = Clock::now() + 6s;
    while (d.commitCount() < 7 && Clock::now() < deadline) {
        d.worker.wake();
        std::this_thread::sleep_for(30ms);
    }
    std::this_thread::sleep_for(300ms);
    const std::vector<Commit> commits = d.takeCommits();
    const Capture cap = takeCapture();
    const std::vector<Shown> shown = shownPictures(commits, file);
    size_t start = 0;
    for (size_t i = 1; i + 1 < shown.size(); ++i) {
        if (shown[i].presentation && shown[i - 1].presentation && *shown[i].presentation < *shown[i - 1].presentation) {
            start = i;
            break;
        }
    }
    if (start == 0) {
        notVerified(kGoals, fmt("no new pass with a picture on each side was found among %.0f commits",
                                static_cast<double>(commits.size())));
        return;
    }
    std::string why;
    const std::optional<FilePlace> places = filePlaces(shown, file, why);
    if (!places) {
        notVerified(kGoals, why);
        return;
    }
    std::vector<std::pair<int64_t, int64_t>> points;
    for (size_t i = 0; i < shown.size(); ++i) points.push_back({places->at[i], commits[i].at});
    const Line line = fitLine(points);
    std::printf("audio_playback_test: slow loop rule: %zu commits, the line through them at slope %.5f, at most %.1f ms "
                "from it\n",
                commits.size(), line.slope, line.worst / 1e6);
    for (size_t i = start; i <= start + 1; ++i) {
        const double off = (commits[i].at - line.fitted[i]) / 1e6;
        std::printf("audio_playback_test: slow loop rule: commit %zu came %+.2f ms from its time on the line\n", i, off);
        check(commits[i].at >= line.fitted[i] - 20 * kMs, "slow loop rule: no picture across the loop comes early",
              fmt("commit %.0f came %+.2f ms from its time on the line", static_cast<double>(i), off));
        check(commits[i].at <= line.fitted[i] + 20 * kMs, "slow loop rule: no picture across the loop comes late",
              fmt("commit %.0f came %+.2f ms from its time on the line", static_cast<double>(i), off));
    }
    const int graded = gradeBeepTimes("slow loop rule, the new pass's first beep", cap,
                                      {{"pass 1 beep 0", line.fitted[start]}}, -5.0, 5.0);
    check(graded == 1, "slow loop rule: the new pass's first beep was graded");
    const int64_t seeksFrom = line.fitted[start] - 1500 * kMs, seeksTo = line.fitted[start + 1];
    const size_t acrossLoop = soundSeeksSince(seeksFrom, seeksTo).size();
    const int64_t serverGap = longestBufferGap(cap, seeksFrom, seeksTo);
    std::printf("audio_playback_test: slow loop rule: %zu sound seeks from 1.5 s before the new pass to its second picture; "
                "the server at most %.1f ms between cycles\n",
                acrossLoop, serverGap / 1e6);
    if (serverGap > kServerLateBound) {
        notVerified("S15.558 (the sound is moved once across the loop)",
                    fmt("the sound server went %.1f ms between cycles, which earns a seek of its own", serverGap / 1e6));
    } else {
        check(acrossLoop == 1, "slow loop rule: the sound is moved once across the loop, to hand it over",
              fmt("%.0f sound seeks", static_cast<double>(acrossLoop)));
    }
    std::optional<int64_t> handedAt;
    for (const probe::SoundSeek& s : soundSeeksSince(line.fitted[start] - 1500 * kMs, line.fitted[start])) {
        if (s.targetNs < 50 * kMs) handedAt = s.at;
    }
    const double lead = handedAt ? (line.fitted[start] - *handedAt) / 1e6 : -1.0;
    std::printf("audio_playback_test: slow loop rule: the hand-over's seek came %.1f ms before the new pass\n", lead);
    check(handedAt && lead <= 200.0, "slow loop rule: the first loop's hand-over waits for a round kHandOverLead ahead",
          handedAt ? fmt("the seek came %.1f ms before the new pass", lead) : "no seek to the pass's start was seen");
}

// GOAL: S15.559 -- catching one screen up to its sound leaves the other screens on time: after an 80 ms
//       stall of a heard 120-a-second screen whose picture reads cost 3 ms, a second, unheard screen
//       playing at the 120-a-second cap draws with no gap over 28 ms, the stall's end counted as
//       an edge. False if the heard screen drops the pictures the sound has passed for as long as that
//       takes in one round, instead of within the round's 4 ms catch-up slice.
// WHY-THIS-SHAPE: 3 ms reads make each drop cost about a slice, so dropping the 9 or so pictures an 80 ms
//       stall leaves behind in one round takes about 40 ms. The other screen at the 120-a-second cap is due
//       every 8.3 ms, so one long round shows as one long gap; at 30 a second it would mostly hide inside
//       the interval. 28 ms sits between a correct run's worst gap (about 16 ms) and the about 40 ms the
//       slice's absence adds. A run whose other screen already went over 20 ms between pictures in the
//       250 ms before the stall, the slow reads on, is not verified: the machine was too busy to tell.
// COVERAGE-ASSERT: the heard screen drew after the stall and had five or more pictures to drop; at least
//       twenty of the other screen's gaps graded.
// CONTROL: control_batch.sh 2026-10-13, reverted, Lavapipe: the slice test in the drop loop made `if (false &&
//       dropped && …`, backend/daemon/wallpaper_manager.cpp -> "FAIL: drop slice rule: the other screen's
//       pictures stay on time while the heard one catches up" (40.0 ms; 15.3 green, 15.5 on the RTX), and the
//       held-pictures line below (2 of 55 late). Test-side, by hand: the render thread held 40 ms by one blit in
//       the window before the stall -> "NOT VERIFIED -- S15.559 ..." (the other screen 48.4 ms apart before it).
void runDropSliceChecks() {
    const char* kGoals = "S15.559 (catching one screen up leaves the others on time)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    StallReset reset;
    Daemon d(smallConfig());
    const std::string heard = asset("wallpaper_tone_rate120.mp4"), other = asset("wallpaper_tone_rate240.mp4");
    if (!startHeard(d, heard, kGoals)) return;
    d.playOther(other);
    std::this_thread::sleep_for(1000ms);
    {
        std::lock_guard<std::mutex> lock(probe::g_stallFileMutex);
        probe::g_slowFile = std::filesystem::path(heard).filename().string();
        probe::g_stallFile = probe::g_slowFile;
    }
    probe::g_slowPictureMs.store(3);
    probe::g_slowPictureReads.store(1'000'000);
    std::this_thread::sleep_for(300ms);
    const int64_t baselineTo = probe::nowNs();
    probe::g_fileStallStartedAt.store(0);
    probe::g_fileStallEndedAt.store(0);
    probe::g_stallFileMs.store(80);
    std::this_thread::sleep_for(1000ms);
    const int64_t seekEnd = probe::nowNs();
    probe::g_slowPictureReads.store(0);
    const int64_t stallStart = probe::g_fileStallStartedAt.load(), stallEnd = probe::g_fileStallEndedAt.load();
    if (stallEnd == 0) {
        check(false, "drop slice rule: the staged stall of the heard file's pictures was reached");
        return;
    }
    const size_t seeks = soundSeeksSince(stallStart, seekEnd).size();
    const int64_t serverGap = longestBufferGap(takeCapture(), stallStart, seekEnd);
    const std::vector<Commit> all = d.takeCommits(true);
    std::vector<Commit> mine, others;
    for (const Commit& c : all) (c.monitor == 0 ? mine : others).push_back(c);
    const int64_t to = stallEnd + 500 * kMs;
    const std::vector<Shown> shown = shownPictures(mine, heard);
    const int64_t interval = pictureInterval(shown);
    int dropped = 0, heardAfter = 0;
    for (size_t i = 1; i < shown.size(); ++i) {
        if (shown[i].commit.at < stallEnd || shown[i].commit.at > to) continue;
        ++heardAfter;
        if (shown[i].presentation && shown[i - 1].presentation) {
            dropped += skippedBetween(*shown[i - 1].presentation, *shown[i].presentation, interval);
        }
    }
    int64_t worst = 0, previous = stallEnd;
    int gaps = 0;
    for (const Commit& c : others) {
        if (c.at <= stallEnd || c.at > to) continue;
        worst = std::max(worst, c.at - previous);
        previous = c.at;
        ++gaps;
    }
    worst = std::max(worst, to - previous);
    // The other screen's own spacing just before the stall, the slow reads already on: how late this
    // machine is drawing it with no catch-up running.
    int64_t baselineWorst = 0;
    std::optional<int64_t> before;
    for (const Commit& c : others) {
        if (c.at < baselineTo - 250 * kMs || c.at > std::min(baselineTo, stallStart)) continue;
        if (before) baselineWorst = std::max(baselineWorst, c.at - *before);
        before = c.at;
    }
    std::printf("audio_playback_test: drop slice rule: after the stall the heard screen drew %d pictures and skipped %d "
                "(%.2f ms a picture); the other screen drew %d, at most %.1f ms apart (%.1f ms before the stall); %zu sound "
                "seeks, the server at most %.1f ms between cycles\n",
                heardAfter, dropped, interval / 1e6, gaps, worst / 1e6, baselineWorst / 1e6, seeks, serverGap / 1e6);
    check(heardAfter >= 1, "drop slice rule: the heard screen drew again after its stall");
    if (heardAfter == 0) return;
    if (interval == 0) {
        notVerified(kGoals, "no two of the heard screen's commits in a row showed later and later pictures");
        return;
    }
    // GOAL: S15.557 on a heavy file -- with every picture read of the heard file at 3 ms, the pictures an
    //       80 ms stall left behind the sound are dropped, not drawn: at least five skipped among the commits
    //       in the 500 ms after it, and at most one of those commits more than two intervals past the line
    //       the commits before the stall set. False if a round whose slice is spent draws the late picture
    //       instead of dropping at least one and holding the rest.
    // WHY-THIS-SHAPE: the skips are read from presentation times (shownPictures), at the file's interval;
    //       a 3 ms read alone nearly spends the round's 4 ms slice, so a drop loop bounded only by the slice
    //       would rarely run. Holding is graded as lateness against the pre-stall line, not commit spacing,
    //       which at 120 a second cannot tell a held picture from a drawn one. A correct draw comes when its
    //       picture is under an interval overdue, plus the draw; two intervals (16.7 ms) leave that a margin.
    // CONTROL: control_batch.sh 2026-10-13, reverted, backend/daemon/wallpaper_manager.cpp: the hold's `return
    //       now;` in advanceSessionLocked's drop loop made `break;` (a spent slice draws the held picture) -> "FAIL:
    //       drop slice rule: a heavy file's held pictures are not drawn late" (7 of 59 over two intervals, the
    //       latest +70.5 ms; green at most +6.8 ms on Lavapipe, +13.4 on the RTX); steerSoundLocked's move rule made
    //       `(overdue - behind > kSoundDrift)`, without `!chased &&` (the sound moved to the pictures instead) ->
    //       "FAIL: drop slice rule: a heavy file's pictures the stall left behind the sound are dropped" (0 skipped;
    //       green 16-17).
    check(dropped >= 5, "drop slice rule: a heavy file's pictures the stall left behind the sound are dropped",
          fmt("%.0f skipped", dropped));
    std::string why;
    if (const std::optional<Lateness> late = lateAfter(shown, stallEnd, to, 2 * interval, why); !late) {
        notVerified("S15.557 (a heavy file's held pictures are not drawn late)", why);
    } else {
        std::printf("audio_playback_test: drop slice rule: %d of %d heard commits after the stall over two intervals late, "
                    "the latest %+.1f ms against the line before it, the next %+.1f ms\n",
                    late->late, late->graded, late->worst / 1e6, late->second / 1e6);
        check(late->late <= 1, "drop slice rule: a heavy file's held pictures are not drawn late",
              fmt("%.0f of %.0f commits over two intervals late", late->late, late->graded));
    }
    if (dropped < 5) {
        notVerified(kGoals, fmt("the heard screen skipped only %.0f pictures, so there was no catch-up to share", dropped));
    } else if (baselineWorst > 20 * kMs) {
        notVerified(kGoals, fmt("the other screen already went %.1f ms between pictures before the stall, too busy a "
                                "machine to grade a 28 ms bound",
                                baselineWorst / 1e6));
    } else {
        check(gaps >= 20, "drop slice rule: the other screen's pictures after the stall were graded", fmt("%.0f gaps", gaps));
        check(worst <= 28 * kMs, "drop slice rule: the other screen's pictures stay on time while the heard one catches up",
              fmt("at most %.1f ms between two of its pictures", worst / 1e6));
    }
    // GOAL: S15.557 -- catching a heavy file up inside kCatchUpHold does not move its sound.
    // WHY-THIS-SHAPE: seeks and the server's gaps over one span, from the stall to the reading; not graded
    //       when the server itself went a cycle plus 20 ms between buffers, which earns a seek (as S15.538).
    // CONTROL: control_batch.sh 2026-10-13, reverted: steerSoundLocked's `(!chased && overdue - behind >
    //       kSoundDrift)` made `(overdue - behind > kSoundDrift)`, backend/daemon/wallpaper_manager.cpp ->
    //       "FAIL: drop slice rule: a heavy file's catch-up does not move the sound" (1 seek).
    if (serverGap > kServerLateBound) {
        notVerified("S15.557 (a heavy file's catch-up does not move the sound)",
                    fmt("the sound server went %.1f ms between cycles, so moving the sound would be correct", serverGap / 1e6));
    } else {
        check(seeks == 0, "drop slice rule: a heavy file's catch-up does not move the sound",
              fmt("%.0f sound seeks", static_cast<double>(seeks)));
    }
}

// GOAL: S15.561 -- a heard file that cannot decode in real time and has no picture to skip plays its
//       pictures as fast as it decodes them once the daemon has seen it cannot keep up: from the eighth
//       picture decoded after the slowdown, none of its decoded pictures is dropped, no commit waits on more
//       than two decoder hand-offs, and the sound is not moved mid-pass -- the one seek in the window hands
//       it to the next pass at the loop. False if such a file is still steered by its sound (it drops
//       pictures it can never reach, so each update costs several decodes, and the sound is moved), or its
//       sound is never put back on the pictures at a loop.
// WHY-THIS-SHAPE: the slowdown is 83 ms (2.5 intervals) spent in avcodec_receive_frame on each picture the
//       decoder returns: a cost per decoded picture, as heavy decoding is, where slowed packet reads made the
//       cost of a picture depend on how many sound packets sat beside it. Baseline H.264 has no unreferenced
//       picture, so skipping cannot help. The daemon's averaged load reaches 1 within five such decodes; the
//       grading starts at the eighth, so the stutter before that (pictures dropped while the file still
//       counted as keeping up) is accepted. "One decode an update" is graded as hand-offs counted between two
//       commits, never as a wall-clock gap, which would grade the scheduler; two allows for one picture
//       handed on just as a round starts. No sound-server guard: a file that cannot keep up is never steered
//       mid-pass, so a late server earns it no seek. The two-second file loops inside the window.
// COVERAGE-ASSERT: at least ten pictures decoded and ten committed in the window, a decode costing more
//       than its interval (else not verified), and a loop crossed.
// CONTROL: control_batch.sh 2026-10-13, reverted, steerSoundLocked, backend/daemon/wallpaper_manager.cpp: its return
//       for a file in State::Behind deleted -> "FAIL: catch-up cap rule: a file that cannot keep up draws every
//       picture it decodes" (56 skipped), "... each update ... costs one decode" (4 hand-offs before one commit) and
//       "... its sound is moved only at the loop" (17 seeks); the same return tested on State::Skips -> the same three;
//       a copy of that return placed before the pass-start branch -> "... its sound is moved only at the loop" (0
//       seeks). Green on both entries: 0 skipped, at most 1 hand-off, one seek at the loop.
void runCatchUpCapChecks() {
    const char* kGoals = "S15.561 (a file that cannot keep up, with nothing to skip, plays unpaced)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    StallReset reset;
    Daemon d(smallConfig());
    const std::string heard = asset("wallpaper_beeps.mp4");
    if (!startHeard(d, heard, kGoals)) return;
    {
        std::lock_guard<std::mutex> lock(probe::g_costMutex);
        probe::g_costFile = std::filesystem::path(heard).filename().string();
    }
    const int64_t costFrom = probe::nowNs();
    probe::g_costMs.store(83);
    std::this_thread::sleep_for(8000ms);
    const int64_t to = probe::nowNs();
    probe::g_costMs.store(0);
    std::vector<int64_t> decodedAt;
    for (const probe::PictureRecord& p : picturesOf(heard)) {
        if (p.at > costFrom && p.at <= to) decodedAt.push_back(p.at);
    }
    if (decodedAt.size() < 18) {
        check(false, "catch-up cap rule: the slowed file went on decoding pictures",
              fmt("%.0f pictures decoded in 8 s of the slowdown", static_cast<double>(decodedAt.size())));
        return;
    }
    const int64_t from = decodedAt[7];
    const int64_t perPicture = (decodedAt.back() - from) / static_cast<int64_t>(decodedAt.size() - 8);
    const int64_t interval = fileInterval(heard);
    const std::vector<Shown> shown = shownPictures(d.takeCommits(), heard);
    int inWindow = 0, dropped = 0, worstHandOffs = 0;
    std::optional<size_t> loopAt;
    int64_t previous = from;
    size_t next = 0;
    const auto handOffsUntil = [&](int64_t until) {
        int n = 0;
        while (next < decodedAt.size() && decodedAt[next] <= until) {
            if (decodedAt[next] > previous) ++n;
            ++next;
        }
        return n;
    };
    for (size_t i = 0; i < shown.size(); ++i) {
        const Shown& s = shown[i];
        if (s.commit.at <= from || s.commit.at > to) continue;
        ++inWindow;
        worstHandOffs = std::max(worstHandOffs, handOffsUntil(s.commit.at));
        previous = s.commit.at;
        if (i == 0 || !s.presentation || !shown[i - 1].presentation) continue;
        if (*s.presentation < *shown[i - 1].presentation) {
            if (!loopAt) loopAt = i;
        } else {
            dropped += skippedBetween(*shown[i - 1].presentation, *s.presentation, interval);
        }
    }
    worstHandOffs = std::max(worstHandOffs, handOffsUntil(to));
    const std::vector<probe::SoundSeek> seeks = soundSeeksSince(from, to);
    std::printf("audio_playback_test: catch-up cap rule: graded from the eighth picture decoded, %.0f ms after the slowdown "
                "began: %zu pictures decoded at %.1f ms each (interval %.1f ms); %d commits, %d pictures skipped, at most "
                "%d decoder hand-offs before a commit; %zu sound seeks\n",
                (from - costFrom) / 1e6, decodedAt.size() - 7, perPicture / 1e6, interval / 1e6, inWindow, dropped,
                worstHandOffs, seeks.size());
    for (const probe::SoundSeek& s : seeks) {
        std::printf("  a seek to %.1f ms of the file, %+.1f ms from the new pass's first commit\n", s.targetNs / 1e6,
                    loopAt ? (s.at - shown[*loopAt].commit.at) / 1e6 : 0.0);
    }
    check(inWindow >= 10 && decodedAt.size() >= 18 && loopAt.has_value(),
          "catch-up cap rule: the slowed file decoded and drew ten pictures, and crossed a loop, in the graded window",
          fmt("%.0f commits, a loop %.0f", inWindow, loopAt ? 1.0 : 0.0));
    if (inWindow < 10 || !loopAt) return;
    if (interval == 0 || perPicture <= interval) {
        notVerified(kGoals, fmt("a picture cost %.1f ms to decode, inside its %.1f ms interval, so there was nothing to "
                                "fall behind",
                                perPicture / 1e6, interval / 1e6));
        return;
    }
    check(dropped == 0, "catch-up cap rule: a file that cannot keep up draws every picture it decodes",
          fmt("%.0f pictures skipped", dropped));
    check(worstHandOffs <= 2, "catch-up cap rule: each update of a file that cannot keep up costs one decode",
          fmt("%.0f pictures handed on before one commit", worstHandOffs));
    const bool atLoop = seeks.size() == 1 && seeks[0].targetNs < 50 * kMs &&
                        std::llabs(seeks[0].at - shown[*loopAt].commit.at) <= 500 * kMs;
    check(atLoop, "catch-up cap rule: its sound is moved only at the loop, onto the new pass",
          fmt("%.0f sound seeks in the window", static_cast<double>(seeks.size())));
}

// GOAL: S15.560 -- a pass's first picture is never dropped: held up past its time while the sound already
//       plays the new pass, it is still drawn. False if the pictures the sound has passed are dropped
//       across a loop, so the new pass opens on a later picture.
// WHY-THIS-SHAPE: only another screen can hold the first picture up after it is decoded, so a second,
//       unheard screen's next read is stalled 600 ms, armed from the heard screen's last-picture commit.
//       A commit is stamped when its round ends, after every screen in it, so the heard screen drawing
//       on time in the very round whose later screen stalls looks held up; that round is told by a blit
//       (the one draw call) recorded after the last commit announced before the stall and before the
//       stall began. Such an attempt, or one drawn before the stall, proves nothing and is tried again, at
//       most three times, then not verified. No heard commit at all after the stall is a failure: the
//       screen stopped.
// COVERAGE-ASSERT: the graded picture was drawn in a round after the stall's own.
// CONTROL: control_batch.sh 2026-10-13, reverted, Lavapipe, backend/daemon/wallpaper_manager.cpp: `held.startsPass ||`
//       deleted from the drop loop -> "FAIL: first picture rule: a pass's first picture held up past its time is
//       drawn, not dropped" (0.567 s); advanceSessionLocked returning 10 s ahead instead of drawing a waiting pass's
//       first picture -> "FAIL: first picture rule: the heard screen drew again after the other screen's stall".
void runLoopFirstPictureChecks() {
    const char* kGoals = "S15.560 (a pass's first picture is never dropped)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    StallReset reset;
    const std::string heard = asset("wallpaper_beeps.mp4"), other = asset("wallpaper_tone.mp4");
    std::atomic<bool> arm{false};
    std::atomic<int64_t> armedAt{0};
    Daemon d(smallConfig());
    d.onCommit = [&](const Commit& c) {
        if (c.monitor != 0 || !arm.load()) return;
        const std::optional<int64_t> p = lastPictureOf(heard);
        // The file's last picture, at 59/30 s.
        if (!p || *p < 1950 * kMs) return;
        arm.store(false);
        armedAt.store(c.at);
        {
            std::lock_guard<std::mutex> lock(probe::g_stallFileMutex);
            probe::g_stallFile = std::filesystem::path(other).filename().string();
        }
        probe::g_stallFileMs.store(600);
    };
    if (!startHeard(d, heard, kGoals)) return;
    d.playOther(other);
    std::this_thread::sleep_for(1000ms);
    bool reached = false;
    for (int attempt = 1; attempt <= 3 && !reached; ++attempt) {
        probe::g_fileStallStartedAt.store(0);
        probe::g_fileStallEndedAt.store(0);
        armedAt.store(0);
        arm.store(true);
        if (!waitFor(5000ms, [] { return probe::g_fileStallEndedAt.load() != 0; })) {
            arm.store(false);
            probe::g_stallFileMs.store(0);
            check(false, "first picture rule: the other screen's staged stall was reached within 5 s");
            return;
        }
        const int64_t started = probe::g_fileStallStartedAt.load(), ended = probe::g_fileStallEndedAt.load();
        const auto drewAfterArm = [&] {
            for (const Commit& c : d.takeCommits()) {
                if (c.at > armedAt.load()) return true;
            }
            return false;
        };
        if (!waitFor(3000ms, drewAfterArm)) {
            check(false, "first picture rule: the heard screen drew again after the other screen's stall");
            return;
        }
        const std::vector<Shown> shown = shownPictures(d.takeCommits(), heard);
        const Shown* firstAfter = nullptr;
        for (const Shown& s : shown) {
            if (s.commit.at > armedAt.load()) {
                firstAfter = &s;
                break;
            }
        }
        if (firstAfter->commit.at < started) {
            std::printf("audio_playback_test: first picture rule: attempt %d: the heard screen drew its next picture before "
                        "the stall began; trying again\n",
                        attempt);
            continue;
        }
        int64_t announced = 0;
        for (const Commit& c : d.takeCommits(true)) {
            if (c.at < started) announced = std::max(announced, c.at);
        }
        bool drawnInStallRound = false;
        {
            std::lock_guard<std::mutex> lock(probe::g_blitMutex);
            for (const int64_t b : probe::g_blits) drawnInStallRound = drawnInStallRound || (b > announced && b < started);
        }
        if (drawnInStallRound) {
            std::printf("audio_playback_test: first picture rule: attempt %d: the heard screen drew in the stall's own round, "
                        "before the stall; trying again\n",
                        attempt);
            continue;
        }
        reached = true;
        const double p = firstAfter->presentation ? *firstAfter->presentation / 1e9 : -1.0;
        std::printf("audio_playback_test: first picture rule: attempt %d: after a %.0f ms stall the heard screen drew the "
                    "picture at %.3f s, %.1f ms after the stall ended\n",
                    attempt, (ended - started) / 1e6, p, (firstAfter->commit.at - ended) / 1e6);
        check(firstAfter->presentation && *firstAfter->presentation < 2 * kMs,
              "first picture rule: a pass's first picture held up past its time is drawn, not dropped",
              fmt("the first picture drawn after the stall is at %.3f s of the file", p));
    }
    if (!reached) notVerified(kGoals, "in three loops the stall never held the new pass's first picture up");
}

// One stall of `ms` on `file`'s next picture read, then 1.5 s: when it began and ended (0 if never
// reached), and the sound seeks and the server's longest gap from the moment it was armed to the end.
struct StallRun {
    int64_t armed = 0, started = 0, ended = 0, end = 0, serverGap = 0;
    std::vector<probe::SoundSeek> seeks;
};
StallRun stallOnce(const std::string& file, int ms) {
    StallRun r;
    {
        std::lock_guard<std::mutex> lock(probe::g_stallFileMutex);
        probe::g_stallFile = std::filesystem::path(file).filename().string();
    }
    probe::g_fileStallStartedAt.store(0);
    probe::g_fileStallEndedAt.store(0);
    r.armed = probe::nowNs();
    probe::g_stallFileMs.store(ms);
    std::this_thread::sleep_for(1500ms);
    r.end = probe::nowNs();
    probe::g_stallFileMs.store(0);
    r.started = probe::g_fileStallStartedAt.load();
    r.ended = probe::g_fileStallEndedAt.load();
    r.seeks = soundSeeksSince(r.armed, r.end);
    r.serverGap = longestBufferGap(takeCapture(), r.armed, r.end);
    return r;
}

// GOAL: S15.562 -- a heard sound ahead of its pictures is left alone while dropping can close the lead, and
//       moved back to them once when it cannot: on a file that decodes quickly, one picture read stalled
//       160 ms and later one stalled 600 ms each earn no sound seek, and the sound plays on through them;
//       with every picture read slowed 10 ms (a decode then takes most of an interval, so each drop gains
//       little), a 600 ms stall earns exactly one seek, no sooner than kCatchUpHold (100 ms) after the stall
//       ends. False if a fast file's stall moves the sound (moved for a lead before any hold), if the slow
//       one's never does (the pictures chase a sound however far ahead), or one stall moves it twice.
// WHY-THIS-SHAPE: each stall is on the heard file's own read (g_stallFile), which says when it slept, and its
//       seeks are counted until the next, under S15.538's sound-server guard. 10 ms reads keep the file
//       under the daemon's "cannot keep up" mark (14 ms crosses it on Lavapipe); a run that dropped no
//       picture after the slowed stall was taken for a file that cannot keep up, which is never steered, so
//       it is not verified rather than graded on its zero seeks. Where a seek lands is printed, not graded.
// COVERAGE-ASSERT: each stall consumed by a picture read; the slowed run dropped pictures to chase.
// GOAL: S15.569 -- one long stall does not make a file that decodes quickly count as one that cannot keep
//       up: after the fast file's 600 ms stall, at most one commit comes before the first picture skipped.
//       False if the stall's decode time, counted whole, tips the averaged load, so the pictures after it
//       are drawn late one by one instead of dropped.
// WHY-THIS-SHAPE: a file taken for one that cannot keep up draws each picture as it is decoded; one being
//       caught up drops before it draws. The first skip in the presentation times shown is where catching
//       up began, so the commits before it are the pictures drawn unpaced.
// CONTROL: control_batch.sh 2026-10-13, reverted, backend/daemon/wallpaper_manager.cpp: steerSoundLocked's `chased`
//       made `false` (moved for a lead before any hold) -> "FAIL: lead limit rule, a 160 ms stall on a file that
//       decodes quickly: the sound is not moved" (1 seek), the 600 ms line
//       (1 seek) and S15.569's line (27 drawn unpaced, the sound moved to them); the drop loop's
//       steerSoundLocked(..., CatchUp::HoldRunOut) call deleted -> "FAIL: lead limit rule, a stall the hold cannot
//       close: the sound is moved back once, after the hold" (0 seeks, 25 dropped); DecodeLoad::Average::add
//       without the cap of 2 on a sample -> "FAIL: lead limit rule: one long stall does not make a quick file count
//       as one that cannot keep up" (7 drawn unpaced); the hold's run-out at kCatchUpHold / 4 -> "... a stall the hold
//       cannot close: ..., after the hold" (the seek 70.5 ms after the stall). Test-side, 2026-10-13 (not re-run since): the sound
//       decoder stalled 1 s in the 160 ms run -> "... a 160 ms stall ...: the sound plays on through it" (41984
//       frames silent). Green on both entries: no seek for either fast
//       stall, 0 drawn unpaced, one seek 141 ms after the slowed stall ended.
void runLeadLimitChecks() {
    const char* kGoals = "S15.562, S15.569 (a sound ahead is moved only once a hold runs out)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    StallReset reset;
    Daemon d(smallConfig());
    const std::string file = asset("wallpaper_beeps_long.mp4");
    if (!startHeard(d, file, kGoals)) return;
    std::this_thread::sleep_for(1500ms);
    const int64_t interval = fileInterval(file);
    const auto printSeeks = [&](const StallRun& r) {
        const std::vector<Shown> shown = shownPictures(d.takeCommits(), file);
        for (const probe::SoundSeek& s : r.seeks) {
            std::optional<int64_t> onScreen;
            for (const Shown& c : shown) {
                if (c.commit.at <= s.at) onScreen = c.presentation;
            }
            std::printf("  a seek %.1f ms after the stall ended, to %.1f ms of the file, with the picture at %.1f ms on "
                        "screen\n",
                        (s.at - r.ended) / 1e6, s.targetNs / 1e6, onScreen ? *onScreen / 1e6 : -1.0);
        }
    };
    for (const int ms : {160, 600}) {
        const StallRun r = stallOnce(file, ms);
        std::printf("audio_playback_test: lead limit rule: a %d ms stall on a file that decodes quickly: %zu sound seeks; the "
                    "server at most %.1f ms between cycles\n",
                    ms, r.seeks.size(), r.serverGap / 1e6);
        printSeeks(r);
        if (r.ended == 0) {
            check(false, "lead limit rule: the staged picture stall was reached", fmt("the %.0f ms stall", ms));
            return;
        }
        const auto [silent, heardFrames] = longestSilence(takeCapture(), r.started, r.started + 1400 * kMs, true);
        check(heardFrames >= 48000ULL * 1400 / 1000 * 9 / 10 && silent <= 48,
              fmt("lead limit rule, a %.0f ms stall on a file that decodes quickly: the sound plays on through it", ms),
              fmt("%.0f frames heard, %.0f silent in a row", static_cast<double>(heardFrames), static_cast<double>(silent)));
        if (r.serverGap > kServerLateBound) {
            notVerified(kGoals, fmt("the sound server went %.1f ms between cycles, which earns a seek of its own", r.serverGap / 1e6));
        } else {
            check(r.seeks.empty(), fmt("lead limit rule, a %.0f ms stall on a file that decodes quickly: the sound is not moved", ms),
                  fmt("%.0f sound seeks", static_cast<double>(r.seeks.size())));
        }
        if (ms != 600) continue;
        int unpaced = 0;
        bool skipped = false;
        std::optional<int64_t> before;
        for (const Shown& s : shownPictures(d.takeCommits(), file)) {
            if (!s.presentation) continue;
            if (s.commit.at > r.ended && !skipped) {
                if (before && skippedBetween(*before, *s.presentation, interval) > 0) {
                    skipped = true;
                } else {
                    ++unpaced;
                }
            }
            before = s.presentation;
        }
        std::printf("audio_playback_test: lead limit rule: %d commits after the 600 ms stall before the first skip\n", unpaced);
        check(skipped && unpaced <= 1, "lead limit rule: one long stall does not make a quick file count as one that cannot keep up",
              fmt("%.0f pictures drawn unpaced before the first skip", unpaced));
    }
    {
        std::lock_guard<std::mutex> lock(probe::g_stallFileMutex);
        probe::g_slowFile = std::filesystem::path(file).filename().string();
    }
    probe::g_slowPictureMs.store(10);
    probe::g_slowPictureReads.store(1'000'000);
    std::this_thread::sleep_for(1500ms);
    const StallRun r = stallOnce(file, 600);
    probe::g_slowPictureReads.store(0);
    int dropped = 0;
    std::optional<int64_t> before;
    for (const Shown& s : shownPictures(d.takeCommits(), file)) {
        if (!s.presentation) continue;
        if (before && s.commit.at > r.ended && s.commit.at <= r.end) dropped += skippedBetween(*before, *s.presentation, interval);
        before = s.presentation;
    }
    std::printf("audio_playback_test: lead limit rule: a 600 ms stall with 10 ms reads: %zu sound seeks, %d pictures dropped "
                "after it; the server at most %.1f ms between cycles\n",
                r.seeks.size(), dropped, r.serverGap / 1e6);
    printSeeks(r);
    if (r.ended == 0) {
        check(false, "lead limit rule: the staged picture stall was reached", "the 600 ms stall with 10 ms reads");
        return;
    }
    if (dropped == 0) {
        notVerified(kGoals, "no picture was dropped after the slowed stall: the file was taken for one that cannot keep up, "
                            "which is never steered");
    } else if (r.serverGap > kServerLateBound) {
        notVerified(kGoals, fmt("the sound server went %.1f ms between cycles, which earns a seek of its own", r.serverGap / 1e6));
    } else {
        const bool afterHold = r.seeks.size() == 1 && r.seeks[0].at - r.ended >= 100 * kMs;
        check(afterHold, "lead limit rule, a stall the hold cannot close: the sound is moved back once, after the hold",
              fmt("%.0f sound seeks, the first %.1f ms after the stall ended", static_cast<double>(r.seeks.size()),
                  r.seeks.empty() ? 0.0 : (r.seeks[0].at - r.ended) / 1e6));
    }
}

// GOAL: S15.563 -- with no clock from the sound server (it never times the stream), a heard sound left more than
//       kSoundDrift off its pictures is still moved back to them: after a 600 ms picture stall, exactly one
//       sound seek. False if a sound with no clock is never steered, and stays 600 ms ahead of its pictures
//       for good. The 20 ms threshold itself, and lateness inside an interval: noClockLead.
// WHY-THIS-SHAPE: pw_stream_get_time_n is interposed to report `now` as 0, which is how a server that has
//       not timed a stream answers, so AudioOutput publishes no clock; the record's own heard times then
//       fall back to the queue time, which nothing here grades. With a clock the same stall earns no seek
//       (S15.562: dropping closes it), so the one seek can only come from the no-clock rule. Seeks are
//       counted from the stall, 300 ms after the clock was hidden, so the hiding itself is not counted.
// COVERAGE-ASSERT: the stall was reached, and at least 20 of AudioOutput's own timing reads were hidden while
//       it played out; the recorder's reads, made under t_inRecord, are not counted.
// CONTROL: control_batch.sh 2026-10-13, reverted: steerSoundLocked's `chased` without `clock &&` (the move waits for
//       a hold with no clock too), backend/daemon/wallpaper_manager.cpp -> "FAIL: no clock rule: a sound left off its
//       pictures with no clock is moved back to them once" (0 seeks; green 1); the stored reading and the taken anchor
//       both ignored -> the same line (0). timingOfNextBuffer in renderer/audio/audio_output.cpp answering "not timed"
//       without asking pw_stream_get_time_n -> "FAIL: no clock rule: the stall was reached while the server's timing
//       was hidden" (0 reported); with the t_inRecord test taken out as well it stays green (70 counted, the
//       recorder's), which is the hollowing this count now sees.
void runNoClockChecks() {
    const char* kGoals = "S15.563 (a sound with no clock is kept with its pictures)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    StallReset reset;
    Daemon d(smallConfig());
    if (!startHeard(d, asset("wallpaper_beeps_long.mp4"), kGoals)) return;
    std::this_thread::sleep_for(1500ms);
    probe::g_hideStreamTime.store(true);
    std::this_thread::sleep_for(300ms);
    const int hiddenBefore = probe::g_streamTimesHidden.load();
    const int64_t at = probe::nowNs();
    probe::g_stallPictureMs.store(600);
    std::this_thread::sleep_for(1500ms);
    const int64_t end = probe::nowNs();
    const int hidden = probe::g_streamTimesHidden.load() - hiddenBefore;
    const int left = probe::g_stallPictureMs.exchange(0);
    probe::g_hideStreamTime.store(false);
    const size_t seeks = soundSeeksSince(at, end).size();
    std::printf("audio_playback_test: no clock rule: %zu sound seeks after the stall; %d of AudioOutput's timing reads hidden\n",
                seeks, hidden);
    check(left == 0 && hidden >= 20, "no clock rule: the stall was reached while the server's timing was hidden",
          fmt("%.0f ms of the stall unused, %.0f reports hidden", left, hidden));
    if (left != 0 || hidden < 20) return;
    check(seeks == 1, "no clock rule: a sound left off its pictures with no clock is moved back to them once",
          fmt("%.0f sound seeks", static_cast<double>(seeks)));
}

// GOAL: S15.564 -- from the second loop on, a heard file's sound is asked for the next pass ahead of time:
//       the sound decoder's seek to the pass's start comes before the old pass's last picture is committed.
//       False if every loop is handed over in the new pass's own wait, after that last commit, as the first
//       loop still is.
// WHY-THIS-SHAPE: an order, not a lead in milliseconds: the seek is made on the sound output's thread once it
//       takes the request up, so a fixed lead bound charges that thread's wake-up to the daemon. Asked
//       kHandOverLead (100 ms) before the pass, the seek comes about 67 ms before the old pass's last commit
//       at 30 a second, which the output's take-up would have to eat whole; asked in the pass's own wait it
//       comes after that commit by construction. Weaker than a lead bound against a partial regression: a
//       hand-over asked 40 ms ahead still comes first and passes. The seek is matched to a pass by its target
//       (under 50 ms) and by being the last such seek in the 500 ms before the pass's first commit; the lead
//       is printed. The first loop is printed, not graded: with no pass learned it hands over late by design.
// COVERAGE-ASSERT: two loops after the first graded.
// CONTROL: control_batch.sh 2026-10-13, reverted: `if (!session.pass || shift != …) return {.timesPicture = true};`
//       made `if (true)` in steerSoundLocked, backend/daemon/wallpaper_manager.cpp -> "FAIL: hand-over rule: from the
//       second loop on, the next pass's sound is asked for ahead" (each seek 0.6-0.7 ms after the last commit; 66.7 ms
//       before it green on both entries).
void runHandOverLeadChecks() {
    const char* kGoals = "S15.564 (each later loop's sound is asked for ahead)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    Daemon d(smallConfig());
    const std::string file = asset("wallpaper_tone.mp4");
    if (!startHeard(d, file, kGoals)) return;
    std::this_thread::sleep_for(7500ms);
    const std::vector<Shown> shown = shownPictures(d.takeCommits(), file);
    const std::vector<probe::SoundSeek> seeks = soundSeeksSince(0);
    int loop = 0, graded = 0;
    for (size_t i = 1; i < shown.size(); ++i) {
        if (!shown[i].presentation || !shown[i - 1].presentation) {
            notVerified(kGoals, fmt("which picture commit %.0f showed is not known", static_cast<double>(i)));
            return;
        }
        if (*shown[i].presentation >= *shown[i - 1].presentation) continue;
        ++loop;
        const int64_t at = shown[i].commit.at, lastOld = shown[i - 1].commit.at;
        std::optional<int64_t> seekAt;
        for (const probe::SoundSeek& s : seeks) {
            if (s.targetNs < 50 * kMs && s.at <= at && s.at > at - 500 * kMs) seekAt = s.at;
        }
        const double lead = seekAt ? (at - *seekAt) / 1e6 : -1.0;
        const double beforeLast = seekAt ? (lastOld - *seekAt) / 1e6 : -1.0;
        std::printf("audio_playback_test: hand-over rule: loop %d: the seek to the pass's start came %.1f ms before its "
                    "first commit, %.1f ms before the old pass's last\n",
                    loop, lead, beforeLast);
        if (loop == 1) continue;
        ++graded;
        check(seekAt && *seekAt < lastOld, "hand-over rule: from the second loop on, the next pass's sound is asked for ahead",
              fmt("loop %.0f: the seek %.1f ms before the old pass's last commit", loop, beforeLast));
    }
    check(graded >= 2, "hand-over rule: two loops after the first were graded", fmt("%.0f", graded));
}

// One run of the heard file restored carrying one death, slowed (83 ms spent in each picture returned) or
// not: the deaths on record before and after, the commits, whether its own sound was heard, and whether the
// death was cleared within `waitMs` -- `untilCleared` ends the wait as soon as it is.
struct CleanRun {
    int before = -1, after = -1;
    size_t commits = 0;
    bool loud = false, cleared = false;
};
CleanRun runRestoredWithDeath(const std::string& heard, bool slow, std::chrono::milliseconds waitMs, bool untilCleared) {
    CleanRun out;
    const char* tmp = std::getenv("TMPDIR");
    const std::filesystem::path dir = std::filesystem::path(tmp != nullptr ? tmp : "/tmp") /
                                      ("wallpaperx-clean-credit." + std::to_string(::getpid()) + (slow ? ".slow" : ".fast"));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "state");
    struct Remove {
        std::filesystem::path dir;
        ~Remove() {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
            probe::g_costMs.store(0);
        }
    } remove{dir};
    std::ofstream(dir / "config.json")
        << nlohmann::json{{"version", 1},
                          {"monitors", nlohmann::json::array({{{"monitor_id", 0}, {"type", "video"}, {"path", heard}}})}}
               .dump();
    std::ofstream(dir / "state" / "playing.json")
        << nlohmann::json{{"run", "earlier"},
                          {"videos", nlohmann::json::array({{{"monitor_id", 0}, {"path", heard}, {"deaths", 1}}})}}
               .dump()
        << "\n";
    WallpaperManager::Config config = smallConfig();
    config.configFile = dir / "config.json";
    config.stateDir = dir / "state";
    config.cleanPlayAfter = 1s;
    const auto deaths = [&] {
        const std::optional<wallpaperx::PlayingHistory> h = wallpaperx::readPlayingRecord(config.stateDir);
        if (!h) return -1;
        for (const wallpaperx::PlayingVideo& v : h->videos) {
            if (v.monitorId == 0) return v.deaths;
        }
        return 0;
    };
    {
        std::lock_guard<std::mutex> lock(probe::g_costMutex);
        probe::g_costFile = std::filesystem::path(heard).filename().string();
    }
    if (slow) probe::g_costMs.store(83);
    resetCapture();
    const int64_t started = probe::nowNs();
    Daemon d(config);
    const std::vector<int> restored = d.manager.restoreSaved();
    d.manager.recordStartingVideos();
    out.before = deaths();
    std::vector<int> redraw;
    d.manager.setMonitorOutput(0, "DP-1", redraw);
    d.manager.setOutputFocus(":1.5", "DP-1", ScreenFocus::Desktop, false);
    for (const int id : restored) d.worker.post(id);
    for (const int id : redraw) d.worker.post(id);
    if (untilCleared) {
        out.cleared = waitFor(waitMs, [&] { return deaths() == 0; });
    } else {
        std::this_thread::sleep_for(waitMs);
        out.cleared = deaths() == 0;
    }
    out.after = deaths();
    out.commits = d.takeCommits().size();
    out.loud = probe::g_lastLoudQueuedAt.load() > started;
    return out;
}

// GOAL: S15.566 -- only pictures drawn on time count as clean play: a heard file that cannot decode in real
//       time, restored with one death on record and cleanPlayAfter 1 s, still carries the death after 3 s of
//       play, every picture drawn late; the same file decoding in real time has it cleared within 5 s, so the
//       record and its clearing are live here. False if late draws count as clean, so a file this machine
//       cannot play clears the deaths that would have quarantined it.
// WHY-THIS-SHAPE: the record is written before the start, as a run that died would leave it, and the start
//       goes through restoreSaved() and recordStartingVideos() as the daemon's does. 83 ms spent in each
//       picture returned (2.5 intervals) makes every picture late; the file is soon marked unable to keep up
//       and drawn as decoded. "Was heard" is sound queued after the run's own start: any earlier phase in
//       the same process also made sound, and a bare "ever heard" counted that.
// COVERAGE-ASSERT: each run started carrying the death, drew at least 10 pictures and was heard.
// CONTROL: control_batch.sh 2026-10-13, reverted, advanceSessionLocked, backend/daemon/wallpaper_manager.cpp: `onTime`
//       made `true` -> "FAIL: clean credit rule: a heard file drawn late does not play clean" (0 deaths after 3 s);
//       creditCleanPlayLocked never adding to `played` -> "FAIL: clean credit rule: the same file drawn on time plays
//       clean" (1 death after 5 s). Test-side: the slowed run focused "none" while phase sync ran first ->
//       "FAIL: clean credit rule: the slowed run started carrying a death, drew and was heard" (heard 0); with the
//       old `g_lastLoudQueuedAt > 0` it stays green, sync's sound counted.
void runCleanCreditChecks() {
    const char* kGoals = "S15.566 (only pictures drawn on time count as clean play)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    StallReset reset;
    const std::string heard = asset("wallpaper_beeps_long.mp4");
    const CleanRun slow = runRestoredWithDeath(heard, true, 3000ms, false);
    std::printf("audio_playback_test: clean credit rule: slowed: %d deaths on record before, %d after 3 s; %zu commits; heard "
                "%d\n",
                slow.before, slow.after, slow.commits, slow.loud ? 1 : 0);
    check(slow.before == 1 && slow.commits >= 10 && slow.loud,
          "clean credit rule: the slowed run started carrying a death, drew and was heard",
          fmt("%.0f deaths at the start, %.0f commits, heard %.0f", slow.before, static_cast<double>(slow.commits),
              slow.loud ? 1.0 : 0.0));
    if (slow.before == 1 && slow.commits >= 10 && slow.loud) {
        check(!slow.cleared, "clean credit rule: a heard file drawn late does not play clean",
              fmt("%.0f deaths on record after 3 s", slow.after));
    }
    const CleanRun fast = runRestoredWithDeath(heard, false, 5000ms, true);
    std::printf("audio_playback_test: clean credit rule: real time: %d deaths on record before, %d after; %zu commits; heard "
                "%d\n",
                fast.before, fast.after, fast.commits, fast.loud ? 1 : 0);
    check(fast.before == 1 && fast.commits >= 10 && fast.loud,
          "clean credit rule: the real-time run started carrying a death, drew and was heard",
          fmt("%.0f deaths at the start, %.0f commits, heard %.0f", fast.before, static_cast<double>(fast.commits),
              fast.loud ? 1.0 : 0.0));
    if (fast.before == 1 && fast.commits >= 10 && fast.loud) {
        check(fast.cleared, "clean credit rule: the same file drawn on time plays clean",
              fmt("%.0f deaths still on record after 5 s", fast.after));
    }
}

// GOAL: S15.571 -- with the clock lost, a heard sound is measured from its last reading, not from where the
//       daemon last asked it to be: with the sound card 0.5 % fast for 9 s (the sound then about 45 ms from
//       the place it was asked to start, its pictures following it) and the sound server then stopped for
//       400 ms, no sound seek during the stop. False if the no-clock steering measures the sound from the
//       asked anchor, which the card has left 45 ms behind, and moves it.
// WHY-THIS-SHAPE: the drift is simulated with S15.555's rate correction; nothing re-asks the sound while
//       the pictures follow it, so the asked anchor stays the first play's. A run whose server did not take
//       frames at the corrected rate, or whose sound was moved before the stop, left no drift between the
//       reading and the anchor and is not verified. SIGSTOP stops the clock without touching the engine; the
//       seeks after the stop ends are the return's (S15.556), not graded here.
// COVERAGE-ASSERT: the server went over 300 ms without a cycle.
// CONTROL: control_batch.sh 2026-10-13, reverted: steerSoundLocked's stored reading used only with a clock (`if (clock
//       && session.heard && …`), backend/daemon/wallpaper_manager.cpp -> "FAIL: last reading rule: a sound with no
//       clock is measured from its last reading, and not moved" (1 seek during the stop; 0 green).
void runLastReadingChecks() {
    const char* kGoals = "S15.571 (no clock: the last reading, not the asked anchor)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    StallReset reset;
    probe::g_streamRate.store(0.995);
    Daemon d(smallConfig());
    const std::string file = asset("wallpaper_beeps_long.mp4");
    if (!startHeard(d, file, kGoals)) return;
    const int64_t playFrom = probe::nowNs();
    std::this_thread::sleep_for(9000ms);
    const size_t movedBefore = soundSeeksSince(playFrom).size();
    const Capture before = takeCapture();
    const pid_t server = sound.server.pid();
    if (server <= 0) {
        harnessFailure("last reading rule: the private server this phase started has no process to stop");
        return;
    }
    const int64_t stopAt = probe::nowNs();
    {
        struct Resume {
            pid_t pid;
            ~Resume() { ::kill(pid, SIGCONT); }
        } resume{server};
        ::kill(server, SIGSTOP);
        std::this_thread::sleep_for(400ms);
    }
    const int64_t contAt = probe::nowNs();
    std::this_thread::sleep_for(500ms);
    const size_t during = soundSeeksSince(stopAt, contAt).size();
    const int64_t serverGap = longestBufferGap(takeCapture(), stopAt - 100 * kMs, contAt + 100 * kMs);
    double taken = 0;
    if (before.buffers.size() > 200) {
        const probe::BufferRecord &a = before.buffers[20], &b = before.buffers.back();
        taken = static_cast<double>(b.first - a.first) * 1e9 / static_cast<double>(b.queuedAt - a.queuedAt);
    }
    const double asked = kRate / 0.995;
    std::printf("audio_playback_test: last reading rule: the server took %.1f frames a second (asked %.1f); %zu sound seeks "
                "in the 9 s before the stop, %zu during it; the server went %.1f ms without a cycle\n",
                taken, asked, movedBefore, during, serverGap / 1e6);
    if (std::fabs(taken / asked - 1.0) > 0.001 || movedBefore != 0) {
        notVerified(kGoals, fmt("no drift was set between the reading and the asked anchor: the server took %.1f frames a "
                                "second, the sound was moved %.0f times before the stop",
                                taken, static_cast<double>(movedBefore)));
        return;
    }
    check(serverGap >= 300 * kMs, "last reading rule: the stopped server went over 300 ms without a cycle",
          fmt("%.1f ms", serverGap / 1e6));
    if (serverGap < 300 * kMs) return;
    check(during == 0, "last reading rule: a sound with no clock is measured from its last reading, and not moved",
          fmt("%.0f sound seeks during the stop", static_cast<double>(during)));
}

// `monitor`'s commits, from every screen's.
std::vector<Commit> commitsOf(Daemon& d, int monitor) {
    std::vector<Commit> out;
    for (const Commit& c : d.takeCommits(true)) {
        if (c.monitor == monitor) out.push_back(c);
    }
    return out;
}

// GOAL: S15.572 -- a heard screen catching up drops at least one picture every round, even in a round whose
//       catch-up slice an earlier screen has spent: with the screen before it in every round resuming a file
//       whose each picture read costs 10 ms, a 110 ms stall of the heard screen's read is closed by dropping,
//       with no sound seek. False if a round whose slice is spent holds the picture without dropping, so the
//       hold runs out still behind and the sound is moved back to it.
// WHY-THIS-SHAPE: the slice is spent before the heard screen's turn only if the screen ahead of it decodes in
//       every round; a playing screen decodes only after its draws, but a resuming one drops its way to its
//       position every round, at least one picture each. The B-frame file without sound has a single key
//       frame, so a resume 1.6 s in decodes about 48 pictures from its start: half a second at 10 ms a read.
//       Its screen is monitor 0 and the heard one monitor 1, as a round runs screens in monitor order. Graded
//       only when the resume began before the stall and was still running when the heard screen drew again.
// COVERAGE-ASSERT: the released screen really released; the stall reached; the resume spanned the catch-up.
// CONTROL: control_batch.sh 2026-10-13, reverted: the drop loop's slice test made the old `if ((dropped ||
//       waiting->catchingUpSince) && checked >= catchUpUntil)`, backend/daemon/wallpaper_manager.cpp -> "FAIL: drop
//       minimum rule: a heard screen behind its sound drops a picture each round though the slice is spent" (1
//       dropped, 1 seek; green 3 dropped, no seek, drawn again 31 ms after the stall).
void runDropMinimumChecks() {
    const char* kGoals = "S15.572 (one drop a round, the slice spent or not)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    StallReset reset;
    WallpaperManager::Config config = smallConfig();
    config.idleSessionRelease = 1s;
    Daemon d(config);
    const std::string ahead = asset("wallpaper_reads_bframes.mp4"), heard = asset("wallpaper_beeps_long.mp4");
    std::vector<int> redraw;
    d.manager.setMonitorOutput(0, "DP-2", redraw);
    d.manager.setMonitorOutput(1, "DP-1", redraw);
    d.manager.setOutputFocus(":1.5", "DP-1", ScreenFocus::Desktop, false);
    resetCapture();
    const int64_t started = probe::nowNs();
    d.manager.requestWallpaper("video", ahead, 0);
    d.worker.post(0);
    d.manager.requestWallpaper("video", heard, 1);
    d.worker.post(1);
    if (!waitFor(15000ms, [&] { return commitsOf(d, 1).size() >= 3 && probe::g_lastLoudQueuedAt.load() > started; })) {
        notVerified(kGoals, "the heard screen did not draw and sound within 15 s");
        return;
    }
    // Hidden once the screen ahead shows a picture 1.6 s or more into its file, so its resume is long.
    if (!waitFor(5000ms, [&] {
            const std::optional<int64_t> p = lastPictureOf(ahead);
            return p && *p >= 1600 * kMs;
        })) {
        notVerified(kGoals, "the screen ahead never reached 1.6 s into its file");
        return;
    }
    d.manager.setOutputVisible("DP-2", false);
    d.worker.wake();
    if (!waitFor(4000ms, [&] { return descriptorsOn(ahead) == 0; })) {
        notVerified(kGoals, "the hidden screen ahead was not released within 4 s, so it would not resume");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(probe::g_stallFileMutex);
        probe::g_slowFile = std::filesystem::path(ahead).filename().string();
    }
    probe::g_slowPictureMs.store(10);
    probe::g_slowPictureReads.store(1'000'000);
    const int64_t shownAt = probe::nowNs();
    d.manager.setOutputVisible("DP-2", true);
    d.worker.wake();
    // The resume's own drops hand nothing on; its slowed reads are what show it running.
    if (!waitFor(3000ms, [] { return probe::g_slowPictureReads.load() <= 1'000'000 - 3; })) {
        notVerified(kGoals, "the screen ahead did not start resuming within 3 s of being shown");
        return;
    }
    const int64_t resumingAt = probe::nowNs();
    // Past the screen ahead's open at its position, which holds one round up about 150 ms and leaves the
    // heard screen behind on its own account, and past the heard screen's catch-up from that.
    std::this_thread::sleep_for(300ms);
    const StallRun r = stallOnce(heard, 110);
    probe::g_slowPictureReads.store(0);
    if (r.ended == 0) {
        check(false, "drop minimum rule: the staged stall of the heard file's read was reached");
        return;
    }
    std::optional<int64_t> aheadDrew, heardDrew;
    for (const Commit& c : commitsOf(d, 0)) {
        if (c.at > shownAt && !aheadDrew) aheadDrew = c.at;
    }
    const std::vector<Shown> shown = shownPictures(commitsOf(d, 1), heard);
    const int64_t interval = fileInterval(heard);
    int dropped = 0;
    std::optional<int64_t> before;
    for (const Shown& s : shown) {
        if (!s.presentation) continue;
        if (s.commit.at > r.ended && !heardDrew) heardDrew = s.commit.at;
        if (before && s.commit.at > r.ended && s.commit.at <= r.ended + 500 * kMs) {
            dropped += skippedBetween(*before, *s.presentation, interval);
        }
        before = s.presentation;
    }
    std::printf("audio_playback_test: drop minimum rule: the screen ahead was resuming at %+.1f ms and drew at %+.1f ms; the "
                "heard stall %+.1f to %+.1f ms, the heard screen drew again at %+.1f ms, %d pictures dropped; %zu sound "
                "seeks, the server at most %.1f ms between cycles\n",
                (resumingAt - shownAt) / 1e6, aheadDrew ? (*aheadDrew - shownAt) / 1e6 : -1.0, (r.started - shownAt) / 1e6,
                (r.ended - shownAt) / 1e6, heardDrew ? (*heardDrew - shownAt) / 1e6 : -1.0, dropped, r.seeks.size(),
                r.serverGap / 1e6);
    if (!heardDrew) {
        check(false, "drop minimum rule: the heard screen drew again after its stall");
        return;
    }
    if (!aheadDrew || *aheadDrew < *heardDrew) {
        notVerified(kGoals, "the screen ahead was not resuming from the stall until the heard screen drew again, so no "
                            "round's slice was known to be spent");
        return;
    }
    if (r.serverGap > kServerLateBound) {
        notVerified(kGoals, fmt("the sound server went %.1f ms between cycles, which earns a seek of its own", r.serverGap / 1e6));
        return;
    }
    check(dropped >= 1 && r.seeks.empty(),
          "drop minimum rule: a heard screen behind its sound drops a picture each round though the slice is spent",
          fmt("%.0f pictures dropped, %.0f sound seeks", dropped, static_cast<double>(r.seeks.size())));
}

// GOAL: S15.573 -- skipping the pictures nothing references keeps a heard file that otherwise cannot keep up
//       in step with its sound: with every picture the decoder returns of the B-frame fixture costing 45 ms
//       (1.35 intervals), once settled (from 2.5 s after the slowdown to 7 s) no sound seek, and every beep
//       within 12 ms of its picture's place on the line through the commits. False if the decoder is never
//       told to skip (the file cannot keep up and drifts from its sound), or a skipping file is left
//       unpaced (the lag built before skipping began never closes).
// GOAL: S15.574 -- skipping removes pictures: in that window the decoder hands on under half the pictures
//       of the file it passes (a third, with two B-frames in three skipped). False if every one is decoded.
// GOAL: S15.575 -- skipping stops once decoding is cheap again: within 1.5 s of the slowdown's end the
//       decoder hands on three pictures one interval apart, which only a decoder that is not skipping does
//       here. False if skipping, once on, stays on.
// WHY-THIS-SHAPE: the cost is spent in avcodec_receive_frame, so a skipped picture costs nothing, as with a
//       real decode, where slowed packet reads would cost the same skipped or not. Skipping cuts a decoded
//       picture to one every 100 ms of file, 45 ms of work: the file keeps pace once the daemon trusts it,
//       after eight samples. The beeps are graded by gradeBeepsAgainstPictures, whose pass and skip
//       reading take the interval from the decoder's own record (S15.570). No seek is graded when the
//       sound server ran a cycle over 20 ms late in the window (as S15.538).
// COVERAGE-ASSERT: at least eight beeps graded; at least 20 pictures handed on in the window.
// CONTROL: control_batch.sh 2026-10-13, reverted: VideoSession::skip() returning PictureSkip::Nothing,
//       backend/daemon/wallpaper_manager.h -> "FAIL: skip rule: a file that cannot keep up decodes under half its
//       pictures" (100 of 100) and "... each beep is heard with the picture at its timestamp" (-205.1 to +204.6 ms);
//       steerSoundLocked's early return taken in every state but Keeps (backend/daemon/wallpaper_manager.cpp) -> "...
//       each beep is heard with the picture at its timestamp" (a steady -118.1 ms); DecodeLoad::add's return to Keeps
//       deleted -> "FAIL: skip rule: skipping stops within 1.5 s of decoding getting cheap" (never, in 2 s). Green:
//       50 of 133 handed on, no seek, beeps within 0.1 ms, skipping off 0.92 s after the slowdown (0.46 s on the RTX).
void runSkipChecks() {
    const char* kGoals = "S15.573-S15.575 (skipping pictures keeps a slow file in step)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    StallReset reset;
    Daemon d(smallConfig());
    const std::string file = asset("wallpaper_beeps_bframes.mp4");
    if (!startHeard(d, file, kGoals)) return;
    std::this_thread::sleep_for(1000ms);
    {
        std::lock_guard<std::mutex> lock(probe::g_costMutex);
        probe::g_costFile = std::filesystem::path(file).filename().string();
    }
    probe::g_costMs.store(45);
    std::this_thread::sleep_for(2500ms);
    const int64_t from = probe::nowNs();
    std::this_thread::sleep_for(4500ms);
    const int64_t to = probe::nowNs();
    probe::g_costMs.store(0);
    const int64_t costEnd = probe::nowNs();
    std::this_thread::sleep_for(2000ms);
    const std::vector<Commit> commits = d.takeCommits();
    const Capture cap = takeCapture();
    const size_t seeks = soundSeeksSince(from, to).size();
    const int64_t serverGap = longestBufferGap(cap, from, to);
    const int64_t interval = fileInterval(file);
    std::vector<int64_t> inWindow;
    // Two one-interval steps in a row: an unreferenced picture decoded. One alone is no proof, as the
    // reference picture before each key picture is one step from it.
    std::optional<int64_t> offAt;
    std::optional<int64_t> previous;
    int singleSteps = 0;
    for (const probe::PictureRecord& p : picturesOf(file)) {
        if (!p.presentationNs) continue;
        if (p.at >= from && p.at <= to) inWindow.push_back(*p.presentationNs);
        if (p.at > costEnd && previous) {
            const int64_t step = *p.presentationNs - *previous;
            singleSteps = step > 0 && step < interval * 3 / 2 ? singleSteps + 1 : 0;
            if (singleSteps >= 2 && !offAt) offAt = p.at;
        }
        previous = p.presentationNs;
    }
    std::sort(inWindow.begin(), inWindow.end());
    for (const probe::PictureRecord& p : picturesOf(file)) {
        if (p.at > costEnd && p.at < costEnd + 1100 * kMs && p.presentationNs) {
            std::printf("  handed on %.1f ms after the slowdown ended: the picture at %.1f ms\n", (p.at - costEnd) / 1e6,
                        *p.presentationNs / 1e6);
        }
    }
    const double passed = inWindow.size() >= 2 && interval > 0
                              ? static_cast<double>(inWindow.back() - inWindow.front()) / static_cast<double>(interval) + 1
                              : 0.0;
    const double share = passed > 0 ? static_cast<double>(inWindow.size()) / passed : 1.0;
    std::printf("audio_playback_test: skip rule: %zu pictures handed on of %.0f passed in the settled window (%.2f); %zu sound "
                "seeks, the server at most %.1f ms between cycles; a non-skipping step %.1f ms after the slowdown ended\n",
                inWindow.size(), passed, share, seeks, serverGap / 1e6, offAt ? (*offAt - costEnd) / 1e6 : -1.0);
    check(inWindow.size() >= 20, "skip rule: the slowed file went on decoding in the settled window",
          fmt("%.0f pictures", static_cast<double>(inWindow.size())));
    if (inWindow.size() < 20) return;
    check(share < 0.5, "skip rule: a file that cannot keep up decodes under half its pictures",
          fmt("%.0f of %.0f", static_cast<double>(inWindow.size()), passed));
    check(offAt && *offAt - costEnd <= 1500 * kMs, "skip rule: skipping stops within 1.5 s of decoding getting cheap",
          offAt ? fmt("%.1f ms after", (*offAt - costEnd) / 1e6) : std::string("never, in 2 s"));
    if (serverGap > kServerLateBound) {
        notVerified("S15.573 (no seek once settled)",
                    fmt("the sound server went %.1f ms between cycles, which earns a seek of its own", serverGap / 1e6));
    } else {
        check(seeks == 0, "skip rule: a skipping file keeps up with its sound without being moved",
              fmt("%.0f sound seeks", static_cast<double>(seeks)));
    }
    int passStarts = 0;
    const int graded = gradeBeepsAgainstPictures("skip rule", cap, shownPictures(commits, file), file, from, 12.0, passStarts);
    if (graded >= 0) check(graded >= 8, "skip rule: at least eight beeps were graded once settled", fmt("%.0f", graded));
}

// GOAL: S15.577 -- a late last picture at a loop moves a sound already handed to the next pass once more,
//       and nothing moves it again: from the second loop on, with no clock from the hand-over on and the
//       pass's last picture held 100 ms as the decoder returns it (drawn about 67 ms late, past
//       kSoundDrift), exactly two sound seeks around the loop -- the hand-over, then the move onto the
//       picture. False if no-clock steering lets a 67 ms lead stand (one seek), or the pass start re-hands
//       a sound the move already lined up (three).
// GOAL: S15.586 -- that move keeps the sound on the pass it was handed to: no seek in the window targets
//       the old pass (1 s or more into the two-second file). False if the move is anchored on the old
//       pass's time, which plays the old pass's end again after the hand-over.
// WHY-THIS-SHAPE: the clock is hidden from the hand-over's seek on (the seek interposer sets it), so the
//       late picture is not dropped to chase the sound but drawn and the sound moved to it; with a live
//       clock it is dropped and nothing is moved. The hold is on the picture's return from the decoder,
//       since the software decoder's frame threads read the pass's last packets before a stalled read
//       could reach them. Targets are read with the sound decoder's 100 ms pre-roll, so the new pass's
//       start shows as -100 ms and the old pass's last picture as about 1867 ms. Seeks counted from 300 ms
//       before the hold to 700 ms after, under S15.538's sound-server guard.
// COVERAGE-ASSERT: a loop graded; in each, the hold reached and the clock hidden by the hand-over before it.
// CONTROL: control_batch.sh 2026-10-13, reverted, steerSoundLocked, backend/daemon/wallpaper_manager.cpp: the
//       no-clock move made `overdue - behind > (clock ? kSoundDrift : kCatchUpHold)` -> "FAIL: late at loop
//       rule: a late picture at a loop moves the handed-on sound once more, and no more" (1 seek a loop); the
//       move anchored at `schedule.from` with no shift -> "FAIL: late at loop rule: the handed-on sound is
//       never sent back to the old pass" (3 seeks a loop, the second to 1866.7 ms; green 2 a loop, both to
//       -100 ms, on both entries). `passAway` made kSoundDrift with the pass known stays green (2 seeks a loop):
//       the move lines the sound up before the pass start, so the margin it gives is not reached here (GOALS.md,
//       Not graded).
void runLateAtLoopChecks() {
    const char* kGoals = "S15.577, S15.586 (a late picture at a loop moves the handed-on sound once, on its new pass)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    StallReset reset;
    const std::string heard = asset("wallpaper_beeps.mp4");
    {
        std::lock_guard<std::mutex> lock(probe::g_holdMutex);
        probe::g_holdFile = std::filesystem::path(heard).filename().string();
    }
    // The file's last picture, at 59/30 s.
    probe::g_holdAtNs.store(59 * 1'000'000'000LL / 30);
    Daemon d(smallConfig());
    if (!startHeard(d, heard, kGoals)) return;
    // The first loop learns the pass; the hand-over is early from the second on.
    if (!waitFor(5000ms, [&] {
            const std::vector<Shown> shown = shownPictures(d.takeCommits(), heard);
            for (size_t i = 1; i < shown.size(); ++i) {
                if (shown[i].presentation && shown[i - 1].presentation && *shown[i].presentation < *shown[i - 1].presentation) {
                    return true;
                }
            }
            return false;
        })) {
        notVerified(kGoals, "the file did not loop within 5 s");
        return;
    }
    int graded = 0;
    for (int loop = 1; loop <= 2; ++loop) {
        probe::g_holdStartedAt.store(0);
        probe::g_holdEndedAt.store(0);
        probe::g_hideStreamTime.store(false);
        probe::g_hideAtHandOver.store(true);
        probe::g_holdMs.store(100);
        if (!waitFor(3000ms, [] { return probe::g_holdEndedAt.load() != 0; })) {
            probe::g_holdMs.store(0);
            probe::g_hideAtHandOver.store(false);
            check(false, "late at loop rule: the staged hold of the pass's last picture was reached within 3 s");
            return;
        }
        const int64_t started = probe::g_holdStartedAt.load();
        const bool hidden = probe::g_hideStreamTime.load();
        std::this_thread::sleep_for(300ms);
        probe::g_hideStreamTime.store(false);
        probe::g_hideAtHandOver.store(false);
        std::this_thread::sleep_for(500ms);
        const std::vector<probe::SoundSeek> seeks = soundSeeksSince(started - 300 * kMs, started + 700 * kMs);
        const int64_t serverGap = longestBufferGap(takeCapture(), started - 300 * kMs, started + 700 * kMs);
        std::printf("audio_playback_test: late at loop rule: loop %d: %zu sound seeks around the hold, the clock hidden from "
                    "the hand-over %d; the server at most %.1f ms between cycles\n",
                    loop, seeks.size(), hidden ? 1 : 0, serverGap / 1e6);
        int onOldPass = 0;
        for (const probe::SoundSeek& s : seeks) {
            std::printf("  a seek %+.1f ms from the hold's start, to %.1f ms of the file\n", (s.at - started) / 1e6,
                        s.targetNs / 1e6);
            if (s.targetNs >= 1000 * kMs) ++onOldPass;
        }
        if (!hidden) {
            notVerified(kGoals, "no hand-over hid the clock before the pass's last picture was held");
            continue;
        }
        if (serverGap > kServerLateBound) {
            notVerified(kGoals, fmt("the sound server went %.1f ms between cycles, which earns a seek of its own", serverGap / 1e6));
            continue;
        }
        ++graded;
        check(seeks.size() == 2 && seeks[0].targetNs < 50 * kMs,
              "late at loop rule: a late picture at a loop moves the handed-on sound once more, and no more",
              fmt("loop %.0f: %.0f sound seeks", loop, static_cast<double>(seeks.size())));
        check(onOldPass == 0, "late at loop rule: the handed-on sound is never sent back to the old pass",
              fmt("loop %.0f: %.0f seeks to 1 s or more into the file", loop, onOldPass));
    }
    check(graded >= 1, "late at loop rule: a loop was graded", fmt("%.0f", graded));
}

// GOAL: S15.578 -- a picture the sound times but draws late earns no clean-play credit: on the two-second
//       heard beeps file, restored with one death and cleanPlayAfter 5 s, with every pass's last draw from
//       the second loop on held 1.8 s (so each new pass's first picture, which is never dropped, is drawn
//       about 1.8 s late while the sound, already handed on, times it), the death is still on record after
//       10.5 s. False if late pictures the sound timed count as on time, which credits each late pass start.
// WHY-THIS-SHAPE: once a catch-up hold runs out the sound is moved to the picture, so that draw is not timed
//       by the sound; the one late draw the sound still times is a pass's first picture. The hold is in the
//       draw (vkCmdBlitImage) of the old pass's last picture, armed when the picture before it is committed,
//       so the decode load is untouched. Credit is the time between draws, counted when a draw is on time,
//       so a short hold would be repaid by the on-time play after it: the hold is most of a pass, and the
//       first two passes, about 3.9 s of credit, play unheld. A correct run then gains only the 0.1-0.2 s
//       a pass between the catch-up and the next hold.
// COVERAGE-ASSERT: the run started carrying the death, was heard, and at least three held draws were each
//       followed by the new pass's first picture a second or more later.
// CONTROL: control_batch.sh 2026-10-13, reverted: advanceSessionLocked's `onTime` given `pace.timesPicture ||`
//       back, backend/daemon/wallpaper_manager.cpp -> "FAIL: late timed rule: a pass's first picture the sound
//       timed but drew late earns no clean credit" (cleared 5841 ms in; green: kept after 10.5 s on both entries). Test-side: the holds
//       taken out -> "FAIL: late timed rule: the run started ... drew three pass starts late" (0), the death then
//       cleared 5123 ms in, so the record and its clearing are live here.
void runLateTimedChecks() {
    const char* kGoals = "S15.578 (late pictures the sound timed earn no clean credit)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    StallReset reset;
    const std::string heard = asset("wallpaper_beeps.mp4");
    const int64_t beforeLast = 58 * 1'000'000'000LL / 30;
    const char* tmp = std::getenv("TMPDIR");
    const std::filesystem::path dir = std::filesystem::path(tmp != nullptr ? tmp : "/tmp") /
                                      ("wallpaperx-late-timed." + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "state");
    struct Remove {
        std::filesystem::path dir;
        ~Remove() {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }
    } remove{dir};
    std::ofstream(dir / "config.json")
        << nlohmann::json{{"version", 1},
                          {"monitors", nlohmann::json::array({{{"monitor_id", 0}, {"type", "video"}, {"path", heard}}})}}
               .dump();
    std::ofstream(dir / "state" / "playing.json")
        << nlohmann::json{{"run", "earlier"},
                          {"videos", nlohmann::json::array({{{"monitor_id", 0}, {"path", heard}, {"deaths", 1}}})}}
               .dump()
        << "\n";
    WallpaperManager::Config config = smallConfig();
    config.configFile = dir / "config.json";
    config.stateDir = dir / "state";
    config.cleanPlayAfter = 5s;
    const auto deaths = [&] {
        const std::optional<wallpaperx::PlayingHistory> h = wallpaperx::readPlayingRecord(config.stateDir);
        if (!h) return -1;
        for (const wallpaperx::PlayingVideo& v : h->videos) {
            if (v.monitorId == 0) return v.deaths;
        }
        return 0;
    };
    // Each held draw: when it was armed. Written on the render thread, read after the daemon stops.
    std::mutex armedMutex;
    std::vector<int64_t> armed;
    std::atomic<bool> holding{true};
    int passEnds = 0;  // render thread only
    resetCapture();
    const int64_t started = probe::nowNs();
    Daemon d(config);
    d.onCommit = [&](const Commit&) {
        if (!holding.load()) return;
        const std::optional<int64_t> p = lastPictureOf(heard);
        if (!p || std::llabs(*p - beforeLast) >= 2 * kMs || ++passEnds < 2) return;
        probe::g_blitHoldMs.store(1800);
        std::lock_guard<std::mutex> lock(armedMutex);
        armed.push_back(probe::nowNs());
    };
    const std::vector<int> restored = d.manager.restoreSaved();
    d.manager.recordStartingVideos();
    const int before = deaths();
    std::vector<int> redraw;
    d.manager.setMonitorOutput(0, "DP-1", redraw);
    d.manager.setOutputFocus(":1.5", "DP-1", ScreenFocus::Desktop, false);
    for (const int id : restored) d.worker.post(id);
    for (const int id : redraw) d.worker.post(id);
    int64_t clearedAt = 0;
    constexpr int kWindowMs = 10500;
    const auto until = Clock::now() + std::chrono::milliseconds(kWindowMs);
    while (Clock::now() < until) {
        if (clearedAt == 0 && deaths() == 0) clearedAt = probe::nowNs();
        std::this_thread::sleep_for(20ms);
    }
    holding.store(false);
    const int after = deaths();
    const bool loud = probe::g_lastLoudQueuedAt.load() > started;
    const std::vector<Shown> shown = shownPictures(d.takeCommits(), heard);
    std::vector<int64_t> holds;
    {
        std::lock_guard<std::mutex> lock(armedMutex);
        holds = armed;
    }
    // A held draw counts once the new pass's first picture came a second or more after it was armed.
    int lateStarts = 0;
    for (const int64_t at : holds) {
        for (size_t i = 1; i < shown.size(); ++i) {
            if (shown[i].commit.at <= at || !shown[i].presentation || !shown[i - 1].presentation) continue;
            if (*shown[i].presentation < *shown[i - 1].presentation) {
                if (shown[i].commit.at - at >= 1000 * kMs) ++lateStarts;
                break;
            }
        }
    }
    const std::vector<probe::SoundSeek> seeks = soundSeeksSince(started);
    std::printf("audio_playback_test: late timed rule: %d deaths on record before, %d after %.1f s (cleared %.0f ms in); "
                "%zu draws held, %d late pass starts, %zu sound seeks; heard %d\n",
                before, after, kWindowMs / 1000.0, clearedAt ? (clearedAt - started) / 1e6 : -1.0, holds.size(),
                lateStarts, seeks.size(), loud ? 1 : 0);
    for (const probe::SoundSeek& s : seeks) {
        std::printf("  a seek %.1f ms in, to %.1f ms of the file\n", (s.at - started) / 1e6, s.targetNs / 1e6);
    }
    check(before == 1 && loud && lateStarts >= 3,
          "late timed rule: the run started carrying a death, was heard, and drew three pass starts late",
          fmt("%.0f deaths at the start, heard %.0f, %.0f late pass starts", before, loud ? 1.0 : 0.0, lateStarts));
    if (before != 1 || !loud || lateStarts < 3) return;
    check(after == 1, "late timed rule: a pass's first picture the sound timed but drew late earns no clean credit",
          fmt("the death cleared %.0f ms after the start", clearedAt ? (clearedAt - started) / 1e6 : -1.0));
}

// GOAL: S15.579 -- a file whose decoding hovers about its own rate is not switched in and out of "cannot keep
//       up": each picture of the heard baseline file costing 41 ms eight times then 27 ms eight times (on
//       average just over its interval, so the averaged load wanders between about 0.92 and 1.12), from 2 s
//       after the slowdown, no picture dropped and no sound seek mid-pass. False if the file counts as
//       keeping up again as soon as the load dips under 1, so it is steered at each dip, its pictures far
//       behind the sound it ran unpaced from, and drops them or moves the sound.
// WHY-THIS-SHAPE: the file has no picture to skip, so skipping changes nothing and the two loads are the
//       same samples. Once the load reaches 1 the file is unpaced, and only a load under kKeepsUp (0.9)
//       steers it again; the pattern keeps it above that. Dropped pictures are read from the presentation
//       times shown, at the file's interval.
// COVERAGE-ASSERT: at least 40 pictures drawn in the window.
// CONTROL: control_batch.sh 2026-10-13, reverted: DecodeLoad::kKeepsUp made 1.0, backend/daemon/wallpaper_manager.h ->
//       "FAIL: hover rule: a file hovering at its own rate is not steered at each dip" (49 dropped, 1 seek; green 0
//       and 0, 168-169 drawn).
void runHoverChecks() {
    const char* kGoals = "S15.579 (a file hovering at its own rate does not flip in and out of keeping up)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    StallReset reset;
    Daemon d(smallConfig());
    const std::string file = asset("wallpaper_beeps_long.mp4");
    if (!startHeard(d, file, kGoals)) return;
    std::this_thread::sleep_for(500ms);
    {
        std::lock_guard<std::mutex> lock(probe::g_costMutex);
        probe::g_costFile = std::filesystem::path(file).filename().string();
        probe::g_costCycle = {41, 41, 41, 41, 41, 41, 41, 41, 27, 27, 27, 27, 27, 27, 27, 27};
        probe::g_costCycleAt = 0;
    }
    probe::g_costMs.store(1);
    std::this_thread::sleep_for(2000ms);
    const int64_t from = probe::nowNs();
    std::this_thread::sleep_for(6000ms);
    const int64_t to = probe::nowNs();
    probe::g_costMs.store(0);
    const size_t seeks = soundSeeksSince(from, to).size();
    const std::vector<Shown> shown = shownPictures(d.takeCommits(), file);
    const int64_t interval = fileInterval(file);
    int drawn = 0, dropped = 0;
    for (size_t i = 1; i < shown.size(); ++i) {
        if (shown[i].commit.at < from || shown[i].commit.at > to || !shown[i].presentation || !shown[i - 1].presentation) continue;
        if (*shown[i].presentation < *shown[i - 1].presentation) continue;
        ++drawn;
        dropped += skippedBetween(*shown[i - 1].presentation, *shown[i].presentation, interval);
    }
    std::printf("audio_playback_test: hover rule: %d pictures drawn, %d dropped, %zu sound seeks in 6 s\n", drawn, dropped,
                seeks);
    check(drawn >= 40, "hover rule: the hovering file went on drawing", fmt("%.0f pictures", drawn));
    if (drawn < 40) return;
    check(dropped == 0 && seeks == 0, "hover rule: a file hovering at its own rate is not steered at each dip",
          fmt("%.0f pictures dropped, %.0f sound seeks", dropped, static_cast<double>(seeks)));
}

// What `file`'s picture decoder handed on in [from, to): how many pictures, and how many presentation
// times they skip over within a pass, at the file's own interval (0 when it is not known yet).
struct HandedOn {
    int decoded = 0, skipped = 0;
    int64_t interval = 0;
};
HandedOn handedOn(const std::string& file, int64_t from, int64_t to) {
    HandedOn out;
    out.interval = fileInterval(file);
    std::optional<int64_t> before;
    for (const probe::PictureRecord& p : picturesOf(file)) {
        if (p.at < from || p.at >= to || !p.presentationNs) continue;
        ++out.decoded;
        if (before) out.skipped += skippedBetween(*before, *p.presentationNs, out.interval);
        before = p.presentationNs;
    }
    return out;
}

void costFile(const std::string& file) {
    std::lock_guard<std::mutex> lock(probe::g_costMutex);
    probe::g_costFile = std::filesystem::path(file).filename().string();
}

// GOAL: S15.580 -- a picture's decode load is measured against the time it is on screen at the playback
//       speed: the silent B-frame file at speed 0.5, each returned picture costing 43 ms (1.3 of its own
//       33 ms interval, 0.65 of the 67 ms it is shown), decodes every picture; at speed 2, at 23 ms (0.7 of
//       its interval, 1.4 of the 17 ms shown) it skips. False if the load is measured against the file's
//       own interval, which keeps the slow speed's pictures from being decoded and lets the fast speed's
//       fall behind with nothing skipped.
// WHY-THIS-SHAPE: silent, so no sound steering and no catch-up dropping enters the count: every gap in the
//       presentation times the decoder handed on is a picture it skipped. The cost is spent where
//       avcodec_receive_frame returns a picture, so a skipped one costs nothing. Graded from 2 s after the
//       cost starts, past the eight-sample settling, for 3 s. Each speed has a fresh daemon.
// COVERAGE-ASSERT: at least 10 pictures handed on in each window (45 at speed 0.5 is every one; skipping
//       lowers it, so no higher floor), and the file's interval known.
// CONTROL: control_batch.sh 2026-10-13, reverted, measureDecode in advanceSessionLocked,
//       backend/daemon/wallpaper_manager.cpp: `intervalAtSpeed(after, speed)` made `ownInterval(after)` and
//       `atSpeed(gap, speed)` made `gap` -> "FAIL: speed load rule: at speed 0.5 a file that keeps up on screen decodes
//       every picture" (16 handed on, 27 skipped) and "... at speed 2 ... skips pictures" (130 handed on, 0 skipped).
//       Green on both entries: 45 handed on and 0 skipped; 66 and 112-114.
void runSpeedLoadChecks() {
    const char* kGoals = "S15.580 (decode load is measured at the playback speed)";
    StallReset reset;
    const std::string file = asset("wallpaper_beeps_bframes.mp4");
    struct Case {
        double speed;
        int costMs;
        bool keepsUp;
    };
    for (const Case c : {Case{0.5, 43, true}, Case{2.0, 23, false}}) {
        probe::g_costMs.store(0);
        Daemon d(smallConfig());
        resetCapture();
        d.play(file, std::nullopt);
        if (!d.waitForCommits(3, 15s)) {
            notVerified(kGoals, "the video drew no pictures within 15 s");
            return;
        }
        d.properties({{"speed", c.speed}});
        costFile(file);
        probe::g_costMs.store(c.costMs);
        std::this_thread::sleep_for(2000ms);
        const int64_t from = probe::nowNs();
        std::this_thread::sleep_for(3000ms);
        const HandedOn h = handedOn(file, from, probe::nowNs());
        probe::g_costMs.store(0);
        std::printf("audio_playback_test: speed load rule: at speed %.1f, %d ms a picture: %d pictures handed on in 3 s, %d "
                    "skipped over\n",
                    c.speed, c.costMs, h.decoded, h.skipped);
        // Skipping lowers the count, so this asks only that the window saw decoding.
        const int least = 10;
        check(h.decoded >= least && h.interval > 0, fmt("speed load rule: at speed %.1f the slowed file went on decoding", c.speed),
              fmt("%.0f pictures, interval %.2f ms", h.decoded, h.interval / 1e6));
        if (h.decoded < least || h.interval == 0) continue;
        if (c.keepsUp) {
            check(h.skipped == 0, "speed load rule: at speed 0.5 a file that keeps up on screen decodes every picture",
                  fmt("%.0f skipped, %.0f handed on", h.skipped, h.decoded));
        } else {
            check(h.skipped > h.decoded, "speed load rule: at speed 2 a file that cannot keep up on screen skips pictures",
                  fmt("%.0f skipped, %.0f handed on", h.skipped, h.decoded));
        }
    }
}

// GOAL: S15.581 -- with a clock, a catch-up hold that runs out ends with the sound moved to the pictures,
//       once: the heard long beeps file at 28 ms a picture (each drop gains about 5 ms) with one draw held
//       120 ms earns exactly one sound seek, at least kCatchUpHold after that draw. False if a lead the
//       hold could not close is left standing because it is under some larger limit: the next picture then
//       starts a fresh hold, and the screen stutters through hold after hold with no seek.
// WHY-THIS-SHAPE: the hold is in the draw (vkCmdBlitImage), outside any decode, so the decode load stays
//       about 0.9 and the file is still steered by its sound. A run that dropped no picture after the hold
//       was taken for one that cannot keep up, which is never steered, so it is not verified rather than
//       graded on its zero seeks. The held draw is the first blit after arming, as g_blits records it.
//       Under S15.538's sound-server guard.
// COVERAGE-ASSERT: the draw hold reached; pictures dropped after it.
// CONTROL: control_batch.sh 2026-10-13, reverted, steerSoundLocked, backend/daemon/wallpaper_manager.cpp: the
//       move made `overdue - behind > (clock ? kCatchUpHold : kSoundDrift)` (a 100 ms limit after the hold)
//       -> "FAIL: hold run-out rule: a hold that runs out moves the sound once, after the hold" (0 seeks). Green on
//       both entries: one seek 169 ms after the held draw, 5 dropped. Test-side: 45 ms a picture -> "NOT VERIFIED
//       -- S15.581 ..." (0 dropped: taken for a file that cannot keep up), not a FAIL.
void runHoldRunOutChecks() {
    const char* kGoals = "S15.581 (a hold that runs out moves the sound once)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    StallReset reset;
    Daemon d(smallConfig());
    const std::string file = asset("wallpaper_beeps_long.mp4");
    if (!startHeard(d, file, kGoals)) return;
    costFile(file);
    probe::g_costMs.store(28);
    std::this_thread::sleep_for(1500ms);
    const int64_t armed = probe::nowNs();
    probe::g_blitHoldMs.store(120);
    std::this_thread::sleep_for(1500ms);
    const int64_t end = probe::nowNs();
    probe::g_costMs.store(0);
    const int left = probe::g_blitHoldMs.exchange(0);
    std::optional<int64_t> heldAt;
    {
        std::lock_guard<std::mutex> lock(probe::g_blitMutex);
        for (const int64_t b : probe::g_blits) {
            if (b >= armed) {
                heldAt = b;
                break;
            }
        }
    }
    if (left != 0 || !heldAt) {
        check(false, "hold run-out rule: the staged draw hold was reached", fmt("%.0f ms unused", left));
        return;
    }
    const int64_t holdEnd = *heldAt + 120 * kMs;
    const std::vector<probe::SoundSeek> seeks = soundSeeksSince(armed, end);
    const int64_t serverGap = longestBufferGap(takeCapture(), armed, end);
    const int64_t interval = fileInterval(file);
    int dropped = 0;
    std::optional<int64_t> before;
    for (const Shown& c : shownPictures(d.takeCommits(), file)) {
        if (!c.presentation) continue;
        if (before && c.commit.at > holdEnd && c.commit.at <= end) dropped += skippedBetween(*before, *c.presentation, interval);
        before = c.presentation;
    }
    std::printf("audio_playback_test: hold run-out rule: %zu sound seeks, %d pictures dropped after the held draw; the server "
                "at most %.1f ms between cycles\n",
                seeks.size(), dropped, serverGap / 1e6);
    for (const probe::SoundSeek& s : seeks) std::printf("  a seek %+.1f ms after the held draw ended\n", (s.at - holdEnd) / 1e6);
    if (dropped == 0) {
        notVerified(kGoals, "no picture was dropped after the held draw: the file was not chasing its sound");
    } else if (serverGap > kServerLateBound) {
        notVerified(kGoals, fmt("the sound server went %.1f ms between cycles, which earns a seek of its own", serverGap / 1e6));
    } else {
        check(seeks.size() == 1 && seeks[0].at - holdEnd >= 100 * kMs,
              "hold run-out rule: a hold that runs out moves the sound once, after the hold",
              fmt("%.0f sound seeks, the first %+.1f ms after the held draw", static_cast<double>(seeks.size()),
                  seeks.empty() ? 0.0 : (seeks[0].at - holdEnd) / 1e6));
    }
}

// GOAL: S15.582 -- a pass that starts more than an interval late has its sound handed on where its first
//       picture is drawn: on the heard two-second beeps file, the first loop's last draw held 150 ms (the
//       new pass's first picture then about 117 ms overdue), exactly one sound seek around the loop -- the
//       hand-on -- and no picture of the new pass's first 400 ms dropped. False if the hand-on anchors the
//       sound at the picture's planned time: it is then about 117 ms ahead of a picture drawn now, and
//       is moved a second time or chased by dropping.
// WHY-THIS-SHAPE: the draw is held, not a decode: the steering works from the round's start time, so only a
//       round that starts late makes the pass start late; a slow decode inside the round would not. The
//       hold is armed from the commit callback, on the render thread, when the picture before the last is
//       committed, so it lands on the last picture's draw. The first loop, because with no pass learned the
//       sound is handed on in the new pass's own round, which is the branch this grades. Seeks counted from
//       200 ms before arming to 1 s after, under S15.538's sound-server guard.
// COVERAGE-ASSERT: the hold armed and spent, and the new pass's first picture drawn 150 ms or more after it.
// CONTROL: control_batch.sh 2026-10-13, reverted, steerSoundLocked, backend/daemon/wallpaper_manager.cpp: the
//       hand-on to a new pass anchored at `planned.timeline` instead of `drawnAt` -> "FAIL: late hand-on
//       rule: a late pass start moves the sound once, onto its first picture, and drops nothing" (2 seeks: to
//       17.4 ms, then -99.3 ms). Green on both entries: 1 seek to -99 ms, 0 dropped, the pass's first picture
//       184 ms after arming. Test-side: the hold made 0 -> "FAIL: late hand-on rule: the held draw made the new
//       pass start late" (66.7 ms).
void runLateHandOnChecks() {
    const char* kGoals = "S15.582 (a late pass start: the sound lands where its picture is drawn)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    StallReset reset;
    const std::string file = asset("wallpaper_beeps.mp4");
    const int64_t beforeLast = 58 * 1'000'000'000LL / 30;
    std::atomic<int64_t> armedAt{0};
    Daemon d(smallConfig());
    d.onCommit = [&](const Commit&) {
        if (armedAt.load() != 0) return;
        const std::optional<int64_t> p = lastPictureOf(file);
        if (!p || std::llabs(*p - beforeLast) >= 2 * kMs) return;
        probe::g_blitHoldMs.store(150);
        armedAt.store(probe::nowNs());
    };
    if (!startHeard(d, file, kGoals)) return;
    if (!waitFor(4000ms, [&] { return armedAt.load() != 0; })) {
        check(false, "late hand-on rule: the first loop's last draw was held within 4 s");
        return;
    }
    const int64_t at = armedAt.load();
    std::this_thread::sleep_for(1000ms);
    const int left = probe::g_blitHoldMs.exchange(0);
    const std::vector<probe::SoundSeek> seeks = soundSeeksSince(at - 200 * kMs, at + 1000 * kMs);
    const int64_t serverGap = longestBufferGap(takeCapture(), at - 200 * kMs, at + 1000 * kMs);
    const std::vector<Shown> shown = shownPictures(d.takeCommits(), file);
    const int64_t interval = fileInterval(file);
    std::optional<int64_t> passStart;
    int dropped = 0;
    for (size_t i = 1; i < shown.size(); ++i) {
        if (shown[i].commit.at <= at || !shown[i].presentation || !shown[i - 1].presentation) continue;
        if (!passStart) {
            if (*shown[i].presentation < *shown[i - 1].presentation) passStart = shown[i].commit.at;
            continue;
        }
        if (shown[i].commit.at > *passStart + 400 * kMs) break;
        dropped += skippedBetween(*shown[i - 1].presentation, *shown[i].presentation, interval);
    }
    std::printf("audio_playback_test: late hand-on rule: the new pass's first picture drawn %.1f ms after the hold was "
                "armed; %zu sound seeks around the loop, %d pictures dropped in its first 400 ms; the server at most "
                "%.1f ms between cycles\n",
                passStart ? (*passStart - at) / 1e6 : -1.0, seeks.size(), dropped, serverGap / 1e6);
    for (const probe::SoundSeek& s : seeks) {
        std::printf("  a seek %+.1f ms from arming, to %.1f ms of the file\n", (s.at - at) / 1e6, s.targetNs / 1e6);
    }
    const bool late = left == 0 && passStart && *passStart - at >= 150 * kMs;
    check(late, "late hand-on rule: the held draw made the new pass start late",
          fmt("%.0f ms of the hold unused, the pass's first picture %.1f ms after arming", left,
              passStart ? (*passStart - at) / 1e6 : -1.0));
    if (!late) return;
    if (serverGap > kServerLateBound) {
        notVerified(kGoals, fmt("the sound server went %.1f ms between cycles, which earns a seek of its own", serverGap / 1e6));
        return;
    }
    check(seeks.size() == 1 && seeks[0].targetNs < 50 * kMs && dropped == 0,
          "late hand-on rule: a late pass start moves the sound once, onto its first picture, and drops nothing",
          fmt("%.0f sound seeks, %.0f pictures dropped", static_cast<double>(seeks.size()), dropped));
}

// GOAL: S15.583 -- a few slow decodes as a session starts do not make a file count as one that cannot
//       keep up: the silent B-frame file whose first three returned pictures cost 70 ms each (twice their
//       interval) and the rest nothing extra skips no picture in its first 2 s. False if the move from
//       "keeps up" to "behind" trusts the average before it holds kSettled samples, so a first slow decode
//       alone turns skipping on.
// WHY-THIS-SHAPE: three, because the first picture is decoded outside the measured path and two capped
//       samples of 2 among eight cheap ones average under 1, so a settled average stays at "keeps up"
//       whatever the cheap decodes cost; a fourth slow one could make the correct average reach 1.
//       Silent, so every presentation-time gap is a picture the decoder skipped.
// COVERAGE-ASSERT: the three slow decodes spent, and at least 40 pictures handed on.
// CONTROL: control_batch.sh 2026-10-13, reverted, DecodeLoad::add, backend/daemon/wallpaper_manager.cpp:
//       `everyPicture.samples >= kSettled &&` deleted -> "FAIL: first slow rule: a few slow first decodes do not turn skipping on" (3 skipped; green 0
//       of 64 on both entries). Test-side: no cost -> "FAIL: first slow rule: the slow first decodes were spent,
//       and the file went on" (0 costed).
void runFirstSlowDecodesChecks() {
    const char* kGoals = "S15.583 (slow first decodes do not turn skipping on)";
    StallReset reset;
    const std::string file = asset("wallpaper_beeps_bframes.mp4");
    costFile(file);
    {
        std::lock_guard<std::mutex> lock(probe::g_costMutex);
        probe::g_costCycle.assign(1000, 0);
        for (int i = 0; i < 3; ++i) probe::g_costCycle[static_cast<size_t>(i)] = 70;
        probe::g_costCycleAt = 0;
    }
    probe::g_costMs.store(1);
    Daemon d(smallConfig());
    resetCapture();
    const int64_t from = probe::nowNs();
    d.play(file, std::nullopt);
    if (!d.waitForCommits(3, 15s)) {
        notVerified(kGoals, "the video drew no pictures within 15 s");
        return;
    }
    std::this_thread::sleep_for(2000ms);
    const HandedOn h = handedOn(file, from, probe::nowNs());
    probe::g_costMs.store(0);
    size_t spent = 0;
    {
        std::lock_guard<std::mutex> lock(probe::g_costMutex);
        spent = probe::g_costCycleAt;
    }
    std::printf("audio_playback_test: first slow rule: %zu costed pictures, %d handed on, %d skipped over\n", spent, h.decoded,
                h.skipped);
    check(spent >= 3 && h.decoded >= 40 && h.interval > 0, "first slow rule: the slow first decodes were spent, and the file went on",
          fmt("%.0f costed, %.0f handed on", static_cast<double>(spent), h.decoded));
    if (spent < 3 || h.decoded < 40 || h.interval == 0) return;
    check(h.skipped == 0, "first slow rule: a few slow first decodes do not turn skipping on", fmt("%.0f skipped", h.skipped));
}

// GOAL: S15.584 -- with no clock, the sound is moved to the pictures once they are more than kSoundDrift off,
//       and measured against where the waiting picture will be shown: the heard long beeps file, its clock
//       hidden, (a) one draw held 60 ms, so the next picture is drawn about 27 ms late, inside its interval,
//       where it keeps its planned time: no sound seek; (b) one draw held 90 ms, the next about 57 ms late,
//       shown where it is drawn: exactly one seek. False if a sound 20-100 ms off its pictures with no clock
//       is left alone (b: no seek), or if lateness inside an interval is counted against the sound though the
//       picture keeps its plan, which moves the sound and then moves it back (a: seeks).
// WHY-THIS-SHAPE: draw holds, as the steering times a round from its start; each hold's seeks counted for
//       700 ms. Whether (a) staged its case is read from the commits: the picture after the held one must
//       come under an interval late against the line through the commits before, and over 20 ms, or (a) is
//       not verified. No sound-server guard: with no clock, the server's timing reaches no decision.
// COVERAGE-ASSERT: both holds spent; at least 20 of AudioOutput's own timing reads hidden while they ran.
// CONTROL: control_batch.sh 2026-10-13, reverted, steerSoundLocked, backend/daemon/wallpaper_manager.cpp: the
//       move made `overdue - behind > (clock ? kSoundDrift : kCatchUpHold)` (a 100 ms limit with no clock) ->
//       "FAIL: no clock lead rule: a picture drawn more than an interval late moves the sound once" (0 seeks); `overdue` taken as `now - planned.timeline` -> "FAIL: no clock lead rule: a picture
//       drawn late inside its interval does not move the sound" (2 seeks: moved, then moved back). Green on both
//       entries: 0 and 1, the picture after the 60 ms hold +26.8 ms (+27.2 on the RTX). Test-side: the first hold
//       made 100 ms -> "NOT VERIFIED -- S15.584 (a picture drawn inside its interval)" (66.8 ms), not a FAIL.
void runNoClockLeadChecks() {
    const char* kGoals = "S15.584 (no clock: moved past kSoundDrift, against where the picture is shown)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    StallReset reset;
    Daemon d(smallConfig());
    const std::string file = asset("wallpaper_beeps_long.mp4");
    if (!startHeard(d, file, kGoals)) return;
    std::this_thread::sleep_for(1500ms);
    probe::g_hideStreamTime.store(true);
    std::this_thread::sleep_for(300ms);
    const int hiddenBefore = probe::g_streamTimesHidden.load();
    struct Held {
        int64_t at = 0;
        bool spent = false;
        std::vector<probe::SoundSeek> seeks;
    };
    const auto holdDraw = [](int ms) {
        Held h;
        h.at = probe::nowNs();
        probe::g_blitHoldMs.store(ms);
        std::this_thread::sleep_for(700ms);
        h.spent = probe::g_blitHoldMs.exchange(0) == 0;
        h.seeks = soundSeeksSince(h.at, probe::nowNs());
        return h;
    };
    const Held within = holdDraw(60);
    const Held past = holdDraw(90);
    const int hidden = probe::g_streamTimesHidden.load() - hiddenBefore;
    probe::g_hideStreamTime.store(false);
    const std::vector<Shown> shown = shownPictures(d.takeCommits(), file);
    const int64_t interval = fileInterval(file);
    std::string why;
    const std::optional<Lateness> late = lateAfter(shown, within.at, within.at + 300 * kMs, interval, why);
    std::printf("audio_playback_test: no clock lead rule: a 60 ms held draw: %zu sound seeks, the picture after it %+.1f ms "
                "against the line; a 90 ms held draw: %zu sound seeks; %d timing reads hidden\n",
                within.seeks.size(), late ? late->second / 1e6 : -1.0, past.seeks.size(), hidden);
    check(within.spent && past.spent && hidden >= 20, "no clock lead rule: both draw holds were reached with the timing hidden",
          fmt("spent %.0f and %.0f, %.0f reads hidden", within.spent ? 1.0 : 0.0, past.spent ? 1.0 : 0.0, hidden));
    if (!within.spent || !past.spent || hidden < 20) return;
    check(past.seeks.size() == 1, "no clock lead rule: a picture drawn more than an interval late moves the sound once",
          fmt("%.0f sound seeks", static_cast<double>(past.seeks.size())));
    if (!late) {
        notVerified("S15.584 (a picture drawn inside its interval)", why);
    } else if (late->second >= interval || late->second <= 20 * kMs) {
        notVerified("S15.584 (a picture drawn inside its interval)",
                    fmt("the picture after the 60 ms hold came %.1f ms late, not between 20 ms and an interval", late->second / 1e6));
    } else {
        check(within.seeks.empty(), "no clock lead rule: a picture drawn late inside its interval does not move the sound",
              fmt("%.0f sound seeks", static_cast<double>(within.seeks.size())));
    }
}

// GOAL: S15.585 -- a loop's restart is not counted as decode load: the silent two-second B-frame file at
//       29 ms a picture (about 0.9 of its interval), its first picture of each pass costing 120 ms (a restart's
//       seek and key picture), never skips a picture over 7 s. False if a pass's first decode is sampled: one
//       capped sample of 2 lifts an average of 0.9 past 1 at every loop, and the file skips after each.
// WHY-THIS-SHAPE: the first picture's cost is a decode hold keyed on its presentation time, repeated each
//       pass, so it lands on that picture however many the decoder returned before; a cycle of costs counted
//       by picture would drift off it. Silent, so every gap the decoder leaves is a skip.
// COVERAGE-ASSERT: at least 150 pictures handed on and three pass starts held in the window.
// CONTROL: control_batch.sh 2026-10-13, reverted, measureDecode in advanceSessionLocked,
//       backend/daemon/wallpaper_manager.cpp: `if (after.startsPass) return;` deleted -> "FAIL: loop
//       restart rule: a slow restart at each loop does not turn skipping on" (18 skipped; green 0 of 202-203 on
//       both entries). Test-side: no pass-start hold -> "FAIL: loop restart rule: the file looped and went on
//       decoding" (0 held).
void runLoopRestartLoadChecks() {
    const char* kGoals = "S15.585 (a loop's restart is not decode load)";
    StallReset reset;
    const std::string file = asset("wallpaper_reads_bframes.mp4");
    costFile(file);
    Daemon d(smallConfig());
    resetCapture();
    d.play(file, std::nullopt);
    if (!d.waitForCommits(3, 15s)) {
        notVerified(kGoals, "the video drew no pictures within 15 s");
        return;
    }
    std::optional<int64_t> first;
    for (const probe::PictureRecord& p : picturesOf(file)) {
        if (p.presentationNs && (!first || *p.presentationNs < *first)) first = p.presentationNs;
    }
    if (!first) {
        notVerified(kGoals, "no picture of the file had a presentation time");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(probe::g_holdMutex);
        probe::g_holdFile = std::filesystem::path(file).filename().string();
    }
    probe::g_holdAtNs.store(*first);
    probe::g_holdRepeats.store(true);
    probe::g_holdMs.store(91);
    probe::g_costMs.store(29);
    std::this_thread::sleep_for(1000ms);
    const int64_t from = probe::nowNs();
    probe::g_holdStartedAt.store(0);
    int holds = 0;
    int64_t lastHold = 0;
    const auto until = Clock::now() + 7000ms;
    while (Clock::now() < until) {
        if (const int64_t h = probe::g_holdStartedAt.load(); h != 0 && h != lastHold) {
            lastHold = h;
            ++holds;
        }
        std::this_thread::sleep_for(20ms);
    }
    const HandedOn h = handedOn(file, from, probe::nowNs());
    probe::g_holdMs.store(0);
    probe::g_costMs.store(0);
    std::printf("audio_playback_test: loop restart rule: %d pictures handed on in 7 s, %d skipped over; %d pass starts held\n",
                h.decoded, h.skipped, holds);
    check(h.decoded >= 150 && holds >= 3 && h.interval > 0, "loop restart rule: the file looped and went on decoding",
          fmt("%.0f handed on, %.0f pass starts held", h.decoded, holds));
    if (h.decoded < 150 || holds < 3 || h.interval == 0) return;
    check(h.skipped == 0, "loop restart rule: a slow restart at each loop does not turn skipping on",
          fmt("%.0f skipped", h.skipped));
}

// GOAL: S15.567 -- a picture the daemon drops is never converted: over five 60 ms stalls of the heard
//       120-a-second tone, conversions beyond the pictures committed stay under half the pictures
//       dropped. False if every decoded picture is converted, dropped or not.
// WHY-THIS-SHAPE: a conversion is FrameConverter's one draw, interposed at vkCmdDraw (three vertices, one
//       instance), and nothing else here draws that. A picture converted while it waits and dropped once it
//       falls behind is still converted, which the fixed code does for a few; so the bound is half the
//       dropped count, not zero.
// COVERAGE-ASSERT: at least five pictures dropped, and the conversions counted at least the commits (less
//       two at the window's edges), so the counter sees every shown picture's conversion.
// CONTROL: control_run.sh 2026-10-12, reverted: `convert();` added after `m_unconverted = std::move(*frame);` in
//       renderer/video/video_pipeline.cpp -> "FAIL: convert rule: a dropped picture is not converted" (420
//       conversions for 389 commits and 31 dropped; 394 for 389 green).
void runConvertDroppedChecks() {
    const char* kGoals = "S15.567 (a dropped picture is never converted)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    StallReset reset;
    Daemon d(smallConfig());
    const std::string file = asset("wallpaper_tone_rate120.mp4");
    if (!startHeard(d, file, kGoals)) return;
    std::this_thread::sleep_for(1500ms);
    const size_t before = d.takeCommits().size();
    const int conversionsBefore = probe::g_conversions.load();
    for (int i = 0; i < 5; ++i) {
        probe::g_stallPictureMs.store(60);
        std::this_thread::sleep_for(700ms);
    }
    const std::vector<Commit> all = d.takeCommits();
    const int conversions = probe::g_conversions.load() - conversionsBefore;
    const int commits = static_cast<int>(all.size() - before);
    const std::vector<Shown> shown = shownPictures(all, file);
    const int64_t interval = pictureInterval(shown);
    int dropped = 0;
    for (size_t i = before + 1; i < shown.size(); ++i) {
        if (shown[i].presentation && shown[i - 1].presentation) {
            dropped += skippedBetween(*shown[i - 1].presentation, *shown[i].presentation, interval);
        }
    }
    std::printf("audio_playback_test: convert rule: %d conversions, %d commits, %d pictures dropped\n", conversions, commits,
                dropped);
    check(dropped >= 5 && conversions + 2 >= commits,
          "convert rule: the stalls left pictures to drop, and every picture shown was seen converted",
          fmt("%.0f dropped, %.0f conversions for %.0f commits", dropped, conversions, commits));
    if (dropped < 5 || conversions + 2 < commits) return;
    check(conversions - commits < dropped / 2, "convert rule: a dropped picture is not converted",
          fmt("%.0f conversions for %.0f commits and %.0f dropped", conversions, commits, dropped));
}

// GOAL: S15.568 -- the harness reads its records right: a picture committed twice in a row stays in its
//       pass and counts no skip; a pass is as long as the file's pictures the decoder handed on, the last
//       included though no commit showed it; the sound server's longest gap counts a gap across either edge
//       of its window; and gradeBeepsAgainstPictures grades a beep picture committed twice in one pass once,
//       and counts it as no pass start. False if these helpers misread the synthetic records below, which
//       would mis-grade every phase built on them (the beep, loop, lead and catch-up rules).
// GOAL: S15.570 -- on a file whose decoder skips pictures (every third handed on), the pictures not shown
//       are counted as skipped, never negative, and a pass is still one file long. False with the interval
//       taken as the commits' median step, three pictures there, which counts each one-picture step -1.
// WHY-THIS-SHAPE: synthetic records, so each case is one the live phases cannot stage on demand: a repeat
//       commit, a dropped last picture, a server stall straddling a window's edge, a repeated beep picture,
//       a decoder that started skipping. The beep capture is step pulses at 0.5, far above the 0.1 onset
//       level, at each beep picture's place on the line, so it tests the grader's pairing, not onset finding.
// CONTROL: control_run.sh 2026-10-12, reverted, test-side, each alone: filePlaces' `p < before` made `p <= before` ->
//       "...: a picture committed again stays in its pass ..."; the pass length taken from the commits' latest
//       picture -> "...: a pass is as long as the file's pictures ..."; longestBufferGap skipping the buffer before
//       `from` -> "...: a sound-server stall across a window's start is measured whole"; its trailing gap to `to`
//       deleted -> "...: ... running past a window's end is measured to that end". control_batch.sh 2026-10-13,
//       reverted, test-side: gradeBeepsAgainstPictures' skip of a picture committed again deleted -> "...: a beep
//       picture committed twice in one pass is graded once, and is no pass start" (7 graded, 2 at a pass start);
//       filePlaces' interval taken from pictureInterval(shown) -> "...: pictures a skipping decoder never handed on
//       count as skipped" (-8) and "...: a skipping file's pass is one file long" (2066.7 ms).
void runGraderSelfTestChecks() {
    const std::string file = "/grader-self-test/pictures.mp4";
    constexpr int64_t kPicture = 1'000'000'000 / 30;
    {
        std::lock_guard<std::mutex> lock(probe::g_pictureMutex);
        for (int k = 0; k < 60; ++k) probe::g_pictures.push_back({0, file, k * kPicture});
    }
    std::vector<Shown> shown;
    int64_t at = 0;
    for (const int k : {0, 1, 2, 2, 3, 58, 0, 1}) shown.push_back({Commit{0, at += kPicture, 0}, k * kPicture});
    std::string why;
    const std::optional<FilePlace> places = filePlaces(shown, file, why);
    {
        std::lock_guard<std::mutex> lock(probe::g_pictureMutex);
        std::erase_if(probe::g_pictures, [&](const probe::PictureRecord& p) { return p.file == file; });
    }
    check(places.has_value(), "grader self-test: the synthetic run is read", why);
    if (places) {
        check(places->passes == std::vector<int>{0, 0, 0, 0, 0, 0, 1, 1},
              "grader self-test: a picture committed again stays in its pass; only a time going back starts one");
        check(places->dropped == 54, "grader self-test: a picture committed again counts no skipped picture",
              fmt("%.0f skipped, 54 wanted", places->dropped));
        check(places->at[6] == 60 * kPicture,
              "grader self-test: a pass is as long as the file's pictures, its last one included though never shown",
              fmt("the second pass starts %.1f ms in, 2000 wanted", places->at[6] / 1e6));
    }
    const int64_t from = 1000 * kMs, to = from + 100 * kMs;
    Capture across;
    for (const int64_t t : {-50, 10, 31, 52, 73, 94, 115}) across.buffers.push_back({from + t * kMs});
    check(longestBufferGap(across, from, to) == 60 * kMs,
          "grader self-test: a sound-server stall across a window's start is measured whole",
          fmt("%.1f ms, 60 wanted", longestBufferGap(across, from, to) / 1e6));
    Capture ending;
    for (const int64_t t : {0, 10, 20}) ending.buffers.push_back({from + t * kMs});
    check(longestBufferGap(ending, from, to) == 80 * kMs,
          "grader self-test: a sound-server stall running past a window's end is measured to that end",
          fmt("%.1f ms, 80 wanted", longestBufferGap(ending, from, to) / 1e6));

    // Two passes of a 60-picture file, picture 0 committed twice at the start of the first, a beep heard
    // exactly where each beep picture falls: six beeps, one at a pass start after a loop.
    const std::string beeps = "/grader-self-test/beeps.mp4";
    {
        std::lock_guard<std::mutex> lock(probe::g_pictureMutex);
        for (int k = 0; k < 60; ++k) probe::g_pictures.push_back({0, beeps, k * kPicture});
    }
    constexpr int64_t kBase = 10'000 * kMs;
    std::vector<Shown> run;
    run.push_back({Commit{0, kBase, 0}, 0});
    run.push_back({Commit{0, kBase + kPicture / 2, 0}, 0});
    for (int k = 1; k < 60; ++k) run.push_back({Commit{0, kBase + k * kPicture, 0}, k * kPicture});
    for (int k = 0; k <= 20; ++k) run.push_back({Commit{0, kBase + (60 + k) * kPicture, 0}, k * kPicture});
    Capture heardBeeps;
    constexpr uint32_t kFrames = 480;
    for (int64_t heardAt = kBase - 500 * kMs; heardAt < kBase + 3500 * kMs; heardAt += 10 * kMs) {
        const uint64_t first = heardBeeps.left.size();
        heardBeeps.buffers.push_back({heardAt, heardAt, first, kFrames, 0.5f});
        for (uint32_t f = 0; f < kFrames; ++f) {
            const int64_t t = heardAt + static_cast<int64_t>(f) * 1'000'000'000 / kRate - kBase;
            const float v = t >= 0 && t % (500 * kMs) < 40 * kMs ? 0.5f : 0.0f;
            heardBeeps.left.push_back(v);
            heardBeeps.right.push_back(v);
        }
    }
    int passStarts = 0;
    const int graded = gradeBeepsAgainstPictures("grader self-test", heardBeeps, run, beeps, kBase - kMs, 1.0, passStarts);
    check(graded == 6 && passStarts == 1,
          "grader self-test: a beep picture committed twice in one pass is graded once, and is no pass start",
          fmt("%.0f graded (6 wanted), %.0f at a pass start (1 wanted)", graded, passStarts));

    // A decoder that skips: pictures 0-8, then every third from 11, then a new pass at 0 and 3.
    const std::string skips = "/grader-self-test/skips.mp4";
    std::vector<int> handed;
    for (int k = 0; k <= 8; ++k) handed.push_back(k);
    for (int k = 11; k < 60; k += 3) handed.push_back(k);
    handed.push_back(0);
    handed.push_back(3);
    std::vector<Shown> skipping;
    {
        std::lock_guard<std::mutex> lock(probe::g_pictureMutex);
        int64_t t = 0;
        for (const int k : handed) {
            probe::g_pictures.push_back({t += kPicture, skips, k * kPicture});
            skipping.push_back({Commit{0, t + 1, 0}, k * kPicture});
        }
    }
    std::string skipWhy;
    const std::optional<FilePlace> skipPlaces = filePlaces(skipping, skips, skipWhy);
    {
        std::lock_guard<std::mutex> lock(probe::g_pictureMutex);
        std::erase_if(probe::g_pictures, [&](const probe::PictureRecord& p) { return p.file == beeps || p.file == skips; });
    }
    check(skipPlaces.has_value(), "grader self-test: the skipping run is read", skipWhy);
    if (skipPlaces) {
        check(skipPlaces->dropped == 36, "grader self-test: pictures a skipping decoder never handed on count as skipped",
              fmt("%.0f skipped, 36 wanted", skipPlaces->dropped));
        check(skipPlaces->at[handed.size() - 2] == 60 * kPicture,
              "grader self-test: a skipping file's pass is one file long",
              fmt("the second pass starts %.1f ms in, 2000 wanted", skipPlaces->at[handed.size() - 2] / 1e6));
    }
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
                              "wallpaper_reads_bframes.mp4", "sound_16384_frames.wav",
                              "wallpaper_beeps_bframes.mp4"};
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
        {"engineReconnect", &runEngineReconnectChecks},
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
        {"driftSlowCard", &runDriftSlowCardChecks},
        {"driftFastCard", &runDriftFastCardChecks},
        {"serverStop", &runServerStopChecks},
        {"slowLoopWakes", &runSlowLoopWakeChecks},
        {"dropSlice", &runDropSliceChecks},
        {"catchUpCap", &runCatchUpCapChecks},
        {"firstPictureKept", &runLoopFirstPictureChecks},
        {"leadLimit", &runLeadLimitChecks},
        {"noClock", &runNoClockChecks},
        {"handOverLead", &runHandOverLeadChecks},
        {"cleanCredit", &runCleanCreditChecks},
        {"lastReading", &runLastReadingChecks},
        {"dropMinimum", &runDropMinimumChecks},
        {"skip", &runSkipChecks},
        {"lateAtLoop", &runLateAtLoopChecks},
        {"lateTimed", &runLateTimedChecks},
        {"hover", &runHoverChecks},
        {"speedLoad", &runSpeedLoadChecks},
        {"holdRunOut", &runHoldRunOutChecks},
        {"lateHandOn", &runLateHandOnChecks},
        {"firstSlowDecodes", &runFirstSlowDecodesChecks},
        {"noClockLead", &runNoClockLeadChecks},
        {"loopRestartLoad", &runLoopRestartLoadChecks},
        {"convertDropped", &runConvertDroppedChecks},
        {"graderSelfTest", &runGraderSelfTestChecks},
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
