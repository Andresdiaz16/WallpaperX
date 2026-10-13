// SPDX-License-Identifier: GPL-2.0-or-later

#include "audio_output.h"

#include "audio_decoder.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/hook.h>

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <exception>

namespace wallpaperx {

namespace {

using SteadyClock = std::chrono::steady_clock;
using TimePoint = SteadyClock::time_point;

constexpr int kChannels = AudioDecoder::kChannels;
constexpr uint32_t kFrameBytes = kChannels * sizeof(float);
// Positions in the ring count frames for ever; the slot is the position modulo its size.
constexpr uint64_t kRingFrames = uint64_t{1} << 16;
// How far ahead of what is heard the decoder keeps a track: past any quantum a desktop runs, and
// short enough that a track re-anchored decodes little it throws away.
constexpr uint64_t kFillFrames = 16384;
// Below this the stream's callback asks for more.
constexpr uint64_t kLowWaterFrames = 8192;
constexpr uint64_t kDecodeChunkFrames = 4096;
// A play is begun this long before its `at`, not sooner: beginning it ends the sound before it,
// which is then decoded this far ahead and no further.
constexpr std::chrono::nanoseconds kBeginAhead{kFillFrames * 1'000'000'000 / AudioDecoder::kRate};
// Every change of gain -- volume, start, stop, a switch between anchors -- is a straight ramp this
// long from end to end, so none of them clicks.
constexpr float kRampFrames = 240.0f;
constexpr int64_t kRampNs = 5'000'000;
static_assert(kRampNs == 240LL * 1'000'000'000 / AudioDecoder::kRate);
// How soon a track whose stream failed, or a server that refused while its socket was there, is
// asked again.
constexpr std::chrono::seconds kRetryAfter{5};
// How late past its expected next cycle a stream's clock still counts as live: scheduling jitter,
// not a stall.
constexpr int64_t kClockSlackNs = 20'000'000;

int64_t steadyNs(TimePoint at) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(at.time_since_epoch()).count();
}

// Frames at the decoder's rate in `ns`, toward zero.
int64_t framesIn(int64_t ns) {
    static_assert(AudioDecoder::kRate == 48000);
    return ns * 48 / 1'000'000;
}

std::optional<TimePoint> earlier(std::optional<TimePoint> a, std::optional<TimePoint> b) {
    if (!a) return b;
    if (!b) return a;
    return std::min(*a, *b);
}

int pollTimeout(std::optional<TimePoint> at) {
    if (!at || *at == TimePoint::max()) return -1;
    const int64_t left = std::chrono::ceil<std::chrono::milliseconds>(*at - SteadyClock::now()).count();
    return static_cast<int>(std::clamp<int64_t>(left, 0, INT_MAX));
}

void pwInitOnce() {
    static std::once_flag once;
    std::call_once(once, [] { pw_init(nullptr, nullptr); });
}

// The socket libpipewire connects to: PIPEWIRE_REMOTE, a name or an absolute path, else pipewire-0,
// in PIPEWIRE_RUNTIME_DIR, else XDG_RUNTIME_DIR, else USERPROFILE. Nothing when no directory is set.
std::optional<std::pair<std::string, std::string>> serverSocket() {
    const auto variable = [](const char* name) -> std::string {
        const char* value = std::getenv(name);
        return value != nullptr ? value : "";
    };
    std::string remote = variable("PIPEWIRE_REMOTE");
    if (remote.empty()) remote = "pipewire-0";
    if (remote.front() == '/') {
        const size_t slash = remote.rfind('/');
        return std::pair{slash == 0 ? std::string("/") : remote.substr(0, slash), remote.substr(slash + 1)};
    }
    for (const char* name : {"PIPEWIRE_RUNTIME_DIR", "XDG_RUNTIME_DIR", "USERPROFILE"}) {
        if (std::string directory = variable(name); !directory.empty()) return std::pair{directory, remote};
    }
    return std::nullopt;
}

}  // namespace

// One play() or stop(), as the stream's callback acts on it. Positions are ring positions; times
// are steady-clock nanoseconds, the clock PipeWire reports its own times in.
struct Segment {
    uint64_t serial = 0;
    uint64_t ringStart = 0;
    // When it replaces the segment before it.
    int64_t switchAt = 0;
    // When the frame at ringStart is heard. Audible segments only.
    int64_t startsAt = 0;
    bool audible = false;
    // The file's time at ringStart, frames running on from it one per frame: audible segments that
    // do not loop, which are the ones with a clock.
    int64_t fileAt = 0;
    bool timed = false;
};

// Everything one track shares between its owner, the output's thread and its stream's realtime
// callback. Each group below is written by one of them.
struct AudioTrackState {
    AudioTrackState(std::string path, std::string owner, std::string shownAs, int wake)
        : file(std::move(path)), tag(std::move(owner)), name(std::move(shownAs)), wakeFd(wake),
          ring(new float[kRingFrames * kChannels]) {}

    const std::string file;
    const std::string tag;
    const std::string name;
    const int wakeFd;

    // Owner -> output thread. The mutex is held for a copy and nothing else.
    std::mutex requestMutex;
    uint64_t requestSerial = 0;
    std::optional<AudioTrack::Anchor> requestPlay;
    std::atomic<float> volume{1.0f};
    std::atomic<bool> closed{false};

