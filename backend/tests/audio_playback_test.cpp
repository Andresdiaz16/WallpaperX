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

// Every change of a picture decoder's skip_frame, seen as it is sent a packet: when, for which file, to
// which AVDISCARD value, and whether that packet was a key packet. Under g_pictureMutex.
struct LevelChange {
    int64_t at = 0;
    std::string file;
    int discard = 0;
    bool keyPacket = false;
};
std::vector<LevelChange> g_levelChanges;
std::map<const AVCodecContext*, int> g_lastDiscard;
// Packets sent per file to a picture decoder with a device (hw_device_ctx set) and without. Under g_pictureMutex.
std::map<std::string, int> g_deviceSends, g_softwareSends;

// Non-zero: every stream of the engine's is given this rate correction (pw_stream_set_rate), so the
// server takes its frames at 48000 / rate a second, as a sound card off its nominal rate would.
std::atomic<double> g_streamRate{0.0};

// Set: pw_stream_get_time_n reports `now` as 0, as for a stream the server has not timed yet, so no
// stream has a clock; and how many of production's reports were hidden. record() reads the time too,
// under t_inRecord, which is not counted.
std::atomic<bool> g_hideStreamTime{false};
std::atomic<int> g_streamTimesHidden{0};
// Set: the next sound seek to under 50 ms of the file (a hand-over to a new pass) sets g_hideStreamTime,
// and records when.
std::atomic<bool> g_hideAtHandOver{false};
std::atomic<int64_t> g_hiddenAtHandOverAt{0};
thread_local bool t_inRecord = false;

// Every picture a picture decoder returns for a file whose name holds g_costFile costs g_costMs more,
// spent where avcodec_receive_frame returns it, so a picture skipped undecoded costs nothing. While
// g_costCycle holds entries, each such picture takes the next one instead, round and round.
std::mutex g_costMutex;
std::string g_costFile;
std::vector<int> g_costCycle;
size_t g_costCycleAt = 0;
std::atomic<int> g_costMs{0};
// Non-zero: the next sound seek to a pass's start (isPassStartSeek: a target 99 ms or more before the file's
// start, as the decoder's pre-roll puts it) sets g_costMs to this, and records when.
std::atomic<int> g_costAtHandOver{0};
std::atomic<int64_t> g_costAtHandOverAt{0};
// Set: the next picture a decoder on the device would return for g_costFile fails with AVERROR_EXTERNAL
// instead, as a hardware decoder failing on a healthy device does; cleared, and the time stored, when it fires.
std::atomic<bool> g_failDeviceOnce{false};
std::atomic<int64_t> g_failedDeviceAt{0};

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
// Non-zero: when a hold above ends, the next pass's first picture (presentation 0) of the same file is held
// this long, once, as it is returned; and when that hold ended.
std::atomic<int> g_thenStartHoldMs{0};
std::atomic<bool> g_startHoldArmed{false};
std::atomic<int64_t> g_startHoldEndedAt{0};
// Non-negative: when that start hold ends, the same file's picture at this presentation time unhides the
// stream time (g_hideStreamTime) as it is returned, once, then is held 50 ms; and when it was unhidden.
std::atomic<int64_t> g_unhideAtNs{-1};
std::atomic<bool> g_unhideArmed{false};
std::atomic<int64_t> g_unhiddenAt{0};

// Draws of three vertices and one instance: FrameConverter's conversion pass, and nothing else this
// binary runs.
std::atomic<int> g_conversions{0};
// When each vkCmdBlitImage was recorded: VideoRenderer showing a converted picture, the one blit here.
std::mutex g_blitMutex;
std::vector<int64_t> g_blits;
// The next vkCmdBlitImage sleeps this long first, once: a draw held up outside any decode.
std::atomic<int> g_blitHoldMs{0};
// The next conversion pass (vkCmdDraw above) sleeps g_convertHoldMs first, once, and records when: a delay
// after a round has steered its sound and before it draws. A picture stall that ends arms it with
// g_convertHoldAfterStallMs, so it lands in the round the stalled read belongs to.
std::atomic<int> g_convertHoldMs{0};
std::atomic<int> g_convertHoldAfterStallMs{0};
std::atomic<int64_t> g_convertHeldAt{0};

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
    if (vertices == 3 && instances == 1) {
        probe::g_conversions.fetch_add(1);
        if (const int ms = probe::g_convertHoldMs.exchange(0); ms > 0) {
            probe::g_convertHeldAt.store(probe::nowNs());
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        }
    }
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

// Whether a sound seek's target is a pass's start. The sound decoder seeks 100 ms, plus the codec's own
// pre-roll, before the place it wants, so only a wanted place under 1 ms reads at -99 ms or earlier; a
// bound such as "under 50 ms" would take a move to anywhere in a pass's first 150 ms for its start.
bool isPassStartSeek(int64_t targetNs) { return targetNs <= -99'000'000; }

extern "C" int avformat_seek_file(AVFormatContext* s, int index, int64_t min, int64_t ts, int64_t max, int flags) {
    using Fn = int (*)(AVFormatContext*, int, int64_t, int64_t, int64_t, int);
    static const Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "avformat_seek_file"));
    if (s != nullptr && index >= 0 && static_cast<unsigned>(index) < s->nb_streams &&
        s->streams[index]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
        const int64_t target = av_rescale_q(ts, s->streams[index]->time_base, AVRational{1, 1'000'000'000});
        if (isPassStartSeek(target) && probe::g_hideAtHandOver.exchange(false)) {
            probe::g_hideStreamTime.store(true);
            probe::g_hiddenAtHandOverAt.store(probe::nowNs());
        }
        if (isPassStartSeek(target)) {
            if (const int ms = probe::g_costAtHandOver.exchange(0); ms != 0) {
                probe::g_costMs.store(ms);
                probe::g_costAtHandOverAt.store(probe::nowNs());
            }
        }
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
            if (const int hold = probe::g_convertHoldAfterStallMs.exchange(0); hold > 0) probe::g_convertHoldMs.store(hold);
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
        // A context not seen before starts at AVDISCARD_DEFAULT, which is no change.
        const auto last = probe::g_lastDiscard.find(avctx);
        const int before = last != probe::g_lastDiscard.end() ? last->second : AVDISCARD_DEFAULT;
        if (avctx->skip_frame != before) {
            const auto file = probe::g_contextFile.find(avctx);
            probe::g_levelChanges.push_back({probe::nowNs(), file != probe::g_contextFile.end() ? file->second : std::string(),
                                             avctx->skip_frame, (avpkt->flags & AV_PKT_FLAG_KEY) != 0});
        }
        probe::g_lastDiscard[avctx] = avctx->skip_frame;
        if (const auto file = probe::g_contextFile.find(avctx); file != probe::g_contextFile.end()) {
            ++(avctx->hw_device_ctx != nullptr ? probe::g_deviceSends : probe::g_softwareSends)[file->second];
        }
    }
    return real(avctx, avpkt);
}

// A freed context's address is reused by later decoders, in this phase or the next: its skip level and file
// go with it, so a new decoder neither inherits a level change nor sends under a dead decoder's file.
extern "C" void avcodec_free_context(AVCodecContext** avctx) {
    using Fn = void (*)(AVCodecContext**);
    static const Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "avcodec_free_context"));
    if (avctx != nullptr && *avctx != nullptr) {
        std::lock_guard<std::mutex> lock(probe::g_pictureMutex);
        probe::g_lastDiscard.erase(*avctx);
        probe::g_contextFile.erase(*avctx);
    }
    real(avctx);
}

