// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

struct pw_core;
struct pw_context;
struct pw_thread_loop;

namespace wallpaperx {

class AudioOutput;
struct AudioCoreListener;
struct AudioTrackState;

// One video's sound, as its owner wants it: playing from a point on the file's timeline, or not.
// Every call only records the wish and wakes the output's thread, so none of them waits on the
// file, the decoder or the sound server -- the owner calls them with the GPU lock held. One owning
// thread; destroy every track before the output that made it.
class AudioTrack {
public:
    ~AudioTrack();
    AudioTrack(const AudioTrack&) = delete;
    AudioTrack& operator=(const AudioTrack&) = delete;

    // The file's time `from` is heard at `at`, and the rest follows on from it at the track's rate. With
    // `loop`, the sound from `from` to its end then plays again and again, with nothing more asked of
    // the owner. With `joins`, `from` starts the file's next pass: should the sound before it run to the
    // file's last frame within a few tens of milliseconds of `at`, this one follows straight on from that
    // frame instead, with no fade on either side, and its clock says where that put it. `next` is where
    // the file's next pass starts, if known: the sound's end is decoded on into it, so a joining play
    // from exactly there goes on through the same stretch rather than starting one.
    struct Anchor {
        std::chrono::nanoseconds from{};
        std::chrono::steady_clock::time_point at;
        bool loop = false;
        bool joins = false;
        std::optional<std::chrono::nanoseconds> next;
        bool operator==(const Anchor&) const = default;
    };

    // Plays from `anchor`, replacing what this track plays at `at`, fading the sound before it out and
    // this one in unless they join. An `at` already past starts as far into the file as the sound would
    // have played by now; one still to come keeps the sound before it until then, however far off, which
    // is how a loop's last picture keeps its sound to its end. A repeat of the last anchor does nothing.
    void play(const Anchor& anchor) noexcept;

    // Fades out at once; the stream then stops and nothing is decoded until the next play().
    void stop() noexcept;

    // 0 to 1, a gain on the samples, reached over a few milliseconds rather than in a step.
    void setVolume(float gain) noexcept;

    // The file's time played each second, 0.5 to 2, pitch kept; 1 at first. What is decoded ahead and
    // not yet handed to the sound server, but for its next two cycles, is decoded again at the new rate,
    // joined on without a gap, so the change is heard within a few of the server's cycles and a rate
    // replaced before then never is; the clock follows it there. A play() begins at the rate set when it
    // is taken up.
    void setRate(double rate) noexcept;
    double rate() const noexcept;

    // Replaces what AudioOutput::track() was given as `name` and `tag`, for the stream playing now and the
    // file's work from here on; the sound plays on untouched. Throws std::bad_alloc.
    void rename(std::string name, std::string tag);

    // What the last play() asked for; nothing before the first and after stop().
    const std::optional<Anchor>& anchor() const { return m_anchor; }

    // Where the sound of the last play() actually is: the file's time `file` is heard at `at`, as the
    // sound server timed its latest cycle, and runs on at the sound card's own rate from there.
    struct Clock {
        std::chrono::nanoseconds file{};
        std::chrono::steady_clock::time_point at;
        // The file's time a second heard from there, until the rate next changes.
        double rate = 1.0;
        std::chrono::steady_clock::time_point heardAt(std::chrono::nanoseconds fileTime) const {
            return at + std::chrono::nanoseconds(std::llround(static_cast<double>((fileTime - file).count()) / rate));
        }
    };
    // Nothing unless that sound is being played out as of `now`: not before it starts or after its
    // last frame, not after stop(), not for a looping anchor, and not once the server has stopped
    // asking for it for a few cycles -- a stall, a loss, a failed track. Reads a few atomics.
    std::optional<Clock> clock(std::chrono::steady_clock::time_point now) const noexcept;

    // Names the latest play() or stop() that changed anything; each such call gives a new value.
    uint64_t request() const noexcept;

    // Whether the file has sound this output can play: Unknown until a play() has had it opened.
    // None and Failed are for good on this track. None is the file's answer: it holds no audio
    // stream this output can decode. Failed is anything that may not happen again on a new track:
    // the sound could not be opened, sought or read, memory or descriptors ran short, or the output
    // stopped serving tracks. Each change is told to the output's `changed`.
    enum class Content { Unknown, Sound, None, Failed };
    Content content() const noexcept;

private:
    friend class AudioOutput;
    AudioTrack(std::shared_ptr<AudioTrackState> state, AudioOutput& output);
    void request(std::optional<Anchor> play) noexcept;

    std::shared_ptr<AudioTrackState> m_state;
    AudioOutput& m_output;
    std::optional<Anchor> m_anchor;
};

// The daemon's sound: one thread that opens each track's file, decodes it a fraction of a second
// ahead of the sound server and starts and stops its stream, and one PipeWire connection, made
// when a track first has something to play. One stream per track, 48 kHz float stereo, fed from a
// ring the stream's realtime callback reads without a lock, and which publishes, each cycle, the
// file time it plays and when that is heard.
//
// Nothing here can stop a picture: a file with no audio or unreadable sound leaves that track
// silent; a sound server that is missing or goes away, or refuses a track's stream, leaves it silent
// until the server can be asked again -- each said once through `log`. A missing server is asked
// again when its socket appears, and every few seconds only when that cannot be watched for or the
// socket is there and refused; a refused stream is asked for again every few seconds.
class AudioOutput {
public:
    // Called on this output's thread around each piece of work on a track's file content --
    // opening, seeking, decoding, and closing it -- with the tag the track was made with, until the
    // output is destroyed. It must call `work` exactly once, on the same thread, let what `work`
    // throws through, and throw nothing of its own. How the owner names the file a fatal signal
    // inside `work` came from.
    using ContentScope = std::function<void(const std::string& tag, const std::function<void()>& work)>;