    // Output thread -> owner: whether the file has sound to play, known once it has been opened.
    std::atomic<AudioTrack::Content> content{AudioTrack::Content::Unknown};
    // Output thread -> owner: the request last taken up.
    std::atomic<uint64_t> takenSerial{0};

    // Output thread -> callback. Frames are written at writePos and read at readPos. A segment
    // starts one position past the last one written before it. playingEnd is the first start
    // published after the segment the callback last said it plays, and newestStart the newest, so
    // the callback never reads a later segment's frames as the one it plays, however many were
    // published since -- even before it has their details, which come through segmentMutex, which
    // the callback only ever tries.
    std::unique_ptr<float[]> ring;
    std::atomic<uint64_t> writePos{0};
    std::atomic<uint64_t> readPos{0};
    std::atomic<uint64_t> newestStart{0};
    std::atomic<uint64_t> playingEnd{0};
    std::mutex segmentMutex;
    Segment newest;
    std::atomic<pw_stream*> callbackStream{nullptr};

    // Callback -> output thread: where the segment it plays starts, the silent segment it has
    // faded down to, and a stream error.
    std::atomic<uint64_t> playingStart{0};
    std::atomic<uint64_t> quietSerial{0};
    std::atomic<bool> streamFailed{false};

    // Callback -> owner: the clock, under a sequence count the callback writes without waiting and
    // the owner retries. Serial 0 is no clock. Also written by the output thread once no callback runs.
    std::atomic<uint32_t> clockSeq{0};
    std::atomic<uint64_t> clockSerial{0};
    std::atomic<int64_t> clockFile{0};
    std::atomic<int64_t> clockAt{0};
    std::atomic<int64_t> clockUntil{0};

    // Callback only, and only for the stream it belongs to: a new stream has played nothing.
    struct Playhead {
        Segment current;
        float gain = 0.0f;
        // The first position worth playing, the ones before it being late.
        uint64_t skipTo = 0;
        // The silence owed before any is played, the segment's start being still to come.
        uint64_t holdFrames = 0;
        // writePos when it last asked for more. It asks again only once something has been
        // written since, so a stalled or finished decoder is not woken on every callback.
        uint64_t askedAt = 0;
    };
    Playhead playhead;