extern "C" int avcodec_receive_frame(AVCodecContext* avctx, AVFrame* frame) {
    using Fn = int (*)(AVCodecContext*, AVFrame*);
    static const Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "avcodec_receive_frame"));
    // Cleared first: libavcodec's own moves inside the call must not count as a picture handed on.
    probe::t_videoFrame = nullptr;
    if (probe::g_failDeviceOnce.load() && avctx != nullptr && avctx->codec_type == AVMEDIA_TYPE_VIDEO &&
        avctx->hw_device_ctx != nullptr) {
        bool costed = false;
        {
            std::lock_guard<std::mutex> lock(probe::g_pictureMutex);
            const auto it = probe::g_contextFile.find(avctx);
            std::lock_guard<std::mutex> costLock(probe::g_costMutex);
            costed = it != probe::g_contextFile.end() && !probe::g_costFile.empty() &&
                     it->second.find(probe::g_costFile) != std::string::npos;
        }
        if (costed && probe::g_failDeviceOnce.exchange(false)) {
            probe::g_failedDeviceAt.store(probe::nowNs());
            return AVERROR_EXTERNAL;
        }
    }
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
        if ((probe::g_holdMs.load() > 0 || probe::g_startHoldArmed.load() || probe::g_unhideArmed.load()) &&
            frame != nullptr && frame->best_effort_timestamp != AV_NOPTS_VALUE && avctx->pkt_timebase.num > 0) {
            const int64_t at = av_rescale_q(frame->best_effort_timestamp, avctx->pkt_timebase, AVRational{1, 1'000'000'000});
            const bool atHold = probe::g_holdMs.load() > 0 && std::llabs(at - probe::g_holdAtNs.load()) < 2'000'000;
            const bool atStart = probe::g_startHoldArmed.load() && std::llabs(at) < 2'000'000;
            const bool atUnhide = probe::g_unhideArmed.load() && std::llabs(at - probe::g_unhideAtNs.load()) < 2'000'000;
            bool ofFile = false;
            if (atHold || atStart || atUnhide) {
                std::lock_guard<std::mutex> lock(probe::g_pictureMutex);
                const auto it = probe::g_contextFile.find(avctx);
                std::lock_guard<std::mutex> holdLock(probe::g_holdMutex);
                ofFile = it != probe::g_contextFile.end() && !probe::g_holdFile.empty() &&
                         it->second.find(probe::g_holdFile) != std::string::npos;
            }
            const bool held = atHold && ofFile;
            if (const int ms = !held ? 0 : probe::g_holdRepeats.load() ? probe::g_holdMs.load() : probe::g_holdMs.exchange(0);
                ms > 0) {
                probe::g_holdStartedAt.store(probe::nowNs());
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                probe::g_holdEndedAt.store(probe::nowNs());
                if (probe::g_thenStartHoldMs.load() > 0) probe::g_startHoldArmed.store(true);
            } else if (atStart && ofFile && probe::g_startHoldArmed.exchange(false)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(probe::g_thenStartHoldMs.load()));
                probe::g_startHoldEndedAt.store(probe::nowNs());
                if (probe::g_unhideAtNs.load() >= 0) probe::g_unhideArmed.store(true);
            } else if (atUnhide && ofFile && probe::g_unhideArmed.exchange(false)) {
                probe::g_hideStreamTime.store(false);
                probe::g_unhiddenAt.store(probe::nowNs());
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
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
        probe::g_levelChanges.clear();
        probe::g_deviceSends.clear();
        probe::g_softwareSends.clear();
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
// CONTROL: control_batch.sh 2026-10-14, reverted, phase heard:
//       `heard &&` deleted from SoundWanted::audible(), backend/daemon/wallpaper_manager.h -> "FAIL: heard rule,
//       before any focus report: the screen is silent" and the same for the other not-heard states; `heard` made
//       `!heard` -> also "FAIL: heard rule, desktop focused: the screen is heard". The decode line, in
//       heardTrackLocked (wallpaper_manager.cpp): the not-wanted branch keeping its track at volume 0 instead of
//       stopping it -> "FAIL: heard rule, focus none: nothing of its sound is decoded" (peak 0.0000, 27
//       swr_convert calls) and the same for the other not-heard states after a heard one.
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
//       for the hidden screen; in renderer/audio/audio_output.cpp service(), control_run.sh
//       2026-10-11, `setActive(t, false);` deleted -> "FAIL: stop rule,
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

void costFile(const std::string& file) {
    std::lock_guard<std::mutex> lock(probe::g_costMutex);
    probe::g_costFile = std::filesystem::path(file).filename().string();
}

// The changes of `file`'s picture decoder's skip level made in [from, to), in order.
std::vector<probe::LevelChange> levelChangesOf(const std::string& file, int64_t from, int64_t to) {
    const std::string name = std::filesystem::path(file).filename().string();
    std::vector<probe::LevelChange> out;
    std::lock_guard<std::mutex> lock(probe::g_pictureMutex);
    for (const probe::LevelChange& c : probe::g_levelChanges) {
        if (c.at >= from && c.at < to && c.file.find(name) != std::string::npos) out.push_back(c);
    }
    return out;
}

// How many of `file`'s packets went to a decoder on the device, and how many to software, so far.
std::pair<int, int> sendsOf(const std::string& file) {
    const std::string name = std::filesystem::path(file).filename().string();
    int device = 0, software = 0;
    std::lock_guard<std::mutex> lock(probe::g_pictureMutex);
    for (const auto& [f, n] : probe::g_deviceSends) {
        if (f.find(name) != std::string::npos) device += n;
    }
    for (const auto& [f, n] : probe::g_softwareSends) {
        if (f.find(name) != std::string::npos) software += n;
    }
    return {device, software};
}

// Whether every packet of `file` went to a decoder on the device (true) or every one to software (false);
// nothing when none was sent or both were, which no single decoder path answers.
std::optional<bool> decodedOnDevice(const std::string& file) {
    const auto [device, software] = sendsOf(file);
    if ((device == 0) == (software == 0)) return std::nullopt;
    return device > 0;
}

// `file`'s skip level as sent with the last packet before `at`: AVDISCARD_DEFAULT before any change.
int levelBefore(const std::string& file, int64_t at) {
    const std::vector<probe::LevelChange> changes = levelChangesOf(file, 0, at);
    return changes.empty() ? AVDISCARD_DEFAULT : changes.back().discard;
}

const char* discardName(int discard) {
    switch (discard) {
        case AVDISCARD_DEFAULT: return "every picture";
        case AVDISCARD_NONREF: return "unreferenced skipped";
        case AVDISCARD_NONINTRA: return "intra only";
        case AVDISCARD_NONKEY: return "key only";
        default: return "other";
    }
}

std::string describeLevels(const std::vector<probe::LevelChange>& changes, int64_t t0) {
    std::string out;
    for (const probe::LevelChange& c : changes) {
        out += (out.empty() ? "" : ", ") + std::string(discardName(c.discard)) + fmt(" at %+.0f ms", (c.at - t0) / 1e6);
    }
    return out.empty() ? "no change" : out;
}

// Sound seeks in [from, to) to anywhere but a pass's start: the sound moved in the middle of a pass. A
// seek to a pass's start reads as -100 ms, the sound decoder's pre-roll.
std::vector<probe::SoundSeek> midPassSeeks(int64_t from, int64_t to) {
    std::vector<probe::SoundSeek> out;
    for (const probe::SoundSeek& s : soundSeeksSince(from, to)) {
        if (!isPassStartSeek(s.targetNs)) out.push_back(s);
    }
    return out;
}

std::vector<Commit> commitsIn(const std::vector<Commit>& all, int64_t from, int64_t to) {
    std::vector<Commit> out;
    for (const Commit& c : all) {
        if (c.at >= from && c.at < to) out.push_back(c);
    }
    return out;
}

// When `file`'s decoder, after `after`, first hands on two pictures in a row each one interval on from the
// one before: a decoder that skips nothing. One such step alone is no proof, as the picture before each key
// picture is one step from it whatever is skipped.
std::optional<int64_t> everyPictureAgain(const std::string& file, int64_t after) {
    const int64_t interval = fileInterval(file);
    std::optional<int64_t> previous;
    int single = 0;
    for (const probe::PictureRecord& p : picturesOf(file)) {
        if (!p.presentationNs) continue;
        if (p.at > after && previous) {
            const int64_t step = *p.presentationNs - *previous;
            single = step > 0 && step < interval * 3 / 2 ? single + 1 : 0;
            if (single >= 2) return p.at;
        }
        previous = p.presentationNs;
    }
    return std::nullopt;
}

// The commits in `shown` that start a pass: whose picture's time goes back from the one before.
std::vector<int64_t> passStartCommits(const std::vector<Shown>& shown) {
    std::vector<int64_t> out;
    for (size_t i = 1; i < shown.size(); ++i) {
        if (shown[i].presentation && shown[i - 1].presentation && *shown[i].presentation < *shown[i - 1].presentation) {
            out.push_back(shown[i].commit.at);
        }
    }
    return out;
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
    int64_t passLength = 0;
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
    out.passLength = passLength;
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
// line's, when graded. Beeps scheduled from `watchFrom` up to `from` are printed, not graded. The beep
// at 0 of pass `firstLoopPass` of `shown` is graded on its own line against `firstLoopMs`.
int gradeBeepsAgainstPictures(const std::string& rule, const Capture& cap, const std::vector<Shown>& shown,
                              const std::string& file, int64_t from, double toleranceMs, int& passStartsGraded,
                              Line* line = nullptr, double nearMs = 2.0, int64_t watchFrom = INT64_MAX,
                              int firstLoopPass = -1, double firstLoopMs = 0.0) {
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
    // A picture up to `nearMs` from its beep stands for it, the beep's time moved along the line by that much;
    // each beep is graded once, keyed by its pass and place. A place that rounds to the pass's end is the
    // next pass's beep 0: the last picture of a fast file is within `nearMs` of it. The pass length is a sum
    // of whole-nanosecond picture steps, so it may fall a few nanoseconds short of the beep's place.
    std::set<std::pair<int, int64_t>> done;
    for (size_t i = 0; i < shown.size(); ++i) {
        const int64_t p = *shown[i].presentation;
        const int64_t rounded = (p + 250 * kMs) / (500 * kMs) * (500 * kMs);
        if (std::llabs(p - rounded) > static_cast<int64_t>(nearMs * 1e6)) continue;
        int pass = places->passes[i];
        int64_t beepAt = rounded;
        if (rounded + kMs >= places->passLength) {
            beepAt = 0;
            ++pass;
        }
        const int64_t scheduled = fitted.fitted[i] + static_cast<int64_t>(std::llround(fitted.slope * static_cast<double>(rounded - p)));
        // Marked done only once in the window: a beep whose first picture falls outside it is still graded on a
        // later picture that falls inside.
        if (scheduled < std::min(from, watchFrom) || scheduled + 250 * kMs > recordEnd) continue;
        if (!done.insert({pass, beepAt}).second) continue;
        std::optional<int64_t> nearest;
        for (const int64_t h : heard) {
            if (!nearest || std::llabs(h - scheduled) < std::llabs(*nearest - scheduled)) nearest = h;
        }
        const double error = nearest ? (*nearest - scheduled) / 1e6 : 1e9;
        if (scheduled < from) {
            std::printf("audio_playback_test: %s: the beep at %.1f s of pass %d heard %+.2f ms from its picture, %.0f ms after "
                        "the watch began (not graded)\n",
                        rule.c_str(), beepAt / 1e9, pass, error, (scheduled - watchFrom) / 1e6);
            continue;
        }
        std::printf("audio_playback_test: %s: the beep at %.1f s of pass %d heard %+.2f ms from its picture's scheduled "
                    "time\n",
                    rule.c_str(), beepAt / 1e9, pass, error);
        if (beepAt == 0 && pass == firstLoopPass) {
            check(std::fabs(error) <= firstLoopMs, rule + ": the first loop's opening beep is heard with its picture",
                  fmt("the beep at 0 s of pass %.0f: %+.2f ms, bound %.0f ms", static_cast<double>(pass), error, firstLoopMs));
        } else {
            check(std::fabs(error) <= toleranceMs, rule + ": each beep is heard with the picture at its timestamp",
                  fmt("the beep at %.1f s of pass %.0f: %+.2f ms", beepAt / 1e9, static_cast<double>(pass), error));
        }
        ++graded;
        if (beepAt == 0 && pass > 0) ++passStartsGraded;
    }
    return graded;
}

// GOAL: S15.495 -- a heard screen plays its sound at the wallpaper's speed, its pictures on it: at speeds 0.5,
//       2 and 1 in turn, each beep of the two-second beeps file is heard within the speed's tolerance of its
//       picture's place on the line through the commits (25 ms at 0.5: in the first ~2 s after a change to 0.5
//       a beep is heard 9 to 23 ms early, by where the stretch starts, accepted by the user 2026-10-15;
//       12 ms at 2 and 1), and that line runs at
//       the speed (the file's time a second within 2 % of it). False if the sound plays unstretched, or the
//       steering treats a stretched sound's clock as running at 1.
// WHY-THIS-SHAPE: one daemon, the speed changed in place, each speed graded from 1.5 s after its change, so
//       a change that only works from a fresh open is not what passes; all six changes the three speeds allow.
// COVERAGE-ASSERT: at least three beeps graded at each speed.
// CONTROL: control_batch.sh 2026-10-14, reverted, phase speed: heardTrackLocked's setRate(wanted.speed) made
//       setRate(1.0), backend/daemon/wallpaper_manager.cpp -> "FAIL: speed rule, speed 0.5: each beep is heard with the
//       picture at its timestamp" and "... the pictures play at the speed"; AudioTrack::Clock::heardAt ignoring the
//       rate, renderer/audio/audio_output.h -> "FAIL: speed rule, speed 0.5: each beep is heard with the picture at
//       its timestamp" and the same at 2. Beeps in the first 1.2 s after a change are printed, not graded: they
//       answer whether the sound recovers, measured -22 to +1.6 ms, the same as after it.
void runSpeedChecks() {
    const char* kGoals = "S15.495 (sound at the speed, on its pictures)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    Daemon d(smallConfig());
    const std::string file = asset("wallpaper_beeps.mp4");
    if (!startHeard(d, file, kGoals)) return;
    struct Case {
        double speed;
        double toleranceMs;
        int windowMs;
    };
    for (const Case c : {Case{0.5, 25, 6000}, Case{2.0, 12, 4000}, Case{1.0, 12, 4000}, Case{2.0, 12, 4000},
                         Case{0.5, 25, 6000}, Case{1.0, 12, 4000}}) {
        d.properties({{"speed", c.speed}});
        std::this_thread::sleep_for(300ms);
        const int64_t watchFrom = probe::nowNs();
        std::this_thread::sleep_for(std::chrono::milliseconds(1200 + c.windowMs));
        const int64_t to = probe::nowNs();
        const int64_t from = watchFrom + 1200 * kMs;
        const std::string rule = fmt("speed rule, speed %.1f", c.speed);
        Line line;
        int passStarts = 0;
        const int graded =
            gradeBeepsAgainstPictures(rule, takeCapture(), shownPictures(commitsIn(d.takeCommits(), watchFrom, to), file), file,
                                      from, c.toleranceMs, passStarts, &line, 2.0, watchFrom);
        if (graded < 0) continue;
        const double played = line.slope > 0 ? 1.0 / line.slope : 0.0;
        std::printf("audio_playback_test: %s: the pictures' line plays %.4f s of the file a second\n", rule.c_str(), played);
        check(graded >= 3, rule + ": at least three beeps were graded", fmt("%.0f", graded));
        check(std::fabs(played / c.speed - 1.0) < 0.02, rule + ": the pictures play at the speed",
              fmt("%.4f s of the file a second, wanted %.2f", played, c.speed));
    }
}

// Each committed picture that starts a beep set against the beep heard nearest that commit: for a run whose
// commits are too uneven for a line through them to stand for the sound (a slideshow, or a level that
// changes inside the window). A picture committed again is graded once. Returns how many were graded.
int gradeBeepsAgainstCommits(const std::string& rule, const Capture& cap, const std::vector<Shown>& shown, double toleranceMs) {
    const std::vector<int64_t> heard = onsets(cap, 0.1f, 480);
    const int64_t recordEnd =
        cap.buffers.empty() ? 0 : cap.heardAt(cap.buffers.back(), cap.buffers.back().first + cap.buffers.back().frames);
    int graded = 0;
    for (size_t i = 0; i < shown.size(); ++i) {
        if (!shown[i].presentation) continue;
        const int64_t p = *shown[i].presentation;
        if (i > 0 && shown[i - 1].presentation && p == *shown[i - 1].presentation) continue;
        if (std::llabs(p - (p + 250 * kMs) / (500 * kMs) * (500 * kMs)) > 2 * kMs) continue;
        const int64_t at = shown[i].commit.at;
        if (at + 250 * kMs > recordEnd) continue;
        std::optional<int64_t> nearest;
        for (const int64_t h : heard) {
            if (!nearest || std::llabs(h - at) < std::llabs(*nearest - at)) nearest = h;
        }
        const double error = nearest ? (*nearest - at) / 1e6 : 1e9;
        std::printf("audio_playback_test: %s: the beep at %.1f s heard %+.2f ms from the commit showing it\n", rule.c_str(),
                    p / 1e9, error);
        check(std::fabs(error) <= toleranceMs, rule + ": each beep is heard with the picture showing it",
              fmt("the beep at %.1f s: %+.2f ms", p / 1e9, error));
        ++graded;
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

// GOAL: S15.500 -- a video with no sound makes no PipeWire stream at all. False if a stream is made
//       before the file is known to have sound.
// CONTROL: control_run.sh 2026-10-11, reverted: `ensureStream(t);` put first in AudioOutput::begin,
//       renderer/audio/audio_output.cpp -> "FAIL: no-sound rule: a video with no sound makes no PipeWire
//       stream" (1 stream made).
void runNoSoundChecks() {
    const char* kGoals = "S15.500 (a silent file makes no stream)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    Daemon d(smallConfig());
    const int streamsBefore = probe::g_streamsMade.load();
    resetCapture();
    d.play(asset("wallpaper_bands.mp4"), ScreenFocus::Desktop);
    if (!d.waitForCommits(3, 15s)) {
        notVerified(kGoals, "the video drew no pictures within 15 s");
        return;
    }
    std::this_thread::sleep_for(1500ms);
    check(probe::g_streamsMade.load() == streamsBefore, "no-sound rule: a video with no sound makes no PipeWire stream",
          fmt("%.0f streams made", static_cast<double>(probe::g_streamsMade.load() - streamsBefore)));
}

// GOAL: S15.517 -- a heard video whose sound stream cannot be read makes no PipeWire
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

// GOAL: S15.501 -- when the sound server goes away the daemon says so in one line,
//       and once a server is back the sound returns by itself within about 5 s, before any loop could
//       restart it; one line says it is back. False if the sound waits for the next anchor (a loop, a
//       pause) to come back.
// GOAL: S15.513 -- and once back the beeps are heard in step with their pictures. False if the new stream inherits
//       the old one's playhead: the track is begun again under the request serial that playhead already
//       holds, so the callback never switches to the new segment, plays what was decoded before the loss,
//       and falls silent.
// WHY-THIS-SHAPE: the twelve-second fixture, lost about a second and a half in, so the retry five
//       seconds later falls well before the loop that would re-anchor the sound anyway; on a short
//       fixture a track that never retried would still come back at the loop and pass.
// CONTROL: control_run.sh 2026-10-11, reverted, in renderer/audio/audio_output.cpp: loseOutput's `dropStream(*t,
//       now)` given a retry at the end of time -> "FAIL: reconnect rule: the sound comes back by itself ..." (not
//       back); its m_log deleted -> "... losing the sound server is said in one line" (0); connect()'s "can be
//       reached again" line deleted -> "... its return is said in one line" (0). S15.513: destroyStream's `t.playhead = {};` deleted stays
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

// GOAL: S15.505 -- a change of volume moves the level over about 5 ms rather than in a step. False for a
//       gain set straight to its target, which clicks.
// WHY-THIS-SHAPE: the envelope is the largest sample over the 110 frames before each one -- one period
//       of the 440 Hz tone -- so a step drops it within a frame or two and a 240-frame ramp takes about
//       190 frames to fall from 90 % to 10 %.
// CONTROL: control_run.sh 2026-10-11, reverted: render()'s gain set straight to its target, in
//       renderer/audio/audio_output.cpp -> "FAIL: ramp rule: a volume change takes 2 to 10 ms, not a step" (18
//       frames).
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
    check(before >= 0.3f && at90 >= 0 && at10 >= 0, "ramp rule: the tone played and then fell after the change",
          fmt("level %.3f before; fell at %.0f, %.0f", before, at90, at10));
    if (before >= 0.3f && at90 >= 0 && at10 >= 0) {
        const int64_t span = at10 - at90;
        std::printf("audio_playback_test: ramp rule: the level fell from 90 %% to 10 %% over %lld frames (%.2f ms)\n",
                    static_cast<long long>(span), span * 1000.0 / kRate);
        check(span >= 96 && span <= 480, "ramp rule: a volume change takes 2 to 10 ms, not a step",
              fmt("%.0f frames", static_cast<double>(span)));
    }
}

// Each beep heard from `from` on, set against the clock: the readings either side of its onset, at one rate
// and each within 50 ms of it so no change of rate lies between, give the file time heard, and the beep is
// the nearest half second. 150 ms of quiet before each: a slow rate repeats a beep's tail after a short quiet.
struct ClockBeep {
    int64_t heardAt;
    double beep;   // ns into the file
    double offMs;  // heard after the clock's place for it, in wall time
    double rate;
};
std::vector<ClockBeep> beepsAgainstClock(const Capture& cap, const std::vector<AudioTrack::Clock>& clocks, int64_t from) {
    std::vector<ClockBeep> out;
    for (const int64_t h : onsets(cap, 0.1f, kRate * 15 / 100)) {
        if (h < from) continue;
        const AudioTrack::Clock* c = nullptr;
        const AudioTrack::Clock* after = nullptr;
        for (const AudioTrack::Clock& k : clocks) {
            if (ns(k.at) > h) {
                after = &k;
                break;
            }
            c = &k;
        }
        if (c == nullptr || after == nullptr || c->rate != after->rate || h - ns(c->at) > 50 * kMs ||
            ns(after->at) - h > 50 * kMs) {
            continue;
        }
        const double file = static_cast<double>(c->file.count()) + static_cast<double>(h - ns(c->at)) * c->rate;
        const double beep = std::round(file / 5e8) * 5e8;
        out.push_back({h, beep, (file - beep) / c->rate / 1e6, c->rate});
    }
    return out;
}

// `c` holding only the buffers queued before `before`.
Capture queuedBefore(const Capture& c, int64_t before) {
    Capture out = c;
    std::erase_if(out.buffers, [&](const probe::BufferRecord& b) { return b.queuedAt >= before; });
    return out;
}

// Over `right` from frame `from`: the largest second difference (a 440 Hz tone at 0.3 has about 0.001; a
// step between two stretches far more), and the lowest envelope -- the largest sample over the 110 frames
// before each, one period of the tone -- and the frame it is lowest at.
struct ClickAndDip {
    float click = 0.0f, dip = 1.0f;
    size_t dipAt = 0;
};
ClickAndDip clickAndDip(const std::vector<float>& right, size_t from) {
    ClickAndDip out;
    for (size_t i = from + 110; i + 1 < right.size(); ++i) {
        out.click = std::max(out.click, std::fabs(right[i + 1] - 2 * right[i] + right[i - 1]));
        float envelope = 0.0f;
        for (size_t j = i - 109; j <= i; ++j) envelope = std::max(envelope, std::fabs(right[j]));
        if (envelope < out.dip) {
            out.dip = envelope;
            out.dipAt = i;
        }
    }
    return out;
}

// GOAL: S15.593 -- a track's rate, changed while it plays, reaches its clock and its sound together and
//       smoothly: beeps_long played from 0 through every change the three speeds allow (1 -> 0.5, 0.5 -> 2,
//       2 -> 0.5, 0.5 -> 1, 1 -> 2, 2 -> 1), each new rate reported by the clock within 600 ms; every beep heard
//       within 40 ms of where the clock puts it; at every change (each one drains the stretch) the clock steps
//       at most 8 ms, 10 ms for a change leaving 0.5; and the right channel's tone, up to 100 ms before the
//       stop, has no gap over 10 ms, no dip (24 frames under 0.05, as S15.498's) and no click (second
//       difference under 0.02) near any change. False if the clock keeps the old rate past the change (or
//       takes the new one before the sound does), jumps at it, or the change seeks or cuts.
// WHY-THIS-SHAPE: driven without the daemon, polling clock() every 5 ms. The clock is published once a
//       server cycle (1024 frames, 21 ms), and where in the cycle the change fell is not known, so a step is
//       graded as how far a reading lies outside the span between the last one run on at the old rate and at
//       the new. The first new-rate reading against the old rate run on is printed, not graded: with no step
//       it already reads up to 21 ms times the change in rate (-34.75 ms measured at 2 -> 0.5), so it cannot
//       tell a forward jump from where in the cycle the change fell any better than the span can. A click is
//       looked for within 60 ms of the time the first new-rate reading speaks for, so one is tied to a change.
// COVERAGE-ASSERT: at least 200 clock readings, at least 20 after each change, every change found in the
//       readings and heard near, and at least ten beeps graded.
// CONTROL: control_batch.sh 2026-10-14, reverted, phase engineRate, renderer/audio/audio_output.cpp markRate()
//       adding 11 ms to the file time of a mark after one at 0.5 -> "FAIL: engine rate rule: a change leaving 0.5
//       steps the clock 10 ms at most" (13.03 and 11.12 ms) and "... a change steps the clock 8 ms at most" (13.91 ms,
//       a mark inside the 0.5 run). Adding 9 ms to every mark after the first did NOT go red (steps under 3.2 ms):
//       a step inside the run-on span is not visible. 9 ms added to a mark at the same rate as the one before
//       (control_batch.sh 2026-10-15) -> "... a change steps the clock 8 ms at most" and the 10 ms line.
//       fadeIn() crossfading from the old run negated (audio_decoder.cpp) -> "... a change of rate does not click".
//       control_run.sh 2026-10-15: render()'s publishClock giving `file` + 50 ms (audio_output.cpp) -> "... every beep is
//       heard where the clock puts it" (93.24 ms); test-side, 1500 frames of the captured tone zeroed -> "... a
//       change of rate leaves no gap and no dip in the tone" (1 gap, 1 dip).
void runEngineRateChecks() {
    const char* kGoals = "S15.593 (a track's rate changed in place)";
    const std::string rule = "engine rate rule";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    resetCapture();
    Engine engine(kGoals);
    if (!engine.output) return;
    std::unique_ptr<AudioTrack> track = engine.output->track(asset("wallpaper_beeps_long.mp4"), "engine");
    const int64_t t0 = probe::nowNs() + 400 * kMs;
    track->play(anchorAt(0, t0));
    struct Change {
        double rate;
        int64_t offsetMs;
        int64_t at = 0;
    };
    // Every change the daemon's three speeds can ask for: 1 -> 0.5, 0.5 -> 2, 2 -> 0.5, 0.5 -> 1, 1 -> 2, 2 -> 1.
    std::vector<Change> changes = {{0.5, 1500}, {2.0, 3500}, {0.5, 5000}, {1.0, 7000}, {2.0, 8500}, {1.0, 9500}};
    std::vector<std::pair<int64_t, AudioTrack::Clock>> readings;
    const int64_t end = t0 + 11000 * kMs;
    size_t next = 0;
    while (probe::nowNs() < end) {
        const int64_t now = probe::nowNs();
        if (next < changes.size() && now >= t0 + changes[next].offsetMs * kMs) {
            changes[next].at = now;
            track->setRate(changes[next++].rate);
        }
        if (const std::optional<AudioTrack::Clock> c = track->clock(Clock::now())) readings.push_back({now, *c});
        std::this_thread::sleep_for(5ms);
    }
    track->stop();
    std::this_thread::sleep_for(200ms);
    const Capture cap = takeCapture();
    check(readings.size() >= 200, rule + ": the clock was read while the track played",
          fmt("%.0f readings", static_cast<double>(readings.size())));
    if (readings.size() < 200) return;
    for (const Change& c : changes) {
        std::optional<int64_t> reached;
        for (const auto& [now, clock] : readings) {
            if (now >= c.at && clock.rate == c.rate) {
                reached = now;
                break;
            }
        }
        std::printf("audio_playback_test: %s: rate %.1f asked at %+.0f ms, on the clock %.0f ms later\n", rule.c_str(), c.rate,
                    (c.at - t0) / 1e6, reached ? (*reached - c.at) / 1e6 : -1.0);
        check(reached && *reached - c.at <= 600 * kMs, rule + ": a rate asked for reaches the clock within 600 ms",
              fmt("rate %.1f: ", c.rate) + (reached ? fmt("%.0f ms", (*reached - c.at) / 1e6) : std::string("never")));
    }
    // The distinct readings, by the time they speak for.
    std::vector<AudioTrack::Clock> clocks;
    for (const auto& [now, clock] : readings) {
        if (clocks.empty() || clock.at != clocks.back().at) clocks.push_back(clock);
    }
    double worstStep = 0;
    size_t worstAt = 0;
    // The largest step after each change, up to the next: changeStep[k] for changes[k].
    std::vector<double> changeStep(changes.size(), 0.0);
    std::vector<int> changeSteps(changes.size(), 0);
    for (size_t i = 1; i < clocks.size(); ++i) {
        const AudioTrack::Clock& a = clocks[i - 1];
        const AudioTrack::Clock& b = clocks[i];
        const double dt = static_cast<double>((b.at - a.at).count());
        const double lo = static_cast<double>(a.file.count()) + dt * std::min(a.rate, b.rate);
        const double hi = static_cast<double>(a.file.count()) + dt * std::max(a.rate, b.rate);
        const double f = static_cast<double>(b.file.count());
        const double step = f < lo ? lo - f : f > hi ? f - hi : 0.0;
        if (step > 4e6) {
            std::printf("  the clock stepped %.2f ms %+.0f ms in (rate %.2f then %.2f)\n", step / 1e6, (ns(b.at) - t0) / 1e6,
                        a.rate, b.rate);
        }
        if (step > worstStep) {
            worstStep = step;
            worstAt = i;
        }
        for (size_t k = changes.size(); k-- > 0;) {
            if (ns(b.at) >= changes[k].at) {
                changeStep[k] = std::max(changeStep[k], step);
                ++changeSteps[k];
                break;
            }
        }
    }
    // The step at each change: the first reading at the new rate against the last at the old rate run on at
    // the old rate. Changes are found in the readings in order, so 1 -> 2 and 2 -> 1 are not confused with
    // earlier changes between the same rates.
    std::vector<std::optional<double>> junction(changes.size());
    std::vector<int64_t> heardFrom(changes.size(), 0);
    {
        size_t i = 0;
        for (size_t k = 0; k < changes.size(); ++k) {
            const double was = k == 0 ? 1.0 : changes[k - 1].rate;
            while (i < clocks.size() && !(ns(clocks[i].at) >= changes[k].at - 50 * kMs && clocks[i].rate == changes[k].rate)) ++i;
            if (i == 0 || i >= clocks.size() || clocks[i - 1].rate != was) continue;
            const AudioTrack::Clock& a = clocks[i - 1];
            const AudioTrack::Clock& b = clocks[i];
            const double ranOn = static_cast<double>(a.file.count()) + static_cast<double>((b.at - a.at).count()) * a.rate;
            junction[k] = (static_cast<double>(b.file.count()) - ranOn) / 1e6;
            heardFrom[k] = ns(b.at);
        }
    }
    for (size_t k = 0; k < changes.size(); ++k) {
        const double was = k == 0 ? 1.0 : changes[k - 1].rate;
        const double bound = was == 0.5 ? 10.0 : 8.0;
        std::printf("audio_playback_test: %s: rate %.1f -> %.1f: the first new reading %s ms from the old rate run on "
                    "(printed); the clock at most %.2f ms outside its run-on span over %d readings after it (bound %.0f ms)\n",
                    rule.c_str(), was, changes[k].rate, junction[k] ? fmt("%+.2f", *junction[k]).c_str() : "(not found)",
                    changeStep[k] / 1e6, changeSteps[k], bound);
        check(changeSteps[k] >= 20 && junction[k].has_value(), rule + ": the clock was read across and after each change",
              fmt("change %.0f: %.0f readings", static_cast<double>(k), changeSteps[k]));
        const double step = changeStep[k] / 1e6;
        check(step <= bound,
              rule + (was == 0.5 ? ": a change leaving 0.5 steps the clock 10 ms at most" : ": a change steps the clock 8 ms at most"),
              fmt("rate %.1f -> %.1f: %.2f ms", was, changes[k].rate, step));
    }
    if (worstAt > 0) {
        const AudioTrack::Clock& a = clocks[worstAt - 1];
        const AudioTrack::Clock& b = clocks[worstAt];
        std::printf("audio_playback_test: %s: the clock's largest step, %+.0f ms in: %.1f ms of the file at rate %.2f, then "
                    "%.1f ms at rate %.2f %.1f ms later\n",
                    rule.c_str(), (ns(b.at) - t0) / 1e6, a.file.count() / 1e6, a.rate, b.file.count() / 1e6, b.rate,
                    (b.at - a.at).count() / 1e6);
    }
    int graded = 0;
    double worstBeep = 0;
    std::vector<double> changeBeep(changes.size(), 0.0);
    std::vector<int> changeBeeps(changes.size(), 0);
    for (const ClockBeep& b : beepsAgainstClock(cap, clocks, t0 + 100 * kMs)) {
        std::printf("  the beep at %.1f s heard %+.0f ms in, %+.2f ms from the clock (rate %.2f)\n", b.beep / 1e9,
                    (b.heardAt - t0) / 1e6, b.offMs, b.rate);
        worstBeep = std::max(worstBeep, std::fabs(b.offMs));
        for (size_t k = changes.size(); k-- > 0;) {
            if (b.heardAt >= changes[k].at) {
                changeBeep[k] = std::max(changeBeep[k], std::fabs(b.offMs));
                ++changeBeeps[k];
                break;
            }
        }
        ++graded;
    }
    for (size_t k = 0; k < changes.size(); ++k) {
        std::printf("audio_playback_test: %s: after the change to rate %.1f, %d beeps, at most %.2f ms from the clock\n",
                    rule.c_str(), changes[k].rate, changeBeeps[k], changeBeep[k]);
    }
    std::vector<float> played;
    std::vector<int64_t> playedAt;
    std::optional<size_t> firstLoud;
    for (const probe::BufferRecord& b : cap.buffers) {
        if (b.queuedAt >= end - 100 * kMs) break;
        for (uint64_t i = b.first; i < b.first + b.frames; ++i) {
            if (!firstLoud && std::fabs(cap.right[i]) > 0.2f) firstLoud = played.size();
            played.push_back(cap.right[i]);
            playedAt.push_back(cap.heardAt(b, i));
        }
    }
    const ClickAndDip tone2 = clickAndDip(played, firstLoud.value_or(played.size()));
    // The largest second difference within 60 ms of each change as heard: where its first new-rate reading speaks for.
    int clicksLookedAt = 0;
    for (size_t k = 0; k < changes.size(); ++k) {
        if (heardFrom[k] == 0 || !firstLoud) continue;
        float worst = 0;
        int64_t worstHeard = 0;
        size_t n = 0;
        for (size_t i = std::max<size_t>(*firstLoud, 1); i + 1 < played.size(); ++i) {
            if (std::llabs(playedAt[i] - heardFrom[k]) > 60 * kMs) continue;
            ++n;
            const float d = std::fabs(played[i + 1] - 2 * played[i] + played[i - 1]);
            if (d > worst) {
                worst = d;
                worstHeard = playedAt[i];
            }
        }
        if (n < kRate / 20) continue;
        ++clicksLookedAt;
        const double was = k == 0 ? 1.0 : changes[k - 1].rate;
        std::printf("audio_playback_test: %s: rate %.1f -> %.1f: largest second difference %.4f near the change, heard %+.1f ms "
                    "in (%+.1f ms from the change)\n",
                    rule.c_str(), was, changes[k].rate, worst, (worstHeard - t0) / 1e6, (worstHeard - heardFrom[k]) / 1e6);
        check(worst <= 0.02f, rule + ": a change of rate does not click", fmt("to rate %.1f: second difference %.4f at %+.1f ms", changes[k].rate,
              worst, (worstHeard - t0) / 1e6));
    }
    check(clicksLookedAt == static_cast<int>(changes.size()), rule + ": the tone was heard near every change",
          fmt("%.0f of %.0f", static_cast<double>(clicksLookedAt), static_cast<double>(changes.size())));
    const float click = tone2.click, dip = tone2.dip;
    if (tone2.dipAt < playedAt.size()) std::printf("  the tone's lowest envelope %.3f heard %+.1f ms in\n", dip, (playedAt[tone2.dipAt] - t0) / 1e6);
    // Gaps (over 480 frames at silence) and dips (24 frames or more under 0.05 between louder ones) in the
    // tone, up to 100 ms before the stop, whose fade is not graded.
    Continuity toneRun = continuity(queuedBefore(cap, end - 100 * kMs), 480, 0.05f, 24, true);
    if (toneRun.trailing.second != 0) toneRun.gaps.push_back(toneRun.trailing);
    for (const auto& [at, frames] : toneRun.gaps) std::printf("  a gap of %llu frames heard %+.1f ms in\n", static_cast<unsigned long long>(frames), (at - t0) / 1e6);
    const size_t gaps = toneRun.gaps.size(), dips = toneRun.dips;
    std::printf("audio_playback_test: %s: %zu clock readings, the clock at most %.2f ms off where it ran on to; %d beeps, at "
                "most %.2f ms from the clock; tone: %zu gaps, %zu dips, largest second difference %.4f, lowest envelope %.3f\n",
                rule.c_str(), clocks.size(), worstStep / 1e6, graded, worstBeep, gaps, dips, click, dip);
    check(graded >= 10, rule + ": at least ten beeps were graded against the clock", fmt("%.0f", graded));
    check(worstBeep <= 40.0, rule + ": every beep is heard where the clock puts it", fmt("%.2f ms off at worst", worstBeep));
    check(gaps == 0 && dips == 0, rule + ": a change of rate leaves no gap and no dip in the tone",
          fmt("%.0f gaps, %.0f dips", static_cast<double>(gaps), static_cast<double>(dips)));
    check(firstLoud && click <= 0.02f, rule + ": the tone does not click anywhere", fmt("second difference %.4f", click));
}

// GOAL: S15.611 -- a speed change cuts the queued sound: (a) a speed replaced at once (2 then 0.5
//       back to back) never reaches the clock, and no beeps from the ask to the next one come closer than 0.5's
//       1 s; (b) every change is on the clock within 150 ms of the ask; (c) 2 held 150 ms is on the clock for as
//       long, within 40 ms; the tone has no gap, dip or click throughout.
// WHY-THIS-SHAPE: durations are taken between the first clock publications (clock.at) at each rate, so both
//       ends carry the same one-cycle quantisation; poll times would add the 5 ms poll. 2 and 0.5 are asked back
//       to back, not a few ms apart: a server cycle may fall between those, and then one cycle of 2 is heard by
//       design, which would grade the scheduler. (c) alone cannot tell an engine that never cuts (both ends are
//       late alike, 149 ms seen); (b) is what does.
// CONTROL: control_batch.sh 2026-10-15, reverted, renderer/audio/audio_output.cpp: cutQueued() never cutting ->
//       "FAIL: rate cut rule: a change is on the clock within 150 ms of the ask" x5 (223-467 ms), (c) green; cut
//       only for changes to 2 (followRate) -> "... a speed held 150 ms is on the clock for as long, within 40 ms"
//       (298.7 ms); AudioTrack::setRate() sleeping 40 ms after waking for 2 -> "... a speed replaced at once never
//       reaches the clock"; that plus no cut, with the first ask at +1750 ms (control_run.sh 2026-10-15) -> also
//       "... the beeps after the replaced speed come at 0.5's spacing" (740.9 ms apart; green 1002.2). Test-side,
//       2026-10-15: 1500 frames of the captured tone zeroed -> "... the cuts leave no gap and no dip in the tone"
//       (1 gap, 1 dip). control_batch.sh 2026-10-15, phase engineRateCut: fadeIn() crossfading from the old run
//       negated (renderer/audio/audio_decoder.cpp) -> "... the cuts do not click" (0.5311) beside the dip line.
// COVERAGE-ASSERT: at least 200 clock readings, every change found on the clock, at least two beeps between
//       the first two asks, and over 8 s of tone looked at.
void runEngineRateCutChecks() {
    const char* kGoals = "S15.611 (a speed change cuts the queue)";
    const std::string rule = "rate cut rule";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    resetCapture();
    Engine engine(kGoals);
    if (!engine.output) return;
    std::unique_ptr<AudioTrack> track = engine.output->track(asset("wallpaper_beeps_long.mp4"), "engine");
    const int64_t t0 = probe::nowNs() + 400 * kMs;
    track->play(anchorAt(0, t0));
    // {rate, ms after t0}; the first is replaced at once by the second, so it is not a change of its own. The
    // first is asked midway between the 1.5 s and 2 s beeps, so no beep heard before the cut opens the stretch.
    struct Change {
        double rate;
        int64_t offsetMs;
        int64_t at = 0;
    };
    std::vector<Change> changes = {{0.5, 1750}, {1.0, 4000}, {2.0, 5500}, {0.5, 5650}, {1.0, 7500}};
    std::vector<std::pair<int64_t, AudioTrack::Clock>> readings;
    const int64_t end = t0 + 9000 * kMs;
    size_t next = 0;
    while (probe::nowNs() < end) {
        const int64_t now = probe::nowNs();
        if (next < changes.size() && now >= t0 + changes[next].offsetMs * kMs) {
            changes[next].at = now;
            if (next == 0) track->setRate(2.0);
            track->setRate(changes[next++].rate);
        }
        if (const std::optional<AudioTrack::Clock> c = track->clock(Clock::now())) readings.push_back({now, *c});
        std::this_thread::sleep_for(5ms);
    }
    track->stop();
    std::this_thread::sleep_for(200ms);
    const Capture cap = takeCapture();
    check(readings.size() >= 200, rule + ": the clock was read while the track played",
          fmt("%.0f readings", static_cast<double>(readings.size())));
    if (readings.size() < 200) return;
    std::vector<AudioTrack::Clock> clocks;
    for (const auto& [now, clock] : readings) {
        if (clocks.empty() || clock.at != clocks.back().at) clocks.push_back(clock);
    }
    // (a) Up to the real ask for 2, no publication at 2.
    int replacedSeen = 0;
    for (const auto& [now, clock] : readings) {
        if (now < changes[2].at && clock.rate == 2.0) ++replacedSeen;
    }
    check(replacedSeen == 0, rule + ": a speed replaced at once never reaches the clock",
          fmt("%.0f readings at rate 2 before 2 was asked for again", static_cast<double>(replacedSeen)));
    // (b) Each change: the first publication at the new rate after the ask, and how long after.
    std::vector<int64_t> onClock(changes.size(), 0);
    for (size_t k = 0; k < changes.size(); ++k) {
        for (const AudioTrack::Clock& c : clocks) {
            if (ns(c.at) >= changes[k].at && c.rate == changes[k].rate) {
                onClock[k] = ns(c.at);
                break;
            }
        }
        const double was = k == 0 ? 1.0 : changes[k - 1].rate;
        std::printf("audio_playback_test: %s: rate %.1f -> %.1f asked at %+.0f ms, first published %.1f ms later\n", rule.c_str(),
                    was, changes[k].rate, (changes[k].at - t0) / 1e6, onClock[k] ? (onClock[k] - changes[k].at) / 1e6 : -1.0);
        check(onClock[k] != 0 && onClock[k] - changes[k].at <= 150 * kMs, rule + ": a change is on the clock within 150 ms of the ask",
              fmt("rate %.1f -> %.1f: ", was, changes[k].rate) +
                  (onClock[k] ? fmt("%.1f ms", (onClock[k] - changes[k].at) / 1e6) : std::string("never")));
    }
    // (c) 2 was in force from changes[2].at to changes[3].at; on the clock from onClock[2] to onClock[3].
    if (onClock[2] != 0 && onClock[3] != 0) {
        const double asked = (changes[3].at - changes[2].at) / 1e6;
        const double heard = (onClock[3] - onClock[2]) / 1e6;
        std::printf("audio_playback_test: %s: rate 2 in force %.1f ms, on the clock %.1f ms\n", rule.c_str(), asked, heard);
        check(std::fabs(heard - asked) <= 40.0, rule + ": a speed held 150 ms is on the clock for as long, within 40 ms",
              fmt("in force %.1f ms, on the clock %.1f ms", asked, heard));
    }
    // (a) The beeps from the ask to the next one: 1 s apart at 0.5, never 2's 0.25 s nor a mix of 2 and 0.5.
    std::vector<int64_t> stretch;
    for (const int64_t h : onsets(cap, 0.1f, kRate * 15 / 100)) {
        if (h >= changes[0].at && h < changes[1].at) stretch.push_back(h);
    }
    double closest = 1e9;
    for (size_t i = 1; i < stretch.size(); ++i) closest = std::min(closest, (stretch[i] - stretch[i - 1]) / 1e6);
    std::printf("audio_playback_test: %s: %zu beeps in the 0.5 stretch, closest %.1f ms apart\n", rule.c_str(), stretch.size(), closest);
    check(stretch.size() >= 2 && closest >= 900.0, rule + ": the beeps after the replaced speed come at 0.5's spacing",
          fmt("%.0f beeps, closest %.1f ms apart", static_cast<double>(stretch.size()), closest));
    // The tone, up to 100 ms before the stop: no gap, no dip, no click.
    std::vector<float> played;
    std::optional<size_t> firstLoud;
    for (const probe::BufferRecord& b : cap.buffers) {
        if (b.queuedAt >= end - 100 * kMs) break;
        for (uint64_t i = b.first; i < b.first + b.frames; ++i) {
            if (!firstLoud && std::fabs(cap.right[i]) > 0.2f) firstLoud = played.size();
            played.push_back(cap.right[i]);
        }
    }
    const ClickAndDip tone = clickAndDip(played, firstLoud.value_or(played.size()));
    const Continuity toneRun = continuity(queuedBefore(cap, end - 100 * kMs), 480, 0.05f, 24, true);
    const size_t gaps = toneRun.gaps.size() + (toneRun.trailing.second != 0 ? 1 : 0), dips = toneRun.dips;
    std::printf("audio_playback_test: %s: tone over %.0f ms: %zu gaps, %zu dips, largest second difference %.4f\n", rule.c_str(),
                played.size() * 1000.0 / kRate, gaps, dips, tone.click);
    check(firstLoud && played.size() > static_cast<size_t>(kRate) * 8, rule + ": the tone was heard across every change",
          fmt("%.0f ms", played.size() * 1000.0 / kRate));
    check(gaps == 0 && dips == 0, rule + ": the cuts leave no gap and no dip in the tone",
          fmt("%.0f gaps, %.0f dips", static_cast<double>(gaps), static_cast<double>(dips)));
    check(firstLoud && tone.click <= 0.02f, rule + ": the cuts do not click", fmt("second difference %.4f", tone.click));
}

// A least-squares line through (x, y): {value at x = 0, slope}.
std::pair<double, double> lineThrough(const std::vector<std::pair<double, double>>& points) {
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (const auto& [x, y] : points) {
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
    }
    const auto n = static_cast<double>(points.size());
    const double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
    return {(sy - slope * sx) / n, slope};
}

// GOAL: S15.615 -- at a speed change's join nothing of the file is heard twice and nothing is
//       dropped beyond 2.5 ms (a 2 -> 1 join under full CPU load lands up to ~2.2 ms off, accepted as unheard, user
//       2026-10-15), for cuts leaving 1 (to 2, to 0.5) and cuts leaving a stretched speed (2, 0.5) for 1. False if
//       the new run starts more than 2.5 ms from where the old one would have gone on.
// WHY-THIS-SHAPE: graded on the heard sound alone, not the clock: the clock's new mark is the decoder's own
//       start, so a misplaced start agrees with it. The left channel is a ramp, so each heard frame says where in
//       the file it came from (a stretcher's blend says the blend's mean). The rate-1 side of the join is an
//       exact slope-1 line; the 2 ms raised-cosine crossfade is unmixed with it to read the other side's place
//       at the crossfade's first frame. A 4 ms break reads 106-226 frames: the join's alignment can pull one a
//       440 Hz period back, under the bound, so some joins of a +4 ms break pass. 0.5<->2 joins are not graded: neither side is exact, so the crossfade cannot be
//       found to the frame. Bound 120 frames (2.5 ms): correct joins read -12..+16 unloaded, +-107 at worst loaded.
//       A join whose crossfade unmixes to no straight line is not graded at all: taking it as a join at 0 would
//       pass the 2.5 ms line for a join that was never measured.
// CONTROL: control_batch.sh 2026-10-15, reverted, phase engineRateJoin, renderer/audio/audio_output.cpp: followRate()'s `from` - 4 ms ->
//       "FAIL: rate join rule: the new run starts where the old one ends, within 2.5 ms" at all 7 joins read
//       (-217.0..-248.4 frames; re-run on peekJoin(), held); `from` + 4 ms -> the same line at 5 of 8 (+192.8..+219.0;
//       three pulled to +106..+119).
// COVERAGE-ASSERT: a crossfade read at two joins leaving 1 and three back to it; the rate-1 ramp is checked
//       unchanged first. control_batch.sh 2026-10-15: fadeWeight() a hard switch at mid-crossfade
//       (renderer/audio/audio_decoder.cpp) -> "... a crossfade was read at two joins leaving 1 and three coming back
//       to it" (2 and 0: the joins back to 1 now not graded); the output's gain at 0.9 (audio_output.cpp) -> "...
//       the ramp is heard unchanged at rate 1" (0 of 28835 frames follow on).
void runEngineRateJoinChecks() {
    const char* kGoals = "S15.615 (nothing heard twice or dropped at a speed cut)";
    const std::string rule = "rate join rule";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    resetCapture();
    Engine engine(kGoals);
    if (!engine.output) return;
    std::unique_ptr<AudioTrack> track = engine.output->track(asset("wallpaper_ramp.mkv"), "engine");
    const int64_t t0 = probe::nowNs() + 400 * kMs;
    track->play(anchorAt(0, t0));
    struct Change {
        double rate;
        int64_t offsetMs;
        int64_t at = 0;
    };
    std::vector<Change> changes = {{2.0, 800},  {1.0, 1600}, {0.5, 2400}, {1.0, 3200},
                                   {2.0, 4000}, {1.0, 4800}, {0.5, 5600}, {1.0, 6400}};
    const int64_t end = t0 + 7200 * kMs;
    size_t next = 0;
    while (probe::nowNs() < end) {
        if (next < changes.size() && probe::nowNs() >= t0 + changes[next].offsetMs * kMs) {
            changes[next].at = probe::nowNs();
            track->setRate(changes[next++].rate);
        }
        std::this_thread::sleep_for(2ms);
    }
    track->stop();
    std::this_thread::sleep_for(200ms);
    const Capture cap = takeCapture();
    // The fixture's ramp: 0.4 s of file from -0.1 to 0.1, read back as a frame of that period.
    constexpr double kPeriod = 19200.0, kRampTop = 0.1;
    constexpr int kFade = 96;
    std::vector<int64_t> heardAt;
    std::vector<double> pos;
    for (const probe::BufferRecord& b : cap.buffers) {
        for (uint64_t i = b.first; i < b.first + b.frames; ++i) {
            heardAt.push_back(cap.heardAt(b, i));
            pos.push_back((cap.left[i] / kRampTop + 1.0) * 0.5 * kPeriod);
        }
    }
    const auto indexAt = [&](int64_t at) {
        return static_cast<size_t>(std::lower_bound(heardAt.begin(), heardAt.end(), at) - heardAt.begin());
    };
    // Frame i+1 follows frame i in the file at rate 1, a wrap of the ramp aside.
    const auto exact = [&](size_t i) {
        if (i + 1 >= pos.size()) return false;
        const double step = pos[i + 1] - pos[i];
        return std::fabs(step - 1.0) <= 0.05 || std::fabs(step - 1.0 + kPeriod) <= 0.05;
    };
    const auto weight = [&](int k) { return 0.5 - 0.5 * std::cos(std::numbers::pi * (k + 0.5) / kFade); };
    const auto clear = [&](double p) { return p >= 600.0 && p <= kPeriod - 600.0; };
    size_t steady = 0, looked = 0;
    for (size_t i = indexAt(t0 + 200 * kMs); i < indexAt(changes[0].at); ++i, ++looked) steady += exact(i) ? 1 : 0;
    check(looked > 20000 && steady + looked / 100 >= looked, rule + ": the ramp is heard unchanged at rate 1",
          fmt("%.0f of %.0f frames follow on", static_cast<double>(steady), static_cast<double>(looked)));
    if (looked <= 20000 || steady + looked / 100 < looked) return;
    int fromOne = 0, toOne = 0;
    for (size_t j = 0; j < changes.size(); ++j) {
        const double was = j == 0 ? 1.0 : changes[j - 1].rate;
        const std::string label = fmt("rate %.1f -> %.1f", was, changes[j].rate);
        const size_t i0 = indexAt(changes[j].at), i1 = indexAt(changes[j].at + 400 * kMs);
        if (i0 < 200 || i1 + 1 >= pos.size()) {
            check(false, rule + ": the sound around each change was captured", label);
            continue;
        }
        // The new run's place minus the old run's at the crossfade's first frame, in frames. The crossfade starts
        // within a frame or two of the first step off the exact rate-1 line. A stretched side whose fragment there
        // also runs at slope 1 leaves no mark, so the first step off is a later fragment's blend, which unmixes to
        // no straight line.
        const auto unmix = [&](size_t c, bool leaving, double exact0, double& spread) {
            std::vector<std::pair<double, double>> other;
            for (int k = leaving ? 24 : 0; k < (leaving ? kFade : 72); ++k) {
                const double w = weight(k), mine = exact0 + k, heard = pos[c + k];
                other.push_back({double(k), leaving ? (heard - (1.0 - w) * mine) / w : (heard - w * mine) / (1.0 - w)});
            }
            const auto [at0, slope] = lineThrough(other);
            spread = 0;
            for (const auto& [k, y] : other) spread = std::max(spread, std::fabs(y - at0 - slope * k));
            return leaving ? at0 - exact0 : exact0 - at0;
        };
        const bool leaving = was == 1.0;
        if (!leaving && changes[j].rate != 1.0) continue;
        size_t edge = 0;
        std::vector<size_t> candidates;
        if (leaving) {
            edge = i0;
            while (edge < i1 && exact(edge)) ++edge;
            if (edge != i1 && edge >= i0 + 2) candidates = {edge - 1, edge, edge + 1};
        } else {
            edge = i1;
            while (edge > i0 && exact(edge - 1)) --edge;
            if (edge > i0 + kFade + 2 && edge + 2 <= i1) candidates = {edge - kFade, edge - kFade + 1, edge - kFade + 2};
        }
        if (candidates.empty()) {
            check(false, rule + ": each join is found in the heard ramp", label + ": no step off the rate-1 run near it");
            continue;
        }
        // The exact run's place at heard frame i.
        const double base = leaving ? pos[edge - 1] - static_cast<double>(edge - 1) : pos[edge] - static_cast<double>(edge);
        const auto line = [&](size_t i) { return base + static_cast<double>(i); };
        if (!clear(line(candidates[0]))) {
            std::printf("audio_playback_test: %s: %s: not graded, the ramp wraps at the join\n", rule.c_str(), label.c_str());
            continue;
        }
        double bestSpread = 1e9, apart = 0;
        size_t c = candidates[0];
        for (const size_t candidate : candidates) {
            double spread = 0;
            const double d = unmix(candidate, leaving, line(candidate), spread);
            if (spread < bestSpread) {
                bestSpread = spread;
                apart = d;
                c = candidate;
            }
        }
        const int64_t joinAt = heardAt[c];
        std::printf("audio_playback_test: %s: %s: first step off the rate-1 run %.1f ms after the ask, unmixed to within "
                    "%.2f frames\n",
                    rule.c_str(), label.c_str(), (joinAt - changes[j].at) / 1e6, bestSpread);
        // A crossfade that unmixes to no straight line gives no place to grade: it is counted as not graded,
        // never as a join at 0, so the coverage line below is what such joins fail.
        if (bestSpread > 20.0) {
            std::printf("audio_playback_test: %s: %s: not graded, no crossfade found to unmix there\n", rule.c_str(),
                        label.c_str());
            continue;
        }
        std::printf("audio_playback_test: %s: %s: join heard %.1f ms after the ask, the new run starts %+.1f frames "
                    "(%+.3f ms of file) from where the old one ends\n",
                    rule.c_str(), label.c_str(), (joinAt - changes[j].at) / 1e6, apart, apart * 1000.0 / kRate);
        check(std::fabs(apart) <= 120.0, rule + ": the new run starts where the old one ends, within 2.5 ms",
              label + fmt(": %+.1f frames (%+.3f ms)", apart, apart * 1000.0 / kRate));
        ++(leaving ? fromOne : toOne);
    }
    check(fromOne >= 2 && toOne >= 3, rule + ": a crossfade was read at two joins leaving 1 and three coming back to it",
          fmt("%.0f leaving 1, %.0f coming back", fromOne, toOne));
}

// GOAL: S15.613 -- a speed change asked while the stream has run dry is cut cleanly once sound is
//       back: 2 then 0.5 asked back to back during a 250 ms decoder stall never reaches the clock, 0.5 is on
//       the clock within 150 ms of the sound's return. False if the change is lost or the replaced speed is
//       published. The tone after the return is printed, not graded: no break made it click, gap or dip.
// WHY-THIS-SHAPE: the change is asked while the decoder is held in the staged stall, so it is pending when the
//       ring refills and the cut is taken while frames are still owed; asked after the return, whether the
//       callback had already skipped ahead would be a race. The return itself (silence to the tone) is a step
//       by design, so the grading starts 110 frames after it.
// CONTROL: control_batch.sh 2026-10-15, reverted, phase engineRateUnderrunCut, renderer/audio/audio_output.cpp:
//       AudioTrack::setRate() sleeping 200 ms after waking for 2 -> "FAIL: underrun cut rule: a speed replaced during
//       the underrun never reaches the clock"; kCutLeadFrames 40000 (no cut fits) -> "... the change is on the clock
//       within 150 ms of the sound's return" (256.0 ms).
void runEngineRateUnderrunCutChecks() {
    const char* kGoals = "S15.613 (a cut after an underrun)";
    const std::string rule = "underrun cut rule";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    resetCapture();
    Engine engine(kGoals);
    if (!engine.output) return;
    std::unique_ptr<AudioTrack> track = engine.output->track(asset("wallpaper_beeps_long.mp4"), "engine");
    const int64_t t0 = probe::nowNs() + 400 * kMs;
    track->play(anchorAt(0, t0));
    sleepUntil(t0 + 600 * kMs);
    probe::g_stallStartedAt.store(0);
    probe::g_stallEndedAt.store(0);
    probe::g_stallMs.store(250);
    probe::g_swrMode.store(probe::kSwrStallOnce);
    std::vector<std::pair<int64_t, AudioTrack::Clock>> readings;
    int64_t askedAt = 0;
    const int64_t end = t0 + 3000 * kMs;
    while (probe::nowNs() < end) {
        const int64_t now = probe::nowNs();
        const int64_t started = probe::g_stallStartedAt.load();
        if (askedAt == 0 && started != 0 && now >= started + 100 * kMs) {
            askedAt = now;
            track->setRate(2.0);
            track->setRate(0.5);
        }
        if (const std::optional<AudioTrack::Clock> c = track->clock(Clock::now())) readings.push_back({now, *c});
        std::this_thread::sleep_for(2ms);
    }
    track->stop();
    std::this_thread::sleep_for(200ms);
    probe::g_swrMode.store(probe::kSwrPass);
    const Capture cap = takeCapture();
    const int64_t stallStarted = probe::g_stallStartedAt.load(), stallEnded = probe::g_stallEndedAt.load();
    if (stallStarted == 0) {
        check(false, rule + ": the engine asked the decoder for more sound after 600 ms of play",
              "no conversion from then to the end of the run, so the staged stall was never reached");
        return;
    }
    // The dry stretch inside the stall, and where the sound came back.
    Continuity tone = continuity(cap, 480, 0.05f, 24, true);
    tone.gaps.push_back(tone.trailing);
    uint64_t dry = 0;
    int64_t returnedAt = 0;
    for (const auto& [at, frames] : tone.gaps) {
        if (at > stallStarted && at < stallEnded && frames > dry) {
            dry = frames;
            returnedAt = at + static_cast<int64_t>(frames) * 1'000'000'000 / kRate;
        }
    }
    std::printf("audio_playback_test: %s: stall %.0f to %.0f ms in, change asked %.0f ms in; %.1f ms without sound, back "
                "%.0f ms in\n",
                rule.c_str(), (stallStarted - t0) / 1e6, (stallEnded - t0) / 1e6, (askedAt - t0) / 1e6, dry * 1000.0 / kRate,
                (returnedAt - t0) / 1e6);
    if (stallEnded == 0 || dry < 960 || askedAt == 0 || askedAt >= stallEnded) {
        notVerified(kGoals, "the change was not asked inside a stall that left the stream 20 ms without sound");
        return;
    }
    int replacedSeen = 0;
    std::optional<int64_t> slowAt;
    for (const auto& [now, clock] : readings) {
        if (clock.rate == 2.0) ++replacedSeen;
        if (!slowAt && now >= askedAt && clock.rate == 0.5) slowAt = ns(clock.at);
    }
    check(replacedSeen == 0, rule + ": a speed replaced during the underrun never reaches the clock",
          fmt("%.0f readings at rate 2", static_cast<double>(replacedSeen)));
    check(slowAt && *slowAt - returnedAt <= 150 * kMs, rule + ": the change is on the clock within 150 ms of the sound's return",
          slowAt ? fmt("%.1f ms", (*slowAt - returnedAt) / 1e6) : std::string("never"));
    // The right channel's tone from 110 frames after the return, for 400 ms.
    std::vector<float> after;
    for (const probe::BufferRecord& b : cap.buffers) {
        for (uint64_t i = b.first; i < b.first + b.frames; ++i) {
            const int64_t h = cap.heardAt(b, i);
            if (h >= returnedAt && h < returnedAt + 400 * kMs) after.push_back(cap.right[i]);
        }
    }
    const ClickAndDip cd = clickAndDip(after, 0);
    Capture afterCap = cap;
    std::erase_if(afterCap.buffers, [&](const probe::BufferRecord& b) {
        return b.heardAt < returnedAt || b.heardAt >= returnedAt + 400 * kMs;
    });
    const Continuity afterRun = continuity(afterCap, 480, 0.05f, 24, true);
    const size_t gaps = afterRun.gaps.size() + (afterRun.trailing.second != 0 ? 1 : 0), dips = afterRun.dips;
    std::printf("audio_playback_test: %s: %.0f ms after the return: largest second difference %.4f at %+.1f ms, lowest "
                "envelope %.3f, %zu gaps, %zu dips\n",
                rule.c_str(), after.size() * 1000.0 / kRate, cd.click, cd.dipAt * 1000.0 / kRate, cd.dip, gaps, dips);
}

// The speeds asked while a play waits to start, each that many ms before the start.
struct WaitingAsk {
    double rate;
    int64_t beforeMs;
};

// GOAL: S15.614 -- a speed change asked while a play waits to start (anchor 1.2 s ahead, from 0.3 s of the
//       file, its segment already written at the old speed) is cut from the segment's start: the last speed
//       asked is on the clock within 60 ms of the start, no other speed is published from the start, and
//       every beep is heard within 40 ms of where the anchor puts it at that speed. False if any of the
//       queued sound at an earlier speed (the original, or a speed asked and then replaced) is heard.
// WHY-THIS-SHAPE: the beeps are graded against the anchor, not the clock: the clock is published from the
//       decoder's marks, so a queue heard at the old speed with matching marks agrees with the clock (the
//       beeps-against-clock shape passes on exactly that). From 0.3 s so the 0.5 s beep falls inside the
//       341 ms an uncut queue would play at the old speed.
// COVERAGE-ASSERT: every beep the window holds (at least four) is graded, and every ask fell >= 300 ms
//       before the start.
void runWaiting(const char* kGoals, const std::string& rule, const std::vector<WaitingAsk>& asks, bool loop) {
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    resetCapture();
    Engine engine(kGoals);
    if (!engine.output) return;
    std::unique_ptr<AudioTrack> track = engine.output->track(asset("wallpaper_beeps_long.mp4"), "engine");
    const int64_t t0 = probe::nowNs() + 1200 * kMs;
    AudioTrack::Anchor anchor = anchorAt(300 * kMs, t0);
    anchor.loop = loop;
    track->play(anchor);
    int64_t lastAsk = 0;
    for (const WaitingAsk& a : asks) {
        sleepUntil(t0 - a.beforeMs * kMs);
        lastAsk = probe::nowNs();
        track->setRate(a.rate);
    }
    const double rate = asks.back().rate;
    std::vector<std::pair<int64_t, AudioTrack::Clock>> readings;
    const int64_t end = t0 + 3800 * kMs;
    while (probe::nowNs() < end) {
        const int64_t now = probe::nowNs();
        if (const std::optional<AudioTrack::Clock> c = track->clock(Clock::now())) readings.push_back({now, *c});
        std::this_thread::sleep_for(5ms);
    }
    track->stop();
    std::this_thread::sleep_for(200ms);
    const Capture cap = takeCapture();
    check(lastAsk < t0 - 300 * kMs, rule + ": every change was asked while the play waited",
          fmt("the last %.0f ms before the start", (t0 - lastAsk) / 1e6));
    std::optional<int64_t> onClock;
    int others = 0;
    if (loop) {
        std::printf("audio_playback_test: %s: a looping anchor publishes no clock (%zu readings), so only the beeps are graded\n",
                    rule.c_str(), readings.size());
        readings.clear();
    }
    for (const auto& [now, clock] : readings) {
        if (ns(clock.at) < t0) continue;
        if (clock.rate != rate) ++others;
        if (!onClock && clock.rate == rate) onClock = ns(clock.at);
    }
    std::printf("audio_playback_test: %s: rate %.1f on the clock %.1f ms after the start; %d readings at another speed\n",
                rule.c_str(), rate, onClock ? (*onClock - t0) / 1e6 : -1.0, others);
    if (!loop) {
        check(onClock && *onClock - t0 <= 60 * kMs, rule + ": the asked speed is on the clock within 60 ms of the start",
              onClock ? fmt("%.1f ms after the start", (*onClock - t0) / 1e6) : std::string("never"));
        check(others == 0, rule + ": no other speed is published once the play starts", fmt("%.0f readings", others));
    }
    // Beep k of the file (k * 0.5 s) is heard (k * 0.5 s - 0.3 s) / rate after the start.
    std::vector<std::pair<std::string, int64_t>> expected;
    for (int k = 1; k <= 24; ++k) {
        const double after = (k * 0.5 - 0.3) / rate;
        if (after > 3.6) break;
        expected.push_back({fmt("the beep at %.1f s", k * 0.5), t0 + static_cast<int64_t>(after * 1e9)});
    }
    const int graded = gradeBeepTimes(rule, cap, expected, -40.0, 40.0);
    check(graded == static_cast<int>(expected.size()) && graded >= 4, rule + ": every beep from the start was graded",
          fmt("%.0f of %.0f", graded, static_cast<double>(expected.size())));
}

// CONTROL: control_batch.sh 2026-10-15, reverted, renderer/audio/audio_output.cpp: cutOnStreamThread() not cutting a
//       segment not yet switched into -> "FAIL: waiting change rule: the asked speed is on the clock within 60 ms of
//       the start" (360.5 ms), "... no other speed is published once the play starts" (67 readings), "... each beep
//       is heard on time" and "... every expected beep is heard within 100 ms of its time".
void runEngineRateWaitingChecks() {
    runWaiting("S15.614 (a change while a play waits)", "waiting change rule", {{2.0, 600}}, false);
}

// GOAL: S15.614 -- two speeds asked while a play waits (2, then 0.5): neither 1 nor 2 is ever heard.
// CONTROL: control_batch.sh 2026-10-15, reverted, renderer/audio/audio_output.cpp: cutOnStreamThread() cutting a
//       not-yet-switched segment only once per process (the second change lost) -> "FAIL: waiting twice rule: each
//       beep is heard on time", "... every expected beep is heard within 100 ms of its time", and the clock lines
//       (0.5 on the clock 381.7 ms after the start, 72 readings at another speed).
void runEngineRateWaitingTwiceChecks() {
    runWaiting("S15.614 (two changes while a play waits)", "waiting twice rule", {{2.0, 700}, {0.5, 400}}, false);
}

// GOAL: S15.614 -- a looping play waiting to start takes a speed asked meanwhile from its start, as above.
// CONTROL: control_batch.sh 2026-10-15, reverted, renderer/audio/audio_output.cpp: followRate() never cutting for a
//       looping anchor (cutQueued() skipped when loopFrom is set) -> "FAIL: waiting loop rule: each beep is heard on
//       time" and "... every expected beep is heard within 100 ms of its time". On the
//       `looping` flag (cut skipped when `t.looping`), control_run.sh 2026-10-15: held, beeps -77 to -85 ms.
void runEngineRateWaitingLoopChecks() {
    runWaiting("S15.614 (a change while a looping play waits)", "waiting loop rule", {{2.0, 600}}, true);
}

// The heard time of the last frame of the right channel's tone (0.3) louder than 0.1.
std::optional<int64_t> lastTone(const Capture& cap) {
    std::optional<int64_t> last;
    for (const probe::BufferRecord& b : cap.buffers) {
        for (uint64_t i = b.first; i < b.first + b.frames; ++i) {
            if (std::fabs(cap.right[i]) > 0.1f) last = std::max(last.value_or(0), cap.heardAt(b, i));
        }
    }
    return last;
}

// Plays beeps_long's last 2 s (from 10 s, not looping) at 1 and asks 0.5 `askMs` after the start; with
// `stop`, stops at the same moment. Holds the file time at the ask by the clock, read before the ask.
struct AtEnd {
    Capture cap;
    int64_t t0 = 0, askedAt = 0;
    std::optional<double> fileAtAsk;
    std::vector<std::pair<int64_t, AudioTrack::Clock>> readings;
    std::vector<probe::SoundSeek> seeks;
};
std::optional<AtEnd> playToEnd(const char* kGoals, int64_t askMs, bool stop) {
    SoundServer sound(kGoals);
    if (!sound.ready) return std::nullopt;
    resetCapture();
    Engine engine(kGoals);
    if (!engine.output) return std::nullopt;
    std::unique_ptr<AudioTrack> track = engine.output->track(asset("wallpaper_beeps_long.mp4"), "engine");
    AtEnd out;
    out.t0 = probe::nowNs() + 400 * kMs;
    track->play(anchorAt(10000 * kMs, out.t0));
    const int64_t end = out.t0 + 3500 * kMs;
    while (probe::nowNs() < end) {
        const int64_t now = probe::nowNs();
        if (out.askedAt == 0 && now >= out.t0 + askMs * kMs) {
            out.askedAt = now;
            track->setRate(0.5);
            if (stop) track->stop();
        }
        if (const std::optional<AudioTrack::Clock> c = track->clock(Clock::now())) {
            if (out.askedAt == 0) out.fileAtAsk = c->file.count() + static_cast<double>(now - ns(c->at)) * c->rate;
            out.readings.push_back({now, *c});
        }
        std::this_thread::sleep_for(5ms);
    }
    if (!stop) track->stop();
    std::this_thread::sleep_for(200ms);
    out.cap = takeCapture();
    out.seeks = soundSeeksSince(out.askedAt);
    return out;
}

// GOAL: S15.623 -- a speed change asked after a non-looping file's sound has been decoded to its end, while
//       its last 0.2 s is still queued, is cut and decoded again: 0.5 on the clock within 60 ms of the ask,
//       the remainder heard stretched to the file's end and silence after, with one sound seek. Where the tone
//       ends says how long after the ask it was still heard at 1 (2 x remainder - end); that must be 10-110 ms,
//       a cut's lead plus the server's latency. False if the queued sound plays out at the old speed (204 ms),
//       or more than about 50 ms of file is heard twice or dropped at the cut, or decoding never ends.
// WHY-THIS-SHAPE: the file time comes from the clock read at the old speed before the ask; nothing read after
//       the change is trusted for place, since a cut started at the wrong file time puts marks on the clock
//       that agree with it. Exact placement at the join is S15.615's (the ramp); this grades gross loss or
//       repeat only. Latency is graded at 150 ms (S15.611's bound for a cut): 61-85 ms measured here.
// CONTROL: control_batch.sh 2026-10-15, reverted, renderer/audio/audio_output.cpp: service() calling followRate() only
//       while filling (no cut once the sound has ended) -> "FAIL: rate at end rule: the new speed is on the clock
//       within 150 ms" (never), "... the remainder is heard once, stretched, ..." (heard at 1 for 207.0 ms) and "...
//       what was queued is decoded again once" (0 seeks); followRate()'s `from` 60 ms back (heard twice) -> "... the
//       remainder is heard once, stretched, ..." alone (-32.8 ms). Green: 90.9 ms at 1.
void runEngineRateAtEndChecks() {
    const char* kGoals = "S15.623 (a speed change after a file's sound ended)";
    const std::string rule = "rate at end rule";
    const std::optional<AtEnd> r = playToEnd(kGoals, 1800, false);
    if (!r) return;
    std::optional<int64_t> onClock;
    std::optional<AudioTrack::Clock> first;
    for (const auto& [now, clock] : r->readings) {
        if (!first && now >= r->askedAt && clock.rate == 0.5) first = clock;
    }
    if (first) onClock = ns(first->at);
    std::printf("audio_playback_test: %s: asked %.0f ms in at %.1f ms of the file; 0.5 on the clock %.1f ms later; %zu "
                "sound seeks after\n",
                rule.c_str(), (r->askedAt - r->t0) / 1e6, r->fileAtAsk.value_or(-1e6) / 1e6,
                onClock ? (*onClock - r->askedAt) / 1e6 : -1.0, r->seeks.size());
    check(r->fileAtAsk && *r->fileAtAsk > 11.6e9 && *r->fileAtAsk < 11.9e9,
          rule + ": the change was asked inside the file's last 0.4 s, with the clock running",
          r->fileAtAsk ? fmt("%.1f ms of the file", *r->fileAtAsk / 1e6) : std::string("no clock"));
    if (!r->fileAtAsk) return;
    check(onClock && *onClock - r->askedAt <= 150 * kMs, rule + ": the new speed is on the clock within 150 ms",
          onClock ? fmt("%.1f ms", (*onClock - r->askedAt) / 1e6) : std::string("never"));
    // Heard at 1 for `lead` after the ask, then at 0.5 to the end: the tone ends 2 * remainder - lead after it.
    const double remainder = (12e9 - *r->fileAtAsk) / 1e6;
    const std::optional<int64_t> last = lastTone(r->cap);
    const double lead = last ? 2 * remainder - (*last - r->askedAt) / 1e6 : -1e9;
    std::printf("audio_playback_test: %s: %.1f ms of file left at the ask; the tone ends %.1f ms after it, so heard at 1 for "
                "%.1f ms after the ask\n",
                rule.c_str(), remainder, last ? (*last - r->askedAt) / 1e6 : -1.0, lead);
    check(last && lead >= 10.0 && lead <= 110.0 && *last < r->t0 + 3300 * kMs,
          rule + ": the remainder is heard once, stretched, to the file's end, then the sound ends",
          last ? fmt("heard at 1 for %.1f ms after the ask by where the tone ends", lead) : std::string("no tone"));
    // GOAL: S15.627 -- the stretched remainder reaches the file's end: the time the tone says the sound stayed at 1
    //       after the ask agrees with the time the clock's 0.5 line says, within 12 ms. False if the stretcher's end
    //       drops the file's last few ms, which shortens the tone (atempo's flush drops ~8 ms of file). Measured:
    //       5.9 (Lavapipe) and 9.0 ms (RTX) apart with the fix (atempo's last ~6 ms of file run fast), 17.7 ms without it.
    // WHY-THIS-SHAPE: the 10-110 ms window above cannot see a few ms lost. The clock's 0.5 line (file F at A) and
    //       the file at the ask give the switch's time: F - fileAtAsk = (switch - ask) + 0.5 (A - switch). A clock
    //       mark's own time is not the switch's, so it cannot be compared directly.
    // CONTROL: control_batch.sh 2026-10-15, reverted, phase engineRateAtEnd: endStretcher() feeding no silence (renderer/audio/audio_decoder.cpp) ->
    //       "FAIL: rate at end rule: the tone ends where the clock puts the file's end" (90.8 ms by the tone, 72.9 by
    //       the clock). Green: 62.4 and 56.5. At the 12 ms bound (control_run.sh 2026-10-15): held, 91.0 against
    //       73.2 (17.8 apart); green 80.4 and 71.5.
    if (first && last) {
        const double byClock = 2.0 * (static_cast<double>(first->file.count()) - *r->fileAtAsk) / 1e6 - (ns(first->at) - r->askedAt) / 1e6;
        std::printf("audio_playback_test: %s: by the clock's 0.5 line, heard at 1 for %.1f ms after the ask\n", rule.c_str(), byClock);
        check(std::fabs(lead - byClock) <= 12.0, rule + ": the tone ends where the clock puts the file's end",
              fmt("heard at 1 for %.1f ms by the tone, %.1f ms by the clock", lead, byClock));
    }
    check(r->seeks.size() == 1, rule + ": what was queued is decoded again once",
          fmt("%.0f sound seeks", static_cast<double>(r->seeks.size())));
}

// GOAL: S15.623 -- a speed change asked after a non-looping file's sound has fully played out decodes
//       nothing again: no sound seek and no tone after the file's end. False if the change restarts decoding.
// CONTROL: control_batch.sh 2026-10-15, reverted, renderer/audio/audio_output.cpp: cutOnStreamThread() cutting
//       whatever is queued (its `audible` and room tests deleted) -> "FAIL: rate after end rule: a speed change
//       after the sound played out decodes nothing again" (1 sound seek).
void runEngineRateAfterEndChecks() {
    const char* kGoals = "S15.623 (a speed change after the sound played out)";
    const std::string rule = "rate after end rule";
    const std::optional<AtEnd> r = playToEnd(kGoals, 2400, false);
    if (!r) return;
    const std::optional<int64_t> last = lastTone(r->cap);
    std::printf("audio_playback_test: %s: the tone ends %+.1f ms after the start; %zu sound seeks after the ask\n", rule.c_str(),
                last ? (*last - r->t0) / 1e6 : -1.0, r->seeks.size());
    check(last && *last > r->t0 + 1900 * kMs && *last < r->askedAt, rule + ": the file played out before the ask, and nothing after",
          last ? fmt("tone ends %+.1f ms in", (*last - r->t0) / 1e6) : std::string("no tone"));
    check(r->seeks.empty(), rule + ": a speed change after the sound played out decodes nothing again",
          fmt("%.0f sound seeks", static_cast<double>(r->seeks.size())));
}

// GOAL: S15.623 -- a speed change and stop() asked together while a non-looping file's last 0.2 s is queued:
//       the stop still fades the sound out (no tone from 100 ms after it) and the stream stops being scheduled
//       (no buffer queued from 300 ms after it). False if the change's re-decode refills the stopped track.
// CONTROL: control_batch.sh 2026-10-15, reverted, renderer/audio/audio_output.cpp: end() never setting `stopping` ->
//       "FAIL: rate at end stop rule: the stream stops being scheduled" (75 buffers). control_batch.sh 2026-10-15:
//       AudioTrack::stop() not sending its request -> "... the stop fades the sound out" (tone ends +321.2 ms after
//       the stop); a stop segment never switched to or faded for in the callback stayed green (+59.8 ms).
//       A cut taken after end() (followRate() moved after the request, its newest-segment tests deleted in it and
//       in cutOnStreamThread) did NOT go red: the callback fades and stops at the stop segment's switch whatever
//       is written behind it, so that break is not visible here.
void runEngineRateAtEndStopChecks() {
    const char* kGoals = "S15.623 (a speed change with a stop after the file's sound ended)";
    const std::string rule = "rate at end stop rule";
    const std::optional<AtEnd> r = playToEnd(kGoals, 1800, true);
    if (!r) return;
    const std::optional<int64_t> last = lastTone(r->cap);
    size_t late = 0;
    for (const probe::BufferRecord& b : r->cap.buffers) late += b.queuedAt >= r->askedAt + 300 * kMs ? 1 : 0;
    std::printf("audio_playback_test: %s: the tone ends %+.1f ms after the stop; %zu buffers queued from 300 ms after it\n",
                rule.c_str(), last ? (*last - r->askedAt) / 1e6 : -1e3, late);
    check(last && *last > r->askedAt - 100 * kMs && *last < r->askedAt + 100 * kMs, rule + ": the stop fades the sound out",
          last ? fmt("tone ends %+.1f ms after the stop", (*last - r->askedAt) / 1e6) : std::string("no tone"));
    check(late == 0, rule + ": the stream stops being scheduled", fmt("%.0f buffers", static_cast<double>(late)));
}

// GOAL: S15.594 -- a track whose play starts after its anchor's time, at a rate, starts as far into the file
//       as the sound would have played by then at that rate: rate 0.5, anchored at the file's start 1.2 s ago,
//       starts 0.6 s in, so each beep from 1 s of the file on is heard within 30 ms of the anchor plus twice its
//       file time. False if the late start skips ahead at 1 (1.2 s in, every beep about 1.2 s early).
// CONTROL: control_batch.sh 2026-10-14, reverted, phase engineRateLateStart: begin()'s late start skipping
//       ahead at rate 1 (renderer/audio/audio_output.cpp) -> "FAIL: rate late start rule: every expected beep is
//       heard within 100 ms of its time" (four beeps) and "... every beep after the late start was graded".
void runEngineRateLateStartChecks() {
    const char* kGoals = "S15.594 (a late start at a rate)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    resetCapture();
    Engine engine(kGoals);
    if (!engine.output) return;
    std::unique_ptr<AudioTrack> track = engine.output->track(asset("wallpaper_beeps_long.mp4"), "engine");
    track->setRate(0.5);
    const int64_t anchor = probe::nowNs() - 1200 * kMs;
    track->play(anchorAt(0, anchor));
    sleepUntil(anchor + 5300 * kMs);
    track->stop();
    std::this_thread::sleep_for(200ms);
    std::vector<std::pair<std::string, int64_t>> expected;
    for (int k = 2; k <= 5; ++k) expected.push_back({fmt("the beep at %.1f s", k * 0.5), anchor + k * 1000 * kMs});
    const int graded = gradeBeepTimes("rate late start rule", takeCapture(), expected, -30.0, 30.0);
    check(graded == 4, "rate late start rule: every beep after the late start was graded", fmt("%.0f of 4", graded));
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
        if (isPassStartSeek(s.targetNs) && s.at < next + 100 * kMs) handedAt = s.at;
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

// GOAL: S15.535 -- a heard video above the 120-a-second picture cap plays its sound at the file's own rate and
//       its pictures on it: the two-second beeps file at 240 pictures a second shows 100 to 125 pictures a
//       second over 6 s, its pictures' line runs at the file's own time (within 2 %), every beep is heard
//       within 12 ms of its picture, and the sound is moved only onto a pass's start, at most once a loop. A
//       file at exactly 120 a second is heard. False if the capped file's pictures are slowed to the cap
//       (half its time, the sound then moved at every loop or chased), or its sound is silenced or stretched.
// WHY-THIS-SHAPE: the line through the commits gives the pictures' own rate, and the beeps graded against it
//       tie the sound to that same rate. Seeks are under S15.538's sound-server guard.
// COVERAGE-ASSERT: at least eight beeps graded, one at a pass start after a loop.
// CONTROL: control_batch.sh 2026-10-14, reverted, phase rateCap, backend/daemon/wallpaper_manager.cpp: the drop loop
//       disabled -> "FAIL: rate cap rule: a capped file's pictures play at its own rate" (0.5000) and "... a capped
//       file's sound is moved only onto a pass's start" (11 seeks, 8 mid-pass); AudioTrack::clock() reporting the
//       file 33 ms ahead (renderer/audio/audio_output.cpp) -> "FAIL: rate cap rule: each beep is heard with the
//       picture at its timestamp". Green: 120.0 a second, 1.0000, beeps -4.1 to +10.5 ms, 3 seeks over 3 loops.
//       control_batch.sh 2026-10-15, reverted: AudioTrack::clock() 40 ms ahead -> "... each beep is heard with the picture at its timestamp" and
//       "... the first loop's opening beep is heard with its picture" (+46.36 ms); 40 ms behind left that beep at
//       +19.80 ms, inside 25, so only the ahead break controls it.
//       control_batch.sh 2026-10-15, reverted, renderer/audio/audio_output.cpp: begin() seeking 120 ms past a
//       target under 20 ms (the sound moved into a pass's first 150 ms, which isPassStartSeek() must not take for a
//       start) -> "FAIL: rate cap rule: a capped file's sound is moved only onto a pass's start" (3 seeks, 3
//       mid-pass), with the pictures' rate and beep lines.
void runRateCapChecks() {
    const char* kGoals = "S15.535 (a video above the picture cap plays at its own rate, its sound with it)";
    const std::string rule = "rate cap rule";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    {
        Daemon d(smallConfig());
        const std::string file = asset("wallpaper_beeps_rate240.mp4");
        if (!startHeard(d, file, kGoals)) return;
        std::this_thread::sleep_for(1000ms);
        const int64_t from = probe::nowNs();
        std::this_thread::sleep_for(6000ms);
        const int64_t to = probe::nowNs();
        const std::vector<Commit> all = d.takeCommits();
        const std::vector<Commit> commits = commitsIn(all, from, to);
        const Capture cap = takeCapture();
        const std::vector<probe::SoundSeek> seeks = soundSeeksSince(from, to);
        const std::vector<probe::SoundSeek> midPass = midPassSeeks(from, to);
        const int64_t serverGap = longestBufferGap(cap, from, to);
        const std::vector<Shown> shown = shownPictures(commits, file);
        const size_t loops = passStartCommits(shown).size();
        const double perSecond = static_cast<double>(commits.size()) / ((to - from) / 1e9);
        // The session's first loop is the watch's pass 1 only when no loop was committed before the watch.
        const size_t loopsBefore = passStartCommits(shownPictures(commitsIn(all, 0, from), file)).size();
        Line line;
        int passStarts = 0;
        // GOAL: S15.535 -- every beep within 12 ms of its picture, but the session's first loop's opening beep within
        //       25 ms: the daemon learns pass 0's end only about 8 ms before pass 1's first picture, too late to hand
        //       the sound on ahead, so the engine skips up to one server cycle of it (accepted, user, 2026-10-15).
        // WHY-THIS-SHAPE: only that one beep is widened; a 25 ms bound for every beep would hide a late sound at
        //       every later loop, which is handed on 100 ms early and lands within a few ms.
        // CONTROL: in the S15.535 block above runRateCapChecks (AudioTrack::clock() 40 ms ahead).
        // Every other picture is shown, odd ones in some passes: a picture one interval from its beep stands for it.
        const int graded = gradeBeepsAgainstPictures(rule, cap, shown, file, from, 12.0, passStarts, &line, 4.5, INT64_MAX,
                                                     loopsBefore == 0 ? 1 : -1, 25.0);
        std::printf("audio_playback_test: %s: %zu loops committed before the watch\n", rule.c_str(), loopsBefore);
        const double played = line.slope > 0 ? 1.0 / line.slope : 0.0;
        std::printf("audio_playback_test: %s: 240 a second: %.1f pictures shown a second, the line plays %.4f s of the file "
                    "a second; %zu sound seeks (%zu mid-pass) over %zu loops; the server at most %.1f ms between cycles\n",
                    rule.c_str(), perSecond, played, seeks.size(), midPass.size(), loops, serverGap / 1e6);
        check(perSecond >= 100 && perSecond <= 125, rule + ": a 240-a-second file shows about 120 pictures a second",
              fmt("%.1f", perSecond));
        if (graded >= 0) {
            check(graded >= 8 && passStarts >= 1, rule + ": at least eight beeps were graded, one at a pass start after a loop",
                  fmt("%.0f graded, %.0f at a pass start", graded, passStarts));
            check(std::fabs(played - 1.0) < 0.02, rule + ": a capped file's pictures play at its own rate",
                  fmt("%.4f s of the file a second", played));
        }
        if (serverGap > kServerLateBound) {
            notVerified(kGoals, fmt("the sound server went %.1f ms between cycles, which earns a seek of its own", serverGap / 1e6));
        } else {
            check(midPass.empty() && seeks.size() <= loops + 1, rule + ": a capped file's sound is moved only onto a pass's start",
                  fmt("%.0f seeks, %.0f mid-pass, over %.0f loops", static_cast<double>(seeks.size()),
                      static_cast<double>(midPass.size()), static_cast<double>(loops)));
        }
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
        probe::g_costAtHandOver.store(0);
        probe::g_holdMs.store(0);
        probe::g_holdRepeats.store(false);
        probe::g_thenStartHoldMs.store(0);
        probe::g_startHoldArmed.store(false);
        probe::g_unhideAtNs.store(-1);
        probe::g_unhideArmed.store(false);
        probe::g_blitHoldMs.store(0);
        probe::g_convertHoldMs.store(0);
        probe::g_convertHoldAfterStallMs.store(0);
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
// CONTROL: control_batch.sh 2026-10-15, reverted: the `if (planned.timeline - now > kHandOverLead) return {.roundAt
//       ...};` line in steerSoundLocked's startsPass branch deleted, backend/daemon/wallpaper_manager.cpp -> "FAIL:
//       slow loop rule: the first loop's hand-over waits for a round kHandOverLead ahead" (339.9 ms; 100.0 green).
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
        if (isPassStartSeek(s.targetNs)) handedAt = s.at;
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
    // Stretches a thread of this process asked to sleep 1 ms was kept off the processor past it: the
    // machine's own load, measured across the graded span.
    std::vector<std::pair<int64_t, int64_t>> offCpu;
    std::jthread sampler([&offCpu](std::stop_token stop) {
        while (!stop.stop_requested()) {
            const int64_t asked = probe::nowNs();
            std::this_thread::sleep_for(1ms);
            const int64_t woke = probe::nowNs();
            if (woke - asked > 2 * kMs) offCpu.emplace_back(asked + kMs, woke);
        }
    });
    probe::g_fileStallStartedAt.store(0);
    probe::g_fileStallEndedAt.store(0);
    probe::g_stallFileMs.store(80);
    std::this_thread::sleep_for(1000ms);
    const int64_t seekEnd = probe::nowNs();
    sampler.request_stop();
    sampler.join();
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
    int64_t worst = 0, previous = stallEnd, worstFrom = 0;
    int gaps = 0;
    const auto widest = [&](int64_t end) {
        if (end - previous > worst) {
            worst = end - previous;
            worstFrom = previous;
        }
    };
    for (const Commit& c : others) {
        if (c.at <= stallEnd || c.at > to) continue;
        widest(c.at);
        previous = c.at;
        ++gaps;
    }
    widest(to);
    // How much of the widest gap, and the longest single stretch in the graded span, this process could not run.
    int64_t offInWorst = 0, offLongest = 0;
    for (const auto& [from, until] : offCpu) {
        offInWorst += std::max<int64_t>(0, std::min(until, worstFrom + worst) - std::max(from, worstFrom));
        if (until > stallEnd && from < to) offLongest = std::max(offLongest, until - from);
    }
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
        std::printf("audio_playback_test: drop slice rule: load across the graded span: off the processor %.1f ms of the "
                    "widest gap, at most %.1f ms at once\n",
                    offInWorst / 1e6, offLongest / 1e6);
        check(gaps >= 20, "drop slice rule: the other screen's pictures after the stall were graded", fmt("%.0f gaps", gaps));
        // GOAL: S15.644 -- the 28 ms bound is charged to the daemon unless the machine's load accounts for the
        //       excess: a gap over 28 ms that is within it once the time this process was kept off the processor
        //       inside that same gap is taken out is reported not verified, with both numbers.
        // WHY-THIS-SHAPE: a load gate on the 250 ms before the stall (kept above) misses load arriving during the
        //       catch-up, which failed this once at 31.5 ms; excusing any loaded run instead would excuse the 40 ms
        //       a missing slice adds, so only load inside the widest gap itself, and enough to cover it, excuses it.
        // CONTROL: control_run.sh 2026-10-16, reverted, both entries: the slice break above
        //       (backend/daemon/wallpaper_manager.cpp) -> "FAIL: ... stay on time while the heard one catches up"
        //       (77.5 ms, 0.0 ms of it off the processor); with the sampler also recording every whole sleep as off
        //       the processor -> "NOT VERIFIED -- S15.559 ..." (77.8 ms, 77.8 ms of it), the entry a skip, no FAIL.
        if (worst > 28 * kMs && worst - offInWorst <= 28 * kMs) {
            notVerified(kGoals, fmt("the other screen went %.1f ms between pictures, but this process was kept off the "
                                    "processor %.1f ms of that gap, so load accounts for the excess over 28 ms",
                                    worst / 1e6, offInWorst / 1e6));
        } else {
            check(worst <= 28 * kMs, "drop slice rule: the other screen's pictures stay on time while the heard one catches up",
                  fmt("at most %.1f ms between two of its pictures, %.1f ms of it off the processor", worst / 1e6,
                      offInWorst / 1e6));
        }
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

bool onlyUnreferenced(const std::vector<probe::LevelChange>& levels) {
    return !levels.empty() &&
           std::all_of(levels.begin(), levels.end(), [](const probe::LevelChange& c) { return c.discard == AVDISCARD_NONREF; });
}

// GOAL: S15.561 -- a heard file that even its key pictures cannot keep up with plays as a slideshow, its
//       sound restarted with its pictures at each loop: the two-second file whose every picture is a key
//       picture, at 60 ms a returned picture (1.8 intervals), goes straight from every picture to key pictures
//       only (no level between), and from 2 s into the slowdown: none of its decoded pictures is dropped, no
//       commit waits on more than two decoder hand-offs; from the first skip level on no seek lands mid-pass,
//       the sound is never sent to a pass's start away from the pictures' loops (no hand-on by its own clock),
//       at each loop the seek to the pass's start comes at or after the loop's first draw (and within 100 ms of
//       its commit),
//       the opening beep is heard 0-60 ms after that commit, and each pass of the pictures hears each of the
//       file's four beeps once: four onsets between one loop and the next. False if such a file is still steered
//       by its sound, its sound is handed on early and then played again when the pictures loop, or starts
//       before the picture it belongs to.
// WHY-THIS-SHAPE: every picture is a key picture, so no level leaves anything out and the climb has nowhere
//       to go but past the last level. The cost is spent where the decoder returns a picture, as heavy decoding
//       is. A pictures' pass takes about 3.6 s against the sound's 2, so the sound waits about 1.6 s each loop
//       (accepted), and a hand-on would fall about 1.6 s before a loop, far from it. "Heard once" counts onsets
//       per pass of the pictures: a hand-on followed by the restart hears the opening beeps twice there, where
//       comparing seeks alone misses a replay with no seek of its own. "One decode an update" counts decoder
//       hand-offs between two commits, never a wall-clock gap, which would grade the scheduler.
// COVERAGE-ASSERT: ten commits and two loops of the pictures in the window, every loop graded, and at least
//       three seconds of sound heard.
// CONTROL: control_batch.sh 2026-10-14, reverted, backend/daemon/wallpaper_manager.cpp, phase catchUpCap unless
//       named: steerSoundLocked's unpaced restart playing at the planned time instead of the draw -> "FAIL: ... the
//       seek to the pass's start comes at or after the loop's first picture is drawn" (re-run 2026-10-16 on
//       catchUpCap and climb: no seek near the loop); the unpaced branch playing the next pass by the sound's
//       clock 200 ms before its end (2026-10-16, climb) -> "... never sent to a pass's start by its own clock"
//       and the line before; the draw-time play anchored 100 ms
//       after the draw -> "... the opening beep is heard 0-60 ms after the loop's first commit" (also behindAtLoop);
//       the unpaced branch handing on by the sound's clock again -> "... each beep is heard once in a pass" and
//       "... never sent to a pass's start by its own clock" (behindAtLoop: the latter); `if (unpaced) return {};`
//       deleted -> "... never moved mid-pass once it climbs"; the climb taking one level at a time -> "... goes
//       straight to key pictures only"; that return made `return {.timesPicture = true};` (2026-10-15) -> "... a
//       file even key pictures cannot keep up with draws every picture it decodes" and "... each update of such a
//       file costs one decode".
//
// GOAL: S15.618 -- on the device, where the decoder's deepest level is unreferenced
//       pictures skipped, a file still behind there goes unpaced: only AVDISCARD_NONREF is sent, nothing else is
//       left out, and the loop rules above hold; with `comeDown`, every picture again within 4.5 s of decoding
//       getting cheap. In software the deepest is key pictures only. False if the device is steered at
//       Unreferenced (its sound moved mid-pass), sent a deeper level, or never comes back down.
// WHY-THIS-SHAPE: the path is read from the packets themselves (a decoder with hw_device_ctx), not from the
//       entry's name, so a hardware entry that fell back to software is graded as software; packets on both
//       paths answer neither and are not verified.
// CONTROL: control_batch.sh 2026-10-14, reverted, RTX 3080 Ti, phase climb unless named: deepestSkip() NonKey on the
//       device (renderer/video/video_decoder.cpp) -> "FAIL: climb rule: on the device a file no level can help goes
//       to unreferenced skipped" (also catchUpCap's line); DecodeLoad::add's climb ending at the deepest level, steered,
//       instead of unpaced (backend/daemon/wallpaper_manager.cpp) -> "... its sound is never moved mid-pass once it
//       climbs"; add()'s step-down loop never entered -> "... every
//       picture sent again within 4.5 s". Green (2026-10-16): 3 loops, seek +0.03 ms after the draw, opening beep
//       +13.2 to +17.2 ms after the commit, 4 beeps.
//
// Runs `file` at `costMs` a returned picture for `windowMs` and grades the above. With `deviceOnly`, returns
// false without grading when the file decodes in software, for the caller to grade its own way.
bool runUnpaced(const char* kGoals, const std::string& rule, const std::string& file, int costMs, int windowMs = 12000,
                bool comeDown = false, bool deviceOnly = false) {
    SoundServer sound(kGoals);
    if (!sound.ready) return true;
    StallReset reset;
    Daemon d(smallConfig());
    if (!startHeard(d, file, kGoals)) return true;
    std::this_thread::sleep_for(2500ms);
    const std::optional<bool> onDevice = decodedOnDevice(file);
    if (!onDevice) {
        notVerified(kGoals, "the file's packets went to both decoder paths, or to none, so its deepest level is not known");
        return true;
    }
    if (deviceOnly && !*onDevice) return false;
    costFile(file);
    const int64_t costFrom = probe::nowNs();
    probe::g_costMs.store(costMs);
    std::this_thread::sleep_for(std::chrono::milliseconds(windowMs));
    const int64_t to = probe::nowNs();
    probe::g_costMs.store(0);
    const int64_t from = costFrom + 2000 * kMs;
    const std::vector<probe::LevelChange> levels = levelChangesOf(file, costFrom, to);
    const Capture cap = takeCapture();
    const auto [silence, heardFrames] = longestSilence(cap, from, to, true);
    std::printf("audio_playback_test: %s: levels %s; the tone's longest silence from 2 s in %.1f ms of %.1f s heard\n",
                rule.c_str(), describeLevels(levels, costFrom).c_str(), silence * 1e3 / kRate, heardFrames / double(kRate));
    check(heardFrames >= static_cast<uint64_t>(kRate) * 3, rule + ": the sound was heard through the window",
          fmt("%.1f s", heardFrames / double(kRate)));
    if (*onDevice) {
        check(onlyUnreferenced(levels),
              rule + ": on the device a file no level can help goes to unreferenced skipped, the deepest there, and nothing else",
              describeLevels(levels, costFrom));
    } else {
        check(!levels.empty() && levels.front().discard == AVDISCARD_NONKEY,
              rule + ": in software a file no level can help goes straight to key pictures only", describeLevels(levels, costFrom));
    }
    std::vector<int64_t> decodedAt;
    for (const probe::PictureRecord& p : picturesOf(file)) {
        if (p.at > from && p.at <= to) decodedAt.push_back(p.at);
    }
    const int64_t interval = fileInterval(file);
    const std::vector<Shown> shown = shownPictures(d.takeCommits(), file);
    int inWindow = 0, dropped = 0, worstHandOffs = 0;
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
    for (size_t i = 1; i < shown.size(); ++i) {
        const Shown& s = shown[i];
        if (s.commit.at <= from || s.commit.at > to) continue;
        ++inWindow;
        worstHandOffs = std::max(worstHandOffs, handOffsUntil(s.commit.at));
        previous = s.commit.at;
        if (s.presentation && shown[i - 1].presentation && *s.presentation >= *shown[i - 1].presentation) {
            dropped += skippedBetween(*shown[i - 1].presentation, *s.presentation, interval);
        }
    }
    // Graded from the first level on: before it the file still keeps pace, and hands its sound on.
    const int64_t unpacedFrom = levels.empty() ? to : std::max(from, levels.front().at);
    std::vector<int64_t> loops, allLoops;
    for (const int64_t at : passStartCommits(shown)) {
        if (at > unpacedFrom) allLoops.push_back(at);
        if (at > unpacedFrom && at < to - 2100 * kMs) loops.push_back(at);
    }
    const std::vector<probe::SoundSeek> seeks = soundSeeksSince(from - 2500 * kMs, to);
    const std::vector<probe::SoundSeek> midPass = midPassSeeks(levels.empty() ? to : levels.front().at, to);
    const std::vector<int64_t> heard = onsets(cap, 0.1f, 480);
    std::vector<int64_t> blits;
    {
        std::lock_guard<std::mutex> lock(probe::g_blitMutex);
        blits = probe::g_blits;
    }
    // GOAL: a loop's restart seek comes after the loop's first picture is drawn. False if the sound is restarted
    //       before the draw, at steer time.
    // WHY-THIS-SHAPE: graded against the draw (the last blit recorded before the loop's commit; one screen
    //       here), not the commit: the commit time is taken in the frame callback, after the round has drawn and
    //       played, so a correct seek lands from about 20 us before it to 20 us after (seen "-0.0 ms" under
    //       load). The blit comes before the play, and the play before the seek, so a correct seek is never
    //       before the draw; green, +0.024 to +0.032 ms.
    // CONTROL: control_run.sh 2026-10-16, reverted, backend/daemon/wallpaper_manager.cpp, phase climb: the unpaced
    //       restart played at steer time, at `now`, with 5 ms before the draw -> "FAIL: climb rule: the seek to
    //       the pass's start comes at or after the loop's first picture is drawn" (each loop) and "... never sent
    //       to a pass's start by its own clock". Others above S15.561's.
    const auto drawnFor = [&](int64_t commitAt) {
        const auto it = std::upper_bound(blits.begin(), blits.end(), commitAt);
        return it == blits.begin() ? commitAt : *std::prev(it);
    };
    // A seek to the pass's start counts for the loop whose first draw it follows, by at most 100 ms past the
    // commit; any other from the window on is a hand-on by the sound's own clock.
    int handOns = 0, gradedLoops = 0;
    for (const probe::SoundSeek& s : seeks) {
        const bool atLoop = std::any_of(allLoops.begin(), allLoops.end(),
                                        [&](int64_t at) { return s.at >= drawnFor(at) && s.at <= at + 100 * kMs; });
        std::printf("  a seek %+.0f ms from the slowdown, to %.1f ms of the file%s\n", (s.at - costFrom) / 1e6, s.targetNs / 1e6,
                    atLoop ? ", at or just after a loop's first draw" : "");
        if (s.at >= unpacedFrom && isPassStartSeek(s.targetNs) && !atLoop) ++handOns;
    }
    for (size_t k = 0; k < loops.size(); ++k) {
        const int64_t at = loops[k];
        const auto seek = std::find_if(seeks.begin(), seeks.end(), [&](const probe::SoundSeek& s) {
            return isPassStartSeek(s.targetNs) && s.at >= at - 400 * kMs && s.at <= at + 100 * kMs;
        });
        std::optional<int64_t> opening;
        for (const int64_t h : heard) {
            if (h >= at - 400 * kMs && h <= at + 150 * kMs) {
                opening = h;
                break;
            }
        }
        // The pass runs to the next loop, or 2.1 s for the last: the whole of the sound's two seconds.
        const int64_t end = k + 1 < loops.size() ? loops[k + 1] : at + 2100 * kMs;
        const auto beeps = std::count_if(heard.begin(), heard.end(), [&](int64_t h) { return h >= at - 400 * kMs && h < end - 400 * kMs; });
        const int64_t drawn = drawnFor(at);
        std::printf("audio_playback_test: %s: loop %+.0f ms from the slowdown, drawn %.3f ms before its commit: the seek to the "
                    "pass's start %+.3f ms from that draw, the opening beep %+.1f ms from the commit, %ld beeps in the pass\n",
                    rule.c_str(), (at - costFrom) / 1e6, (at - drawn) / 1e6, seek != seeks.end() ? (seek->at - drawn) / 1e6 : -1e9,
                    opening ? (*opening - at) / 1e6 : -1e9, static_cast<long>(beeps));
        ++gradedLoops;
        check(seek != seeks.end() && seek->at >= drawn,
              rule + ": the seek to the pass's start comes at or after the loop's first picture is drawn",
              seek != seeks.end() ? fmt("%+.3f ms", (seek->at - drawn) / 1e6) : std::string("no seek near the loop"));
        check(opening && *opening >= at && *opening <= at + 60 * kMs,
              rule + ": the opening beep is heard 0-60 ms after the loop's first commit",
              opening ? fmt("%+.1f ms", (*opening - at) / 1e6) : std::string("no beep near the loop"));
        check(beeps == 4, rule + ": each beep is heard once in a pass", fmt("%.0f beeps", static_cast<double>(beeps)));
    }
    std::printf("audio_playback_test: %s: from 2 s into the slowdown: %d commits, %d pictures skipped, at most %d decoder "
                "hand-offs before a commit; %zu loops of the pictures graded; %d hand-ons by the sound's own clock; %zu "
                "mid-pass seeks from the first level on\n",
                rule.c_str(), inWindow, dropped, worstHandOffs, loops.size(), handOns, midPass.size());
    check(inWindow >= 10 && loops.size() >= 2 && gradedLoops == static_cast<int>(loops.size()),
          rule + ": ten commits and two loops of the pictures in the window, each graded",
          fmt("%.0f commits, %.0f loops", inWindow, static_cast<double>(loops.size())));
    check(dropped == 0, rule + ": a file even key pictures cannot keep up with draws every picture it decodes",
          fmt("%.0f pictures skipped", dropped));
    check(worstHandOffs <= 2, rule + ": each update of such a file costs one decode",
          fmt("%.0f pictures handed on before one commit", worstHandOffs));
    check(!levels.empty() && midPass.empty(), rule + ": its sound is never moved mid-pass once it climbs",
          fmt("%.0f seeks mid-pass", static_cast<double>(midPass.size())));
    check(handOns == 0, rule + ": its sound is never sent to a pass's start by its own clock",
          fmt("%.0f seeks to the start away from the pictures' loops", handOns));
    // Read from the level sent, not from the pictures returned: unreferenced skipping leaves nothing out of a
    // file with no B pictures, so its pictures look like every picture while it is still unpaced.
    if (comeDown) {
        std::this_thread::sleep_for(6000ms);
        std::optional<int64_t> again;
        for (const probe::LevelChange& c : levelChangesOf(file, to, probe::nowNs())) {
            if (c.discard == AVDISCARD_DEFAULT) {
                again = c.at;
                break;
            }
        }
        std::printf("audio_playback_test: %s: after the slowdown: %s\n", rule.c_str(),
                    describeLevels(levelChangesOf(file, to, probe::nowNs()), to).c_str());
        check(again && *again - to <= 4500 * kMs, rule + ": every picture sent again within 4.5 s of decoding getting cheap",
              again ? fmt("%.0f ms after", (*again - to) / 1e6) : std::string("never, in 6 s"));
    }
    return true;
}

// GOAL: S15.617 -- a file that turns unpaced after its sound was handed on to the next pass by the paced
//       steering, and before that pass's first picture is drawn, keeps that sound: no second seek to the pass's
//       start from the loop's first commit to 100 ms after, and the handed-on pass hears its four beeps once each.
//       False if the restart at the draw ignores that the sound already plays this pass and replays its opening.
// WHY-THIS-SHAPE: a cost sweep cannot place the turn between the hand-on and the loop (it went unpaced before any
//       hand-on), so the order is staged. A pass's first picture is never counted at the first level, so the turn
//       needs a slow decode of an earlier picture after the hand-on; at 1x the decoder is already past them. At
//       speed 2 the hand-on's 100 ms lead is two pictures of the all-key 10-a-second file: it runs at 44 (or 42)
//       ms a picture (paced, about 0.88 of the 50 ms interval), then from 75 ms before a hand-on, timed from the
//       one before (they come 1 s apart), costs 60 then 200 ms. Raising the cost at the hand-on seek instead
//       set up only 6 runs in 8: the hand-on round and the draw before it tie, and when the draw went first the
//       slow decode fell on the pass's uncounted first picture. The 200 ms sample, capped at 2, lifts the average past 1.0.
//       200 ms and not more: the loop then lands 0.6 s into the handed-on sound, between beeps, so a replay from
//       the start adds an onset; at 300 ms it landed on a beep and the replay was inaudible to the count.
//       The order is read back from the records, never assumed. A hand-on seek proves the file was still paced
//       (an unpaced file never hands on). The level change is recorded at the next packet sent, and every packet
//       sent before the second picture of the pass is committed carries a level chosen at or before the decode of
//       the pass's first picture, which precedes that picture's draw. So: the hand-on, then the first level change,
//       recorded before the commit after the loop's first; the loop at least 300 ms after the hand-on. A bound on
//       the level change's time alone cannot replace this: the decoder sends packets ahead, by a depth the test
//       does not control. An attempt not showing this order is printed and not graded.
// COVERAGE-ASSERT: one staged attempt of three, else not verified; a start whose sound never arrives is not
//       verified too, not a failure of this rule.
// CONTROL: control_batch.sh 2026-10-15, reverted, phase unpacedAfterHandOn: steerSoundLocked's unpaced restart
//       made unconditional (`if (true || lead < -kSoundDrift ...)`, backend/daemon/wallpaper_manager.cpp) -> "FAIL:
//       unpaced after hand-on rule: the sound handed on keeps playing, with no second seek to the pass's start" and
//       "... each beep of the handed-on pass is heard once" (5 beeps), staged on the first attempt. Green: turn +200 ms, loop +401 ms, 4 beeps;
//       10 of 10 Lavapipe runs staged.
void runUnpacedAfterHandOnChecks() {
    const char* kGoals = "S15.617 (a file turning unpaced after a hand-on keeps its sound)";
    const std::string rule = "unpaced after hand-on rule";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    StallReset reset;
    const std::string file = asset("wallpaper_beeps_allkey10.mp4");
    std::string unstaged;
    for (int attempt = 1; attempt <= 3; ++attempt) {
        probe::g_costMs.store(0);
        probe::g_costAtHandOver.store(0);
        probe::g_costAtHandOverAt.store(0);
        Daemon d(smallConfig());
        resetCapture();
        const int64_t started = probe::nowNs();
        d.play(file, ScreenFocus::Desktop);
        d.properties({{"speed", 2.0}});
        if (!d.waitForCommits(3, 15s) ||
            !waitFor(kSoundArrivalDeadline, [&] { return probe::g_lastLoudQueuedAt.load() > started; })) {
            notVerified(kGoals, "the daemon did not start drawing and sounding the file, so nothing could be staged");
            return;
        }
        std::this_thread::sleep_for(2000ms);
        costFile(file);
        const int64_t costFrom = probe::nowNs();
        probe::g_costMs.store(attempt == 2 ? 42 : 44);
        std::this_thread::sleep_for(1000ms);
        // Hand-ons at speed 2 come one 1 s pass apart, and a picture is decoded in the round that follows the
        // draw before it, every 50 ms. Armed 75 ms before the next hand-on, the first decode after it is the
        // one due at the hand-on: costing 60 ms, it ends after the hand-on's own round would have run, so the
        // round that ends it hands on first (else the two tie, and the draw going first leaves the next decode
        // to the pass's uncounted first picture); the 200 ms decode after it is then a counted one.
        int64_t lastHandOn = 0;
        const bool sawHandOn = waitFor(2500ms, [&] {
            for (const probe::SoundSeek& sk : soundSeeksSince(costFrom + 1000 * kMs, probe::nowNs())) {
                if (isPassStartSeek(sk.targetNs)) lastHandOn = sk.at;
            }
            return lastHandOn != 0;
        });
        if (sawHandOn) {
            sleepUntil(lastHandOn + 925 * kMs);
            {
                std::lock_guard<std::mutex> lock(probe::g_costMutex);
                probe::g_costCycle.assign(64, 200);
                probe::g_costCycle.front() = 60;
                probe::g_costCycleAt = 0;
            }
            // Only records when the next hand-on comes; the cycle sets the cost.
            probe::g_costAtHandOver.store(200);
        }
        const bool fired = sawHandOn && waitFor(4500ms, [] { return probe::g_costAtHandOverAt.load() != 0; });
        std::this_thread::sleep_for(3000ms);
        const int64_t to = probe::nowNs();
        probe::g_costMs.store(0);
        probe::g_costAtHandOver.store(0);
        {
            std::lock_guard<std::mutex> lock(probe::g_costMutex);
            probe::g_costCycle.clear();
            probe::g_costCycleAt = 0;
        }
        if (!fired) {
            unstaged += fmt(sawHandOn ? "attempt %.0f: no hand-on came within 4.5 s of the slow decode; "
                                      : "attempt %.0f: no hand-on came within 2.5 s at the paced cost; ",
                            attempt);
            continue;
        }
        const int64_t handOn = probe::g_costAtHandOverAt.load();
        const std::vector<probe::LevelChange> levels = levelChangesOf(file, costFrom, to);
        const std::vector<probe::SoundSeek> seeks = soundSeeksSince(costFrom, to);
        const std::vector<Shown> shown = shownPictures(d.takeCommits(), file);
        const std::vector<int64_t> loops = passStartCommits(shown);
        const Capture cap = takeCapture();
        const auto loop = std::find_if(loops.begin(), loops.end(), [&](int64_t l) { return l > handOn; });
        std::optional<int64_t> afterLoop;
        for (const Shown& sh : shown) {
            if (loop != loops.end() && sh.commit.at > *loop) {
                afterLoop = sh.commit.at;
                break;
            }
        }
        const std::optional<int64_t> turned = levels.empty() ? std::nullopt : std::optional(levels.front().at);
        std::printf("audio_playback_test: %s: attempt %d: levels %s; hand-on %+.0f ms from the slowdown, the next loop %+.0f ms, "
                    "the commit after it %+.0f ms\n",
                    rule.c_str(), attempt, describeLevels(levels, costFrom).c_str(), (handOn - costFrom) / 1e6,
                    loop != loops.end() ? (*loop - costFrom) / 1e6 : -1.0, afterLoop ? (*afterLoop - costFrom) / 1e6 : -1.0);
        for (const probe::SoundSeek& sk : seeks) {
            std::printf("  a seek %+.0f ms from the slowdown, to %.1f ms\n", (sk.at - costFrom) / 1e6, sk.targetNs / 1e6);
        }
        if (loop == loops.end() || !afterLoop || !turned || *turned <= handOn || *turned >= *afterLoop ||
            *loop - handOn < 300 * kMs || *loop + 1500 * kMs > to) {
            unstaged += fmt("attempt %.0f: the order hand-on, turn, loop was not shown; ", attempt);
            continue;
        }
        const bool restarted = std::any_of(seeks.begin(), seeks.end(), [&](const probe::SoundSeek& sk) {
            return isPassStartSeek(sk.targetNs) && sk.at >= *loop && sk.at <= *loop + 100 * kMs;
        });
        const std::vector<int64_t> heard = onsets(cap, 0.1f, 480);
        const auto beeps = std::count_if(heard.begin(), heard.end(),
                                         [&](int64_t h) { return h >= handOn - 50 * kMs && h < handOn + 950 * kMs; });
        std::printf("audio_playback_test: %s: staged: the turn %+.0f ms and the loop %+.0f ms from the hand-on; %s; %ld beeps "
                    "in the handed-on pass\n",
                    rule.c_str(), (*turned - handOn) / 1e6, (*loop - handOn) / 1e6,
                    restarted ? "a second seek to the start at the loop" : "no second seek to the start", static_cast<long>(beeps));
        check(!restarted, rule + ": the sound handed on keeps playing, with no second seek to the pass's start",
              fmt("the loop %.0f ms after the hand-on", (*loop - handOn) / 1e6));
        check(beeps == 4, rule + ": each beep of the handed-on pass is heard once", fmt("%.0f beeps", static_cast<double>(beeps)));
        return;
    }
    notVerified(kGoals, "no attempt turned unpaced between a hand-on and the next loop: " + unstaged);
}

void runCatchUpCapChecks() {
    runUnpaced("S15.561 (a file even key pictures cannot keep up with plays unpaced under its sound)", "catch-up cap rule",
               asset("wallpaper_beeps_allkey.mp4"), 60);
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
//       cannot close: ..., after the hold" (the seek 70.5 ms after the stall). Test-side, 2026-10-13: the sound
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
// CONTROL: control_batch.sh 2026-10-13, control_run.sh again 2026-10-16, reverted: steerSoundLocked's `chased` made
//       `catchUp == CatchUp::Chasing` (the move waits for a hold with no clock too), backend/daemon/wallpaper_manager.cpp
//       -> "FAIL: no clock rule: a sound left off its pictures with no clock is moved back to them once" (0 seeks;
//       green 1); the stored reading and the taken anchor both ignored -> the same line (0). timingOfNextBuffer in renderer/audio/audio_output.cpp answering "not timed"
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
    probe::g_convertHoldAfterStallMs.store(40);
    probe::g_stallPictureMs.store(600);
    std::this_thread::sleep_for(1500ms);
    const int64_t end = probe::nowNs();
    const int hidden = probe::g_streamTimesHidden.load() - hiddenBefore;
    const int left = probe::g_stallPictureMs.exchange(0);
    const bool convertHeld = probe::g_convertHoldMs.exchange(0) == 0 && probe::g_convertHoldAfterStallMs.exchange(0) == 0 &&
                             probe::g_convertHeldAt.load() > at;
    probe::g_hideStreamTime.store(false);
    const std::vector<probe::SoundSeek> found = soundSeeksSince(at, end);
    const size_t seeks = found.size();
    const Capture cap = takeCapture();
    const int64_t serverGap = longestBufferGap(cap, at, end);
    std::printf("audio_playback_test: no clock rule: %zu sound seeks after the stall; %d of AudioOutput's timing reads hidden; "
                "the server at most %.1f ms between cycles\n",
                seeks, hidden, serverGap / 1e6);
    // Printed, not graded: whether the server or the daemon's own drawing was late near each seek.
    const std::vector<Shown> shown = shownPictures(d.takeCommits(), asset("wallpaper_beeps_long.mp4"));
    for (const probe::SoundSeek& s : found) {
        int64_t commitGap = 0;
        for (size_t i = 1; i < shown.size(); ++i) {
            if (shown[i].commit.at > s.at - 200 * kMs && shown[i].commit.at <= s.at + 20 * kMs) {
                commitGap = std::max(commitGap, shown[i].commit.at - shown[i - 1].commit.at);
            }
        }
        std::printf("  a seek %.1f ms after the stall began, to %.1f ms of the file; in the 200 ms before it the server at most "
                    "%.1f ms between cycles, the daemon at most %.1f ms between commits\n",
                    (s.at - at) / 1e6, s.targetNs / 1e6, longestBufferGap(cap, s.at - 200 * kMs, s.at + 20 * kMs) / 1e6,
                    commitGap / 1e6);
    }
    check(left == 0 && hidden >= 20, "no clock rule: the stall was reached while the server's timing was hidden",
          fmt("%.0f ms of the stall unused, %.0f reports hidden", left, hidden));
    check(convertHeld, "no clock rule: the round after the stall was held 40 ms between steering and drawing");
    if (left != 0 || hidden < 20 || !convertHeld) return;
    // GOAL: S15.563's count is graded only where it is the daemon's: a run where the server went past
    //       kServerLateBound between cycles in [at, end) is reported not verified, the gap printed.
    // WHY-THIS-SHAPE: such a late cycle earns a seek of its own (as in runLeadLimitChecks), so grading
    //       `seeks == 1` there charges the server's lateness to the daemon; dropping the bound instead
    //       would grade nothing. The per-seek gaps above say, on a failure, whether lateness was near it.
    // CONTROL: control_run.sh 2026-10-16, reverted: the first no-clock move in steerSoundLocked,
    //       backend/daemon/wallpaper_manager.cpp, played 3 * kSoundDrift late (so it is moved again) -> "FAIL: no
    //       clock rule: a sound left off its pictures with no clock is moved back to them once" (2 seeks, 633 and
    //       667 ms after the stall began, the server at most 21.4 ms between cycles; green 1 seek). Test-side, by
    //       hand: this bound made 0 -> NOT VERIFIED with the 21.4 ms gap printed, the entry a skip, no FAIL.
    //
    // GOAL: a moved sound with no clock is anchored where its picture is recorded as shown, not where the round
    //       predicted before converting and drawing: with that round held 40 ms between steering and drawing,
    //       still exactly one seek. False if the play is made at steer time at `drawnAt`: the sound is then 40 ms
    //       ahead of a picture nothing re-anchors, and the next round moves it again.
    // WHY-THIS-SHAPE: the hold is in the conversion pass (vkCmdDraw), after steerSoundLocked and before the
    //       round's `now` for the draw, armed by the stall's end so it lands in the round that moves the sound.
    //       Without it the steer-to-draw gap is under a millisecond and the old code passes on an idle machine
    //       (observed 2026-10-16: the break below with the hold unarmed and its gate removed, 1 seek, green).
    // CONTROL: control_run.sh 2026-10-16, reverted: playOnDrawn's `planned.due <= now` branch disabled, so the move
    //       plays at once at `drawnAt` (backend/daemon/wallpaper_manager.cpp) -> "FAIL: no clock rule: a sound left
    //       off its pictures with no clock is moved back to them once" (2 seeks, 625 and 665 ms after the stall
    //       began, the server at most 21.4 ms between cycles); green 1 seek. Test-side: the hold not armed -> "FAIL:
    //       no clock rule: the round after the stall was held 40 ms between steering and drawing".
    if (serverGap > kServerLateBound) {
        notVerified(kGoals, fmt("the sound server went %.1f ms between cycles, which earns a seek of its own", serverGap / 1e6));
        return;
    }
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
// CONTROL: control_batch.sh 2026-10-15, reverted: `if (session.pass && shift == zero)`, the branch that asks for
//       the next pass kHandOverLead ahead, made `if (false && ...)` in steerSoundLocked,
//       backend/daemon/wallpaper_manager.cpp -> "FAIL: hand-over rule: from the second loop on, the next pass's sound
//       is asked for ahead" (each seek 0.6 ms after the last commit; 66.7 ms before it green on both entries).
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
            if (isPassStartSeek(s.targetNs) && s.at <= at && s.at > at - 500 * kMs) seekAt = s.at;
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
// not: the deaths on record before and after, the commits and the time from the first to the last, whether
// its own sound was heard, and whether the death was cleared within `waitMs` -- `untilCleared` ends the wait
// as soon as it is. `levels` names the decoder's skip levels, `device` where it decoded.
struct CleanRun {
    int before = -1, after = -1;
    size_t commits = 0;
    double spanMs = 0;
    bool loud = false, cleared = false;
    std::string levels, device;
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
    const std::vector<Commit> commits = d.takeCommits();
    out.commits = commits.size();
    if (commits.size() >= 2) out.spanMs = (commits.back().at - commits.front().at) / 1e6;
    out.loud = probe::g_lastLoudQueuedAt.load() > started;
    const std::string name = std::filesystem::path(heard).filename().string();
    out.levels = describeLevels(levelChangesOf(name, started, probe::nowNs()), started);
    const std::optional<bool> onDevice = decodedOnDevice(name);
    out.device = !onDevice ? "unknown" : *onDevice ? "device" : "software";
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
//       the same process also made sound, and a bare "ever heard" counted that. The slowed run's coverage is
//       the time its draws span, not their number: decoded in software (Lavapipe) the late file is cut to intra
//       pictures only, about 8 draws in 3 s, while those draws, had they counted, span past cleanPlayAfter.
//       A count of 10 failed there on a correct daemon; 10 draws inside half a second would earn no credit.
// COVERAGE-ASSERT: each run started carrying the death and was heard; the slowed run's draws span at least
//       1.5 s (cleanPlayAfter and a half), the real-time run drew at least 10 pictures.
// CONTROL: control_batch.sh, --phase '^cleanCredit$', 2026-10-16, reverted, advanceSessionLocked,
//       backend/daemon/wallpaper_manager.cpp: creditCleanPlayLocked passed `true || onTime` -> "FAIL: clean credit
//       rule: a heard file drawn late does not play clean" on both entries (0 deaths after 3 s; Lavapipe 8 commits
//       over 2249 ms, decoded in software, intra only from +822 ms; NVIDIA 28 over 2668 ms, on the device).
//       2026-10-13: creditCleanPlayLocked never adding to `played` -> "FAIL: clean credit rule: the same file drawn on time plays
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
    std::printf("audio_playback_test: clean credit rule: slowed: %d deaths on record before, %d after 3 s; %zu commits over "
                "%.0f ms; heard %d; decoded on %s; skip levels: %s\n",
                slow.before, slow.after, slow.commits, slow.spanMs, slow.loud ? 1 : 0, slow.device.c_str(),
                slow.levels.empty() ? "none" : slow.levels.c_str());
    // CONTROL: test-side, 2026-10-16, both drivers: the slowed run given 1200 ms instead of 3000 ms -> this line
    //       red, 7 commits over 919 ms (NVIDIA) and 4 over 748 ms (Lavapipe). Restored.
    const bool slowCovered = slow.before == 1 && slow.spanMs >= 1500 && slow.loud;
    check(slowCovered, "clean credit rule: the slowed run started carrying a death, drew and was heard",
          fmt("%.0f deaths at the start, draws over %.0f ms, heard %.0f", slow.before, slow.spanMs, slow.loud ? 1.0 : 0.0));
    if (slowCovered) {
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
// CONTROL: control_batch.sh 2026-10-14, reverted, phase skip: DecodeLoad::skip() returning PictureSkip::Nothing,
//       backend/daemon/wallpaper_manager.h -> "FAIL: skip rule: a file that cannot keep up decodes under half its
//       pictures" (100 of 100); stepping down removed from DecodeLoad::add (wallpaper_manager.cpp) -> "... skipping
//       stops within 1.5 s of decoding getting cheap" (never). control_batch.sh 2026-10-15, phase skip:
//       AudioTrack::clock() 40 ms ahead (renderer/audio/audio_output.cpp) -> "FAIL: skip rule: each beep is heard
//       with the picture at its timestamp" (+39.98 ms each). Marking every level above none unpaced stays green
//       here (the unreferenced level keeps up). Green: 49 of 132, no seek, every picture again 0.74 s after.
//       The seek line: `decoderBehind() ||` taken out of `chased`, or the move rule's bound cut to 2 ms, stays green
//       here (0 seeks: the pictures sit on the clock); steerSoundLocked moving the sound every round (`if (true ||
//       behind > kSoundDrift ...`, backend/daemon/wallpaper_manager.cpp, control_batch.sh 2026-10-15) -> "FAIL: skip
//       rule: a skipping file keeps up with its sound without being moved" (49 seeks).
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

// GOAL: S15.573 -- while a heard file is still settling on the skip level it just entered (fewer than eight
//       pictures measured there), pictures late from the level before do not move its sound: no sound seek from
//       the first level change to 1.5 s after. False if a catch-up hold that runs out while the decoder is still
//       settling moves the sound to the late picture.
// WHY-THIS-SHAPE: the `skip` phase's 45 ms cost closes the lag within two pictures of the first level, so its
//       hold never runs out while settling and it stayed green with the settling rule removed. At 70 and 85 ms a
//       picture of the B-frame file the first level keeps up only just, so the lag built before it closes slowly
//       and the hold runs out inside the settling window. The one move before the first level change is the
//       accepted one and is not graded. Not graded when the sound server ran a cycle over kServerLateBound late
//       in the window (as S15.538): such a cycle earns a seek of its own.
// CONTROL: control_batch.sh 2026-10-15, reverted, Lavapipe, phase skipSettling: `session.decodeLoad.decoderBehind()
//       ||` taken out of `chased` in steerSoundLocked (backend/daemon/wallpaper_manager.cpp) -> "FAIL: skip settling
//       rule: at 70 ms a picture, no sound seek while the level settles" and the same at 85 ms, with the server
//       guard in place (re-run 2026-10-15, held). Green: no seek in the window at either cost.
void runSkipSettlingChecks() {
    const char* kGoals = "S15.573 (a file still settling on a skip level is not moved)";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    StallReset reset;
    const std::string file = asset("wallpaper_beeps_bframes.mp4");
    for (const int cost : {70, 85}) {
        Daemon d(smallConfig());
        if (!startHeard(d, file, kGoals)) return;
        std::this_thread::sleep_for(1000ms);
        costFile(file);
        const int64_t from = probe::nowNs();
        probe::g_costMs.store(cost);
        std::this_thread::sleep_for(3000ms);
        const int64_t to = probe::nowNs();
        probe::g_costMs.store(0);
        const std::vector<probe::LevelChange> levels = levelChangesOf(file, from, to);
        const std::vector<probe::SoundSeek> seeks = soundSeeksSince(from, to);
        std::printf("audio_playback_test: skip settling rule: %d ms: levels %s\n", cost, describeLevels(levels, from).c_str());
        for (const probe::SoundSeek& sk : seeks) {
            std::printf("  a seek %+.0f ms from the slowdown, to %.1f ms\n", (sk.at - from) / 1e6, sk.targetNs / 1e6);
        }
        if (levels.empty()) {
            notVerified(kGoals, "the slowed file never changed its skip level");
            return;
        }
        const int64_t first = levels.front().at;
        const auto settling = std::count_if(seeks.begin(), seeks.end(), [&](const probe::SoundSeek& sk) {
            return sk.at >= first && sk.at <= first + 1500 * kMs;
        });
        const int64_t serverGap = longestBufferGap(takeCapture(), first, first + 1500 * kMs);
        if (serverGap > kServerLateBound) {
            notVerified(kGoals, fmt("at %.0f ms a picture the sound server went ", cost) +
                                    fmt("%.1f ms between cycles, which earns a seek of its own", serverGap / 1e6));
            continue;
        }
        check(settling == 0, fmt("skip settling rule: at %.0f ms a picture, no sound seek while the level settles", cost),
              fmt("%.0f seeks", static_cast<double>(settling)));
    }
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
//       -100 ms, on both entries). `passAway` made kSoundDrift stays green (2 seeks a loop, both entries,
//       2026-10-13): the move lines the sound up before the pass start, so the margin is not reached here;
//       it is graded by S15.587 (behindAtLoop) and S15.588 (longLastLoop).
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
        check(seeks.size() == 2 && isPassStartSeek(seeks[0].targetNs),
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
//       followed by the new pass's first picture a second or more later, within 10.5 s of the first draw. The
//       window starts at that draw, not at the daemon's start: start-up slowed by load earns no credit either
//       way, but counted in the window it left two late pass starts, not three, and failed this assert.
// CONTROL: control_run.sh 2026-10-16, reverted: advanceSessionLocked's `onTime` given `pace.timesPicture ||`
//       back, backend/daemon/wallpaper_manager.cpp -> "FAIL: late timed rule: a pass's first picture the sound
//       timed but drew late earns no clean credit" (cleared 5961 ms in); green: kept after 10.5 s, 3 late pass
//       starts. Test-side: the holds taken out -> "FAIL: late timed rule: the run started ... drew three pass
//       starts late" (0), the death then cleared, so the record and its clearing are live here.
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
    // The window runs from the first draw: start-up under load is not play, earns no credit, and would
    // otherwise eat the late pass starts the window must hold.
    if (!waitFor(5000ms, [&] { return d.commitCount() > 0; })) {
        check(false, "late timed rule: the restored wallpaper was drawn within 5 s");
        return;
    }
    const int64_t firstDrawn = d.takeCommits().front().at;
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
    std::printf("audio_playback_test: late timed rule: first drawn %.0f ms in; %d deaths on record before, %d after %.1f s "
                "from then (cleared %.0f ms in); %zu draws held, %d late pass starts, %zu sound seeks; heard %d\n",
                (firstDrawn - started) / 1e6, before, after, kWindowMs / 1000.0, clearedAt ? (clearedAt - started) / 1e6 : -1.0,
                holds.size(), lateStarts, seeks.size(), loud ? 1 : 0);
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

// GOAL: S15.579 -- a heard file whose decoding hovers about its own rate keeps its sound unmoved, however its
//       skip level moves: the twelve-second baseline beeps file, each picture costing 41 ms eight times then
//       27 ms eight times (just over its interval on average), from 2 s after the slowdown for 6 s, goes on
//       drawing and its sound is never moved. False if the sound is moved to the pictures while the level settles.
// WHY-THIS-SHAPE: the level may go up and down in this window -- coming down on the first cheap picture is
//       the chosen behaviour, and a file at the edge of keeping up may cycle -- so the levels are printed, not
//       graded. That a file still over load does not step down is S15.599's; beeps on their pictures while
//       skipping are S15.597's and S15.601's. The window lies inside one pass (1 s to 11 s of the file), so any
//       seek is a move mid-pass; under S15.538's sound-server guard.
// COVERAGE-ASSERT: six pictures drawn in the window.
// CONTROL: control_batch.sh 2026-10-14, reverted, phase hover, backend/daemon/wallpaper_manager.cpp: decoderBehind()
//       taken out of steerSoundLocked's `chased` -> "FAIL: hover rule: a hovering file's sound is never moved" (1
//       seek). control_run.sh 2026-10-15, reverted, test-side: picture reads stalled 3.2 s from 3 s into the window
//       -> "FAIL: hover rule: the hovering file went on drawing" (4 pictures, 2517 ms without one).
void runHoverChecks() {
    const char* kGoals = "S15.579 (a file hovering at its own rate stays in step with its sound)";
    const std::string rule = "hover rule";
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
    const int64_t costFrom = probe::nowNs();
    probe::g_costMs.store(1);
    std::this_thread::sleep_for(2000ms);
    const int64_t from = probe::nowNs();
    std::this_thread::sleep_for(6000ms);
    const int64_t to = probe::nowNs();
    probe::g_costMs.store(0);
    const std::vector<probe::SoundSeek> seeks = soundSeeksSince(from, to);
    const Capture cap = takeCapture();
    const int64_t serverGap = longestBufferGap(cap, from, to);
    const std::vector<Commit> commits = commitsIn(d.takeCommits(), from, to);
    std::printf("audio_playback_test: %s: %zu pictures drawn in 6 s, %zu sound seeks; the server at most %.1f ms between "
                "cycles; levels %s\n",
                rule.c_str(), commits.size(), seeks.size(), serverGap / 1e6,
                describeLevels(levelChangesOf(file, costFrom, to), costFrom).c_str());
    // GOAL: (coverage of the seek check) the file draws throughout the window, not a burst then a stall,
    //       which would leave no picture for the sound to be moved to and the seek check trivially true.
    // WHY-THIS-SHAPE: a bare count of six cannot tell those apart; the longest stretch without a picture,
    //       the window's edges included, can. 1.5 s is the 1 s key spacing at the key-only level plus slack.
    // CONTROL: the test-side stall in the hover phase's block above.
    int64_t longestDark = 0;
    {
        int64_t last = from;
        for (const Commit& c : commits) {
            longestDark = std::max(longestDark, c.at - last);
            last = c.at;
        }
        longestDark = std::max(longestDark, to - last);
    }
    check(commits.size() >= 6 && longestDark <= 1500 * kMs, rule + ": the hovering file went on drawing",
          fmt("%.0f pictures, at most %.0f ms without one", static_cast<double>(commits.size()), longestDark / 1e6));
    if (commits.size() < 6 || longestDark > 1500 * kMs) return;
    if (serverGap > kServerLateBound) {
        notVerified(kGoals, fmt("the sound server went %.1f ms between cycles, which earns a seek of its own", serverGap / 1e6));
    } else {
        check(seeks.empty(), rule + ": a hovering file's sound is never moved",
              fmt("%.0f sound seeks", static_cast<double>(seeks.size())));
    }
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
//       Green on both entries: 45 handed on and 0 skipped; 66 and 112-114. Re-run on the current
//       DecodeLoad::span() (control_run.sh 2026-10-15): held, 16 and 27; 129 and 0.
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
        // GOAL: S15.633 -- one unrepresentative picture just after a level change does not send the climb past a
        //       level that keeps up: at speed 2 the file stays at skipping unreferenced pictures, which hands on about
        //       one picture in three, and does not fall to key pictures only (one in 30). False if a level's span is
        //       trusted from its first sample, where a P picture before a key one reads as 1.
        // WHY-THIS-SHAPE: the floor of 10 above only says the window saw decoding; a file with more key pictures
        //       would pass it at the key level. A quarter of the pictures lies between the two levels' shares.
        // CONTROL: control_run.sh 2026-10-15, reverted, phase speedLoad: DecodeLoad::span() trusting a level's
        //       measured span from one sample (backend/daemon/wallpaper_manager.cpp) -> this line red (6 handed on, 145
        //       skipped) and "... at speed 2.0 the slowed file went on decoding". Green: 66 and 112.
        if (!c.keepsUp) {
            check(h.decoded * 4 >= h.decoded + h.skipped,
                  "speed load rule: at speed 2 the climb stops at the level that keeps up (a quarter of the pictures handed on)",
                  fmt("%.0f handed on, %.0f skipped", h.decoded, h.skipped));
        }
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
// CONTROL: control_run.sh 2026-10-16, reverted, steerSoundLocked, backend/daemon/wallpaper_manager.cpp: the
//       hand-on to a new pass (`return playOnDrawn({*next.presentation, drawnAt})` in the startsPass branch)
//       anchored at `planned.timeline` instead of `drawnAt` -> "FAIL: late hand-on rule: a late pass start moves the
//       sound once, onto its first picture, and drops nothing" (1 seek, to 17.0 ms of the file, and 3 pictures
//       dropped); green 1 seek, 0 dropped, the pass's first picture 184 ms after arming. Test-side (2026-10-15):
//       the hold made 0 -> "FAIL: late hand-on rule: the held draw made the new pass start late" (66.7 ms).
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
    check(seeks.size() == 1 && isPassStartSeek(seeks[0].targetNs) && dropped == 0,
          "late hand-on rule: a late pass start moves the sound once, onto its first picture, and drops nothing",
          fmt("%.0f sound seeks, %.0f pictures dropped", static_cast<double>(seeks.size()), dropped));
}

// The commit time of the first commit in (from, to] that showed the picture at `presentation`.
std::optional<int64_t> commitShowing(const std::vector<Shown>& shown, int64_t presentation, int64_t from, int64_t to) {
    for (const Shown& s : shown) {
        if (s.commit.at > from && s.commit.at <= to && s.presentation && std::llabs(*s.presentation - presentation) < 2 * kMs) {
            return s.commit.at;
        }
    }
    return std::nullopt;
}

// GOAL: S15.587 -- S15.561's restart holds when the pictures trail the sound by little: the all-key beeps file
//       at 10 pictures a second and 110 ms a returned picture (a pass of pictures about 2.2 s against the
//       sound's 2) meets every S15.561 line at each loop. False if the "already plays this pass" test takes the
//       previous loop's restart, a pass ahead, for this pass's sound and skips the restart, or the sound is
//       handed on by its own clock about 200 ms before the pictures loop and then played again.
// WHY-THIS-SHAPE: the same grading as S15.561 (runUnpaced) at the other end of the trail: there the sound
//       waits about 1.6 s, here about 0.2 s, inside half a pass, where a margin-based skip is most likely
//       to misfire.
// CONTROL: see the block on runUnpaced, which names this phase's controls too.
void runBehindAtLoopChecks() {
    runUnpaced("S15.587 (an unpaced file trailing by little restarts its sound with the pictures)", "behind at loop rule",
               asset("wallpaper_beeps_allkey10.mp4"), 110);
}

// Frames above `level` heard in [from, to), on the left channel: how long a beep sounded there.
uint64_t loudFrames(const Capture& c, int64_t from, int64_t to, float level) {
    uint64_t n = 0;
    for (const probe::BufferRecord& b : c.buffers) {
        for (uint64_t i = b.first; i < b.first + b.frames; ++i) {
            const int64_t at = c.heardAt(b, i);
            if (at >= from && at < to && std::fabs(c.left[i]) > level) ++n;
        }
    }
    return n;
}

// One graded attempt of stageLongLastLoop's staging, as the grading sees it.
struct LongLastAttempt {
    int number = 0;
    int64_t ended = 0;  // when the hold of the picture before the last ended
    std::vector<probe::SoundSeek> seeks;  // around the loop
    std::vector<Shown> shown;
    int64_t unhiddenAt = 0;
    uint64_t opening = 0, next = 0;  // loud frames of the pass's opening beep and of its 0.5 s one
};

// S15.588's staging, up to four attempts: beeps_last_long with the picture before the last held 50 ms and the
// new pass's first picture 60 ms as the decoder returns them. With `unhideAt`, the clock is also hidden from the
// hand-over seek on and unhidden as the new pass's picture at that time is returned, after the first-picture
// hold. An attempt that passes the staging gates is handed to `grade`, which grades it or returns why it is not
// graded. Under S15.538's sound-server guard; no attempt graded is not verified.
void stageLongLastLoop(const char* kGoals, const char* rule, std::optional<int64_t> unhideAt,
                       const std::function<std::string(const LongLastAttempt&)>& grade) {
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    StallReset reset;
    const std::string heard = asset("wallpaper_beeps_last_long.mp4");
    // The fixture's pictures at 1.9 s, 1.91 s (held) and 1.92 s; its pass ends at 1.9833 s.
    const int64_t twoBefore = 1900 * kMs, beforeLast = 1910 * kMs, step = 10 * kMs;
    {
        std::lock_guard<std::mutex> lock(probe::g_holdMutex);
        probe::g_holdFile = std::filesystem::path(heard).filename().string();
    }
    probe::g_holdAtNs.store(beforeLast);
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
    std::string skipped;
    for (int attempt = 1; attempt <= 4; ++attempt) {
        probe::g_holdStartedAt.store(0);
        probe::g_holdEndedAt.store(0);
        probe::g_startHoldEndedAt.store(0);
        probe::g_startHoldArmed.store(false);
        probe::g_hiddenAtHandOverAt.store(0);
        probe::g_unhiddenAt.store(0);
        probe::g_unhideArmed.store(false);
        probe::g_unhideAtNs.store(unhideAt.value_or(-1));
        const int hiddenBefore = probe::g_streamTimesHidden.load();
        probe::g_hideAtHandOver.store(unhideAt.has_value());
        probe::g_thenStartHoldMs.store(60);
        probe::g_holdMs.store(50);
        if (!waitFor(3000ms, [] { return probe::g_holdEndedAt.load() != 0; })) {
            probe::g_holdMs.store(0);
            check(false, std::string(rule) + ": the staged hold of the picture before the last was reached within 3 s");
            return;
        }
        const int64_t started = probe::g_holdStartedAt.load(), ended = probe::g_holdEndedAt.load();
        std::this_thread::sleep_for(1000ms);
        probe::g_startHoldArmed.store(false);
        probe::g_unhideArmed.store(false);
        probe::g_hideAtHandOver.store(false);
        probe::g_hideStreamTime.store(false);
        const int64_t startHeldTo = probe::g_startHoldEndedAt.load();
        const int64_t hiddenFrom = probe::g_hiddenAtHandOverAt.load(), unhiddenAt = probe::g_unhiddenAt.load();
        const int hidden = probe::g_streamTimesHidden.load() - hiddenBefore;
        const std::vector<probe::SoundSeek> seeks = soundSeeksSince(started - 300 * kMs, ended + 700 * kMs);
        const Capture cap = takeCapture();
        const int64_t serverGap = longestBufferGap(cap, started - 300 * kMs, ended + 700 * kMs);
        const std::vector<Shown> shown = shownPictures(d.takeCommits(), heard);
        std::optional<int64_t> before;
        for (const Shown& s : shown) {
            if (s.commit.at > started - 200 * kMs && s.commit.at <= started && s.presentation &&
                std::llabs(*s.presentation - twoBefore) < 2 * kMs) {
                before = s.commit.at;
            }
        }
        const std::optional<int64_t> held = commitShowing(shown, beforeLast, ended, ended + 200 * kMs);
        const int64_t late = before && held ? *held - *before - step : -1;
        bool handedOver = false;
        for (const probe::SoundSeek& s : seeks) {
            if (isPassStartSeek(s.targetNs) && s.at < started) handedOver = true;
        }
        // The new pass's sound starts about 35 ms after the hold ends, its 0.5 s beep about 535 ms after.
        const uint64_t opening = loudFrames(cap, ended - 150 * kMs, ended + 300 * kMs, 0.1f);
        const uint64_t next = loudFrames(cap, ended + 400 * kMs, ended + 700 * kMs, 0.1f);
        std::printf("audio_playback_test: %s: attempt %d: %zu sound seeks around the loop, the held "
                    "picture drawn %.1f ms late, the pass's first picture held %s; the opening beep loud %.1f ms, the "
                    "next %.1f ms; the server at most %.1f ms between cycles\n",
                    rule, attempt, seeks.size(), late / 1e6, startHeldTo > ended ? "yes" : "no", opening * 1e3 / kRate,
                    next * 1e3 / kRate, serverGap / 1e6);
        if (unhideAt) {
            std::printf("  the clock hidden %+.1f ms from the hold's end, unhidden %+.1f ms; %d timing reads hidden\n",
                        hiddenFrom ? (hiddenFrom - ended) / 1e6 : 0.0, unhiddenAt ? (unhiddenAt - ended) / 1e6 : 0.0,
                        hidden);
        }
        for (const probe::SoundSeek& s : seeks) {
            std::printf("  a seek %+.1f ms from the hold's end, to %.1f ms of the file\n", (s.at - ended) / 1e6, s.targetNs / 1e6);
        }
        std::string why;
        if (!handedOver) {
            why = "no hand-over seek before the held decode";
        } else if (startHeldTo <= ended) {
            why = "the new pass's first picture was not held";
        } else if (!before || !held) {
            why = "the held picture or the one before it was not drawn";
        } else if (late < 30 * kMs || late > 55 * kMs) {
            why = fmt("the held picture was drawn %.1f ms late, outside 30-55 ms", late / 1e6);
        } else if (next < static_cast<uint64_t>(kRate) / 50) {
            why = fmt("the pass's 0.5 s beep sounded only %.1f ms", next * 1e3 / kRate);
        } else if (serverGap > kServerLateBound) {
            why = fmt("the sound server went %.1f ms between cycles", serverGap / 1e6);
        } else if (unhideAt && (hiddenFrom == 0 || hiddenFrom >= started || hidden <= 0)) {
            why = "the clock was not hidden from the hand-over seek on";
        } else if (unhideAt && unhiddenAt <= startHeldTo) {
            why = fmt("the clock was not unhidden at the pass's %.1f ms picture", *unhideAt / 1e6);
        } else {
            why = grade(LongLastAttempt{attempt, ended, seeks, shown, unhiddenAt, opening, next});
        }
        if (!why.empty()) {
            std::printf("audio_playback_test: %s: attempt %d not graded: %s\n", rule, attempt, why.c_str());
            skipped += (skipped.empty() ? "" : "; ") + why;
            continue;
        }
        ++graded;
    }
    if (graded == 0) notVerified(kGoals, "no attempt was staged: " + skipped);
}

// GOAL: S15.588 -- a heard file that keeps up is not handed on again at a pass start whose pictures trail its
//       early-handed sound by under half of kShortestHandedPass: on beeps_last_long (30 a second to 1.9 s,
//       then pictures 10 ms apart, the last shown 63 ms), with the picture before the last held 50 ms and
//       the new pass's first picture held 60 ms as the decoder returns them, exactly one sound seek around
//       the loop -- the hand-over -- and the beep that opens the pass sounds at most 1.25 times as long as the
//       next. False if the pass start is tested against kSoundDrift: the new pass's sound, already playing, is
//       played from its start again (a second seek), and the opening is heard twice.
// WHY-THIS-SHAPE: the trail needs a picture drawn more than its interval late while nothing steers it: after
//       the hand-over, before the handed-on sound is heard. The short steps put the hand-over before the held
//       decode (on a constant 30 a second it lands in the same steer, which drops the late picture); the long
//       last picture keeps the next draw before the pass's end, so nothing chases it; the held first picture
//       puts the pass start's first steer after the new sound has started. Graded only when the hand-over came
//       first, both holds ran and the held picture was 30-55 ms late (its commit against the one before);
//       else printed, and none graded is not verified. Loud frames against the 0.5 s beep, not onsets: the
//       restart lands at the 40 ms beep's end, so the two run together. Under S15.538's sound-server guard.
// COVERAGE-ASSERT: the first hold reached in every attempt, and at least one attempt graded.
// CONTROL: control_run.sh 2026-10-13, reverted, steerSoundLocked, backend/daemon/wallpaper_manager.cpp: `passAway`
//       made `kSoundDrift` -> "FAIL: long last picture rule: a file that keeps up is not handed on again at a pass
//       start ..." (2 seeks, the second 70 ms after the hold) and "... the pass's opening beep is heard once" (loud
//       42.5-57.2 ms against 31.7), 4 of 4 attempts on both entries. Green: 1 seek,
//       30.3 ms against 31.7. Test-side:
//       the 50 ms hold made 20 -> "NOT VERIFIED -- S15.588 ...: drawn 10.1 ms late, outside 30-55 ms", not a FAIL.
void runLongLastChecks() {
    const char* kGoals = "S15.588 (a file that keeps up is not handed on again under the margin at a pass start)";
    stageLongLastLoop(kGoals, "long last picture rule", std::nullopt, [](const LongLastAttempt& a) {
        check(a.seeks.size() == 1,
              "long last picture rule: a file that keeps up is not handed on again at a pass start trailing its sound "
              "by under half of kShortestHandedPass",
              fmt("attempt %.0f: %.0f sound seeks around the loop", a.number, static_cast<double>(a.seeks.size())));
        check(a.opening * 4 <= a.next * 5, "long last picture rule: the pass's opening beep is heard once",
              fmt("attempt %.0f: loud %.1f ms, the next beep %.1f ms", a.number, a.opening * 1e3 / kRate,
                  a.next * 1e3 / kRate));
        return std::string();
    });
}

// GOAL: S15.589 -- a file that keeps up, whose early-handed sound has no clock yet, is not moved by the no-clock
//       rule through the pass's first kCatchUpHold of pictures: S15.588's staging with the clock hidden from the
//       hand-over seek on and unhidden as the new pass's 66.7 ms picture is returned (held 50 ms after), exactly one
//       sound seek around the loop and the opening beep at most 1.25 times the next. False if the protection
//       ends after the pass's first picture: its second picture's steer, still with no clock, moves the sound
//       back to the pass's start.
// WHY-THIS-SHAPE: S15.588 unhides nothing, so its pass start is steered with a clock; this one keeps the clock
//       away past the second picture, which only a no-clock pass start reaches. The 50 ms hold after unhiding lets
//       a clock be published before the 66.7 ms picture is steered, so the case is "no clock inside the cap",
//       not "no clock past it" (noClockPastCap). Graded only on S15.588's gates plus: the hide began at the
//       hand-over, before the held decode, and the unhiding ran; else printed, none graded is not verified.
// COVERAGE-ASSERT: as S15.588's, and at least one of AudioOutput's timing reads hidden in a graded attempt.
// CONTROL: control_batch.sh 2026-10-13, reverted, steerSoundLocked, backend/daemon/wallpaper_manager.cpp: `chased` set
//       back to the first-picture-only `clock || (next.startsPass && passAway > kSoundDrift)` -> "FAIL: no clock at a
//       pass start rule: an early-handed sound with no clock is not moved ..." (2 seeks, the second +73.5 ms from the
//       hold's end) and "... the pass's opening beep is heard once" (43.8-51.4 ms against 31.7), 4 of 4 attempts on
//       both entries. Green: 1 seek, 30.3 ms. Test-side: the clock never hidden -> "NOT VERIFIED -- S15.589 ...: the
//       clock was not hidden from the hand-over seek on" (entry skipped), not a FAIL.
void runNoClockPassStartChecks() {
    const char* kGoals = "S15.589 (an early-handed sound with no clock is not moved inside the pass's first kCatchUpHold)";
    stageLongLastLoop(kGoals, "no clock at a pass start rule", 2'000'000'000LL / 30, [](const LongLastAttempt& a) {
        check(a.seeks.size() == 1,
              "no clock at a pass start rule: an early-handed sound with no clock is not moved inside the pass's "
              "first kCatchUpHold",
              fmt("attempt %.0f: %.0f sound seeks around the loop", a.number, static_cast<double>(a.seeks.size())));
        check(a.opening * 4 <= a.next * 5, "no clock at a pass start rule: the pass's opening beep is heard once",
              fmt("attempt %.0f: loud %.1f ms, the next beep %.1f ms", a.number, a.opening * 1e3 / kRate,
                  a.next * 1e3 / kRate));
        return std::string();
    });
}

// GOAL: S15.589 -- past the pass's first kCatchUpHold of pictures the no-clock rule moves an early-handed sound
//       that still has no clock: the staging above with the clock unhidden only at the 200 ms picture, exactly two
//       sound seeks around the loop, the second after the pass's 100 ms picture is shown and before the unhiding.
//       False if the protection lasts until a clock arrives however late (a missing clock never ends it).
// WHY-THIS-SHAPE: two seeks, not "at least one more": the hand-over plus one move is what the rule makes; a
//       second move would be a separate defect. The second seek is placed between the 100 ms picture's commit
//       and the unhiding so that it is the no-clock rule acting past the cap, not an earlier steer inside it, nor a
//       clock rule after it. The commit is recorded on the render thread before it steers the next picture, and
//       the seek follows that steer, so "after" is causal; the unhiding (at the 200 ms picture's decode) came
//       about 66 ms after the move in every run. Graded on the gates above plus the 100 ms picture shown first.
// COVERAGE-ASSERT: as above.
// CONTROL: control_batch.sh / control_run.sh 2026-10-13, reverted, steerSoundLocked, backend/daemon/wallpaper_manager.cpp:
//       the `<= kCatchUpHold` clause of `handedUnheard` deleted -> "FAIL: no clock past the cap rule: an early-handed
//       sound still with no clock is moved once ..." (1 seek); the cap made zero (first picture only) -> "FAIL: ...
//       the move comes after the pass's 100 ms picture and before a clock" (the move +73.5 ms, the 100 ms picture
//       +173.5 ms); 4 of 4 attempts on both entries. Green: 2 seeks, the second +173.5 ms, the unhiding +240 ms.
void runNoClockPastCapChecks() {
    const char* kGoals = "S15.589 (an early-handed sound with no clock is moved past the pass's first kCatchUpHold)";
    stageLongLastLoop(kGoals, "no clock past the cap rule", 200 * kMs, [](const LongLastAttempt& a) {
        const std::optional<int64_t> cap = commitShowing(a.shown, 100 * kMs, a.ended, a.unhiddenAt);
        if (!cap) return std::string("the pass's 100 ms picture was not shown before the unhiding");
        std::printf("audio_playback_test: no clock past the cap rule: attempt %d: the 100 ms picture shown %+.1f ms "
                    "from the hold's end, the clock unhidden %+.1f ms\n",
                    a.number, (*cap - a.ended) / 1e6, (a.unhiddenAt - a.ended) / 1e6);
        check(a.seeks.size() == 2,
              "no clock past the cap rule: an early-handed sound still with no clock is moved once past the pass's "
              "first kCatchUpHold",
              fmt("attempt %.0f: %.0f sound seeks around the loop", a.number, static_cast<double>(a.seeks.size())));
        if (a.seeks.size() == 2) {
            check(a.seeks[1].at > *cap && a.seeks[1].at < a.unhiddenAt,
                  "no clock past the cap rule: the move comes after the pass's 100 ms picture and before a clock",
                  fmt("attempt %.0f: the move %+.1f ms from the 100 ms picture's commit, %+.1f ms from the unhiding",
                      a.number, (a.seeks[1].at - *cap) / 1e6, (a.seeks[1].at - a.unhiddenAt) / 1e6));
        }
        return std::string();
    });
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
//       and the file went on" (0 costed). The first break re-run on the current
//       DecodeLoad::span() it compares against (control_run.sh 2026-10-15): held (12 skipped).
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
//       of its window; and gradeBeepsAgainstPictures grades each beep once -- a picture committed twice, two
//       pictures near one beep, or a pass's last picture near the next pass's beep 0 -- and counts a repeat as
//       no pass start. False if these helpers misread the synthetic records below, which
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
//       deleted -> "...: ... running past a window's end is measured to that end". control_run.sh 2026-10-15,
//       reverted, test-side: gradeBeepsAgainstPictures' `done` set not consulted -> "...: a beep picture committed
//       twice in one pass is graded once, and is no pass start" and "...: a beep reached by two pictures, or by a
//       pass's last picture, is graded once"; its fold of a place at the pass's end onto the next pass's beep 0
//       made `if (false)` -> the second line alone. control_batch.sh 2026-10-13, reverted, test-side: filePlaces' interval taken from pictureInterval(shown) -> "...: pictures a skipping decoder never handed on
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
    // The same run with pictures up to 40 ms from a beep standing for it: two different pictures reach each
    // beep (14 and 15 reach 0.5 s), and the last picture of pass 0, 33 ms before the end, reaches pass 1's beep 0.
    int widePassStarts = 0;
    const int wide = gradeBeepsAgainstPictures("grader self-test, 40 ms", heardBeeps, run, beeps, kBase - kMs, 1.0,
                                               widePassStarts, nullptr, 40.0);
    check(wide == 6 && widePassStarts == 1,
          "grader self-test: a beep reached by two pictures, or by a pass's last picture, is graded once",
          fmt("%.0f graded (6 wanted), %.0f at a pass start (1 wanted)", wide, widePassStarts));

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
//       release. False if a failure is recorded as "no sound", which never reopens.
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
}

void runParkedFailedReopenChecks() {
    const char* kGoals = "S15.553 (a failed sound is opened again)";
    parkedFailedReopen(kGoals, "decode");
    parkedFailedReopen(kGoals, "open");
}

// GOAL: S15.554 -- volume 0 counts as not heard for a one-picture file: it is released by the idle timer,
//       decodes nothing and is not opened again while at 0. False if volume 0 keeps the sound decoding at
//       zero gain, so the file is never released, or if the reopen for sound ignores the volume and opens
//       it at 0.
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
//       (the first seek targets 7926.2 ms into a 2 s file). Re-run with begin() handing the loop
//       to the decoder (control_run.sh 2026-10-15): held, 7925.6 ms.
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

// What a run of AudioDecoder::read() calls gave: the frames, and for each call where read() said its first
// frame lies and at what rate its frames run.
struct Stretched {
    std::vector<float> left, right;
    struct Call {
        size_t at = 0;  // its first frame's index in left and right
        size_t asked = 0, got = 0;
        AudioDecoder::Position first;
    };
    std::vector<Call> calls;
    // Where in the file read() puts frame `i`, in seconds.
    double fileSeconds(size_t i) const {
        const Call* c = &calls.front();
        for (const Call& k : calls) {
            if (k.at > i) break;
            c = &k;
        }
        return c->first.file.count() / 1e9 + static_cast<double>(i - c->at) * c->first.rate / kRate;
    }
};

void readStretched(AudioDecoder& decoder, Stretched& out, size_t frames, size_t chunk = 4800) {
    std::vector<float> buffer(chunk * 2);
    for (size_t done = 0; done < frames;) {
        const size_t asked = std::min(chunk, frames - done);
        AudioDecoder::Position first;
        const size_t got = decoder.read(buffer.data(), asked, &first);
        if (got == 0) break;
        out.calls.push_back({out.left.size(), asked, got, first});
        for (size_t i = 0; i < got; ++i) {
            out.left.push_back(buffer[2 * i]);
            out.right.push_back(buffer[2 * i + 1]);
        }
        done += got;
    }
}

// Each beep's start in the output (left channel, past 0.05 after 300 ms of the file quieter), set against
// where read() placed it: how far, in ms of the file, from the nearest multiple of half a second. The quiet
// is measured in the file's time because a stretch at a slow rate repeats a beep's tail after a short quiet,
// which is not a beep's start.
std::vector<double> beepPlacements(const Stretched& s) {
    std::vector<double> out;
    double quietSince = -1.0;
    for (size_t i = 0; i < s.left.size(); ++i) {
        if (std::fabs(s.left[i]) > 0.05f) {
            if (quietSince < 0 || s.fileSeconds(i) - quietSince >= 0.3) {
                const double at = s.fileSeconds(i);
                out.push_back((at - std::round(at * 2) / 2) * 1e3);
            }
            quietSince = s.fileSeconds(i);
        }
    }
    return out;
}

// The right channel's frequency over frames [from, to), from its sign changes.
double toneFrequency(const std::vector<float>& right, size_t from, size_t to) {
    int changes = 0;
    for (size_t i = from + 1; i < to && i < right.size(); ++i) {
        if ((right[i] >= 0) != (right[i - 1] >= 0)) ++changes;
    }
    return to > from ? changes / 2.0 / (static_cast<double>(to - from) / kRate) : 0.0;
}

// How far each call's first frame lies from where the call before it said its frames run on to, in ms of
// the file, and how many calls came back short of what was asked before the last.
std::pair<double, int> placementSteps(const Stretched& s) {
    double worst = 0;
    int shortReads = 0;
    for (size_t k = 1; k < s.calls.size(); ++k) {
        const Stretched::Call& before = s.calls[k - 1];
        const double reached = before.first.file.count() / 1e9 + static_cast<double>(before.got) * before.first.rate / kRate;
        worst = std::max(worst, std::fabs(s.calls[k].first.file.count() / 1e9 - reached) * 1e3);
        if (before.got < before.asked) ++shortReads;
    }
    return {worst, shortReads};
}

// Where `p` lies, in frames from the file's time 0, and whether that is a whole frame.
std::pair<int64_t, bool> frameOf(const AudioDecoder::Position& p) {
    const double f = static_cast<double>(p.file.count()) * kRate / 1e9;
    return {std::llround(f), std::fabs(f - std::round(f)) < 0.01};
}

// The largest difference, over both channels, between `s`'s frames [from, to) and `ref`'s from `refAt` on;
// infinity when `ref` runs out first.
double differenceFrom(const Stretched& s, size_t from, size_t to, const Stretched& ref, int64_t refAt) {
    double worst = 0;
    for (size_t i = from; i < to && i < s.left.size(); ++i) {
        const int64_t r = refAt + static_cast<int64_t>(i - from);
        if (r < 0 || static_cast<size_t>(r) >= ref.left.size()) return INFINITY;
        worst = std::max({worst, static_cast<double>(std::fabs(s.left[i] - ref.left[r])),
                          static_cast<double>(std::fabs(s.right[i] - ref.right[r]))});
    }
    return worst;
}

// GOAL: S15.591 (decoder level, every change drained) -- with the file decoded at 1 from 0 as the reference:
//       (a) after every change to 1 the new run's first frame is placed on a whole frame, and after a change
//       from 1 within 153 frames (96 of shift, 57 of drift) of the frame after the last one read; a run at 1 is the file itself from its
//       97th frame on (the 2 ms crossfade changes level only, for 96 frames); (c) a change to 1 within 46 ms of
//       the end ends the stream (ended()) with every frame from its start to the end played once; (d) a seek
//       right after a change plays the file from
//       the seek, none of the frames handed back. False if a junction loses or repeats frames, the fade runs
//       past 96 frames, or a seek keeps frames meant for the old place.
// WHY-THIS-SHAPE: compared sample for sample against a reference decode, since only runs at 1 have an exact
//       expected content; stretched runs are not compared, as a fresh atempo fed the same frames is not
//       bitwise the same stretch (its guide channels carry the frame count). 1e-3 lets through nothing a one-
//       frame slip of the 440 Hz tone would (about 0.017). The near-end change is tried at several distances
//       from the end, since where the stretcher stands when the change lands is the stretcher's own.
// COVERAGE-ASSERT: five changes placed, at least two runs at 1 compared, and a near-end change whose run at 1
//       starts within 46 ms of the end with frames left to play.
// CONTROL: control_batch.sh 2026-10-14, reverted, phase decoderRate, renderer/audio/audio_decoder.cpp: fadeIn()'s
//       span doubled -> "FAIL: ... a run at 1 plays the file itself after its first 96 frames"; planChange()'s
//       Change starting one frame late -> that line (0.072 off; re-run 2026-10-15, held) and "... a change within
//       46 ms of the end plays every frame to the end once" (2112 of 2111). (d): control_run.sh 2026-10-15,
//       reverted, seek() not clearing the stretched frames -> "... a seek right after a change plays the file from
//       the seek, not the frames handed back" (0.687193 off; green 0.000000). seek() keeping m_change stays
//       green. Every run started 250 frames late (2026-10-15) -> "... a change from 1 starts within 153 frames" (160
//       late did not: green has 1 -> 0.5 start 39 frames early). control_batch.sh 2026-10-15: startRun() placing a
//       run at 1 a quarter frame on -> "... a change to 1 starts its run on a whole frame" (2 -> 1 and 0.5 -> 1);
//       ended() needing both no stretcher and its end (`&&` for `||`) -> "... a change within 46 ms of the end still
//       ends the stream" (ended 0); peekJoin() not handing its frames back (`unpull` deleted) -> "... a run at 1
//       plays the file itself after its first 96 frames" (0.5 -> 1: 0.507 off) and "... a change from 1 starts
//       within 153 frames". Green: 0.000000 off at 1, the near-end run 44 ms from the end.
void runDecoderJunctionChecks() {
    const std::string rule = "decoder junction rule";
    const std::string file = asset("wallpaper_beeps_long.mp4");
    try {
        std::unique_ptr<AudioDecoder> ref = AudioDecoder::open(file);
        Stretched r;
        readStretched(*ref, r, size_t{kRate} * 30, kRate / 2);
        const int64_t total = static_cast<int64_t>(r.left.size());
        check(total > kRate * 5 && frameOf(r.calls.front().first).first == 0, rule + ": the reference decode starts at 0",
              fmt("%.0f frames", static_cast<double>(total)));
        if (total <= kRate * 5) return;

        // (a) 1 -> 0.5 -> 2 -> 1 -> 2 -> 0.5 -> 1, in 3000-frame reads, each leg 0.4 s of output.
        std::unique_ptr<AudioDecoder> d = AudioDecoder::open(file);
        Stretched s;
        const double legs[] = {1.0, 0.5, 2.0, 1.0, 2.0, 0.5, 1.0};
        std::vector<size_t> legAt;
        for (const double rate : legs) {
            d->setRate(rate);
            legAt.push_back(s.left.size());
            readStretched(*d, s, kRate * 4 / 10, 3000);
        }
        int placed = 0, comparedAtOne = 0;
        for (size_t k = 1; k < std::size(legs); ++k) {
            // A change waits for the old run to be read out, so the new run can start some calls into the leg.
            const auto first = std::find_if(s.calls.begin(), s.calls.end(),
                                            [&](const Stretched::Call& c) { return c.at >= legAt[k] && c.first.rate == legs[k]; });
            if (first == s.calls.end() || first == s.calls.begin()) continue;
            ++placed;
            const auto [n, whole] = frameOf(first->first);
            const size_t legEnd = k + 1 < std::size(legs) ? legAt[k + 1] : s.left.size();
            std::string detail = fmt("%.1f -> %.1f: starts at frame %.0f", legs[k - 1], legs[k], static_cast<double>(n));
            // A stretched run's place carries the stretcher's drift, updated within the read, so only a run at 1
            // is placed exactly; a stretched one from 1 is held to the 2 ms of the fade.
            if (legs[k] == 1.0) check(whole, rule + ": a change to 1 starts its run on a whole frame", detail);
            if (legs[k - 1] == 1.0) {
                const auto before = std::prev(first);
                const int64_t ranTo = frameOf(before->first).first + static_cast<int64_t>(before->got);
                check(std::llabs(n - ranTo) <= 153,
                      rule + ": a change from 1 starts within 153 frames (96 of shift, 57 of drift) of the frame after the last one read",
                      detail + fmt(", the last read ran to %.0f", static_cast<double>(ranTo)));
            }
            if (legs[k] == 1.0) {
                ++comparedAtOne;
                const double after = differenceFrom(s, first->at + 96, legEnd, r, n + 96);
                const double inFade = differenceFrom(s, first->at, first->at + 96, r, n);
                std::printf("audio_playback_test: %s: %s; at 1 from its 97th frame %.6f off the file, %.4f in the fade\n",
                            rule.c_str(), detail.c_str(), after, inFade);
                check(after <= 1e-3, rule + ": a run at 1 plays the file itself after its first 96 frames",
                      detail + fmt(", %.6f off", after));
            } else {
                std::printf("audio_playback_test: %s: %s\n", rule.c_str(), detail.c_str());
            }
        }
        check(placed == 6 && comparedAtOne == 2, rule + ": every change was found in the reads",
              fmt("%.0f placed, %.0f at 1", placed, comparedAtOne));

        // (c) a change to 1 within 46 ms of the end.
        int nearEnd = 0;
        for (int64_t leftMs = 200; leftMs >= 20 && nearEnd == 0; leftMs -= 5) {
            std::unique_ptr<AudioDecoder> e = AudioDecoder::open(file);
            e->setRate(2.0);
            e->seek(std::chrono::nanoseconds((total - kRate * 3 / 10) * 1'000'000'000LL / kRate));
            Stretched t;
            for (;;) {
                const size_t was = t.calls.size();
                readStretched(*e, t, 480, 480);
                if (t.calls.size() == was) break;
                const Stretched::Call& c = t.calls.back();
                if (frameOf(c.first).first + static_cast<int64_t>(c.got * 2) >= total - leftMs * kRate / 1000) break;
            }
            e->setRate(1.0);
            const size_t at = t.left.size();
            readStretched(*e, t, kRate, 4800);
            const auto first = std::find_if(t.calls.begin(), t.calls.end(), [&](const Stretched::Call& c) { return c.at >= at && c.first.rate == 1.0; });
            if (first == t.calls.end()) continue;
            const int64_t n = frameOf(first->first).first;
            const int64_t played = static_cast<int64_t>(t.left.size() - first->at);
            if (total - n > 46 * kRate / 1000 || total - n <= 96) continue;
            ++nearEnd;
            const double after = differenceFrom(t, first->at + 96, t.left.size(), r, n + 96);
            std::printf("audio_playback_test: %s: a change to 1 read %.0f ms before the end: its run starts %.1f ms before "
                        "the end, plays %.0f frames of %.0f, %.6f off the file; ended %d\n",
                        rule.c_str(), static_cast<double>(leftMs), (total - n) * 1e3 / kRate, static_cast<double>(played),
                        static_cast<double>(total - n), after, static_cast<int>(e->ended()));
            check(e->ended(), rule + ": a change within 46 ms of the end still ends the stream");
            check(played == total - n && after <= 1e-3,
                  rule + ": a change within 46 ms of the end plays every frame to the end once",
                  fmt("%.0f frames of %.0f, %.6f off", static_cast<double>(played), static_cast<double>(total - n), after));
        }
        check(nearEnd > 0, rule + ": a change to 1 landed within 46 ms of the end");

        // (d) a seek right after a change.
        {
            std::unique_ptr<AudioDecoder> g = AudioDecoder::open(file);
            g->setRate(2.0);
            g->seek(1s);
            Stretched t;
            readStretched(*g, t, 9600, 4800);
            g->setRate(1.0);
            g->seek(3s);
            Stretched u;
            readStretched(*g, u, 9600, 4800);
            const double off = u.calls.empty() ? INFINITY : differenceFrom(u, 0, u.left.size(), r, 3 * kRate);
            std::printf("audio_playback_test: %s: a seek to 3 s right after 2 -> 1: first frame %.0f, %.6f off the file\n",
                        rule.c_str(), u.calls.empty() ? -1.0 : static_cast<double>(frameOf(u.calls.front().first).first), off);
            check(!u.calls.empty() && frameOf(u.calls.front().first).first == 3 * kRate && off <= 1e-3,
                  rule + ": a seek right after a change plays the file from the seek, not the frames handed back",
                  fmt("%.6f off", off));
        }
    } catch (const std::exception& e) {
        check(false, rule + ": the decoder threw: " + e.what());
    }
}

// One read() of up to `frames`, recorded into `out`: a single call, so a change waiting behind the run is not
// started by a second.
void readOnce(AudioDecoder& decoder, Stretched& out, size_t frames) {
    std::vector<float> buffer(frames * 2);
    AudioDecoder::Position first;
    const size_t got = decoder.read(buffer.data(), frames, &first);
    if (got == 0) return;
    out.calls.push_back({out.left.size(), frames, got, first});
    for (size_t i = 0; i < got; ++i) {
        out.left.push_back(buffer[2 * i]);
        out.right.push_back(buffer[2 * i + 1]);
    }
}

// beeps_long at `r0`, `pre` frames read, then each of `asks` asked in turn with nothing read between, but one
// read of `between` frames after the first; then, if `seekTo` is given, a seek there; then `after` frames read.
// `from` receives the number of reads made before the first ask.
Stretched driveAsks(const std::string& file, double r0, size_t pre, const std::vector<double>& asks, size_t between,
                    std::optional<std::chrono::nanoseconds> seekTo, size_t after, size_t& from) {
    std::unique_ptr<AudioDecoder> d = AudioDecoder::open(file);
    Stretched s;
    d->setRate(r0);
    readStretched(*d, s, pre, 4800);
    from = s.calls.size();
    for (size_t i = 0; i < asks.size(); ++i) {
        d->setRate(asks[i]);
        if (i == 0 && between > 0) readOnce(*d, s, between);
    }
    if (seekTo) d->seek(*seekTo);
    readStretched(*d, s, after, 4800);
    return s;
}

// Empty when `a` and `b` are the same reads, frame for frame within `level` and place for place within `frames`
// frames; else the first difference.
std::string sameReads(const Stretched& a, const Stretched& b, double frames = 0, double level = 0) {
    if (a.calls.size() != b.calls.size()) {
        return fmt("%.0f reads against %.0f", static_cast<double>(a.calls.size()), static_cast<double>(b.calls.size()));
    }
    for (size_t k = 0; k < a.calls.size(); ++k) {
        const Stretched::Call& x = a.calls[k];
        const Stretched::Call& y = b.calls[k];
        if (x.got != y.got || x.first.rate != y.first.rate ||
            std::fabs(static_cast<double>((x.first.file - y.first.file).count())) * kRate / 1e9 > frames) {
            return fmt("read %.0f: ", static_cast<double>(k)) +
                   fmt("%.0f frames at %.7f s, rate %.1f", static_cast<double>(x.got), x.first.file.count() / 1e9, x.first.rate) +
                   fmt(" against %.0f frames at %.7f s, rate %.1f", static_cast<double>(y.got), y.first.file.count() / 1e9,
                       y.first.rate);
        }
    }
    for (size_t i = 0; i < a.left.size(); ++i) {
        if (std::fabs(a.left[i] - b.left[i]) > level || std::fabs(a.right[i] - b.right[i]) > level) {
            return fmt("frame %.0f: %.7f against %.7f", static_cast<double>(i), a.right[i], b.right[i]) +
                   fmt(" (left %.7f against %.7f)", a.left[i], b.left[i]);
        }
    }
    return {};
}

// GOAL: S15.612 -- AudioDecoder::seek(at, onward, onwardRate), with `onward` a second decoder's
//       frames from `at` at the old rate, at 1 -> 2 and 2 -> 0.5: (a) the reads before the new rate report the old
//       rate and total at most 48 frames; (b) the first read at the new rate starts within 153 frames of file (96
//       of shift, 57 of drift, as S15.591 (a)) of `at` plus what was played on; (c) the old run's last frames
//       followed by the new reads have no click (second difference over 0.02) and the tone no run of 24 frames
//       under 0.05; (d) a setRate() before any read gives exactly the reads of a decoder asked only that rate
//       before the same seek.
// WHY-THIS-SHAPE: `onward` comes from a real decode at the old rate at the same place, as the engine's ring
//       holds, so the join lines up real sound; zeros or a synthetic tone would hide a misaligned join.
// CONTROL: control_batch.sh 2026-10-15, reverted, renderer/audio/audio_decoder.cpp: seek(at, onward, ...)
//       ignoring `onward` -> "FAIL: decoder cut seek rule: the join does not click" (2 -> 0.5: 0.1699, 1 -> 2: 0.0692;
//       re-run 2026-10-15 on peekJoin(), held); the planned
//       change's start 200 frames late (planChange) -> "... the new rate's run starts at the cut, within 153 frames
//       of file" (2 -> 0.5: +197.8); read()'s `if (m_change)` made `if (false && m_change)` -> "... a setRate()
//       before any read replaces the cut's change exactly" (both cases). control_batch.sh 2026-10-15: planChange()
//       playing all of `onward` on (shift = kJoinFrames) -> "... the old rate is read for at most 48 frames before
//       the new one" (144 frames, both cases); fadeIn() writing 0 for crossfade frames 20-59 -> "... the join leaves
//       no dip" (40 frames under 0.05, both cases).
void runDecoderCutSeekChecks() {
    const std::string rule = "decoder cut seek rule";
    const std::string file = asset("wallpaper_beeps_long.mp4");
    struct Case {
        double r0, r1, replacedBy;
    };
    const Case cases[] = {{1.0, 2.0, 0.5}, {2.0, 0.5, 1.0}};
    int graded = 0;
    try {
        for (const Case& c : cases) {
            const std::string name = fmt("%.1f -> %.1f", c.r0, c.r1);
            // The old rate's sound, as the ring holds it: read 1 s of output, then take the next kJoinFrames.
            std::unique_ptr<AudioDecoder> old = AudioDecoder::open(file);
            old->setRate(c.r0);
            old->seek(std::chrono::nanoseconds(0));
            Stretched o;
            readStretched(*old, o, kRate, 4800);
            const Stretched::Call& last = o.calls.back();
            const auto at = last.first.file +
                            std::chrono::nanoseconds(std::llround(static_cast<double>(last.got) * last.first.rate * 1e9 / kRate));
            Stretched tail;
            readOnce(*old, tail, AudioDecoder::kJoinFrames);
            std::vector<float> onward;
            for (size_t i = 0; i < tail.left.size(); ++i) {
                onward.push_back(tail.left[i]);
                onward.push_back(tail.right[i]);
            }
            check(tail.calls.size() == 1 && tail.calls[0].first.rate == c.r0 && tail.left.size() == AudioDecoder::kJoinFrames &&
                      std::llabs((tail.calls[0].first.file - at).count()) < 1'000'000,
                  rule + ": the old run's onward frames were read where the cut is", name);
            std::unique_ptr<AudioDecoder> d = AudioDecoder::open(file);
            d->setRate(c.r1);
            d->seek(at, onward, c.r0);
            Stretched s;
            readStretched(*d, s, kRate / 2, 4800);
            size_t playedOn = 0;
            auto firstNew = s.calls.begin();
            while (firstNew != s.calls.end() && firstNew->first.rate != c.r1) {
                if (firstNew->first.rate == c.r0) playedOn += firstNew->got;
                ++firstNew;
            }
            const bool oldOnly = std::all_of(s.calls.begin(), firstNew, [&](const Stretched::Call& k) { return k.first.rate == c.r0; });
            check(firstNew != s.calls.end() && oldOnly && playedOn <= 48,
                  rule + ": the old rate is read for at most 48 frames before the new one", name + fmt(": %.0f frames played on",
                                                                                                 static_cast<double>(playedOn)));
            if (firstNew == s.calls.end()) continue;
            const double expected = (at.count() + static_cast<double>(playedOn) * c.r0 * 1e9 / kRate) * kRate / 1e9;
            const double startsAt = firstNew->first.file.count() * static_cast<double>(kRate) / 1e9;
            check(std::fabs(startsAt - expected) <= 153.0, rule + ": the new rate's run starts at the cut, within 153 frames of file",
                  name + fmt(": starts %+.1f frames from the cut plus what was played on", startsAt - expected));
            size_t low = 0, longest = 0;
            for (size_t i = 0; i < std::min<size_t>(s.right.size(), kRate / 10); ++i) {
                low = std::fabs(s.right[i]) < 0.05f ? low + 1 : 0;
                longest = std::max(longest, low);
            }
            // The old run's last 220 frames, then the new reads: the click is looked for across the join itself.
            std::vector<float> joined(o.right.end() - 220, o.right.end());
            joined.insert(joined.end(), s.right.begin(), s.right.begin() + std::min<ptrdiff_t>(s.right.size(), kRate / 10));
            const ClickAndDip join = clickAndDip(joined, 0);
            std::printf("audio_playback_test: %s: %s: %zu frames played on, the new run %+.1f frames from the cut, longest run "
                        "under 0.05 %zu frames, largest second difference %.4f\n",
                        rule.c_str(), name.c_str(), playedOn, startsAt - expected, longest, join.click);
            check(longest < 24, rule + ": the join leaves no dip", name + fmt(": %.0f frames under 0.05", static_cast<double>(longest)));
            check(join.click <= 0.02f, rule + ": the join does not click", name + fmt(": %.4f", join.click));
            // (d) replaced before any read, against a decoder asked only the replacing rate.
            std::unique_ptr<AudioDecoder> e = AudioDecoder::open(file);
            e->setRate(c.r1);
            e->seek(at, onward, c.r0);
            e->setRate(c.replacedBy);
            Stretched x;
            readStretched(*e, x, kRate / 2, 4800);
            std::unique_ptr<AudioDecoder> f = AudioDecoder::open(file);
            f->setRate(c.replacedBy);
            f->seek(at, onward, c.r0);
            Stretched y;
            readStretched(*f, y, kRate / 2, 4800);
            const bool neverReplaced = std::none_of(x.calls.begin(), x.calls.end(), [&](const Stretched::Call& k) { return k.first.rate == c.r1; });
            const std::string diff = sameReads(x, y);
            check(neverReplaced && diff.empty() && x.calls.size() >= 5,
                  rule + ": a setRate() before any read replaces the cut's change exactly",
                  name + fmt(" replaced by %.1f: ", c.replacedBy) + (neverReplaced ? diff : std::string("a read at the replaced rate")));
            ++graded;
        }
    } catch (const std::exception& e) {
        check(false, rule + ": the decoder threw: " + e.what());
    }
    check(graded == 2, rule + ": both cases were graded", fmt("%.0f of 2", graded));
}

// GOAL: S15.616 (decoder level) -- a speed change that has handed out nothing yet is replaced by the next one, as
//       if never asked: across 1 -> 2 -> 0.5, 0.5 -> 2 -> 1, 1 -> 2 -> 0.5 -> 2, 2 -> 1 -> 0.5 (a waiting run at 1)
//       and 0.5 -> 2 -> 1 with 300 frames of the old run read between, (a) no read reports a rate asked and then
//       replaced, and (b) every read from the start is the same, frame for frame and place for place, as a second
//       decoder's that was asked only the last rate at the same point. False if the in-between rate is heard, the
//       replaced change keeps or loses file frames, its play-on frames are lost or doubled, or a waiting run at 1
//       places frames.
// WHY-THIS-SHAPE: exact equality with a second decoder, not a tolerance against the file: the stretcher is
//       deterministic for the same frames fed from the same count, so any leftover of the replaced change shows as
//       a difference. A rate-only check would pass a swap that drops or repeats frames.
//       The seek cases allow 1e-3 per sample: the reference reached the seek from another place, and the
//       decode before a seek leaves differences of about 1e-4. The 300-frame case allows half a frame of place:
//       the replacing change's play-on is placed with the old run's drift as it stands after those reads.
// COVERAGE-ASSERT: every case reads the last rate asked, over at least five reads after the asks.
// CONTROL: control_batch.sh 2026-10-15, reverted, phase decoderSwap, renderer/audio/audio_decoder.cpp setRate():
//       the waiting change started and a new one applied after it (`startRun(); applyRate();`) -> "FAIL: ... a
//       replaced rate is never read" (1-2 reads at it) and "... it plays exactly as the last rate asked alone";
//       `if (false && m_change)` (the new change planned on top) -> "... plays exactly ..." in five cases;
//       dropChange() not moving the read point back (its `unpull()` deleted; held) -> "... plays exactly
//       ..."; dropChange() not giving the unread play-on back (held) -> "... plays exactly ...". dropChange() setting m_applied to the dropped rate is an equivalent mutant (planChange resets it).
//
// GOAL: S15.616 -- back to the speed already playing before anything is heard (1 -> 2 -> 1, 1 -> 0.5 -> 1, 2 ->
//       0.5 -> 2): the in-between rate is never read, and at 1 every frame read from the start is the file itself,
//       none repeated or skipped (the crossfade blends identical sound). False if the return builds a junction that
//       loses or doubles frames.
// CONTROL: the `if (false && m_change)` break above -> "FAIL: ... 1 -> 2.0 waiting -> 1: every frame read is the
//       file itself" and the same at 0.5. The 2 -> 0.5 -> 2 line: control_batch.sh 2026-10-15, setRate() ignoring
//       an ask while another waits (first ask wins) -> "... 2 -> 0.5 waiting -> 2: a replaced rate is never read"
//       (10 reads at it).
//
// GOAL: S15.591 (d) -- a seek while a change waits drops it: 1 -> 2 then a seek, and 1 -> 2 -> 0.5 then a seek,
//       read exactly as a decoder asked only the last rate and then sought to the same place.
// WHY-THIS-SHAPE: the two cases with nothing read between the ask and the seek never plan the change (planning
//       happens in read()), so no Change waits at the seek; only the 0.5 -> 2 case with 300 frames read puts one there.
// CONTROL: control_run.sh 2026-10-15, reverted, seek() not resetting m_change
//       (renderer/audio/audio_decoder.cpp) -> "FAIL: decoder swap rule: 0.5 -> 2 waiting, 300 frames read, then a
//       seek to 3 s: it plays exactly as the last rate asked alone" (read 0 placed at 3.3323 s against 2.9993 s). The
//       two cases without a read stay green under it, and under seek() not taking up the rate: with no Change
//       planned they grade only that the seek starts a fresh run. control_batch.sh 2026-10-15: seek() keeping the
//       old run (its `m_run = Run{...}` deleted) -> "... 1 -> 2 waiting, then a seek to 3 s: it plays exactly ..."
//       (read 0 at 3.5150 s, rate 1, against 2.9790 s) and the same for 1 -> 2 -> 0.5. Test-side, 2026-10-15: the
//       reference given 4 s of reads after its seek -> "... reads in all against ... after the seek alone" (a
//       reported failure where the tail's start index used to wrap).
void runDecoderSwapChecks() {
    const std::string rule = "decoder swap rule";
    const std::string file = asset("wallpaper_beeps_long.mp4");
    struct Swap {
        const char* name;
        double r0;
        std::vector<double> asks;
        size_t between;
        std::optional<std::chrono::nanoseconds> seekTo;
    };
    const Swap swaps[] = {
        {"1 -> 2 waiting -> 0.5", 1.0, {2.0, 0.5}, 0, {}},
        {"0.5 -> 2 waiting -> 1", 0.5, {2.0, 1.0}, 0, {}},
        {"1 -> 2 -> 0.5 waiting -> 2", 1.0, {2.0, 0.5, 2.0}, 0, {}},
        {"2 -> 1 waiting -> 0.5", 2.0, {1.0, 0.5}, 0, {}},
        {"0.5 -> 2 waiting, 300 frames read, -> 1", 0.5, {2.0, 1.0}, 300, {}},
        {"1 -> 2 waiting, then a seek to 3 s", 1.0, {2.0}, 0, std::chrono::nanoseconds(3s)},
        {"1 -> 2 -> 0.5 waiting, then a seek to 3 s", 1.0, {2.0, 0.5}, 0, std::chrono::nanoseconds(3s)},
        {"0.5 -> 2 waiting, 300 frames read, then a seek to 3 s", 0.5, {2.0}, 300, std::chrono::nanoseconds(3s)},
    };
    try {
        int cases = 0;
        for (const Swap& w : swaps) {
            size_t from = 0, refFrom = 0;
            const Stretched a = driveAsks(file, w.r0, kRate, w.asks, w.between, w.seekTo, kRate * 6 / 10, from);
            std::optional<std::chrono::nanoseconds> refSeek = w.seekTo;
            const Stretched b = w.seekTo ? driveAsks(file, w.asks.back(), 0, {}, 0, refSeek, kRate * 6 / 10, refFrom)
                                         : driveAsks(file, w.r0, kRate, {w.asks.back()}, w.between, {}, kRate * 6 / 10, refFrom);
            int replaced = 0;
            bool lastRead = false;
            for (size_t k = from; k < a.calls.size(); ++k) {
                const double r = a.calls[k].first.rate;
                if (r == w.asks.back()) lastRead = true;
                if (r != w.asks.back() && r != w.r0) ++replaced;
            }
            std::string diff;
            if (!w.seekTo) {
                diff = sameReads(a, b, w.between > 0 ? 0.5 : 0, 0);
            } else if (b.calls.size() > a.calls.size()) {
                // More reads after the seek in the reference than in the whole run: a failure, not an index.
                diff = fmt("%.0f reads in all against %.0f after the seek alone", static_cast<double>(a.calls.size()),
                           static_cast<double>(b.calls.size()));
            } else {
                // Only what follows the seek is compared with the decoder that was only sought.
                Stretched tail;
                const size_t at = a.calls.size() - b.calls.size();
                tail.calls.assign(a.calls.begin() + static_cast<ptrdiff_t>(at), a.calls.end());
                const size_t f0 = at < a.calls.size() ? a.calls[at].at : a.left.size();
                for (Stretched::Call& c : tail.calls) c.at -= f0;
                tail.left.assign(a.left.begin() + static_cast<ptrdiff_t>(f0), a.left.end());
                tail.right.assign(a.right.begin() + static_cast<ptrdiff_t>(f0), a.right.end());
                diff = sameReads(tail, b, 0, 1e-3);
            }
            std::printf("audio_playback_test: %s: %s: %zu reads after the first ask, %d at a replaced rate; %s\n", rule.c_str(),
                        w.name, a.calls.size() - from, replaced,
                        diff.empty() ? "the same as the last rate asked alone" : diff.c_str());
            check(replaced == 0, rule + ": " + w.name + ": a replaced rate is never read",
                  fmt("%.0f reads at it", static_cast<double>(replaced)));
            check(diff.empty(), rule + ": " + w.name + ": it plays exactly as the last rate asked alone", diff);
            if (lastRead && a.calls.size() >= from + 5) ++cases;
        }
        check(cases == static_cast<int>(std::size(swaps)), rule + ": every case read its last rate over at least 5 reads",
              fmt("%.0f of %.0f", static_cast<double>(cases), static_cast<double>(std::size(swaps))));

        // Back to the speed already playing.
        std::unique_ptr<AudioDecoder> ref = AudioDecoder::open(file);
        Stretched r;
        readStretched(*ref, r, size_t{kRate} * 3, kRate / 2);
        for (const double interim : {2.0, 0.5}) {
            size_t from = 0;
            const Stretched a = driveAsks(file, 1.0, kRate, {interim, 1.0}, 0, {}, kRate, from);
            int replaced = 0;
            for (size_t k = from; k < a.calls.size(); ++k) {
                if (a.calls[k].first.rate != 1.0) ++replaced;
            }
            const double off = differenceFrom(a, 0, a.left.size(), r, 0);
            const std::string name = fmt("1 -> %.1f waiting -> 1", interim);
            std::printf("audio_playback_test: %s: %s: %.0f frames, %d reads at the replaced rate, %.6f off the file\n",
                        rule.c_str(), name.c_str(), static_cast<double>(a.left.size()), replaced, off);
            check(replaced == 0, rule + ": " + name + ": a replaced rate is never read");
            check(a.left.size() == size_t{kRate} * 2 && off <= 1e-6,
                  rule + ": " + name + ": every frame read is the file itself, none repeated or skipped",
                  fmt("%.0f frames, %.6f off", static_cast<double>(a.left.size()), off));
        }
        {
            size_t from = 0;
            const Stretched a = driveAsks(file, 2.0, kRate, {0.5, 2.0}, 0, {}, kRate, from);
            int replaced = 0;
            for (size_t k = from; k < a.calls.size(); ++k) {
                if (a.calls[k].first.rate != 2.0) ++replaced;
            }
            const auto [step, shortReads] = placementSteps(a);
            std::printf("audio_playback_test: %s: 2 -> 0.5 waiting -> 2: %d reads at the replaced rate; reads at most %.2f ms "
                        "off where the one before ran on to\n",
                        rule.c_str(), replaced, step);
            check(replaced == 0 && a.calls.size() > from, rule + ": 2 -> 0.5 waiting -> 2: a replaced rate is never read",
                  fmt("%.0f reads at it", static_cast<double>(replaced)));
        }
    } catch (const std::exception& e) {
        check(false, rule + ": the decoder threw: " + e.what());
    }
}

// GOAL: S15.592 (junction level) -- for six changes (1->2, 2->0.5, 0.5->1, 1->0.5, 2->1, 0.5->2), each made at 22
//       points over one period of the 440 Hz tone, the tone through the junction has no 24-frame run under 0.05
//       and its half-period envelope stays at 0.2 or over (the tone is 0.288). False if a junction cancels.
// WHY-THIS-SHAPE: a 110-frame envelope, as the change rule uses, spans the whole 96-frame fade and reads full level
//       through a cancellation; half a period cannot. This does NOT grade the phase alignment: with alignShift()
//       returning 0 the lowest envelope over all 132 is still 0.282 (control_batch.sh 2026-10-15), since both sides
//       of a decoder junction start at the same file frame and never met in opposite phase here.
// COVERAGE-ASSERT: all 132 junctions found.
// CONTROL: control_batch.sh 2026-10-15, reverted, phase decoderAlign, fadeIn() fading from the old run negated
//       (renderer/audio/audio_decoder.cpp) -> "FAIL: ... no junction has a 24-frame run under 0.05" (28 frames) and
//       "... the tone keeps its level through every junction" (0.094).
//
// GOAL: S15.591 (placement of the shift) -- a change from 1 starts its run within 153 frames (96 of shift at 2,
//       57 of drift) of where the last read ran to; a change during silence plays nothing on at 1.
// CONTROL: planChange() starting every run 160 frames late -> "FAIL: ... a change from 1 starts within 153 frames"
//       (green: 136.9 at worst, 1 -> 0.5); alignShift() taking ties on the play-on side (`>=`) -> "... in
//       silence: nothing is shifted", both rates (48 frames played on; re-run 2026-10-15 on peekJoin(), held,
//       renderer/audio/audio_decoder.cpp). The same tie on the dropping side did NOT go red: a dropped
//       shift moves only the start, which the drift hides.
void runDecoderAlignChecks() {
    const std::string rule = "decoder alignment rule";
    const std::string file = asset("wallpaper_beeps_long.mp4");
    const std::pair<double, double> pairs[] = {{1.0, 2.0}, {2.0, 0.5}, {0.5, 1.0}, {1.0, 0.5}, {2.0, 1.0}, {0.5, 2.0}};
    try {
        int junctions = 0, fromOne = 0;
        size_t longest = 0, mostPlayed = 0;
        float lowest = 1.0f;
        double worstPlace = 0;
        std::string lowestAt, longestAt, placeAt;
        for (const auto& [was, to] : pairs) {
            for (size_t k = 0; k < 110; k += 5) {
                std::unique_ptr<AudioDecoder> d = AudioDecoder::open(file);
                d->setRate(was);
                Stretched s;
                readStretched(*d, s, kRate / 2 + k, 4800);
                const size_t at = s.left.size(), callsBefore = s.calls.size();
                d->setRate(to);
                readStretched(*d, s, 9600, 4800);
                const auto first = std::find_if(s.calls.begin() + static_cast<ptrdiff_t>(callsBefore), s.calls.end(),
                                                [&](const Stretched::Call& c) { return c.first.rate == to; });
                if (first == s.calls.end() || first->at < 300) continue;
                ++junctions;
                const std::string where = fmt("%.1f -> %.1f, %.0f frames in", was, to, static_cast<double>(k));
                const size_t j = first->at;
                size_t run = 0;
                for (size_t i = j - 200; i < j + 300 && i < s.right.size(); ++i) {
                    run = std::fabs(s.right[i]) < 0.05f ? run + 1 : 0;
                    if (run > longest) {
                        longest = run;
                        longestAt = where;
                    }
                }
                for (size_t i = j - 150; i < j + 250 && i < s.right.size(); ++i) {
                    float envelope = 0;
                    for (size_t m = i - 54; m <= i; ++m) envelope = std::max(envelope, std::fabs(s.right[m]));
                    if (envelope < lowest) {
                        lowest = envelope;
                        lowestAt = where;
                    }
                }
                if (was == 1.0) {
                    ++fromOne;
                    const size_t played = first->at - at;
                    if (played > mostPlayed) mostPlayed = played;
                    const auto before = std::prev(first);
                    const double ranTo = before->first.file.count() * 1e-9 * kRate + static_cast<double>(before->got);
                    const double n = first->first.file.count() * 1e-9 * kRate;
                    const double off = std::fabs(n - ranTo);
                    if (off > worstPlace || placeAt.empty()) {
                        worstPlace = std::max(worstPlace, off);
                        placeAt = where + fmt(": starts %.1f frames from where the last read ran to", n - ranTo);
                    }
                }
            }
        }
        std::printf("audio_playback_test: %s: %d junctions; longest run under 0.05 %zu frames (%s); lowest half-period "
                    "envelope %.3f (%s); from 1, at most %zu frames read at 1 after the change, the start at most %.1f "
                    "frames from where the last read ran to (%s)\n",
                    rule.c_str(), junctions, longest, longestAt.c_str(), lowest, lowestAt.c_str(), mostPlayed, worstPlace,
                    placeAt.c_str());
        check(junctions == 132 && fromOne == 44, rule + ": every junction was found",
              fmt("%.0f of 132, %.0f from 1", junctions, fromOne));
        check(longest < 24, rule + ": no junction has a 24-frame run under 0.05",
              fmt("%.0f frames", static_cast<double>(longest)) + " at " + longestAt);
        check(lowest >= 0.2f, rule + ": the tone keeps its level through every junction", fmt("%.3f", lowest) + " at " + lowestAt);
        check(worstPlace <= 153, rule + ": a change from 1 starts within 153 frames (96 of shift, 57 of drift) of where the last read ran to",
              placeAt);

        // Silence, before the stream's first sample: nothing to line up.
        for (const double to : {2.0, 0.5}) {
            std::unique_ptr<AudioDecoder> d = AudioDecoder::open(file);
            d->seek(-2s);
            Stretched s;
            readStretched(*d, s, kRate / 2, 4800);
            const size_t callsBefore = s.calls.size();
            const bool silent = std::all_of(s.right.begin(), s.right.end(), [](float v) { return v == 0.0f; });
            d->setRate(to);
            readStretched(*d, s, 9600, 4800);
            size_t atOne = 0;
            bool reached = false;
            for (size_t k = callsBefore; k < s.calls.size(); ++k) {
                if (s.calls[k].first.rate == 1.0 && !reached) atOne += s.calls[k].got;
                if (s.calls[k].first.rate == to) reached = true;
            }
            std::printf("audio_playback_test: %s: 1 -> %.1f %s: %zu frames at 1 after the change; the new rate %s; "
                        "what was read before was %s\n",
                        rule.c_str(), to, "in silence", atOne, reached ? "read" : "not read", silent ? "silent" : "not silent");
            const std::string name = fmt("1 -> %.1f in silence", to);
            check(silent && reached, rule + ": " + name + ": the change was made in silence and read");
            check(atOne == 0, rule + ": " + name + ": nothing is shifted", fmt("%.0f frames played on at 1", static_cast<double>(atOne)));
        }
    } catch (const std::exception& e) {
        check(false, rule + ": the decoder threw: " + e.what());
    }
}

// GOAL: S15.591 -- AudioDecoder stretches the sound to its rate with the pitch kept and says truly where each
//       frame lies: on beeps_long (1 kHz beeps left every half second, a 440 Hz tone right), at rates 0.5, 1 and 2
//       after a seek to 0.25 s, the first frame is placed at 0.25 s (within 5 ms), every beep starts within 20 ms
//       of the file time read() places it at, and the tone stays 440 Hz within 2 %. False if the sound is
//       resampled (the pitch moves) or not stretched.
// WHY-THIS-SHAPE: read() is driven directly, so a frame's place is read()'s own claim, graded against the
//       beeps the frame carries. The tone, not the beeps, gives the pitch: a beep is 40 ms of the file, too
//       few cycles to tell 2 % apart from noise. A beep's start is its first frame past 0.05 after 300 ms of the
//       file quieter: a slow stretch repeats a beep's tail after a short quiet. The bounds hold atempo's own
//       smearing of an onset (up to 16 ms at rate 2).
// COVERAGE-ASSERT: at least four beeps placed at each rate.
// CONTROL: control_batch.sh 2026-10-15, reverted, renderer/audio/audio_decoder.cpp, phase decoderRate: stretches()
//       returning false (nothing stretched) -> "FAIL: decoder rate rule, rate 0.5: every beep lies where read() places
//       it" (124.99 ms) and at rate 2 (249.96 ms); every other stretched frame dropped in takeStretched() -> "... rate
//       0.5: the pitch is kept" (879.9 Hz) and at rate 2; seek() placing its start 20 ms late -> "... rate 0.5: after a
//       seek the first frame is placed at the seek, at the rate" (0.2712 s) and at 1 and 2. NOT controlled here: the
//       drift update in read() deleted, or takeStretched()'s guide count replaced by counting at the rate, leave the
//       phase green (beeps at 0.5 move from -5.1 to -6.6 ms on average), so "placed by counting frames at the rate" is
//       not graded at 0.5, 1 and 2. Green: at worst 15.9 ms (rate 2), tone 439.6-440.0 Hz.
//
// GOAL: S15.592 -- a change of rate is heard with no click, dip or step, and a read never mixes two rates:
//       0.5, 2, 0.5, 1, 2, 1, each read for a second in half-second reads, give a right channel whose largest
//       second difference stays under 0.02 and whose envelope never falls under 0.2; each read's first frame lies
//       within 20 ms of where the read before ran on to; every beep is placed within 20 ms. False if a change
//       drops or repeats what the old stretch held, or a read's frames are placed at one rate across a change.
// WHY-THIS-SHAPE: placement steps catch a read that crossed a change -- its frames past the change are
//       placed at the old rate, so the next read's first frame lands away from where it said it ran on to by
//       the difference in rate times those frames, hundreds of ms with half-second reads. A correct change
//       restarts the placement and steps it a few ms, which the 20 ms bound lets through.
// COVERAGE-ASSERT: at least one read cut short before the end (a change was crossed), and every rate asked
//       for reported by some read.
// CONTROL: control_batch.sh 2026-10-15, reverted, renderer/audio/audio_decoder.cpp, phase decoderRate: applyRate()
//       not draining the old stretch (runOnward() not called) -> "FAIL: decoder rate change rule: a change of rate
//       does not click" (0.5821) and "... a read never mixes two rates" (43.79 ms); the drained frames zeroed ->
//       "... does not click" (0.3008; the dip check stays green, the fade covering only 96 frames); each new run's
//       first 2400 frames zeroed in fadeIn() -> "... a change of rate does not dip" (envelope 0.000); read() no
//       longer stopping at a change (`if (written > 0) break;` deleted) -> "... a read never mixes two rates"
//       (2187.76 ms), "... a read was cut short at a change" (0 short) and "... every beep lies where read() places it
//       across the changes" (202.08 ms). Green: 5 short, at most 4.50 ms off, second difference 0.0031, envelope 0.287.
void runDecoderRateChecks() {
    std::unique_ptr<AudioDecoder> decoder;
    try {
        decoder = AudioDecoder::open(asset("wallpaper_beeps_long.mp4"));
    } catch (const std::exception& e) {
        check(false, std::string("decoder rate rule: the long fixture opens: ") + e.what());
        return;
    }
    if (!decoder) {
        check(false, "decoder rate rule: the long fixture's sound is found");
        return;
    }
    try {
        for (const double rate : {0.5, 1.0, 2.0}) {
            const std::string rule = fmt("decoder rate rule, rate %.1f", rate);
            decoder->setRate(rate);
            decoder->seek(250ms);
            Stretched s;
            readStretched(*decoder, s, static_cast<size_t>(3.0 / rate * kRate));
            if (s.calls.empty()) {
                check(false, rule + ": the decoder reads sound");
                continue;
            }
            const std::vector<double> placed = beepPlacements(s);
            double worst = 0, mean = 0;
            for (const double e : placed) {
                worst = std::max(worst, std::fabs(e));
                mean += e / static_cast<double>(placed.size());
            }
            const double pitch = toneFrequency(s.right, 4800, s.right.size());
            const auto [step, shortReads] = placementSteps(s);
            std::string all;
            for (const double e : placed) all += fmt(" %+.2f", e);
            std::printf("audio_playback_test: %s: first frame at %.4f s, rate %.2f; beeps placed%s ms (mean %+.2f); tone %.1f "
                        "Hz; reads at most %.2f ms off where the one before ran on to, %d short\n",
                        rule.c_str(), s.calls.front().first.file.count() / 1e9, s.calls.front().first.rate, all.c_str(),
                        mean, pitch, step, shortReads);
            check(std::llabs(s.calls.front().first.file.count() - 250 * kMs) <= 5 * kMs && s.calls.front().first.rate == rate,
                  rule + ": after a seek the first frame is placed at the seek, at the rate",
                  fmt("%.4f s at %.2f", s.calls.front().first.file.count() / 1e9, s.calls.front().first.rate));
            check(placed.size() >= 4, rule + ": at least four beeps were placed", fmt("%.0f", static_cast<double>(placed.size())));
            check(worst <= 20.0, rule + ": every beep lies where read() places it", fmt("%.2f ms off at worst", worst));
            check(std::fabs(pitch / 440.0 - 1.0) <= 0.02, rule + ": the pitch is kept", fmt("%.1f Hz, wanted 440", pitch));
        }

        const std::string rule = "decoder rate change rule";
        decoder->setRate(0.5);
        decoder->seek(0ns);
        Stretched s;
        const std::pair<double, double> legs[] = {{0.5, 1.0}, {2.0, 1.0}, {0.5, 1.0}, {1.0, 1.0}, {2.0, 1.0}, {1.0, 1.0}};
        for (const auto& [rate, seconds] : legs) {
            decoder->setRate(rate);
            readStretched(*decoder, s, static_cast<size_t>(seconds * kRate), kRate / 2);
        }
        const std::vector<double> placed = beepPlacements(s);
        double worst = 0;
        for (const double e : placed) worst = std::max(worst, std::fabs(e));
        const auto [step, shortReads] = placementSteps(s);
        const ClickAndDip cd = clickAndDip(s.right, 4800);
        const float click = cd.click, dip = cd.dip;
        std::set<double> rates;
        for (const Stretched::Call& c : s.calls) rates.insert(c.first.rate);
        std::printf("audio_playback_test: %s: %zu reads, %d short, at most %.2f ms off where the one before ran on to; %zu "
                    "rates read; %zu beeps, at most %.2f ms off; largest second difference %.4f, lowest envelope %.3f\n",
                    rule.c_str(), s.calls.size(), shortReads, step, rates.size(), placed.size(), worst, click, dip);
        check(shortReads >= 1 && rates.size() == 3,
              rule + ": a read was cut short at a change, and every rate asked for was read",
              fmt("%.0f short, %.0f rates", shortReads, static_cast<double>(rates.size())));
        check(step <= 20.0, rule + ": a read never mixes two rates", fmt("a read %.2f ms off where the one before ran on to", step));
        check(placed.size() >= 8 && worst <= 20.0, rule + ": every beep lies where read() places it across the changes",
              fmt("%.0f beeps, %.2f ms off at worst", static_cast<double>(placed.size()), worst));
        check(click <= 0.02, rule + ": a change of rate does not click", fmt("second difference %.4f", click));
        check(dip >= 0.2f, rule + ": a change of rate does not dip", fmt("envelope down to %.3f", dip));
    } catch (const std::exception& e) {
        check(false, std::string("decoder rate rule: the decoder threw: ") + e.what());
    }
    runDecoderJunctionChecks();
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
//       throws a content error naming the read" (1.86 s read, then no throw). The one-descriptor guard has no
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
//       twice is refused at the next start" (status '').
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

// ---------------------------------------------------------------------------------------------- //
// Skip levels: climbing, holding, coming down
// ---------------------------------------------------------------------------------------------- //

// GOAL: S15.595 -- a heard file that falls behind climbs straight to the lowest skip level predicted to keep
//       up: the two-second baseline beeps file (no B pictures, a key picture each second) at 83 ms a returned
//       picture goes from every picture to intra only, with no level between. False if it climbs one level at
//       a time through levels that leave nothing out of this file.
// GOAL: S15.596 -- a climb moves no sound mid-pass: from the first skip level sent to the slowdown's end, every
//       sound seek targets a pass's start. False if the late pictures the decoder still holds from the old level
//       are taken for a sound to move. Before the first level the file still counts as keeping up, and a hold
//       that runs out moves its sound by S15.581's rule; that is not graded here.
// GOAL: S15.597 -- at a level whose skipped pictures include a pass's last ones, the pass still ends where the
//       file does: from 3 s into the slowdown, the sound's hand-overs to the next pass come at least 1.8 s
//       apart, and the beeps the pictures show are heard within 12 ms of them. False if the pass end is taken
//       from the last picture returned (about 1 s here), which hands the sound on every second.
// GOAL: S15.598 -- the file comes back to every picture within 4.5 s of decoding getting cheap. False if a
//       level, once climbed, needs samples the file no longer gives to come down.
// WHY-THIS-SHAPE: the levels are read where the decoder is sent each packet (skip_frame), so a level the
//       daemon chose but never applied is not seen, and one it applied for a single packet is. Seeks are under
//       S15.538's sound-server guard. 4.5 s is the coder's 3.0 s measurement on Lavapipe with room for a pass.
// COVERAGE-ASSERT: at least two hand-overs and three beeps graded from 3 s in.
// CONTROL: control_run.sh 2026-10-15, reverted, Lavapipe, phase climb: DecodeLoad::add's climb taking one level at a time
//       (its `while ... ++to` deleted, backend/daemon/wallpaper_manager.cpp) -> "FAIL: climb rule: a file with no B
//       pictures climbs straight to intra only" (unreferenced at +524 ms, intra at +607 ms; re-run on the current
//       span(), 2026-10-15: held, +522 / +605 ms); control_batch.sh 2026-10-14:
//       decoderBehind() out of `chased` -> "... a climb moves no sound mid-pass" (4 seeks); VideoPipeline::decode()
//       not passing the pass end on (renderer/video/video_pipeline.cpp) -> "... handed on once a pass" (1033 ms
//       apart); DecodeLoad's samples not weighted by the intervals they span -> "... every picture again within
//       4.5 s" (never in 6 s); AudioTrack::clock() 33 ms ahead -> "... each beep is heard with the picture at its
//       timestamp". Green: intra only at +522 ms, hand-overs 2000 ms apart, every picture again after 2.66 s.
// On the device the climb file goes unpaced at unreferenced skipped (decided): graded by runUnpaced over 16 s,
// two of its five-second passes, and coming back down. The checks below are software's.
void runClimbChecks() {
    const char* kGoals = "S15.595-S15.598 (a climb straight to its level, in step, and back down)";
    const std::string rule = "climb rule";
    if (runUnpaced("S15.618 (on the device a climb ends unpaced at unreferenced skipped, its sound waiting at the loop)", rule,
                   asset("wallpaper_beeps.mp4"), 83, 16000, true, true)) {
        return;
    }
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    StallReset reset;
    Daemon d(smallConfig());
    const std::string file = asset("wallpaper_beeps.mp4");
    if (!startHeard(d, file, kGoals)) return;
    std::this_thread::sleep_for(2500ms);
    costFile(file);
    const int64_t costFrom = probe::nowNs();
    probe::g_costMs.store(83);
    std::this_thread::sleep_for(8000ms);
    const int64_t costEnd = probe::nowNs();
    probe::g_costMs.store(0);
    std::this_thread::sleep_for(6000ms);
    const int64_t settled = costFrom + 3000 * kMs;
    const std::vector<probe::LevelChange> levels = levelChangesOf(file, costFrom, costEnd);
    const Capture cap = takeCapture();
    const int64_t serverGap = longestBufferGap(cap, costFrom, costEnd);
    const std::vector<probe::SoundSeek> midPass = midPassSeeks(levels.empty() ? costEnd : levels.front().at, costEnd);
    const std::vector<probe::SoundSeek> handOvers = soundSeeksSince(settled, costEnd);
    int64_t closest = INT64_MAX;
    for (size_t i = 1; i < handOvers.size(); ++i) closest = std::min(closest, handOvers[i].at - handOvers[i - 1].at);
    const std::optional<int64_t> again = everyPictureAgain(file, costEnd);
    std::printf("audio_playback_test: %s: levels %s; %zu mid-pass seeks, %zu hand-overs from 3 s in, the closest %.0f ms apart; "
                "every picture again %.0f ms after the slowdown; the server at most %.1f ms between cycles\n",
                rule.c_str(), describeLevels(levels, costFrom).c_str(), midPass.size(), handOvers.size(),
                closest == INT64_MAX ? -1.0 : closest / 1e6, again ? (*again - costEnd) / 1e6 : -1.0, serverGap / 1e6);
    for (const probe::SoundSeek& sk : soundSeeksSince(costFrom - 500 * kMs, costEnd)) {
        std::printf("  a seek %+.0f ms from the slowdown, to %.1f ms\n", (sk.at - costFrom) / 1e6, sk.targetNs / 1e6);
    }
    const bool between = std::any_of(levels.begin(), levels.end(), [](const probe::LevelChange& c) {
        return c.discard == AVDISCARD_NONREF;
    });
    check(!levels.empty() && levels.front().discard == AVDISCARD_NONINTRA && !between,
          rule + ": a file with no B pictures climbs straight to intra only", describeLevels(levels, costFrom));
    if (serverGap > kServerLateBound) {
        notVerified(kGoals, fmt("the sound server went %.1f ms between cycles, which earns a seek of its own", serverGap / 1e6));
    } else {
        check(midPass.empty(), rule + ": a climb moves no sound mid-pass",
              fmt("%.0f seeks mid-pass", static_cast<double>(midPass.size())));
        check(handOvers.size() >= 2 && closest >= 1800 * kMs, rule + ": a pass whose last pictures are skipped is handed on once a pass",
              fmt("%.0f hand-overs, the closest %.0f ms apart", static_cast<double>(handOvers.size()),
                  closest == INT64_MAX ? -1.0 : closest / 1e6));
    }
    int passStarts = 0;
    const int graded = gradeBeepsAgainstPictures(rule, cap, shownPictures(commitsIn(d.takeCommits(), settled, costEnd), file),
                                                 file, settled, 12.0, passStarts);
    if (graded >= 0) check(graded >= 3, rule + ": at least three beeps were graded once climbed", fmt("%.0f", graded));
    check(again && *again - costEnd <= 4500 * kMs, rule + ": every picture again within 4.5 s of decoding getting cheap",
          again ? fmt("%.0f ms after", (*again - costEnd) / 1e6) : std::string("never, in 6 s"));
}

// GOAL: S15.599 -- a climb ends at the level that keeps up and no higher: the heard B-frame file at 200 ms a
//       returned picture (6 intervals) first skips unreferenced pictures, then goes to intra only within 1.5 s,
//       and never to key pictures only. False if the change of level is measured on pictures decoded at the
//       old one, which overshoots to the key level.
// GOAL: S15.600 -- a file still over load does not step down: from intra only until the cost changes (at
//       least 5 s), no lower level is sent. False if the every-picture load is capped below what the file
//       costs, so a level is predicted to keep up that does not.
// GOAL: S15.601 -- at the levels of a B-frame file the pictures stay on the sound: from 7 s in each beep shown
//       is heard within 12 ms of the commit showing it.
// GOAL: S15.602 -- when the cost falls to 60 ms (1.8 intervals) the level comes down to unreferenced pictures
//       skipped and stays there: that is the level sent when the cost changes again 6 s later, and no packet in
//       between is sent skipping nothing. False if it stays at intra only, or comes down to every picture,
//       which this cost cannot keep up with.
// WHY-THIS-SHAPE: levels read at the decoder's sends, as S15.595. The B-frame file's B pictures are all
//       unreferenced, so skipping B pictures is no level of its own here. Sync from 7 s: on software decode the
//       ~15 pictures already in flight at the old level take about 3 s at 200 ms each to clear, and are late
//       while they do; beeps against their own commits, since one picture a second gives no line to fit. That
//       no sound moves mid-pass during a climb is S15.596's (same steering code).
// COVERAGE-ASSERT: intra only reached at least 5 s before the cost changes; three beeps graded.
// CONTROL: control_batch.sh 2026-10-14, reverted, phase climbHeavy, backend/daemon/wallpaper_manager.cpp: a level
//       measured on a picture whose predecessor was decoded at the old level, or on any picture whatever its level
//       -> "FAIL: heavy climb rule: a climb goes to unreferenced, then intra only within 1.5 s, and no further" (key
//       only at +4830 / +3627 ms); no stepping down -> "... at 60 ms the level comes down to unreferenced" (no change); AudioTrack::clock()
//       33 ms ahead -> "... each beep is heard with the picture showing it". The every-picture cap flat at 2
//       (control_run.sh 2026-10-15) reddens the first and fourth lines instead (intra only only at +4833 ms;
//       intra only at the end). control_batch.sh 2026-10-15: DecodeLoad::add's step down taking a lower level as
//       keeping up at three times kKeepsUp -> "... a file still over load does not step down" (unreferenced again
//       at +5032 ms after intra only at +2031 ms, and so on every second).
void runClimbHeavyChecks() {
    const char* kGoals = "S15.599-S15.602 (a heavy climb stops at its level, holds it, and comes down to the right one)";
    const std::string rule = "heavy climb rule";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    StallReset reset;
    Daemon d(smallConfig());
    const std::string file = asset("wallpaper_beeps_bframes.mp4");
    if (!startHeard(d, file, kGoals)) return;
    std::this_thread::sleep_for(1000ms);
    costFile(file);
    const int64_t costFrom = probe::nowNs();
    probe::g_costMs.store(200);
    std::this_thread::sleep_for(12000ms);
    const int64_t lowered = probe::nowNs();
    probe::g_costMs.store(60);
    std::this_thread::sleep_for(6000ms);
    const int64_t costEnd = probe::nowNs();
    probe::g_costMs.store(0);
    const std::vector<probe::LevelChange> heavy = levelChangesOf(file, costFrom, lowered);
    const std::vector<probe::LevelChange> lighter = levelChangesOf(file, lowered, costEnd);
    const Capture cap = takeCapture();
    std::printf("audio_playback_test: %s: at 200 ms: %s; at 60 ms: %s\n", rule.c_str(), describeLevels(heavy, costFrom).c_str(),
                describeLevels(lighter, lowered).c_str());
    for (const probe::SoundSeek& sk : soundSeeksSince(costFrom - 500 * kMs, lowered)) {
        std::printf("  a seek %+.0f ms from the slowdown, to %.1f ms\n", (sk.at - costFrom) / 1e6, sk.targetNs / 1e6);
    }
    const std::optional<bool> onDevice = decodedOnDevice(file);
    if (!onDevice) {
        notVerified(kGoals, "the file's packets went to both decoder paths, or to none, so its deepest level is not known");
        return;
    }
    // GOAL: S15.618, heavy file -- on the device the B-frame file at 200 then 60 ms sends only
    //       AVDISCARD_NONREF once it climbs. False if the device is sent a level deeper than it applies.
    // WHY-THIS-SHAPE: intra only, its hold, its step down and beeps on their pictures are software's levels and
    //       software's pacing; on the device the pictures lag the sound by design, so those lines are not graded
    //       there and say so. No-sound-move lines were tried here and could not be made red (steering at the
    //       deepest level and handing on by the sound's clock both left them green in this window); phase climb
    //       grades those on the device.
    // CONTROL: control_batch.sh 2026-10-14, reverted, renderer/video/video_decoder.cpp: deepestSkip() returning NonKey
    //       on the device -> "FAIL: heavy climb rule: on the device the climb sends unreferenced skipped and nothing else".
    if (*onDevice) {
        std::printf("audio_playback_test: %s: on the device: the intra, hold, step-down and beep lines are software's and "
                    "not graded here\n", rule.c_str());
        std::vector<probe::LevelChange> both = heavy;
        both.insert(both.end(), lighter.begin(), lighter.end());
        check(onlyUnreferenced(heavy) && onlyUnreferenced(both),
              rule + ": on the device the climb sends unreferenced skipped and nothing else", describeLevels(both, costFrom));
        return;
    }
    std::optional<int64_t> intraAt;
    bool keyOnly = false, steppedDown = false;
    for (const probe::LevelChange& c : heavy) {
        if (c.discard == AVDISCARD_NONINTRA && !intraAt) intraAt = c.at;
        if (c.discard == AVDISCARD_NONKEY) keyOnly = true;
        if (intraAt && c.at > *intraAt && c.discard < AVDISCARD_NONINTRA) steppedDown = true;
    }
    check(!heavy.empty() && heavy.front().discard == AVDISCARD_NONREF && intraAt && *intraAt - heavy.front().at <= 1500 * kMs &&
              !keyOnly,
          rule + ": a climb goes to unreferenced, then intra only within 1.5 s, and no further", describeLevels(heavy, costFrom));
    check(intraAt && lowered - *intraAt >= 5000 * kMs, rule + ": intra only was held at least 5 s before the cost changed",
          intraAt ? fmt("%.0f ms", (lowered - *intraAt) / 1e6) : std::string("never reached"));
    check(intraAt && !steppedDown, rule + ": a file still over load does not step down", describeLevels(heavy, costFrom));
    const int64_t settled = costFrom + 7000 * kMs;
    const int graded = gradeBeepsAgainstCommits(rule, cap, shownPictures(commitsIn(d.takeCommits(), settled, lowered), file), 12.0);
    check(graded >= 3, rule + ": at least three beeps were graded once climbed", fmt("%.0f", graded));
    const bool skippedNothing = std::any_of(lighter.begin(), lighter.end(),
                                            [](const probe::LevelChange& c) { return c.discard == AVDISCARD_DEFAULT; });
    const int last = levelBefore(file, costEnd);
    check(last == AVDISCARD_NONREF && !skippedNothing,
          rule + ": at 60 ms the level comes down to unreferenced pictures skipped, and no further",
          std::string(discardName(last)) + " at the end" + (skippedNothing ? ", every picture sent meanwhile" : ""));
}

// GOAL: S15.603 -- a heard file with one key picture a pass shows it every pass at the key level, comes down
//       on the first cheap key picture, and is back to every picture within 9 s: the two-second one-key file at
//       83 ms a picture shows its first picture at least twice from 2 s into the slowdown to its end; after the
//       cost goes, a lower level is sent before the third picture handed on, and every picture again within
//       9 s. False if a pass that returns one picture while skipping is taken for a one-picture file (it parks
//       and shows nothing more), or one cheap key picture counts for one sample of eight.
// WHY-THIS-SHAPE: with software frame threads the decoder has read the next pass's key packet at the old level
//       before the first cheap sample lands, so the lower level is sent with the key packet after that: by the
//       third picture handed on. 9 s is three passes and a margin over the 6.0 s measured.
// COVERAGE-ASSERT: the key level reached, and the first picture shown twice in the window.
// CONTROL: control_batch.sh 2026-10-14, reverted, phase oneKey, backend/daemon/wallpaper_manager.cpp: the park test
//       without its "nothing skipped" terms -> "FAIL: one key rule: a file with one key picture a pass shows it every
//       pass" (0 times); a pass's first picture not sampled while skipping, or samples not weighted by the
//       intervals they span -> "... the level comes down on the first cheap key picture" (never). Green: shown 3
//       times, a lower level after 1 picture, every picture again after 5.9 s.
void runOneKeyChecks() {
    const char* kGoals = "S15.603 (one key picture a pass: shown every pass, and back down at once)";
    const std::string rule = "one key rule";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    StallReset reset;
    Daemon d(smallConfig());
    const std::string file = asset("wallpaper_beeps_onekey.mp4");
    if (!startHeard(d, file, kGoals)) return;
    std::this_thread::sleep_for(2500ms);
    costFile(file);
    const int64_t costFrom = probe::nowNs();
    probe::g_costMs.store(83);
    std::this_thread::sleep_for(8000ms);
    const int64_t costEnd = probe::nowNs();
    probe::g_costMs.store(0);
    std::this_thread::sleep_for(12000ms);
    const std::vector<probe::LevelChange> heavy = levelChangesOf(file, costFrom, costEnd);
    const std::vector<probe::LevelChange> after = levelChangesOf(file, costEnd, probe::nowNs());
    int firstShown = 0;
    for (const Shown& s : shownPictures(commitsIn(d.takeCommits(), costFrom + 2000 * kMs, costEnd), file)) {
        if (s.presentation && *s.presentation < 2 * kMs) ++firstShown;
    }
    std::vector<int64_t> handedOn;
    for (const probe::PictureRecord& p : picturesOf(file)) {
        if (p.at > costEnd) handedOn.push_back(p.at);
    }
    std::optional<int64_t> lowerAt;
    int before = levelBefore(file, costEnd);
    for (const probe::LevelChange& c : after) {
        if (c.discard < before) {
            lowerAt = c.at;
            break;
        }
        before = c.discard;
    }
    const std::optional<int64_t> again = everyPictureAgain(file, costEnd);
    int beforeLower = 0;
    for (const int64_t at : handedOn) {
        if (lowerAt && at < *lowerAt) ++beforeLower;
    }
    std::printf("audio_playback_test: %s: at 83 ms: %s; its first picture shown %d times from 2 s in; after: %s; a lower level "
                "sent after %d pictures handed on; every picture again %.0f ms after the slowdown\n",
                rule.c_str(), describeLevels(heavy, costFrom).c_str(), firstShown, describeLevels(after, costEnd).c_str(),
                beforeLower, again ? (*again - costEnd) / 1e6 : -1.0);
    const std::optional<bool> onDevice = decodedOnDevice(file);
    if (!onDevice) {
        notVerified(kGoals, "the file's packets went to both decoder paths, or to none, so its deepest level is not known");
        return;
    }
    // On the device the deepest level is unreferenced skipped (decided), so the climb ends there, unpaced.
    // CONTROL: control_batch.sh 2026-10-14, reverted, RTX: deepestSkip() NonKey on the device
    //       (renderer/video/video_decoder.cpp) -> "FAIL: one key rule: on the device the slowed one-key file goes to
    //       unreferenced skipped and nothing else"; DecodeLoad::add's step-down loop never entered
    //       (backend/daemon/wallpaper_manager.cpp) -> "... on the device every picture is sent again within 9 s".
    if (*onDevice) {
        check(onlyUnreferenced(heavy), rule + ": on the device the slowed one-key file goes to unreferenced skipped and nothing else",
              describeLevels(heavy, costFrom));
        // A device pass at 83 ms takes 5 s, so the 6 s window holds one first picture by design, and leaving
        // unreferenced skipping needs no key packet, so the third-picture bound (the key gate's) is not graded.
        std::printf("audio_playback_test: %s: on the device the key-level lines are software's; graded: every picture "
                    "sent again\n", rule.c_str());
        // Read from the level sent: unreferenced skipping leaves nothing out of this file, so its pictures
        // look like every picture while it is still unpaced.
        check(lowerAt && *lowerAt - costEnd <= 9000 * kMs, rule + ": on the device every picture is sent again within 9 s of decoding getting cheap",
              lowerAt ? fmt("%.0f ms after", (*lowerAt - costEnd) / 1e6) : std::string("never, in 12 s"));
        return;
    } else {
        const bool keyLevel = std::any_of(heavy.begin(), heavy.end(), [](const probe::LevelChange& c) {
            return c.discard >= AVDISCARD_NONINTRA;
        });
        check(keyLevel, rule + ": the slowed one-key file climbed to its key picture", describeLevels(heavy, costFrom));
    }
    check(firstShown >= 2, rule + ": a file with one key picture a pass shows it every pass", fmt("%.0f times", firstShown));
    check(lowerAt && beforeLower < 3, rule + ": the level comes down on the first cheap key picture",
          lowerAt ? fmt("after %.0f pictures handed on", beforeLower) : std::string("never"));
    check(again && *again - costEnd <= 9000 * kMs, rule + ": every picture again within 9 s of decoding getting cheap",
          again ? fmt("%.0f ms after", (*again - costEnd) / 1e6) : std::string("never, in 12 s"));
}

// GOAL: S15.620 -- a heard file unpaced on the device whose hardware decoder then fails
//       goes on in software with software's levels: within 5 s of the failure its packets go to software, and
//       a level deeper than unreferenced (intra or key only) is sent (measured 2.5 s on the RTX 3080 Ti). False if the climb state carried over
//       from the device keeps the software decoder capped at unreferenced, so it stays unpaced for nothing.
// WHY-THIS-SHAPE: the failure is AVERROR_EXTERNAL from the device decoder's next picture, what a hardware
//       failure on a healthy device returns; the decoder itself then reopens in software. Not answerable in
//       software (Lavapipe), nor when the failure did not fire or nothing went to software after it: those
//       are not verified, never passed.
// COVERAGE-ASSERT: the failure fired, and software packets were sent after it.
// CONTROL: control_batch.sh 2026-10-14, reverted, RTX, renderer/video/video_decoder.cpp: deepestSkip() returning
//       Unreferenced on both paths -> "FAIL: hardware fallback rule: after a hardware failure while unpaced, software
//       sends a level deeper than unreferenced" (unreferenced at +2513 ms). Green: intra only 2.5 s after the failure.
//       Re-run on the RTX after the phase's change, control_batch.sh 2026-10-15: the same line red, green restored.
void runHardwareFallbackChecks() {
    const char* kGoals = "S15.620 (a hardware failure while unpaced lets software apply its levels)";
    const std::string rule = "hardware fallback rule";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    StallReset reset;
    Daemon d(smallConfig());
    const std::string file = asset("wallpaper_beeps.mp4");
    if (!startHeard(d, file, kGoals)) return;
    std::this_thread::sleep_for(2500ms);
    const std::optional<bool> onDevice = decodedOnDevice(file);
    if (!onDevice || !*onDevice) {
        notVerified(kGoals, "the file does not decode on the device here, so no hardware failure can be staged");
        return;
    }
    costFile(file);
    const int64_t costFrom = probe::nowNs();
    probe::g_costMs.store(83);
    std::this_thread::sleep_for(3000ms);
    const int climbed = levelBefore(file, probe::nowNs());
    probe::g_failedDeviceAt.store(0);
    const int softwareBefore = sendsOf(file).second;
    probe::g_failDeviceOnce.store(true);
    const bool fired = waitFor(3000ms, [] { return probe::g_failedDeviceAt.load() != 0; });
    probe::g_failDeviceOnce.store(false);
    if (fired) std::this_thread::sleep_for(5000ms);
    const int64_t to = probe::nowNs();
    probe::g_costMs.store(0);
    const int64_t failedAt = probe::g_failedDeviceAt.load();
    const std::vector<probe::LevelChange> after = fired ? levelChangesOf(file, failedAt, to) : std::vector<probe::LevelChange>{};
    std::printf("audio_playback_test: %s: on the device %s; the failure %s; after it: %s\n", rule.c_str(),
                describeLevels(levelChangesOf(file, costFrom, fired ? failedAt : to), costFrom).c_str(),
                fired ? fmt("%+.0f ms from the slowdown", (failedAt - costFrom) / 1e6).c_str() : "never fired",
                describeLevels(after, fired ? failedAt : to).c_str());
    if (climbed != AVDISCARD_NONREF) {
        notVerified(kGoals, std::string("the file was at ") + discardName(climbed) + ", not unreferenced skipped, before the failure");
        return;
    }
    if (!fired) {
        notVerified(kGoals, "the staged hardware failure never fired within 3 s");
        return;
    }
    if (sendsOf(file).second <= softwareBefore) {
        notVerified(kGoals, "no packet went to software after the hardware failure");
        return;
    }
    const bool deeper = std::any_of(after.begin(), after.end(), [](const probe::LevelChange& c) {
        return c.discard == AVDISCARD_NONINTRA || c.discard == AVDISCARD_NONKEY;
    });
    check(deeper, rule + ": after a hardware failure while unpaced, software sends a level deeper than unreferenced",
          describeLevels(after, failedAt));
}

// GOAL: S15.604 -- one slow decode does not start a climb: the silent long baseline file at 16 ms a returned
//       picture (about half its interval), one picture held a further 333 ms (ten intervals), sends no other
//       skip level from the slowdown to 2.5 s after the hold. False if the every-picture load takes a single
//       decode at more than twice the average, so ten intervals lift a load of 0.5 past 1.
// WHY-THIS-SHAPE: silent, so nothing but the load moves the level. The hold is on one picture's return from
//       the decoder, 1 s ahead of the latest handed on, so its cost lands in one decode's sample.
// COVERAGE-ASSERT: the hold ran.
// CONTROL: control_batch.sh 2026-10-14, reverted, phase loadSpike: DecodeLoad's every-picture cap made 1e9
//       (backend/daemon/wallpaper_manager.cpp) -> "FAIL: load spike rule: one slow decode at half load does not start
//       a climb" (intra only at the hold). Re-run on the current span() (control_run.sh 2026-10-15): held.
void runLoadSpikeChecks() {
    const char* kGoals = "S15.604 (one slow decode does not start a climb)";
    const std::string rule = "load spike rule";
    StallReset reset;
    const std::string file = asset("wallpaper_beeps_long.mp4");
    Daemon d(smallConfig());
    resetCapture();
    d.play(file, std::nullopt);
    if (!d.waitForCommits(3, 15s)) {
        notVerified(kGoals, "the video drew no pictures within 15 s");
        return;
    }
    costFile(file);
    const int64_t costFrom = probe::nowNs();
    probe::g_costMs.store(16);
    std::this_thread::sleep_for(3000ms);
    const std::optional<int64_t> latest = lastPictureOf(file);
    if (!latest) {
        check(false, rule + ": the file's pictures carry presentation times");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(probe::g_holdMutex);
        probe::g_holdFile = std::filesystem::path(file).filename().string();
    }
    const int64_t frame = (*latest + 1'000'000'000LL) * 30 / 1'000'000'000LL;
    probe::g_holdAtNs.store(frame * 1'000'000'000LL / 30);
    probe::g_holdEndedAt.store(0);
    probe::g_holdMs.store(333);
    if (!waitFor(3000ms, [] { return probe::g_holdEndedAt.load() != 0; })) {
        probe::g_holdMs.store(0);
        check(false, rule + ": the staged hold was reached within 3 s");
        return;
    }
    const int64_t held = probe::g_holdEndedAt.load();
    std::this_thread::sleep_for(2500ms);
    const std::vector<probe::LevelChange> levels = levelChangesOf(file, costFrom, probe::nowNs());
    std::printf("audio_playback_test: %s: the picture at %.3f s held 333 ms, %.0f ms into the slowdown; levels %s\n",
                rule.c_str(), frame / 30.0, (held - costFrom) / 1e6, describeLevels(levels, costFrom).c_str());
    check(levels.empty(), rule + ": one slow decode at half load does not start a climb", describeLevels(levels, costFrom));
}

// GOAL: S15.605 -- key pictures more than 5 s apart keep their spacing at the key level: the heard 18 s file
//       with key pictures at 0, 6 and 12 s, at 83 ms a returned picture, shows each key picture after the one
//       before by its file distance (the loop's from 12 s to the pass's end and on to 0) within 50 ms. False if a gap past the 5 s ceiling falls back to the picture's own duration, which
//       shows the key pictures as fast as they decode.
// WHY-THIS-SHAPE: graded from 3 s in, by then at the key level. The pass's length is the fixture's own 18 s:
//       at the key level the decoder never returns the last picture that would give it.
// COVERAGE-ASSERT: at least two gaps of 5 s or more graded.
// CONTROL: control_batch.sh 2026-10-14, reverted, phase farKeys: gapBetween's ceiling not scaled by the packets read
//       (backend/daemon/wallpaper_manager.cpp) -> "FAIL: far keys rule: key pictures far apart are shown their file's
//       distance apart" (84 ms on the wall for 6000 ms; again 2026-10-15, Lavapipe). Green: 6000.0 for 6000.0.
void runFarKeysChecks() {
    const char* kGoals = "S15.605 (key pictures far apart keep their spacing)";
    const std::string rule = "far keys rule";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    StallReset reset;
    Daemon d(smallConfig());
    const std::string file = asset("wallpaper_beeps_farkeys.mp4");
    if (!startHeard(d, file, kGoals)) return;
    std::this_thread::sleep_for(1000ms);
    costFile(file);
    const int64_t costFrom = probe::nowNs();
    probe::g_costMs.store(83);
    std::this_thread::sleep_for(21000ms);
    const int64_t costEnd = probe::nowNs();
    const std::optional<bool> onDevice = decodedOnDevice(file);
    // The heavier stage only where a key level exists: software.
    int64_t heavyEnd = costEnd;
    if (onDevice && !*onDevice) {
        probe::g_costMs.store(5500);
        std::this_thread::sleep_for(12000ms);
        heavyEnd = probe::nowNs();
    }
    probe::g_costMs.store(0);
    const int64_t from = costFrom + 3000 * kMs;
    constexpr int64_t kPass = 18'000 * kMs;
    const std::vector<Commit> commits = d.takeCommits();
    // The worst distance off its file's, over steps of 5 s or more of the file, and how many there were.
    const auto spacing = [&](int64_t start, int64_t end, int& graded) {
        double worst = 0;
        std::optional<Shown> last;
        for (const Shown& s : shownPictures(commitsIn(commits, start, end), file)) {
            if (!s.presentation) continue;
            if (last && *s.presentation != *last->presentation) {
                const int64_t fileStep = *s.presentation > *last->presentation ? *s.presentation - *last->presentation
                                                                               : kPass - *last->presentation + *s.presentation;
                const int64_t wallStep = s.commit.at - last->commit.at;
                std::printf("audio_playback_test: %s: %.2f s -> %.2f s of the file: %.1f ms of the file, %.1f ms on the wall\n",
                            rule.c_str(), *last->presentation / 1e9, *s.presentation / 1e9, fileStep / 1e6, wallStep / 1e6);
                if (fileStep >= 5000 * kMs) {
                    ++graded;
                    worst = std::max(worst, std::fabs((wallStep - fileStep) / 1e6));
                }
            }
            // From the held picture's first commit: a redraw of it is no new showing.
            if (!last || *s.presentation != *last->presentation) last = s;
        }
        return worst;
    };
    int graded = 0;
    const double worst = spacing(from, costEnd, graded);
    const std::vector<probe::LevelChange> levels = levelChangesOf(file, costFrom, costEnd);
    std::printf("audio_playback_test: %s: levels %s\n", rule.c_str(), describeLevels(levels, costFrom).c_str());
    if (!onDevice) {
        notVerified(kGoals, "the file's packets went to both decoder paths, or to none, so its deepest level is not known");
        return;
    }
    // On the device no key level exists (decided): the file goes unpaced at unreferenced skipped and every
    // picture is drawn as decoded, so no key-to-key gap is there to grade.
    // CONTROL: control_batch.sh 2026-10-14, reverted, RTX: deepestSkip() NonKey on the device
    //       (renderer/video/video_decoder.cpp) -> "FAIL: far keys rule: on the device the slowed file goes to
    //       unreferenced skipped and nothing else". A no-sound-move line here stayed green with the climb steered at
//       the deepest level, so it is not graded; phase climb grades that on the device.
    if (*onDevice) {
        std::printf("audio_playback_test: %s: on the device there is no key level, so the key spacing is not graded\n", rule.c_str());
        check(onlyUnreferenced(levels), rule + ": on the device the slowed file goes to unreferenced skipped and nothing else",
              describeLevels(levels, costFrom));
        return;
    }
    check(graded >= 2, rule + ": at least two gaps of 5 s or more were graded", fmt("%.0f", graded));
    check(graded >= 2 && worst <= 50.0, rule + ": key pictures far apart are shown their file's distance apart",
          fmt("%.1f ms off at worst", worst));
    // GOAL: S15.641 -- an intra-only level whose pictures are 6 s apart is judged by its measured span (180
    //       intervals) once it has decoded pictures there, not by the type prediction (the pictures decoded before
    //       the climb, a few seconds' worth, over the one key among them): with each picture then costing 5.5 s
    //       (165 intervals, under 180) the level holds and no other level is sent. False if the span settles only
    //       after 8 pictures there (48 s): the level reads as behind and goes unpaced, which sends key only.
    // WHY-THIS-SHAPE: the span is not reported, so it is read from the one decision that turns on which side of
    //       the cost it lies; 3 s (90 intervals) was tried and left the old rule green, the prediction being over
    //       90 here. Not the on-screen spacing: unpaced or not, a decode shorter than its picture is in time.
    // CONTROL: control_run.sh 2026-10-16, reverted, Lavapipe: Span::add counting one interval a picture (settled only
    //       after 8), backend/daemon/wallpaper_manager.cpp -> "FAIL: far keys rule: at 5.5 s a picture of 6 s, the
    //       measured span holds the level" (key only at +8123 ms); green: no change.
    // COVERAGE-ASSERT: the file was at intra only when the cost rose.
    const int atRise = levelBefore(file, costEnd);
    const std::vector<probe::LevelChange> heavyLevels = levelChangesOf(file, costEnd, heavyEnd);
    std::printf("audio_playback_test: %s: at 5.5 s a picture, from %s: %s\n", rule.c_str(), discardName(atRise),
                describeLevels(heavyLevels, costEnd).c_str());
    check(atRise == AVDISCARD_NONINTRA, rule + ": the file was at intra only when each picture came to cost 5.5 s",
          discardName(atRise));
    check(atRise == AVDISCARD_NONINTRA && heavyLevels.empty(),
          rule + ": at 5.5 s a picture of 6 s, the measured span holds the level", describeLevels(heavyLevels, costEnd));
}

// GOAL: S15.606 -- a single timestamp jump past the 5 s ceiling between two packets in a row is still read as a
//       discontinuity: the silent file whose pictures jump from 0.967 s to 7 s shows the 7 s picture less than
//       1 s after the one before, at every pass. False if the ceiling is scaled however few packets lie between,
//       which holds the screen 6 s on one picture.
// WHY-THIS-SHAPE: silent, so the sound cannot time the pictures and only the gap rule places them. Nine
//       seconds: four jumps when the rule holds, and still one, graded, when it does not.
// COVERAGE-ASSERT: at least one jump crossed.
// CONTROL: control_batch.sh 2026-10-14, reverted, phase timestampJump: gapBetween's ceiling made 1000 times 5 s
//       (backend/daemon/wallpaper_manager.cpp) -> "FAIL: timestamp jump rule: a single jump past the ceiling is not
//       waited out" (6033 ms on one picture; again 2026-10-15, Lavapipe). Green: 5 jumps, 33.3 ms.
void runTimestampJumpChecks() {
    const char* kGoals = "S15.606 (one timestamp jump past the ceiling is a discontinuity)";
    const std::string rule = "timestamp jump rule";
    const std::string file = asset("wallpaper_jump.ts");
    Daemon d(smallConfig());
    resetCapture();
    d.play(file, std::nullopt);
    if (!d.waitForCommits(3, 15s)) {
        notVerified(kGoals, "the video drew no pictures within 15 s");
        return;
    }
    std::this_thread::sleep_for(9000ms);
    const std::vector<Shown> shown = shownPictures(d.takeCommits(), file);
    int jumps = 0;
    double longest = 0;
    // Each step is timed from the first commit of the picture held before it: a redraw of the held picture
    // during a 6 s hold would otherwise shorten the hold to the redraw's distance.
    std::optional<size_t> held;
    for (size_t i = 0; i < shown.size(); ++i) {
        if (!shown[i].presentation) continue;
        if (held && *shown[i].presentation == *shown[*held].presentation) continue;
        if (held && *shown[i].presentation - *shown[*held].presentation >= 5000 * kMs) {
            ++jumps;
            longest = std::max(longest, (shown[i].commit.at - shown[*held].commit.at) / 1e6);
        }
        held = i;
    }
    std::printf("audio_playback_test: %s: %d jumps crossed, the longest %.1f ms on the wall\n", rule.c_str(), jumps, longest);
    check(jumps >= 1, rule + ": the jump was crossed", fmt("%.0f times", jumps));
    check(jumps >= 1 && longest < 1000.0, rule + ": a single jump past the ceiling is not waited out",
          fmt("%.1f ms on one picture", longest));
}

// Where read() places the last frame of `s`, and the furthest frame it places, in frames of the file.
std::pair<double, double> lastAndFurthestPlaced(const Stretched& s) {
    double last = -1, furthest = -1;
    for (const Stretched::Call& c : s.calls) {
        last = static_cast<double>(c.first.file.count()) * kRate / 1e9 + static_cast<double>(c.got - 1) * c.first.rate;
        furthest = std::max(furthest, last);
    }
    return {last, furthest};
}

// The root mean square of the right channel's last `frames` frames of `s`.
double tailLevel(const Stretched& s, size_t frames) {
    double sum = 0;
    const size_t from = s.right.size() > frames ? s.right.size() - frames : 0;
    for (size_t i = from; i < s.right.size(); ++i) sum += double(s.right[i]) * s.right[i];
    return s.right.size() > from ? std::sqrt(sum / double(s.right.size() - from)) : 0.0;
}

// GOAL: S15.627 -- a stretched read run to the file's end, with no change of speed, hands out the whole file and
//       nothing past it: the ramp fixture from 9 s at 0.5 and at 2 gives 1 s of file within 6 ms (output frames
//       times the rate), its tone is loud to the last frame, and the last frame is placed within 6 ms of the file's
//       end. False if the stretcher's flush drops the end (~8 ms of file at 0.5). 6 ms, not a frame or two: atempo
//       runs the last ~6 ms of file faster than 0.5 (the coder's reported, unfixed remainder; 5.2 ms measured).
// GOAL: S15.624 -- the same reads, phase decoderStretchPastEnd, place no frame past the file's end (480000 frames).
//       False if what the stretcher makes from the silence fed after the end is handed out, placed past it.
// WHY-THIS-SHAPE: read through AudioDecoder alone, so the sound server's timing is not in the number. The tone's level
//       over the last 5 ms catches silence handed out after the sound, which the frame count would accept as file.
// CONTROL: control_batch.sh 2026-10-15, reverted, phase decoderStretchEnd, renderer/audio/audio_decoder.cpp: endStretcher() feeding no silence ->
//       "FAIL: stretch end rule, rate 0.5: the whole remaining second of the file is handed out" (47504 frames of
//       file; green 47748.5) and "... the last frame is placed at the file's end" (479571.2; green 479807.6);
//       takeStretched() keeping frames made from the silence -> "... rate 0.5: the tone is heard to the last frame"
//       (0.000; green 0.312), at 2 and in every change case too; both re-run and held 2026-10-15. S15.624: control_run.sh
//       2026-10-15, phase decoderStretchPastEnd, glideToRunEnd() returning at once -> "FAIL: stretch end rule,
//       rate 2.0: no frame is placed past the file's end" (480042.8) and the 0.5 -> 2.0 3000-frame case. Green 479999.6.
// COVERAGE-ASSERT: each rate reads at least one read's worth of frames, and the end is reached (read() returns 0).
void stretchEnd(bool pastEnd) {
    constexpr double kEnd = 480000.0;  // wallpaper_ramp.mkv: 10 s of lossless sound
    try {
        std::unique_ptr<AudioDecoder> decoder = AudioDecoder::open(asset("wallpaper_ramp.mkv"));
        if (!decoder) {
            check(false, "stretch end rule: the ramp fixture's sound is found");
            return;
        }
        for (const double rate : {0.5, 2.0}) {
            const std::string rule = fmt("stretch end rule, rate %.1f", rate);
            decoder->setRate(rate);
            decoder->seek(9000ms);
            Stretched s;
            readStretched(*decoder, s, static_cast<size_t>(3.0 / rate * kRate), 256);
            const bool ended = decoder->ended();
            const auto [last, furthest] = lastAndFurthestPlaced(s);
            const double ofFile = static_cast<double>(s.left.size()) * rate;
            const double level = tailLevel(s, 240);
            std::printf("audio_playback_test: %s: %zu frames out (%.1f frames of file, of 48000); last placed %.1f, furthest "
                        "%.1f (end %.0f); tone over the last 5 ms %.3f; ended %d\n",
                        rule.c_str(), s.left.size(), ofFile, last, furthest, kEnd, level, ended ? 1 : 0);
            check(s.calls.size() > 1 && ended, rule + ": the read reaches the file's end",
                  fmt("%.0f reads, ended %.0f", static_cast<double>(s.calls.size()), ended ? 1.0 : 0.0));
            if (pastEnd) {
                check(furthest < kEnd + 0.5, rule + ": no frame is placed past the file's end", fmt("%.1f of %.0f", furthest, kEnd));
                continue;
            }
            check(std::fabs(ofFile - 48000.0) <= 288.0, rule + ": the whole remaining second of the file is handed out",
                  fmt("%.1f frames of file", ofFile));
            check(level >= 0.2, rule + ": the tone is heard to the last frame", fmt("%.3f over the last 5 ms", level));
            check(last >= kEnd - 288.0, rule + ": the last frame is placed at the file's end", fmt("%.1f of %.0f", last, kEnd));
        }
        // GOAL: S15.627 -- a speed change asked within a stretcher window (2048 frames) of the file's end hands the rest
        //       of the file on whole: from 0.5, a change to 1 or to 2 asked 300 to 3000 frames before the end still ends
        //       on the file's last frame, placed within 6 ms of it, the tone loud to the last frame, nothing placed past
        //       it. False if the run cut near the end drops the file's tail or hands out the stretcher's padding.
        // WHY-THIS-SHAPE: several distances, since where the decoder has already pulled to decides whether the
        //       stretcher has met the end before the change; the rest is read at the new rate in one place.
        // CONTROL: control_batch.sh 2026-10-15, reverted, the same no-silence break -> "FAIL: stretch end change rule, 0.5 -> 2.0 3000 frames before
        //       the end: the last frame is placed at the file's end" (479371.3; green 479983 at 1, in reach of the
        //       runOnward() path), and the same line at the 300-2000 cases (479570; green 479803).
        // COVERAGE-ASSERT: every case reached its change point and the end.
        int cases = 0;
        for (const double to : {1.0, 2.0}) {
            for (const double before : {300.0, 1000.0, 2000.0, 3000.0}) {
                const std::string rule = fmt("stretch end change rule, 0.5 -> %.1f %.0f frames before the end", to, before);
                decoder->setRate(0.5);
                decoder->seek(9800ms);
                Stretched s;
                double last = 0;
                while (last < kEnd - before) {
                    const size_t had = s.calls.size();
                    readStretched(*decoder, s, 64, 64);
                    if (s.calls.size() == had) break;
                    last = lastAndFurthestPlaced(s).first;
                }
                const double at = last;
                decoder->setRate(to);
                readStretched(*decoder, s, 3 * kRate, 256);
                const bool ended = decoder->ended();
                const auto [end, furthest] = lastAndFurthestPlaced(s);
                const double level = tailLevel(s, 240);
                std::printf("audio_playback_test: %s: changed at %.1f; last placed %.1f, furthest %.1f; tone over the last "
                            "5 ms %.3f; ended %d\n",
                            rule.c_str(), at, end, furthest, level, ended ? 1 : 0);
                if (at >= kEnd - before && at < kEnd && ended) ++cases;
                if (pastEnd) {
                    check(furthest < kEnd + 0.5, rule + ": no frame is placed past the file's end",
                          fmt("%.1f of %.0f", furthest, kEnd));
                    continue;
                }
                check(level >= 0.2, rule + ": the tone is heard to the last frame", fmt("%.3f over the last 5 ms", level));
                check(end >= kEnd - 288.0, rule + ": the last frame is placed at the file's end", fmt("%.1f of %.0f", end, kEnd));
            }
        }
        check(cases == 8, "stretch end change rule: every change was asked before the end and read to it",
              fmt("%.0f of 8", cases));
    } catch (const std::exception& e) {
        check(false, std::string("stretch end rule: the decoder threw: ") + e.what());
    }
}
void runDecoderStretchEndChecks() { stretchEnd(false); }
// The S15.624 half of the reads above, a phase of its own so each half can be run and broken alone.
void runDecoderStretchPastEndChecks() { stretchEnd(true); }

// wallpaper_ramp.mkv's left channel (0.4 s of file from -0.1 to 0.1) read as the file frame a sample was made
// from -- a blend of two fragments reads as their weighted mean -- in the period nearest `near`.
double rampOrigin(float left, double near) {
    constexpr double kPeriod = 19200.0;
    const double pos = (static_cast<double>(left) / 0.1 + 1.0) * 0.5 * kPeriod;
    return pos + kPeriod * std::round((near - pos) / kPeriod);
}

// GOAL: S15.629 -- read to a file's end at 0.5 and 2 (ramp from 9 s, 256-frame reads), the last frame is placed
//       within the file's last 48 frames (1 ms), and no read's first frame is placed before the one before it nor
//       more than 24 frames (0.5 ms) before where that read's last frame was placed. False if the end correction
//       is missing (placed 192 short at 0.5, 64 past at 2) or made in one jump.
// WHY-THIS-SHAPE: graded against the file's end, not the last frame's own origin: that frame is blended with the
//       silence fed after the sound, so the ramp under-reads it (472863 measured) and only the decoder's guide
//       channel, which is not visible here, knows its origin. The last frame is made from the file's last frames
//       (decoderStretchEnd: the tone is loud to it), so 48 frames, under both errors and the engine's 2 ms
//       re-mark slack, stands for "on its origin". 24 frames is twice the 11-13 the correction steps back at 2;
//       comparing only first positions (never going back) would pass a correction made in one read.
// CONTROL: control_batch.sh 2026-10-15, reverted, phase decoderStretchEndPlace,
//       renderer/audio/audio_decoder.cpp: glideToRunEnd() returning at once -> "FAIL: stretch end place rule,
//       rate 0.5: the last frame is placed in the file's last ms" (479842.7) and at 2.0 (480042.8); glideToRunEnd()
//       steering to the run's last frame in one read (`to = runLast`) -> "... rate 2.0: no read steps back more than
//       24 frames" (510.00) and "... first frame is placed past the one before" at both rates. Green: 479999.7 /
//       479999.6, steps back at most 2.61 / 6.13 frames.
// COVERAGE-ASSERT: at least 80 read-to-read comparisons per rate, and the end reached.
void runDecoderStretchEndPlaceChecks() {
    try {
        std::unique_ptr<AudioDecoder> decoder = AudioDecoder::open(asset("wallpaper_ramp.mkv"));
        if (!decoder) {
            check(false, "stretch end place rule: the ramp fixture's sound is found");
            return;
        }
        for (const double rate : {0.5, 2.0}) {
            const std::string rule = fmt("stretch end place rule, rate %.1f", rate);
            decoder->setRate(rate);
            decoder->seek(9000ms);
            Stretched s;
            readStretched(*decoder, s, static_cast<size_t>(3.0 / rate * kRate), 256);
            const bool ended = decoder->ended();
            check(ended && !s.left.empty(), rule + ": the read reaches the file's end");
            if (!ended || s.left.empty()) continue;
            const double last = lastAndFurthestPlaced(s).first;
            int compared = 0, wentBack = 0;
            double worstBack = -INFINITY;
            for (size_t k = 1; k < s.calls.size(); ++k) {
                const Stretched::Call& a = s.calls[k - 1];
                const Stretched::Call& b = s.calls[k];
                const double aFirst = static_cast<double>(a.first.file.count()) * kRate / 1e9;
                const double bFirst = static_cast<double>(b.first.file.count()) * kRate / 1e9;
                ++compared;
                if (bFirst <= aFirst) ++wentBack;
                worstBack = std::max(worstBack, aFirst + static_cast<double>(a.got - 1) * a.first.rate - bFirst);
            }
            std::printf("audio_playback_test: %s: last frame placed %.1f (end 480000); %d reads compared, the largest step "
                        "back %.2f frames, %d first frames not past the one before\n",
                        rule.c_str(), last, compared, worstBack, wentBack);
            check(last >= 480000.0 - 48.0 && last < 480000.0 + 0.5, rule + ": the last frame is placed in the file's last ms",
                  fmt("placed %.1f of 480000", last));
            check(compared >= 80 && wentBack == 0, rule + ": every read's first frame is placed past the one before",
                  fmt("%.0f of %.0f went back", wentBack, compared));
            check(compared >= 80 && worstBack <= 24.0, rule + ": no read steps back more than 24 frames",
                  fmt("%.2f frames over %.0f reads", worstBack, compared));
        }
    } catch (const std::exception& e) {
        check(false, std::string("stretch end place rule: the decoder threw: ") + e.what());
    }
}

// GOAL: S15.630 -- a stretch started from 5 s of the ramp (over 2048 frames of sound before it) at 0.5 and at 2
//       is on the rate from its first frame: frame 0 is made within 32 frames of 5 s, and each of the first 4096
//       output frames within 200 file frames of 5 s + i x rate. False if the lead-in is skipped (atempo's cold
//       start runs +218..+539 ahead at 0.5 by frame 1024-4096, lags up to 280 at 2) or what it made is kept.
// WHY-THIS-SHAPE: every frame's origin is read from the ramp itself, not from read()'s reported positions, which
//       average the drift over 8192 frames and so would hide the first 4096 frames' error. 200 is atempo's own
//       swing (162.7 measured at 2) with a margin, below the cold start's +383..+539 at 0.5. 32 at frame 0: a
//       Matroska seek lands up to 16 frames off (1 ms timestamps; +16 measured here), a kept lead-in thousands.
// CONTROL: control_batch.sh 2026-10-15, reverted, phase decoderLeadIn, renderer/audio/audio_decoder.cpp
//       buildStretcher(): lead forced to 0 -> "FAIL: lead in rule, rate 0.5: every frame is on the rate from the
//       first" (250.9) and at 2.0 (206.8, a thin margin); takeStretched() keeping what the lead-in made
//       (`if (false && m_leadUntil)`) -> "... frame 0 is made from where the play starts" (-4080.0) at both rates.
//       Green: worst 134.7 (0.5) and 162.7 (2.0), frame 0 +15.7 / +16.8.
// COVERAGE-ASSERT: all 4096 frames graded at each rate.
void runDecoderLeadInChecks() {
    try {
        for (const double rate : {0.5, 2.0}) {
            const std::string rule = fmt("lead in rule, rate %.1f", rate);
            std::unique_ptr<AudioDecoder> decoder = AudioDecoder::open(asset("wallpaper_ramp.mkv"));
            if (!decoder) {
                check(false, rule + ": the ramp fixture's sound is found");
                return;
            }
            decoder->setRate(rate);
            decoder->seek(5000ms);
            Stretched s;
            readStretched(*decoder, s, 4096, 256);
            constexpr double kStart = 5.0 * kRate;
            size_t graded = 0;
            double worst = 0, firstOff = INFINITY;
            for (size_t i = 0; i < s.left.size() && i < 4096; ++i) {
                const double ideal = kStart + static_cast<double>(i) * rate;
                const double off = rampOrigin(s.left[i], ideal) - ideal;
                if (i == 0) firstOff = off;
                worst = std::max(worst, std::fabs(off));
                ++graded;
            }
            const auto offAt = [&](size_t i) {
                const double ideal = kStart + static_cast<double>(i) * rate;
                return i < s.left.size() ? rampOrigin(s.left[i], ideal) - ideal : NAN;
            };
            std::printf("audio_playback_test: %s: origin against 5 s + i x rate at frame 0 %+.1f, 1024 %+.1f, 2048 %+.1f, "
                        "3072 %+.1f, 4095 %+.1f; the largest %.1f over %zu frames\n",
                        rule.c_str(), offAt(0), offAt(1024), offAt(2048), offAt(3072), offAt(4095), worst, graded);
            check(std::fabs(firstOff) <= 32.0, rule + ": frame 0 is made from where the play starts",
                  fmt("%+.1f frames", firstOff));
            check(graded == 4096 && worst <= 200.0, rule + ": every frame is on the rate from the first",
                  fmt("%.1f frames at worst over %.0f frames", worst, static_cast<double>(graded)));
        }
    } catch (const std::exception& e) {
        check(false, std::string("lead in rule: the decoder threw: ") + e.what());
    }
}

// The largest second difference of each channel of `s` over frames [from, to).
std::pair<double, double> secondDifferences(const Stretched& s, size_t from, size_t to) {
    double left = 0, right = 0;
    for (size_t i = std::max<size_t>(from, 1); i + 1 < to && i + 1 < s.left.size(); ++i) {
        left = std::max(left, static_cast<double>(std::fabs(s.left[i + 1] - 2 * s.left[i] + s.left[i - 1])));
        right = std::max(right, static_cast<double>(std::fabs(s.right[i + 1] - 2 * s.right[i] + s.right[i - 1])));
    }
    return {left, right};
}

// beeps_long's own largest second difference is about 0.001 on the right (the 440 Hz tone) and 0.050 on the left
// (a beep's edge); a cold stretcher's unfaded switch from copying to blending reached 0.036 and 0.13.
constexpr double kClickRight = 0.02, kClickLeft = 0.07;

// GOAL: S15.631 -- no click where a fresh stretcher switches from copying to blending, after a change of speed:
//       for each of the six changes among 0.5, 1 and 2, at 200 cut points 487 file frames apart from 1 s of
//       beeps_long, the 0.4 s around and after the change stays under 0.02 (right) and 0.07 (left) of second
//       difference. False if the 2 ms crossfade at the switch (ColdHead) is removed (9-12 of 200 cuts clicked).
// WHY-THIS-SHAPE: one cut point (decoderCutSeek's) misses it: only cuts within ~25 ms of a beep onset click.
//       487 is prime to the beeps' 24000-frame period, so 200 cuts sweep the period four times about 49 points
//       apart each: a click hitting 5% of cut points is hit about ten times, and missed by all 200 only if it
//       never falls within 25 ms of an onset. Graded per change, so one change cannot hide behind the others.
// CONTROL: control_batch.sh 2026-10-15, reverted, phase decoderColdHeadChange,
//       renderer/audio/audio_decoder.cpp takeStretched(): `if (false && m_coldHead)` -> "FAIL: cold head change
//       rule, 2.0 -> 0.5: no cut point clicks" (7 of 200, right 0.0364) and 1.0 -> 0.5 (9 of 200). The four changes
//       to 1 or 2 stay green under it, and under the ColdHead crossfade blending against frames 100 back
//       (control_batch.sh 2026-10-15, which reddens both changes to 0.5, 183-184 of 200): no cold-head click is
//       seen there on this file, so for them the line grades the change's own junction. fadeIn() crossfading from
//       the old run negated (control_batch.sh 2026-10-15) -> "... no cut point clicks" at all six (200 of 200).
//       Green: right at most 0.0013, left 0.0487.
// COVERAGE-ASSERT: 200 cuts graded per change, each with its new rate read.
void runDecoderColdHeadChangeChecks() {
    const std::string file = asset("wallpaper_beeps_long.mp4");
    const std::pair<double, double> changes[] = {{2.0, 0.5}, {1.0, 0.5}, {0.5, 2.0}, {0.5, 1.0}, {1.0, 2.0}, {2.0, 1.0}};
    try {
        for (const auto& [r0, r1] : changes) {
            const std::string rule = fmt("cold head change rule, %.1f -> ", r0) + fmt("%.1f", r1);
            std::unique_ptr<AudioDecoder> d = AudioDecoder::open(file);
            int graded = 0, clicked = 0;
            double worstLeft = 0, worstRight = 0;
            for (int k = 0; k < 200; ++k) {
                d->setRate(r0);
                d->seek(std::chrono::nanoseconds((kRate + int64_t{487} * k) * 1'000'000'000LL / kRate));
                Stretched s;
                readStretched(*d, s, 4800, 4800);
                const size_t cut = s.left.size();
                d->setRate(r1);
                // The old run is read out first, up to about 6000 frames at 0.5.
                readStretched(*d, s, 4 * 4800, 4800);
                const bool reached = cut == 4800 && std::any_of(s.calls.begin(), s.calls.end(), [&](const Stretched::Call& c) {
                    return c.first.rate == r1;
                });
                if (!reached) continue;
                ++graded;
                const auto [left, right] = secondDifferences(s, cut - 220, s.left.size());
                worstLeft = std::max(worstLeft, left);
                worstRight = std::max(worstRight, right);
                if (left > kClickLeft || right > kClickRight) ++clicked;
            }
            std::printf("audio_playback_test: %s: %d cuts graded, %d clicked; largest second difference left %.4f, right "
                        "%.4f\n",
                        rule.c_str(), graded, clicked, worstLeft, worstRight);
            check(graded == 200, rule + ": every cut point reached the new rate", fmt("%.0f of 200", graded));
            check(graded == 200 && clicked == 0, rule + ": no cut point clicks",
                  fmt("%.0f of 200 clicked; left %.4f, ", clicked, worstLeft) + fmt("right %.4f", worstRight));
        }
    } catch (const std::exception& e) {
        check(false, std::string("cold head change rule: the decoder threw: ") + e.what());
    }
}

// GOAL: S15.631/S15.632 -- a play started near beeps_long's start does not click: starts every 1 ms over its
//       first 90 ms, at 0.5 and at 2, give 0.2 s of output under 0.02 (right) and 0.07 (left) of second difference.
//       False if the ColdHead crossfade is removed (a start at 0 clicked 0.030 at frame 334), or a start under
//       2048 frames in is led in by what little is held (starts 4-10 ms in clicked up to 0.095).
// WHY-THIS-SHAPE: every 1 ms, since which starts click depends on where atempo's switch falls against the tone.
//       S15.632's "no lead-in under 2048 frames" is graded by this, its consequence: from outside a short lead-in
//       is not otherwise visible (a led-in or cold atempo both copy fragments at 1, so a plain-copy test of the
//       first frames failed at 2 and could not tell the two apart).
// CONTROL: control_batch.sh 2026-10-15, reverted, phase decoderColdHeadStart,
//       renderer/audio/audio_decoder.cpp: `if (false && m_coldHead)` -> "FAIL: cold head start rule, rate 0.5: no
//       start clicks" (19 starts, right 0.0446); buildStretcher() leading in whenever anything is held
//       (`held > 0`) -> the same line (starts 4-10 ms, right 0.1109). Rate 2 stays green under both; the ColdHead
//       crossfade blending against frames 100 back (takeStretched(), control_batch.sh 2026-10-15) -> "FAIL: cold
//       head start rule, rate 2.0: no start clicks" and at 0.5.
//       Green: right at most 0.0029, left 0.0468.
// COVERAGE-ASSERT: 91 starts graded at each rate.
void runDecoderColdHeadStartChecks() {
    const std::string file = asset("wallpaper_beeps_long.mp4");
    try {
        for (const double rate : {0.5, 2.0}) {
            const std::string rule = fmt("cold head start rule, rate %.1f", rate);
            std::unique_ptr<AudioDecoder> d = AudioDecoder::open(file);
            int graded = 0;
            std::vector<int> clickedAt;
            double worstLeft = 0, worstRight = 0;
            for (int ms = 0; ms <= 90; ++ms) {
                d->setRate(rate);
                d->seek(std::chrono::milliseconds(ms));
                Stretched s;
                readStretched(*d, s, 9600, 4800);
                if (s.left.size() != 9600) continue;
                ++graded;
                const auto [left, right] = secondDifferences(s, 1, s.left.size());
                worstLeft = std::max(worstLeft, left);
                worstRight = std::max(worstRight, right);
                if (left > kClickLeft || right > kClickRight) clickedAt.push_back(ms);
            }
            std::string at;
            for (const int ms : clickedAt) at += fmt(" %.0f", ms);
            std::printf("audio_playback_test: %s: %d starts graded; largest second difference left %.4f, right %.4f; "
                        "clicked at ms:%s\n",
                        rule.c_str(), graded, worstLeft, worstRight, at.c_str());
            check(graded == 91 && clickedAt.empty(), rule + ": no start clicks",
                  fmt("%.0f of 91 graded, clicked at ms:", graded) + at);
        }
    } catch (const std::exception& e) {
        check(false, std::string("cold head start rule: the decoder threw: ") + e.what());
    }
}

// GOAL: S15.627 -- a non-looping play at 0.5 or 2, asked before it starts, sounds to the file's end: the tone of
//       beeps_long played from 11 s ends where its anchor puts the file's end (1 s of file / rate after the start),
//       within 15 ms at 0.5 and 4.5 ms at 2. False if the stretcher's end drops the file's last ms.
// WHY-THIS-SHAPE: the anchor, not the clock, is the reference, so a short sound cannot agree with marks it made.
//       Measured with the stretcher's end fed silence: about -2.5 ms at 0.5, +2.2 ms at 2; without it +7.0 ms at 2.
// CONTROL: control_run.sh 2026-10-15, reverted, phase engineRateToEnd: endStretcher() feeding no silence
//       (renderer/audio/audio_decoder.cpp) -> "FAIL: rate to end rule, rate 2.0: the tone ends at the file's end"
//       (+9.3 ms); at 0.5 that break gives -11.6 ms, inside the 15 ms bound. control_batch.sh 2026-10-15:
//       endStretcher() neither padding nor flushing atempo -> "FAIL: rate to end rule, rate 0.5: the tone ends at the
//       file's end" (-72.4 ms) and at 2 (-30.2 ms). Green: -2.5 and +3.3 ms.
void runEngineRateToEndChecks() {
    const char* kGoals = "S15.627 (a stretched play sounds to the file's end)";
    for (const double rate : {0.5, 2.0}) {
        const std::string rule = fmt("rate to end rule, rate %.1f", rate);
        SoundServer sound(kGoals);
        if (!sound.ready) return;
        resetCapture();
        Engine engine(kGoals);
        if (!engine.output) return;
        std::unique_ptr<AudioTrack> track = engine.output->track(asset("wallpaper_beeps_long.mp4"), "engine");
        track->setRate(rate);
        const int64_t t0 = probe::nowNs() + 400 * kMs;
        track->play(anchorAt(11000 * kMs, t0));
        const int64_t endAt = t0 + static_cast<int64_t>(1000.0 / rate) * kMs;
        sleepUntil(endAt + 600 * kMs);
        track->stop();
        std::this_thread::sleep_for(200ms);
        const std::optional<int64_t> last = lastTone(takeCapture());
        std::printf("audio_playback_test: %s: the tone ends %+.1f ms from the file's end by the anchor\n", rule.c_str(),
                    last ? (*last - endAt) / 1e6 : -1e9);
        const int64_t bound = rate < 1.0 ? 15 * kMs : 4500 * 1000;
        check(last && std::llabs(*last - endAt) <= bound, rule + ": the tone ends at the file's end",
              last ? fmt("%+.1f ms", (*last - endAt) / 1e6) : std::string("no tone"));
    }
}

// GOAL: S15.627 -- a looping play at 0.5 is heard for its full stretched length every pass, with no drift: beeps_allkey
//       (2 s, a beep every 0.5 s) looped from its anchor at 0.5 hears every beep of passes 1 to 3 within 25 ms of the
//       anchor's place for it (the 0.5 bound). False if the stretch is restarted or cut short at each wrap, which
//       brings each later pass ~6 ms earlier than the one before.
// WHY-THIS-SHAPE: graded against the anchor, so loss accumulates over passes instead of hiding in a re-anchor;
//       three passes put a per-loop loss of 6 ms past the bound.
// CONTROL: control_run.sh 2026-10-15, reverted, phase engineRateLoopEnd, renderer/audio/audio_output.cpp:
//       begin() not handing the loop to the decoder (setLoop(nullopt)) and fill() seeking back at the stream's end,
//       which rebuilds the stretcher at each wrap -> "FAIL: rate loop end rule: each beep is heard on
//       time" (passes 1-3 start -6.21, -12.46, -18.71 ms; -30.25 at worst). Green: -6.68 / -10.93 / -6.68, worst -15.64.
// COVERAGE-ASSERT: twelve beeps graded.
void runEngineRateLoopEndChecks() {
    const char* kGoals = "S15.627 (a looping play at 0.5 loses nothing at each loop)";
    const std::string rule = "rate loop end rule";
    SoundServer sound(kGoals);
    if (!sound.ready) return;
    resetCapture();
    Engine engine(kGoals);
    if (!engine.output) return;
    std::unique_ptr<AudioTrack> track = engine.output->track(asset("wallpaper_beeps_allkey.mp4"), "engine");
    track->setRate(0.5);
    const int64_t t0 = probe::nowNs() + 400 * kMs;
    AudioTrack::Anchor anchor = anchorAt(0, t0);
    anchor.loop = true;
    track->play(anchor);
    sleepUntil(t0 + 16300 * kMs);
    track->stop();
    std::this_thread::sleep_for(200ms);
    std::vector<std::pair<std::string, int64_t>> expected;
    for (int pass = 1; pass <= 3; ++pass) {
        for (int k = 0; k < 4; ++k) {
            expected.push_back({fmt("the beep at %.1f s of pass %.0f", k * 0.5, pass), t0 + (pass * 4000 + k * 1000) * kMs});
        }
    }
    const int graded = gradeBeepTimes(rule, takeCapture(), expected, -25.0, 25.0);
    check(graded == 12, rule + ": every beep of passes 1 to 3 was graded", fmt("%.0f of 12", graded));
}

// GOAL: S15.634 -- a stretched decoder loop (ramp from 8 s, setLoop(8 s), 1024-frame reads, 0.5 and 2) keeps every
//       read inside one pass of the file: no read's frames, placed from its reported first frame at the rate, lie
//       past 10 s (1 ms slack), and each wrap lands the output on the line `k` passes of 2 s / rate put it, within
//       15 ms of output, three wraps in. False if a read runs across the wrap, if a pass is counted from the wrong
//       frame, or if a wrap loses or repeats part of the file.
// WHY-THIS-SHAPE: the decoder alone, so the sound server's timing is not in the number. The length is graded as
//       where each wrap falls against k whole passes, not each pass on its own: atempo's own ~5 ms swing per pass
//       does not add up, a loss at the wrap does. A wrap is a read placed more than 1 s before the one before.
// CONTROL: control_run.sh 2026-10-15, reverted, phase decoderLoopWrap, renderer/audio/audio_decoder.cpp:
//       read() not stopping at a pass's end (`&& from[take] < end` deleted) -> "FAIL: decoder loop wrap rule, rate
//       0.5: no read runs past the end of its pass" (11.3 ms; 16.8 at 2); fetch()'s wrap seeking 20 ms past the
//       loop's start -> "... each wrap falls on the line of whole passes" (-71.2 ms at 0.5, -21.5 at 2); a wrap's
//       pass counted 2400 frames late -> "... no read runs past the end of its pass" (49.9 ms). Green: past 10 s
//       0.76 / 0.00 ms, wraps -4.1 / -1.5 ms at worst.
// COVERAGE-ASSERT: exactly three wraps seen per rate, over a hundred reads.
void runDecoderLoopWrapChecks() {
    try {
        std::unique_ptr<AudioDecoder> decoder = AudioDecoder::open(asset("wallpaper_ramp.mkv"));
        if (!decoder) {
            check(false, "decoder loop wrap rule: the ramp fixture's sound is found");
            return;
        }
        for (const double rate : {0.5, 2.0}) {
            const std::string rule = fmt("decoder loop wrap rule, rate %.1f", rate);
            decoder->setRate(rate);
            decoder->setLoop(8000ms);
            decoder->seek(8000ms);
            const double passFrames = 2.0 / rate * kRate;
            Stretched s;
            readStretched(*decoder, s, static_cast<size_t>(3.5 * passFrames), 1024);
            std::vector<size_t> wraps;
            double worstPast = 0;
            for (size_t i = 0; i < s.calls.size(); ++i) {
                const Stretched::Call& c = s.calls[i];
                const double from = c.first.file.count() / 1e9;
                const double to = from + static_cast<double>(c.got - 1) * c.first.rate / kRate;
                worstPast = std::max(worstPast, to - 10.0);
                if (i > 0 && from < s.calls[i - 1].first.file.count() / 1e9 - 1.0) wraps.push_back(c.at);
            }
            double worstDrift = 0;
            std::string at;
            for (size_t k = 0; k < wraps.size(); ++k) {
                const double off = (static_cast<double>(wraps[k]) - static_cast<double>(k + 1) * passFrames) / kRate * 1000.0;
                at += fmt(" %+.1f", off);
                if (std::fabs(off) > std::fabs(worstDrift)) worstDrift = off;
            }
            std::printf("audio_playback_test: %s: %zu reads, %zu wraps; past 10 s %.3f ms; wraps off the line by ms:%s\n",
                        rule.c_str(), s.calls.size(), wraps.size(), worstPast * 1000.0, at.c_str());
            check(wraps.size() == 3 && s.calls.size() > 100, rule + ": three wraps were read",
                  fmt("%.0f wraps in %.0f reads", static_cast<double>(wraps.size()), static_cast<double>(s.calls.size())));
            check(worstPast <= 0.001, rule + ": no read runs past the end of its pass",
                  fmt("%.3f ms past 10 s", worstPast * 1000.0));
            check(!wraps.empty() && std::fabs(worstDrift) <= 15.0, rule + ": each wrap falls on the line of whole passes",
                  fmt("worst %+.1f ms of output", worstDrift));
        }
    } catch (const std::exception& e) {
        check(false, std::string("decoder loop wrap rule: the decoder threw: ") + e.what());
    }
}

// Copies `source`'s video stream into a Matroska file at `target`, every packet presented at `fromNs` or later moved
// `shiftNs` later. Returns why it failed, empty on success.
std::string remuxShifted(const std::string& source, const std::string& target, int64_t fromNs, int64_t shiftNs) {
    using Input = std::unique_ptr<AVFormatContext, void (*)(AVFormatContext*)>;
    AVFormatContext* rawIn = nullptr;
    if (avformat_open_input(&rawIn, source.c_str(), nullptr, nullptr) < 0) return "cannot open " + source;
    Input in(rawIn, [](AVFormatContext* c) { avformat_close_input(&c); });
    if (avformat_find_stream_info(in.get(), nullptr) < 0) return "no stream information in " + source;
    const int v = av_find_best_stream(in.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (v < 0) return "no video stream in " + source;
    AVFormatContext* rawOut = nullptr;
    if (avformat_alloc_output_context2(&rawOut, nullptr, "matroska", target.c_str()) < 0 || rawOut == nullptr) {
        return "no Matroska muxer";
    }
    Input out(rawOut, [](AVFormatContext* c) {
        if (c->pb != nullptr) avio_closep(&c->pb);
        avformat_free_context(c);
    });
    AVStream* os = avformat_new_stream(out.get(), nullptr);
    if (os == nullptr || avcodec_parameters_copy(os->codecpar, in->streams[v]->codecpar) < 0) return "cannot copy the stream";
    os->codecpar->codec_tag = 0;
    os->time_base = in->streams[v]->time_base;
    if (avio_open(&out->pb, target.c_str(), AVIO_FLAG_WRITE) < 0) return "cannot write " + target;
    if (avformat_write_header(out.get(), nullptr) < 0) return "cannot write the header";
    std::unique_ptr<AVPacket, void (*)(AVPacket*)> pkt(av_packet_alloc(), [](AVPacket* p) { av_packet_free(&p); });
    const AVRational tb = in->streams[v]->time_base, ns{1, 1'000'000'000};
    const int64_t from = av_rescale_q(fromNs, ns, tb), shift = av_rescale_q(shiftNs, ns, tb);
    int written = 0;
    while (av_read_frame(in.get(), pkt.get()) >= 0) {
        if (pkt->stream_index == v) {
            if (pkt->pts != AV_NOPTS_VALUE && pkt->pts >= from) {
                pkt->pts += shift;
                if (pkt->dts != AV_NOPTS_VALUE) pkt->dts += shift;
            }
            pkt->stream_index = 0;
            pkt->pos = -1;
            av_packet_rescale_ts(pkt.get(), tb, os->time_base);
            if (av_interleaved_write_frame(out.get(), pkt.get()) < 0) return "cannot write a packet";
            ++written;
        }
        av_packet_unref(pkt.get());
    }
    if (av_write_trailer(out.get()) < 0) return "cannot write the trailer";
    return written > 0 ? std::string() : "no video packet copied";
}

// GOAL: S15.628 -- a timestamp jump of minutes met while skipping to key pictures, with many packets read since the
//       last shown picture, is a discontinuity: farkeys' pictures with those from 6 s on moved 120 s later, played
//       silent with every returned picture slowed 83 ms (so it skips to key pictures), show the 126 s key picture
//       less than 1 s after the one before. False if the gap ceiling grows 5 s for each packet read since (a 3 s
//       stretch at the key level reads ~90, so a ceiling of minutes), which holds the screen on one picture for 2 min.
// WHY-THIS-SHAPE: the file is made at run time from a generated fixture, so the jump lies inside a stretch the key
//       level skips. A hold never ended inside the watch is graded by how long it had lasted at the watch's end, so
//       a frozen screen fails here and does not merely leave the jump uncrossed.
// CONTROL: control_batch.sh 2026-10-15, reverted, phase deepJump: gapBetween's ceiling made kMaxPictureInterval * readSince, the old one
//       (backend/daemon/wallpaper_manager.cpp) -> "FAIL: deep jump rule: a jump of minutes met while skipping deep is
//       not waited out" (the 0.767 s picture still on screen after 10307.6 ms). Green: 83.9 ms.
// COVERAGE-ASSERT: the file was skipping to key pictures (a key-only level recorded in the watch).
void runDeepJumpChecks() {
    const char* kGoals = "S15.628 (a timestamp jump met while skipping deep is a discontinuity)";
    const std::string rule = "deep jump rule";
    wallpaperx::testing::ScratchDir scratch("deepjump");
    if (!scratch.ok()) {
        notVerified(kGoals, "no scratch folder for the fixture");
        return;
    }
    const std::string file = (scratch.path() / "wallpaper_deep_jump.mkv").string();
    if (const std::string why = remuxShifted(asset("wallpaper_beeps_farkeys.mp4"), file, 6000 * kMs, 120000 * kMs);
        !why.empty()) {
        notVerified(kGoals, "the fixture could not be made: " + why);
        return;
    }
    StallReset reset;
    Daemon d(smallConfig());
    resetCapture();
    d.play(file, std::nullopt);
    if (!d.waitForCommits(3, 15s)) {
        notVerified(kGoals, "the video drew no pictures within 15 s");
        return;
    }
    costFile(file);
    const int64_t costFrom = probe::nowNs();
    probe::g_costMs.store(83);
    std::this_thread::sleep_for(12000ms);
    const int64_t costEnd = probe::nowNs();
    probe::g_costMs.store(0);
    const std::vector<Shown> shown = shownPictures(commitsIn(d.takeCommits(), costFrom, costEnd), file);
    const std::vector<probe::LevelChange> levels = levelChangesOf(file, costFrom, costEnd);
    std::printf("audio_playback_test: %s: levels %s\n", rule.c_str(), describeLevels(levels, costFrom).c_str());
    const std::optional<bool> onDevice = decodedOnDevice(file);
    if (!onDevice) {
        notVerified(kGoals, "the file's packets went to both decoder paths, or to none, so its deepest level is not known");
        return;
    }
    if (*onDevice) {
        notVerified(kGoals, "decoded on the device, where no key level exists, so no deep skip reaches the jump");
        return;
    }
    std::optional<size_t> held;
    std::optional<double> crossedMs;
    for (size_t i = 0; i < shown.size(); ++i) {
        if (!shown[i].presentation) continue;
        if (held && *shown[i].presentation == *shown[*held].presentation) continue;
        if (!crossedMs && held && *shown[*held].presentation < 6000 * kMs && *shown[i].presentation >= 126000 * kMs) {
            crossedMs = (shown[i].commit.at - shown[*held].commit.at) / 1e6;
            std::printf("audio_playback_test: %s: %.3f s -> %.3f s of the file after %.1f ms on the wall\n", rule.c_str(),
                        *shown[*held].presentation / 1e9, *shown[i].presentation / 1e9, *crossedMs);
        }
        held = i;
    }
    double holdMs = -1;
    if (crossedMs) {
        holdMs = *crossedMs;
    } else if (held && *shown[*held].presentation < 6000 * kMs) {
        holdMs = (costEnd - shown[*held].commit.at) / 1e6;
        std::printf("audio_playback_test: %s: the picture at %.3f s still on screen after %.1f ms\n", rule.c_str(),
                    *shown[*held].presentation / 1e9, holdMs);
    }
    const bool keyOnly = std::any_of(levels.begin(), levels.end(),
                                     [](const probe::LevelChange& c) { return c.discard >= AVDISCARD_NONINTRA; });
    check(keyOnly, rule + ": the file was skipping to key pictures in the watch", describeLevels(levels, costFrom));
    check(holdMs >= 0 && holdMs < 1000.0, rule + ": a jump of minutes met while skipping deep is not waited out",
          holdMs < 0 ? std::string("the jump was never reached") : fmt("%.1f ms on the picture before the jump", holdMs));
}

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
                              "wallpaper_beeps_bframes.mp4", "wallpaper_beeps_rate240.mp4", "wallpaper_beeps_allkey.mp4",
                              "wallpaper_beeps_onekey.mp4", "wallpaper_beeps_farkeys.mp4", "wallpaper_jump.ts",
                              "wallpaper_beeps_allkey10.mp4"};
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
        {"decoderRate", &runDecoderRateChecks},
        {"decoderSwap", &runDecoderSwapChecks},
        {"decoderAlign", &runDecoderAlignChecks},
        {"decoderCutSeek", &runDecoderCutSeekChecks},
        {"decoderStretchEnd", &runDecoderStretchEndChecks},
        {"decoderStretchPastEnd", &runDecoderStretchPastEndChecks},
        {"decoderStretchEndPlace", &runDecoderStretchEndPlaceChecks},
        {"decoderLeadIn", &runDecoderLeadInChecks},
        {"decoderColdHeadChange", &runDecoderColdHeadChangeChecks},
        {"decoderColdHeadStart", &runDecoderColdHeadStartChecks},
        {"decoderReadError", &runDecoderReadErrorChecks},
        {"decoderSeekStart", &runDecoderSeekStartChecks},
        {"decoderPlaylist", &runDecoderPlaylistChecks},
        {"engineLoop", &runEngineLoopChecks},
        {"engineShortAudio", &runEngineShortAudioChecks},
        {"engineLateStart", &runEngineLateStartChecks},
        {"engineRamp", &runEngineRampChecks},
        {"engineRate", &runEngineRateChecks},
        {"engineRateLateStart", &runEngineRateLateStartChecks},
        {"engineRateCut", &runEngineRateCutChecks},
        {"engineRateJoin", &runEngineRateJoinChecks},
        {"engineRateUnderrunCut", &runEngineRateUnderrunCutChecks},
        {"engineRateWaiting", &runEngineRateWaitingChecks},
        {"engineRateWaitingTwice", &runEngineRateWaitingTwiceChecks},
        {"engineRateWaitingLoop", &runEngineRateWaitingLoopChecks},
        {"engineRateAtEnd", &runEngineRateAtEndChecks},
        {"engineRateAfterEnd", &runEngineRateAfterEndChecks},
        {"engineRateAtEndStop", &runEngineRateAtEndStopChecks},
        {"engineRateToEnd", &runEngineRateToEndChecks},
        {"engineRateLoopEnd", &runEngineRateLoopEndChecks},
        {"decoderLoopWrap", &runDecoderLoopWrapChecks},
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
        {"unpacedAfterHandOn", &runUnpacedAfterHandOnChecks},
        {"firstPictureKept", &runLoopFirstPictureChecks},
        {"leadLimit", &runLeadLimitChecks},
        {"noClock", &runNoClockChecks},
        {"handOverLead", &runHandOverLeadChecks},
        {"cleanCredit", &runCleanCreditChecks},
        {"lastReading", &runLastReadingChecks},
        {"dropMinimum", &runDropMinimumChecks},
        {"skip", &runSkipChecks},
        {"skipSettling", &runSkipSettlingChecks},
        {"lateAtLoop", &runLateAtLoopChecks},
        {"lateTimed", &runLateTimedChecks},
        {"hover", &runHoverChecks},
        {"speedLoad", &runSpeedLoadChecks},
        {"holdRunOut", &runHoldRunOutChecks},
        {"lateHandOn", &runLateHandOnChecks},
        {"behindAtLoop", &runBehindAtLoopChecks},
        {"longLastLoop", &runLongLastChecks},
        {"noClockPassStart", &runNoClockPassStartChecks},
        {"noClockPastCap", &runNoClockPastCapChecks},
        {"firstSlowDecodes", &runFirstSlowDecodesChecks},
        {"noClockLead", &runNoClockLeadChecks},
        {"loopRestartLoad", &runLoopRestartLoadChecks},
        {"climb", &runClimbChecks},
        {"climbHeavy", &runClimbHeavyChecks},
        {"oneKey", &runOneKeyChecks},
        {"loadSpike", &runLoadSpikeChecks},
        {"farKeys", &runFarKeysChecks},
        {"hwFallback", &runHardwareFallbackChecks},
        {"timestampJump", &runTimestampJumpChecks},
        {"deepJump", &runDeepJumpChecks},
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