    // `log` receives one line per event worth a line, on this output's thread. Its text can carry
    // a file path as given. `changed` is called on this output's thread, never the stream's realtime
    // one, after a track's content() changes; it must only signal its owner and throw nothing, as
    // the owner may be calling into a track meanwhile under its own locks.
    AudioOutput(std::function<void(const std::string&)> log, ContentScope scope, std::function<void()> changed = {});
    ~AudioOutput();

    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    // A silent track for `file`, opened on this output's thread at its first play(). `tag` is what
    // the content scope is given for it; `name` is what the sound server shows its stream as. Any
    // thread.
    std::unique_ptr<AudioTrack> track(const std::string& file, std::string tag, std::string name = "WallpaperX");

private:
    using TimePoint = std::chrono::steady_clock::time_point;
    friend class AudioTrack;
    void run();
    void wake() noexcept;
    void service(AudioTrackState& track, TimePoint now);
    void begin(AudioTrackState& track, uint64_t serial, const AudioTrack::Anchor& anchor);
    void end(AudioTrackState& track, uint64_t serial);
    void fill(AudioTrackState& track);
    // Hands the decoder the rate the owner asks for, once the segment has room to record it. True when what was
    // queued was cut and the decoder sought back to the cut, so the segment must be filled again from there.
    bool followRate(AudioTrackState& track);
    // Stops `track`'s writing back at the first frame its stream's next cycles leave to be replaced, and returns
    // where: the segment's start if the stream has not switched into it yet. Nothing before the stream's first
    // cycle, or when too little is queued past the stream's next cycles.
    std::optional<uint64_t> cutQueued(AudioTrackState& track);
    // Records that the frames read to ring position `at` on lie in the file from `file` on at `rate`,
    // when that is not where the segment's last mark puts them.
    void markRate(AudioTrackState& track, uint64_t at, std::chrono::nanoseconds file, double rate);
    // Starts a segment past the last frame written, and returns where.
    uint64_t newSegment(AudioTrackState& track);
    // When `track` next needs a pass that no wake will ask for, if ever.
    std::optional<TimePoint> nextPassAt(AudioTrackState& track);
    bool ensureStream(AudioTrackState& track);
    void setActive(AudioTrackState& track, bool active);
    void destroyStream(AudioTrackState& track) noexcept;
    // `track` loses its stream and its file, and plays again from `retryAt` on.
    void dropStream(AudioTrackState& track, TimePoint retryAt) noexcept;
    bool mayConnect(TimePoint now) const;
    bool connect();
    // The connection only; the loop and the context stay for the next one.
    void disconnect() noexcept;
    void shutdown() noexcept;
    // Sets when the server may be asked again after it could not be reached.
    void holdOffServer();
    // Takes the events of the server's socket directory.
    void watchServer() noexcept;
    // Every stream goes with the connection; the tracks wait for the next one.
    void loseOutput(const std::vector<std::shared_ptr<AudioTrackState>>& tracks, const std::string& why);

    // Runs `work` in m_scope for `track`'s tag.
    template <typename Work>
    void onContent(const AudioTrackState& track, Work&& work);
    void closeDecoder(AudioTrackState& track) noexcept;
    void closeAfterFailure(AudioTrackState& track) noexcept;
    void setContent(AudioTrackState& track, AudioTrack::Content content) noexcept;

    std::function<void(const std::string&)> m_log;
    ContentScope m_scope;
    std::function<void()> m_changed;
    int m_wakeFd = -1;
    // An inotify descriptor on the directory the server's socket is made in, or -1.
    int m_serverWatch = -1;
    std::string m_serverSocket;
    std::string m_serverSocketName;

    std::mutex m_tracksMutex;
    std::vector<std::shared_ptr<AudioTrackState>> m_tracks;
    // Nothing serves tracks any more, or soon will not: the thread never started, ended, or is
    // asked to end. Set under m_tracksMutex wherever track() could be running.
    std::atomic<bool> m_stopping{false};

    // This output's thread only, apart from m_broken, which the connection's error event sets.
    pw_thread_loop* m_loop = nullptr;
    pw_context* m_context = nullptr;
    pw_core* m_core = nullptr;
    std::unique_ptr<AudioCoreListener> m_coreListener;
    std::atomic<bool> m_broken{false};
    // When the server may be asked again: nothing while it may be now, the end of time while that
    // waits on its socket appearing.
    std::optional<TimePoint> m_retryConnectAt;
    bool m_connectFailureLogged = false;
    // The last pass over the tracks was cut short; one more is owed.
    bool m_passFailed = false;

    // Last member: the thread reads every field above.
    std::thread m_thread;
};

}  // namespace wallpaperx