    // Output thread only. The file is open while a stream plays it.
    std::unique_ptr<AudioDecoder> decoder;
    pw_stream* stream = nullptr;
    spa_hook listener{};
    bool active = false;
    // The request last acted on.
    uint64_t serviced = 0;
    uint64_t segmentStart = 0;
    bool filling = false;
    bool stopping = false;
    // A request left without a stream -- no server, or the server failed this track's -- acted on
    // again from then on, once the server may be asked.
    std::optional<TimePoint> retryAt;
    bool streamFailureLogged = false;
    // A looping segment's `from`, and the ring position of its last return there.
    std::optional<std::chrono::nanoseconds> loopFrom;
    std::optional<uint64_t> loopedAt;
};

struct AudioCoreListener {
    std::atomic<bool>* broken = nullptr;
    int wakeFd = -1;
    spa_hook hook{};
};

namespace {

void signalWake(int fd) noexcept {
    if (fd < 0) return;
    const uint64_t one = 1;
    // Nothing to do on failure: the counter is already non-zero, so the thread wakes anyway.
    [[maybe_unused]] const ssize_t written = ::write(fd, &one, sizeof one);
}

// When the first frame of a buffer queued now is heard, and when the server's cycle asking for it
// began -- nothing when the server has not timed the stream yet, which then counts as heard now.
struct BufferTiming {
    int64_t heardAt = 0;
    std::optional<int64_t> cycleAt;
};
BufferTiming timingOfNextBuffer(pw_stream* stream) {
    pw_time time{};
    if (pw_stream_get_time_n(stream, &time, sizeof time) < 0 || time.now == 0 || time.rate.denom == 0) {
        return {steadyNs(SteadyClock::now()), std::nullopt};
    }
    const int64_t delay = std::max<int64_t>(time.delay, 0) * 1'000'000'000 * time.rate.num / time.rate.denom;
    const auto queued = static_cast<int64_t>(time.buffered + time.queued);
    return {time.now + delay + queued * 1'000'000'000 / AudioDecoder::kRate, time.now};
}

void publishClock(AudioTrackState& t, uint64_t serial, int64_t file, int64_t at, int64_t until) noexcept {
    const uint32_t seq = t.clockSeq.load(std::memory_order_relaxed);
    t.clockSeq.store(seq + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    t.clockSerial.store(serial, std::memory_order_relaxed);
    t.clockFile.store(file, std::memory_order_relaxed);
    t.clockAt.store(at, std::memory_order_relaxed);
    t.clockUntil.store(until, std::memory_order_relaxed);
    t.clockSeq.store(seq + 2, std::memory_order_release);
}

// The realtime half: fills `out` with `frames` frames as `timing` places them, and publishes the
// clock of the segment it ends in. Touches nothing but atomics, the ring and the callback's own
// fields, and only ever tries the segment lock.
void render(AudioTrackState& t, float* out, uint32_t frames, const BufferTiming& timing) {
    const int64_t heardAt = timing.heardAt;
    // The first frame played from the ring this cycle in the segment it was in: serial, position, heard at.
    struct Sample {
        uint64_t serial;
        uint64_t position;
        int64_t at;
    };
    std::optional<Sample> sample;
    AudioTrackState::Playhead& p = t.playhead;
    Segment candidate = p.current;
    if (t.segmentMutex.try_lock()) {
        candidate = t.newest;
        t.segmentMutex.unlock();
    }
    // writePos before the starts: a newer segment's start is published before any of its frames.
    const uint64_t written = t.writePos.load(std::memory_order_acquire);
    const uint64_t newestStart = t.newestStart.load(std::memory_order_acquire);
    const uint64_t playingEnd = t.playingEnd.load(std::memory_order_acquire);
    const float volume = std::clamp(t.volume.load(std::memory_order_relaxed), 0.0f, 1.0f);
    uint64_t position = t.readPos.load(std::memory_order_relaxed);

    for (uint32_t i = 0; i < frames; ++i) {
        const int64_t frameAt = heardAt + static_cast<int64_t>(i) * 1'000'000'000 / AudioDecoder::kRate;
        // The sound before a switch fades out over the ramp ending at it, so the new one starts on time.
        const bool switchDue = candidate.serial != p.current.serial && frameAt + kRampNs >= candidate.switchAt;
        if (switchDue && (p.gain <= 0.0f || !p.current.audible)) {
            p.current = candidate;
            t.playingStart.store(candidate.ringStart, std::memory_order_release);
            p.gain = 0.0f;
            position = std::max(position, candidate.ringStart);
            p.skipTo = candidate.ringStart;
            p.holdFrames = 0;
            if (candidate.audible) {
                const int64_t late = framesIn(frameAt - candidate.startsAt);
                if (late > 0) p.skipTo += static_cast<uint64_t>(late);
                if (late < 0) p.holdFrames = static_cast<uint64_t>(-late);
            }
        }
        const bool fadingToSwitch = candidate.serial != p.current.serial && frameAt + kRampNs >= candidate.switchAt;
        // Nothing while it holds back to its start, so the ramp up begins with its first frame.
        const float target = p.current.audible && !fadingToSwitch && p.holdFrames == 0 ? volume : 0.0f;

        float left = 0.0f;
        float right = 0.0f;
        if (p.current.audible) {
            const uint64_t next = playingEnd > p.current.ringStart ? playingEnd : newestStart;
            const uint64_t end = next > p.current.ringStart ? std::min(written, next - 1) : written;
            if (p.holdFrames > 0) {
                --p.holdFrames;
            } else {
                if (position < p.skipTo) position = std::min(p.skipTo, end);
                if (position >= p.skipTo && position < end) {
                    if (!sample || sample->serial != p.current.serial) sample = Sample{p.current.serial, position, frameAt};
                    const float* frame = &t.ring[(position % kRingFrames) * kChannels];
                    left = frame[0];
                    right = frame[1];
                    ++position;
                } else {
                    // Late or ended: the frame this one stood for is owed, so the sound keeps to its time.
                    p.skipTo = std::max(p.skipTo, position) + 1;
                }
            }
        }
        p.gain += std::clamp(target - p.gain, -1.0f / kRampFrames, 1.0f / kRampFrames);
        out[i * kChannels] = left * p.gain;
        out[i * kChannels + 1] = right * p.gain;
    }
    t.readPos.store(position, std::memory_order_release);

    // Live until two more cycles of this length have gone by unasked, and a little more.
    if (timing.cycleAt && sample && sample->serial == p.current.serial && p.current.timed) {
        const int64_t cycleNs = static_cast<int64_t>(frames) * 1'000'000'000 / AudioDecoder::kRate;
        const auto played = static_cast<int64_t>(sample->position - p.current.ringStart);
        publishClock(t, sample->serial, p.current.fileAt + played * 1'000'000'000 / AudioDecoder::kRate, sample->at,
                     *timing.cycleAt + 2 * cycleNs + kClockSlackNs);
    } else if (t.clockSerial.load(std::memory_order_relaxed) != 0) {
        publishClock(t, 0, 0, 0, 0);
    }

    bool wake = false;
    if (p.current.audible && newestStart == p.current.ringStart) {
        // Asked again while low, not only on the way in: frames owed after a stall can take every
        // frame one fill decodes, leaving it as low as before.
        if (written - std::min(written, position) < kLowWaterFrames && written != p.askedAt) {
            p.askedAt = written;
            wake = true;
        }
    } else if (!p.current.audible && p.gain <= 0.0f &&
               t.quietSerial.load(std::memory_order_relaxed) != p.current.serial) {
        t.quietSerial.store(p.current.serial, std::memory_order_release);
        wake = true;
    }
    if (wake) signalWake(t.wakeFd);
}

void onProcess(void* data) {
    auto& t = *static_cast<AudioTrackState*>(data);
    pw_stream* stream = t.callbackStream.load(std::memory_order_acquire);
    if (stream == nullptr) return;
    pw_buffer* buffer = pw_stream_dequeue_buffer(stream);
    if (buffer == nullptr) return;
    spa_data& data0 = buffer->buffer->datas[0];
    if (data0.data != nullptr && data0.chunk != nullptr) {
        uint32_t frames = data0.maxsize / kFrameBytes;
        if (buffer->requested != 0) frames = std::min<uint32_t>(frames, static_cast<uint32_t>(buffer->requested));
        render(t, static_cast<float*>(data0.data), frames, timingOfNextBuffer(stream));
        data0.chunk->offset = 0;
        data0.chunk->stride = kFrameBytes;
        data0.chunk->size = frames * kFrameBytes;
        buffer->size = frames;
    }
    pw_stream_queue_buffer(stream, buffer);
}

void onStreamState(void* data, pw_stream_state, pw_stream_state state, const char*) {
    if (state != PW_STREAM_STATE_ERROR) return;
    auto& t = *static_cast<AudioTrackState*>(data);
    t.streamFailed.store(true, std::memory_order_release);
    signalWake(t.wakeFd);
}

const pw_stream_events& streamEvents() {
    static const pw_stream_events events = [] {
        pw_stream_events e{};
        e.version = PW_VERSION_STREAM_EVENTS;
        e.state_changed = &onStreamState;
        e.process = &onProcess;
        return e;
    }();
    return events;
}

// An error on the core itself is the connection: the server has gone or dropped this client.
void onCoreError(void* data, uint32_t id, int, int, const char*) {
    if (id != PW_ID_CORE) return;
    auto& listener = *static_cast<AudioCoreListener*>(data);
    listener.broken->store(true, std::memory_order_release);
    signalWake(listener.wakeFd);
}

}  // namespace

AudioTrack::AudioTrack(std::shared_ptr<AudioTrackState> state, AudioOutput& output)
    : m_state(std::move(state)), m_output(output) {}

AudioTrack::~AudioTrack() {
    m_state->closed.store(true, std::memory_order_release);
    m_output.wake();
}

void AudioTrack::play(const Anchor& anchor) noexcept {
    if (m_anchor == anchor) return;
    m_anchor = anchor;
    request(anchor);
}

void AudioTrack::stop() noexcept {
    if (!m_anchor) return;
    m_anchor.reset();
    request(std::nullopt);
}

void AudioTrack::setVolume(float gain) noexcept { m_state->volume.store(gain, std::memory_order_relaxed); }

AudioTrack::Content AudioTrack::content() const noexcept { return m_state->content.load(std::memory_order_acquire); }

std::optional<AudioTrack::Clock> AudioTrack::clock(std::chrono::steady_clock::time_point now) const noexcept {
    if (!m_anchor) return std::nullopt;
    const AudioTrackState& s = *m_state;
    // Bounded: a reading that keeps meeting a write in progress is taken as no clock this time.
    for (int attempt = 0; attempt < 4; ++attempt) {
        const uint32_t before = s.clockSeq.load(std::memory_order_acquire);
        if ((before & 1U) != 0) continue;
        const uint64_t serial = s.clockSerial.load(std::memory_order_relaxed);
        const int64_t file = s.clockFile.load(std::memory_order_relaxed);
        const int64_t at = s.clockAt.load(std::memory_order_relaxed);
        const int64_t until = s.clockUntil.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (s.clockSeq.load(std::memory_order_relaxed) != before) continue;
        // requestSerial is written only on this thread, so it is read here without its lock.
        if (serial == 0 || serial != s.requestSerial || steadyNs(now) > until) return std::nullopt;
        return Clock{std::chrono::nanoseconds(file), TimePoint(std::chrono::nanoseconds(at))};
    }
    return std::nullopt;
}

// requestSerial is written only on this thread, so it is read here without its lock.
uint64_t AudioTrack::request() const noexcept { return m_state->requestSerial; }

bool AudioTrack::taken() const noexcept {
    return m_state->takenSerial.load(std::memory_order_acquire) == m_state->requestSerial;
}

// noexcept is truthful: the anchor copies without allocating, and the lock is one this class never
// misuses, which is the only way std::mutex::lock throws.
void AudioTrack::request(std::optional<Anchor> play) noexcept {
    {
        std::lock_guard<std::mutex> lock(m_state->requestMutex);
        ++m_state->requestSerial;
        m_state->requestPlay = play;
    }
    m_output.wake();
}

AudioOutput::AudioOutput(std::function<void(const std::string&)> log, ContentScope scope,
                         std::function<void()> changed)
    : m_log(std::move(log)), m_scope(std::move(scope)), m_changed(std::move(changed)) {
    m_wakeFd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (m_wakeFd < 0) {
        m_stopping.store(true, std::memory_order_release);
        m_log(std::string("AudioOutput: no sound this run: cannot make its wake-up descriptor (") +
              std::strerror(errno) + ")");
        return;
    }
    // Before the thread: a socket made before the watch would otherwise go unseen.
    if (const auto socket = serverSocket()) {
        m_serverSocket = socket->first + "/" + socket->second;
        m_serverSocketName = socket->second;
        m_serverWatch = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (m_serverWatch >= 0 && ::inotify_add_watch(m_serverWatch, socket->first.c_str(), IN_CREATE | IN_MOVED_TO) < 0) {
            ::close(m_serverWatch);
            m_serverWatch = -1;
        }
    }
    try {
        m_thread = std::thread([this] { run(); });
    } catch (const std::exception& e) {
        m_stopping.store(true, std::memory_order_release);
        m_log(std::string("AudioOutput: no sound this run: cannot start its thread (") + e.what() + ")");
    }
}

AudioOutput::~AudioOutput() {
    m_stopping.store(true, std::memory_order_release);
    wake();
    if (m_thread.joinable()) m_thread.join();
    if (m_serverWatch >= 0) ::close(m_serverWatch);
    if (m_wakeFd >= 0) ::close(m_wakeFd);
}

void AudioOutput::wake() noexcept { signalWake(m_wakeFd); }

template <typename Work>
void AudioOutput::onContent(const AudioTrackState& t, Work&& work) {
    if (!m_scope) return work();
    // By reference: std::function holds a reference_wrapper without allocating.
    m_scope(t.tag, std::function<void()>(std::ref(work)));
}

void AudioOutput::closeDecoder(AudioTrackState& t) noexcept {
    if (!t.decoder) return;
    try {
        onContent(t, [&t]() noexcept { t.decoder.reset(); });
    } catch (...) {
        // Only a scope breaking its contract throws here; the file is closed below all the same.
    }
    t.decoder.reset();
}

void AudioOutput::closeAfterFailure(AudioTrackState& t) noexcept {
    closeDecoder(t);
    setContent(t, AudioTrack::Content::Failed);
}

void AudioOutput::setContent(AudioTrackState& t, AudioTrack::Content content) noexcept {
    if (t.content.exchange(content, std::memory_order_acq_rel) == content || !m_changed) return;
    try {
        m_changed();
    } catch (...) {
        // The contract forbids it; the owner then learns of the change at its next look.
    }
}

std::unique_ptr<AudioTrack> AudioOutput::track(const std::string& file, std::string tag, std::string name) {
    auto state = std::make_shared<AudioTrackState>(file, std::move(tag), std::move(name), m_wakeFd);
    {
        std::lock_guard<std::mutex> lock(m_tracksMutex);
        // With nothing to serve it, it stays silent.
        if (!m_stopping.load(std::memory_order_acquire)) {
            m_tracks.push_back(state);
        } else {
            state->content.store(AudioTrack::Content::Failed, std::memory_order_release);
        }
    }
    wake();
    return std::unique_ptr<AudioTrack>(new AudioTrack(std::move(state), *this));
}

bool AudioOutput::mayConnect(TimePoint now) const {
    return m_core != nullptr || !m_retryConnectAt || now >= *m_retryConnectAt;
}

void AudioOutput::run() {
    std::vector<std::shared_ptr<AudioTrackState>> tracks;
    std::optional<TimePoint> nextPass;
    for (;;) {
        pollfd events[2] = {{m_wakeFd, POLLIN, 0}, {m_serverWatch, POLLIN, 0}};
        if (::poll(events, m_serverWatch >= 0 ? 2 : 1, pollTimeout(nextPass)) < 0 && errno != EINTR) {
            m_log(std::string("AudioOutput: stopped: cannot wait for work (") + std::strerror(errno) + ")");
            break;
        }
        uint64_t count = 0;
        [[maybe_unused]] const ssize_t got = ::read(m_wakeFd, &count, sizeof count);
        if (m_serverWatch >= 0 && events[1].revents != 0) watchServer();
        if (m_stopping.load(std::memory_order_acquire)) break;

        const TimePoint now = SteadyClock::now();
        nextPass.reset();
        try {
            std::vector<std::shared_ptr<AudioTrackState>> closed;
            {
                std::lock_guard<std::mutex> lock(m_tracksMutex);
                const auto gone = std::stable_partition(m_tracks.begin(), m_tracks.end(), [](const auto& t) {
                    return !t->closed.load(std::memory_order_acquire);
                });
                closed.assign(std::make_move_iterator(gone), std::make_move_iterator(m_tracks.end()));
                m_tracks.erase(gone, m_tracks.end());
                tracks = m_tracks;
            }
            for (const auto& t : closed) destroyStream(*t);
            closed.clear();

            if (m_broken.exchange(false, std::memory_order_acq_rel) && m_core != nullptr) {
                loseOutput(tracks, "the sound server went away");
            }
            for (const auto& t : tracks) {
                try {
                    // One stream's fault, not the connection's: only this track waits.
                    if (t->streamFailed.exchange(false, std::memory_order_acq_rel) && t->stream != nullptr) {
                        dropStream(*t, now + kRetryAfter);
                        if (!t->streamFailureLogged) {
                            t->streamFailureLogged = true;
                            m_log("AudioOutput: the sound server failed a wallpaper's stream; it is asked for again "
                                  "every few seconds");
                        }
                    }
                    service(*t, now);
                } catch (const std::exception& e) {
                    // Not the file's doing: what its content throws is caught where it is worked on.
                    dropStream(*t, now + kRetryAfter);
                    m_log(std::string("AudioOutput: a wallpaper's sound stopped for now: ") + e.what());
                } catch (...) {
                    dropStream(*t, now + kRetryAfter);
                    m_log("AudioOutput: a wallpaper's sound stopped for now");
                }
            }
            for (const auto& t : tracks) nextPass = earlier(nextPass, nextPassAt(*t));
            m_passFailed = false;
        } catch (...) {
            if (!m_passFailed) {
                try {
                    m_log("AudioOutput: the wallpapers' sound could not be served; it is tried again in a few seconds");
                } catch (...) {
                }
            }
            m_passFailed = true;
        }
        if (m_passFailed) nextPass = earlier(nextPass, SteadyClock::now() + kRetryAfter);
    }
    {
        std::lock_guard<std::mutex> lock(m_tracksMutex);
        // A loop that broke on an error ends as the destructor's ask does: later tracks are silent.
        m_stopping.store(true, std::memory_order_release);
        tracks = std::move(m_tracks);
        m_tracks.clear();
    }
    for (const auto& t : tracks) {
        destroyStream(*t);
        setContent(*t, AudioTrack::Content::Failed);
    }
    shutdown();
}

void AudioOutput::watchServer() noexcept {
    alignas(inotify_event) char buffer[4096];
    for (;;) {
        const ssize_t got = ::read(m_serverWatch, buffer, sizeof buffer);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0 && errno == EAGAIN) return;
        if (got <= 0) {
            // A watch that cannot be read is given up for the timer, rather than polled in a spin.
            ::close(m_serverWatch);
            m_serverWatch = -1;
            m_retryConnectAt.reset();
            return;
        }
        for (const char* at = buffer; at < buffer + got;) {
            const auto* event = reinterpret_cast<const inotify_event*>(at);
            // An overflow may have hidden the socket; a directory gone ends the watch.
            if ((event->mask & (IN_Q_OVERFLOW | IN_IGNORED)) != 0 || (event->len > 0 && m_serverSocketName == event->name)) {
                m_retryConnectAt.reset();
            }
            if ((event->mask & IN_IGNORED) != 0) {
                ::close(m_serverWatch);
                m_serverWatch = -1;
                return;
            }
            at += sizeof(inotify_event) + event->len;
        }
    }
}

void AudioOutput::loseOutput(const std::vector<std::shared_ptr<AudioTrackState>>& tracks, const std::string& why) {
    const TimePoint now = SteadyClock::now();
    for (const auto& t : tracks) dropStream(*t, now);
    disconnect();
    holdOffServer();
    m_connectFailureLogged = true;
    m_log("AudioOutput: " + why + "; wallpapers are silent until it can be reached again");
}

void AudioOutput::service(AudioTrackState& t, TimePoint now) {
    uint64_t serial = 0;
    std::optional<AudioTrack::Anchor> play;
    {
        std::lock_guard<std::mutex> lock(t.requestMutex);
        serial = t.requestSerial;
        play = t.requestPlay;
    }
    // Acted on again as though new: its anchor still says where the sound should be by now.
    if (t.retryAt && now >= *t.retryAt && mayConnect(now)) {
        t.retryAt.reset();
        t.serviced = 0;
    }
    // A play far off waits while the sound before it is fed, so that sound lasts until it.
    const bool later = play && t.filling && now < play->at - kBeginAhead;
    if (serial != t.serviced && !later) {
        t.serviced = serial;
        t.takenSerial.store(serial, std::memory_order_release);
        t.retryAt.reset();
        if (play) {
            begin(t, serial, *play);
        } else {
            end(t, serial);
        }
    }
    if (t.filling) fill(t);
    if (t.stopping && t.quietSerial.load(std::memory_order_acquire) == t.serviced) {
        t.stopping = false;
        setActive(t, false);
    }
}

std::optional<AudioOutput::TimePoint> AudioOutput::nextPassAt(AudioTrackState& t) {
    std::optional<TimePoint> next;
    if (t.retryAt) {
        next = m_core == nullptr && m_retryConnectAt ? std::max(*t.retryAt, *m_retryConnectAt) : *t.retryAt;
    }
    // A play put off while the sound before it was fed, whether or not that sound has since ended:
    // the callback stops asking for passes once nothing more is written.
    std::lock_guard<std::mutex> lock(t.requestMutex);
    if (t.requestSerial != t.serviced && t.requestPlay) next = earlier(next, t.requestPlay->at - kBeginAhead);
    return next;
}

void AudioOutput::begin(AudioTrackState& t, uint64_t serial, const AudioTrack::Anchor& anchor) {
    int64_t at = steadyNs(anchor.at);
    // The sound before this one plays until `at`, on what it has decoded by now.
    if (t.filling && at > steadyNs(SteadyClock::now())) fill(t);
    t.stopping = false;
    t.filling = false;
    const AudioTrack::Content content = t.content.load(std::memory_order_relaxed);
    if (content == AudioTrack::Content::None || content == AudioTrack::Content::Failed) return;
    try {
        if (!t.decoder) {
            onContent(t, [&t] { t.decoder = AudioDecoder::open(t.file); });
            setContent(t, t.decoder ? AudioTrack::Content::Sound : AudioTrack::Content::None);
            if (!t.decoder) return;
        }
    } catch (const std::exception& e) {
        setContent(t, AudioTrack::Content::Failed);
        m_log(std::string("AudioOutput: no sound for a wallpaper: ") + e.what());
        return;
    }
    if (!ensureStream(t)) return;

    // A loop's anchor is never moved while it wraps round, so begun again later -- after its stream
    // or the server failed -- it starts over at `from` rather than seeking past the file's end.
    if (anchor.loop) at = std::max(at, steadyNs(SteadyClock::now()));
    // Decoding starts where the sound will be by now, not at `from`, which may be well past.
    const int64_t startsAt = std::max(at, steadyNs(SteadyClock::now()));
    const std::chrono::nanoseconds fileAt = anchor.from + std::chrono::nanoseconds(startsAt - at);
    try {
        onContent(t, [&] { t.decoder->seek(fileAt); });
    } catch (const std::exception& e) {
        closeAfterFailure(t);
        m_log(std::string("AudioOutput: no sound for a wallpaper: ") + e.what());
        return;
    }

    // The boundary first, so none of this segment's frames is ever read as the last one's; its
    // details after the first frames are in, so the callback never switches into a ring with
    // nothing to play yet.
    const uint64_t ringStart = newSegment(t);
    t.segmentStart = ringStart;
    t.loopFrom = anchor.loop ? std::optional(anchor.from) : std::nullopt;
    t.loopedAt.reset();
    t.filling = true;
    fill(t);
    {
        std::lock_guard<std::mutex> lock(t.segmentMutex);
        t.newest = Segment{serial, ringStart, at, startsAt, true, fileAt.count(), !anchor.loop};
    }
    if (!t.active) setActive(t, true);
}

void AudioOutput::end(AudioTrackState& t, uint64_t serial) {
    t.filling = false;
    const uint64_t ringStart = newSegment(t);
    {
        std::lock_guard<std::mutex> lock(t.segmentMutex);
        t.newest = Segment{serial, ringStart, steadyNs(SteadyClock::now()), 0, false};
    }
    // Stopped once the callback has faded out, which it says through quietSerial.
    t.stopping = t.active;
}

uint64_t AudioOutput::newSegment(AudioTrackState& t) {
    const uint64_t ringStart = t.writePos.load(std::memory_order_relaxed) + 1;
    // A playingStart read before the callback moved on only keeps an earlier end, which is still
    // the end of the segment it plays then.
    if (t.playingEnd.load(std::memory_order_relaxed) <= t.playingStart.load(std::memory_order_acquire)) {
        t.playingEnd.store(ringStart, std::memory_order_release);
    }
    t.newestStart.store(ringStart, std::memory_order_release);
    t.writePos.store(ringStart, std::memory_order_release);
    return ringStart;
}

void AudioOutput::fill(AudioTrackState& t) {
    while (t.filling) {
        const uint64_t written = t.writePos.load(std::memory_order_relaxed);
        const uint64_t read = t.readPos.load(std::memory_order_acquire);
        const uint64_t ahead = written - std::min(written, std::max(read, t.segmentStart));
        const uint64_t room = kRingFrames - (written - std::min(written, read));
        const uint64_t slot = written % kRingFrames;
        const uint64_t want = std::min({kFillFrames - std::min(ahead, kFillFrames), room, kDecodeChunkFrames,
                                        kRingFrames - slot});
        if (want == 0) return;
        size_t got = 0;
        bool looped = false;
        try {
            onContent(t, [&] {
                got = t.decoder->read(&t.ring[slot * kChannels], want);
                // A looping sound carries on from `from` with no gap, unless the last return there
                // found nothing to play before the end.
                if (got < want && t.loopFrom && t.loopedAt != written + got) {
                    t.decoder->seek(*t.loopFrom);
                    looped = true;
                }
            });
        } catch (const std::exception& e) {
            closeAfterFailure(t);
            t.filling = false;
            m_log(std::string("AudioOutput: a wallpaper's sound stopped: ") + e.what());
            return;
        }
        t.writePos.store(written + got, std::memory_order_release);
        if (looped) {
            t.loopedAt = written + got;
        } else if (got < want) {
            // The end of the stream: the callback plays silence after it until the next anchor.
            t.filling = false;
        }
    }
}

bool AudioOutput::connect() {
    if (m_core != nullptr) return true;
    if (!mayConnect(SteadyClock::now())) return false;
    pwInitOnce();
    std::string why;
    if (m_context == nullptr) {
        m_loop = pw_thread_loop_new("wallpaperx-audio", nullptr);
        if (m_loop != nullptr) {
            // No realtime module and no D-Bus support: the module asks RTKit or the desktop portal for
            // thread priority over D-Bus, which nothing else here uses. The cost is a stream callback at
            // ordinary priority, which a loaded machine can make late.
            pw_properties* properties = pw_properties_new("module.rt", "false", "support.dbus", "false",
                                                          PW_KEY_APP_NAME, "WallpaperX", nullptr);
            if (properties != nullptr) m_context = pw_context_new(pw_thread_loop_get_loop(m_loop), properties, 0);
        }
        if (m_context == nullptr || pw_thread_loop_start(m_loop) != 0) {
            why = std::strerror(errno);
            shutdown();
        }
    }
    if (m_context != nullptr) {
        // Before the loop is locked, so nothing allocated under the lock can throw out of it.
        m_coreListener = std::make_unique<AudioCoreListener>();
        m_coreListener->broken = &m_broken;
        m_coreListener->wakeFd = m_wakeFd;
        pw_thread_loop_lock(m_loop);
        m_core = pw_context_connect(m_context, nullptr, 0);
        if (m_core == nullptr) {
            why = std::strerror(errno);
        } else {
            static const pw_core_events events = [] {
                pw_core_events e{};
                e.version = PW_VERSION_CORE_EVENTS;
                e.error = &onCoreError;
                return e;
            }();
            pw_core_add_listener(m_core, &m_coreListener->hook, &events, m_coreListener.get());
        }
        pw_thread_loop_unlock(m_loop);
    }
    if (m_core == nullptr) {
        m_coreListener.reset();
        holdOffServer();
        if (!m_connectFailureLogged) {
            m_log("AudioOutput: no sound server to play to (" + why +
                  "); wallpapers are silent until one can be reached");
        }
        m_connectFailureLogged = true;
        return false;
    }
    m_broken.store(false, std::memory_order_release);
    if (m_connectFailureLogged) m_log("AudioOutput: the sound server can be reached again");
    m_retryConnectAt.reset();
    m_connectFailureLogged = false;
    return true;
}

void AudioOutput::holdOffServer() {
    struct stat info {};
    const bool awaited = m_context != nullptr && m_serverWatch >= 0 && ::stat(m_serverSocket.c_str(), &info) != 0 &&
                         errno == ENOENT;
    m_retryConnectAt = awaited ? TimePoint::max() : SteadyClock::now() + kRetryAfter;
}

void AudioOutput::disconnect() noexcept {
    if (m_core != nullptr) {
        pw_thread_loop_lock(m_loop);
        spa_hook_remove(&m_coreListener->hook);
        pw_core_disconnect(m_core);
        pw_thread_loop_unlock(m_loop);
        m_core = nullptr;
    }
    m_coreListener.reset();
}

void AudioOutput::shutdown() noexcept {
    disconnect();
    if (m_loop != nullptr) pw_thread_loop_stop(m_loop);
    if (m_context != nullptr) {
        pw_context_destroy(m_context);
        m_context = nullptr;
    }
    if (m_loop != nullptr) {
        pw_thread_loop_destroy(m_loop);
        m_loop = nullptr;
    }
}

bool AudioOutput::ensureStream(AudioTrackState& t) {
    if (t.stream != nullptr) return true;
    if (!connect()) {
        dropStream(t, SteadyClock::now());
        return false;
    }
    pw_thread_loop_lock(m_loop);
    pw_properties* properties =
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE, "Music",
                          PW_KEY_NODE_NAME, "wallpaperx", PW_KEY_NODE_DESCRIPTION, t.name.c_str(), PW_KEY_MEDIA_NAME,
                          t.name.c_str(), nullptr);
    // Takes the properties, even when it fails.
    pw_stream* stream = properties != nullptr ? pw_stream_new(m_core, t.name.c_str(), properties) : nullptr;
    int err = stream == nullptr ? -errno : 0;
    if (stream != nullptr) {
        t.listener = spa_hook{};
        pw_stream_add_listener(stream, &t.listener, &streamEvents(), &t);
        uint8_t podBuffer[1024];
        spa_pod_builder builder{};
        spa_pod_builder_init(&builder, podBuffer, sizeof podBuffer);
        spa_audio_info_raw info{};
        info.format = SPA_AUDIO_FORMAT_F32;
        info.rate = AudioDecoder::kRate;
        info.channels = kChannels;
        info.position[0] = SPA_AUDIO_CHANNEL_FL;
        info.position[1] = SPA_AUDIO_CHANNEL_FR;
        const spa_pod* params[1] = {spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info)};
        t.callbackStream.store(stream, std::memory_order_release);
        err = pw_stream_connect(stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                                static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                                             PW_STREAM_FLAG_RT_PROCESS | PW_STREAM_FLAG_INACTIVE),
                                params, 1);
        if (err < 0) {
            pw_stream_destroy(stream);
            t.callbackStream.store(nullptr, std::memory_order_release);
            stream = nullptr;
        }
    }
    pw_thread_loop_unlock(m_loop);
    if (stream == nullptr) {
        // The server's doing, not the file's.
        dropStream(t, SteadyClock::now() + kRetryAfter);
        if (!t.streamFailureLogged) {
            t.streamFailureLogged = true;
            m_log(std::string("AudioOutput: no stream could be made for a wallpaper's sound (") + std::strerror(-err) +
                  "); it is asked for again every few seconds");
        }
        return false;
    }
    t.stream = stream;
    t.active = false;
    return true;
}

void AudioOutput::setActive(AudioTrackState& t, bool active) {
    if (t.stream == nullptr || m_loop == nullptr) return;
    pw_thread_loop_lock(m_loop);
    pw_stream_set_active(t.stream, active);
    pw_thread_loop_unlock(m_loop);
    t.active = active;
}

void AudioOutput::destroyStream(AudioTrackState& t) noexcept {
    if (t.stream != nullptr && m_loop != nullptr) {
        pw_thread_loop_lock(m_loop);
        // Synchronous with the data thread: no callback runs once it returns.
        pw_stream_destroy(t.stream);
        pw_thread_loop_unlock(m_loop);
    }
    t.stream = nullptr;
    t.callbackStream.store(nullptr, std::memory_order_release);
    t.streamFailed.store(false, std::memory_order_release);
    // Its callback is gone. The next stream switches into whatever segment it is given, even one
    // begun again under the same request serial after the server returns.
    t.playhead = {};
    t.playingStart.store(0, std::memory_order_release);
    t.playingEnd.store(0, std::memory_order_release);
    publishClock(t, 0, 0, 0, 0);
    t.active = false;
    // The file goes with the stream; a play that makes a new stream opens it again.
    closeDecoder(t);
}

void AudioOutput::dropStream(AudioTrackState& t, TimePoint retryAt) noexcept {
    destroyStream(t);
    t.filling = false;
    t.stopping = false;
    // Whatever the request: one that ended the sound is acted on again harmlessly.
    t.retryAt = retryAt;
}

}  // namespace wallpaperx
