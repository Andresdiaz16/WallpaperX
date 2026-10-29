// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "../video/ffmpeg_file.h"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct AVCodecContext;
struct AVFilterContext;
struct AVFilterGraph;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;
struct SwrContext;

namespace wallpaperx {

// One file's audio as 48 kHz interleaved float stereo, read from any point on the file's timeline,
// stretched to play faster or slower with its pitch kept. It has a demuxer and a descriptor of its own,
// opened the way VideoDecoder opens the file, so the picture's seeks and scans never move it. Software
// decode only. One thread at a time.
class AudioDecoder {
public:
    static constexpr int kRate = 48000;
    static constexpr int kChannels = 2;
    // The rates setRate() takes: one atempo's, from 0.5 up.
    static constexpr double kMinRate = 0.5;
    static constexpr double kMaxRate = 2.0;
    // How much of the sound before a join the join lines up and fades against: a 2 ms crossfade, shifted up to
    // 1 ms either way.
    static constexpr size_t kJoinFrames = kRate * 3 / 1000;

    // The time `frames` frames at kRate take, to the nearest nanosecond: how far on in the file they lie.
    static std::chrono::nanoseconds spanOf(double frames) {
        return std::chrono::nanoseconds(std::llround(frames * 1e9 / kRate));
    }

    // Nothing when the file holds no audio stream this FFmpeg can decode. Throws
    // WallpaperContentError for a file that cannot be opened or read, and ResourceExhausted when
    // out of memory or descriptors.
    static std::unique_ptr<AudioDecoder> open(const std::string& file);
    ~AudioDecoder();

    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;

    // The next frame read() returns is the one at `at`, on the timeline DecodedFrame::presentationTime()
    // uses, so audio and pictures of one file line up. Silence stands in for any time before the stream's
    // first sample. Keeps the rate. Where read() stands at a setNext() pass from `at`, at the rate asked for,
    // it goes on into that pass through the same stretch, so the end before it joins it as a loop's wrap does.
    // Throws as open() does.
    void seek(std::chrono::nanoseconds at);
    // As seek(at), at the rate asked for, but joined onto `onward` -- what was to be heard from `at` on, at
    // `onwardRate` -- as a change of rate joins: lined up and crossfaded, so up to 1 ms of `onward` may be read
    // first, placed at `onwardRate`. Only its first kJoinFrames are used.
    void seek(std::chrono::nanoseconds at, std::vector<float> onward, double onwardRate);

    // Frames read from here on play `rate` of the file's time each frame's time, pitch kept: 1 is the
    // file as decoded, anything else is stretched. Clamped to [kMinRate, kMaxRate]. Taken up by the next
    // read() or seek(), so only the last of several asks between them counts. Nothing is sought: the stretch
    // so far is drained and read first, and a new one starts at the next frame, crossfaded in over 2 ms from
    // what the old one went on with. To line the two waveforms up, the old one may first play up to 1 ms on,
    // or the new one's first 1 ms be dropped; either is in the places read() reports. A change whose run has
    // handed out nothing yet is replaced, as if never asked: its rate is never heard.
    void setRate(double rate);
    // What the last setRate() asked for.
    double rate() const { return m_rate; }

    // With `from`, the stream's end is followed straight on by the file from `from`, again and again, through
    // the same stretch, so a looping stretched sound keeps its rate across the wrap. Nothing: the stream ends.
    // Holds across seeks; taken up the next time the end is reached. A wrap that finds nothing to play before
    // the end again ends the stream.
    void setLoop(std::optional<std::chrono::nanoseconds> from) { m_loop = from; }
    // Without a loop, the stream's end is decoded on into the file from `from`, through the same stretch, but
    // read() stops there as at the end until a seek to `from` lets it go on. Holds across seeks.
    void setNext(std::optional<std::chrono::nanoseconds> from) { m_next = from; }

    // Where a frame lies in the file, and the rate of the frames read with it.
    struct Position {
        std::chrono::nanoseconds file{};
        double rate = 1.0;
    };

    // Writes up to `frames` frames into `out`, fewer only at the end of the stream, where the rate changes
    // or where a loop wraps, so one call's frames share one rate and one pass; `first`, when given, receives
    // where the first of them lies. A stretched frame is placed by the frame the stretcher made it from, not by
    // counting, so its place holds however the stretcher aligns. Throws as open() does; a packet that does not
    // decode is skipped.
    size_t read(float* out, size_t frames, Position* first = nullptr);
    // Whether every frame of the stream has been read.
    bool ended() const;

private:
    explicit AudioDecoder(std::string file);
    void openContainer();
    // False when the file has no decodable audio stream.
    bool openCodec();
    void closeAll() noexcept;
    // Configures the resampler for `frame`'s format when it differs from the one in use.
    void matchResampler(const AVFrame& frame);
    // Decodes until m_pending or m_silence holds a frame to read, or the stream is exhausted.
    void decodeMore();
    // seek(at), always from a new stretch.
    void seekAnew(std::chrono::nanoseconds at);
    // Points the demuxer and decoder at `at`, for the next frame decoded to be the one there.
    void seekStream(std::chrono::nanoseconds at);
    // Appends up to `frames` newly decoded frames to m_history, going back to the loop's start at the stream's
    // end. Returns how many: none only at the end.
    size_t fetch(size_t frames);
    // Whether the stream's end, reached now, would wrap back to the loop's start or on to setNext()'s.
    bool wrapsAtEnd() const;
    uint64_t heldEnd() const { return m_historyFrom + m_history.size() / kChannels; }
    // read() before stretching: frames at the file's own rate, across a loop's wraps, fewer only at the end of
    // the stream.
    size_t pull(float* out, size_t frames);
    // Hands the last `frames` frames pull() returned back to it, to return again first.
    void unpull(size_t frames) { m_pulled -= frames; }
    // The next kJoinFrames frames pull() returns, fewer only at the stream's end, left for it to return again.
    std::vector<float> peekJoin();
    bool pulledAll() const;
    // The pass the frame pull() returned `frame` frames after the first lies in.
    size_t passOf(double frame) const;
    // The pass of the frame read() hands out next, where that is known without decoding more.
    std::optional<size_t> nextPass() const;

    // Makes m_applied the rate frames are produced at from the next frame pulled, after the old stretch
    // is drained.
    void applyRate();
    // Plans the change to m_rate from the frame pull() returns next, after the stretchedFrames() the run
    // still hands out, crossfading in from `onward`: what that run goes on with past them.
    void planChange(std::vector<float> onward);
    // Undoes the change not yet started, leaving pull() and the run as they were before it was planned but
    // for what of the run has been read since. Returns what the run goes on with past what it still hands out.
    std::vector<float> dropChange();
    // `leadIn`: fed first what pull() returned before, if a window of it is held, so the run starts on the rate. Not
    // where a run is crossfaded from the one before: cold, the stretcher's first fragment is the file itself, which
    // the fade meets in phase.
    void buildStretcher(bool leadIn);
    void freeStretcher() noexcept;
    // Feeds the stretcher one chunk, or its end, and takes what it gives. False once it has given all. Builds it
    // first if there is none, as buildStretcher(leadIn).
    bool stretchMore(bool leadIn);
    // Moves read() past the change of rate it has reached.
    void startRun();
    // Once every frame the run still hands out is made, places the read just done -- `written` frames from the
    // run's `firstRead`th, the last made from `lastFrom`, the run's drift `driftBefore` before it -- so the run's
    // line meets its last frame's real place by that frame. The drift alone would carry the line past it where
    // the stretcher's last fragment covers the file slower or faster than the rate.
    void glideToRunEnd(uint64_t firstRead, size_t written, double driftBefore, double lastFrom);
    // `in` holds the frames pull() returned from the `first`th on.
    void feedStretcher(const float* in, size_t frames, uint64_t first);
    void endStretcher();
    void takeStretched();
    // What the run read now hands out goes on with past the frame pull() returns next, for the next run to
    // fade in over; empty when the run has nothing past it.
    std::vector<float> runOnward();
    // The first frames a run at m_applied from the frame pull() returns next will hand out, `old` frames of
    // the run before still standing ahead of them. A stretched run's stretcher is built, as
    // buildStretcher(leadIn), and they stay in m_stretched; a run at 1's go back to pull().
    std::vector<float> runHead(size_t old, bool leadIn);
    // Blends the run's first frames, `frames` of them at `out`, in from what the run before went on with.
    void fadeIn(float* out, size_t frames) const;
    size_t stretchedFrames() const { return m_stretched.size() / kChannels - m_stretchedAt; }

    std::string m_file;
    ffmpeg_file::Reader m_reader;
    AVFormatContext* m_format = nullptr;
    AVCodecContext* m_codec = nullptr;
    AVPacket* m_packet = nullptr;
    AVFrame* m_frame = nullptr;
    SwrContext* m_resampler = nullptr;
    int m_stream = -1;
    // The input format the resampler was configured for: rate, sample format, channel count.
    int m_resamplerRate = 0;
    int m_resamplerFormat = -1;
    int m_resamplerChannels = 0;

    // Resampled frames not yet read, from m_pendingAt on, after m_silence frames of silence.
    std::vector<float> m_pending;
    size_t m_pendingAt = 0;
    uint64_t m_silence = 0;
    // Since the last seek: where the next frame read must start, until the first decoded frame says
    // how far from it the stream lies. Frames before it are dropped; a gap before it is silence.
    std::optional<std::chrono::nanoseconds> m_alignTo;
    uint64_t m_dropFrames = 0;
    // The last of the frames dropped for lying before m_alignTo, up to a stretch's lead.
    std::vector<float> m_before;
    bool m_sentEnd = false;
    bool m_drained = false;
    // Frames are counted along what is decoded since the last seek, whose point is a stretch's lead in. Each pass of a
    // loop starts at a count, the frame there lying at `at` in the file; the first starts at the seek's point. A pass
    // begun at the stream's end for setNext() Waits, read() handing out none of it, until a seek to it makes it a Wrap.
    enum class Begun { Seek, Wrap, Waits };
    struct Pass {
        uint64_t from = 0;
        std::chrono::nanoseconds at{};
        Begun begun = Begun::Seek;
    };
    std::vector<Pass> m_passes{Pass{}};
    std::optional<std::chrono::nanoseconds> m_loop;
    std::optional<std::chrono::nanoseconds> m_next;
    // The count of the next frame pull() returns.
    uint64_t m_pulled = 0;
    // The frames decoded, from count m_historyFrom on: those past m_pulled were handed back by unpull(), and those
    // before it can lead a new stretch in.
    std::vector<float> m_history;
    uint64_t m_historyFrom = 0;

    // abuffer, atempo, abuffersink; none while the rate is 1. Built when first fed.
    AVFilterGraph* m_graph = nullptr;
    AVFilterContext* m_graphIn = nullptr;
    AVFilterContext* m_graphOut = nullptr;
    AVFrame* m_stretchFrame = nullptr;
    // Set while the stretcher makes what it was fed before the run's first frame, which leads it in onto the
    // rate: the count of that frame. What it makes from before it is dropped, up to the first made from it on.
    std::optional<uint64_t> m_leadUntil;
    // A new stretcher first hands out what it is fed as it is, then switches to blending fragments in one frame,
    // which clicks. Set while that copy lasts: `next` is the frame it would copy next. Once it switches, `faded`
    // frames have been crossfaded from the copy's own continuation from `next` on, up to kCrossfadeFrames.
    struct ColdHead {
        uint64_t next = 0;
        size_t faded = 0;
    };
    std::optional<ColdHead> m_coldHead;
    // Once the stretcher has been fed its end: the count of the first frame past the sound fed. What it makes
    // from there on is the silence fed after it, and dropped.
    std::optional<uint64_t> m_stretchEnd;
    // A chunk pulled to be fed; stretched frames not yet read, from m_stretchedAt on; and the count of the frame
    // pull() returned that each was made from.
    std::vector<float> m_input;
    std::vector<float> m_stretched;
    std::vector<double> m_stretchedFrom;
    size_t m_stretchedAt = 0;
    double m_lastFrom = 0;

    // Asked for, and what frames are produced at now.
    double m_rate = 1.0;
    double m_applied = 1.0;
    // The frames read() hands out now: the first is the frame pull() returned at count `start`, and each after
    // it `rate` on, give or take `drift` -- how far the stretcher has placed
    // them from that, averaged over the last few thousand, and steered onto the run's last frame once all are
    // made (glideToRunEnd). `read` of them have been read. The first are crossfaded from `fadeFrom`: what the
    // run before would have gone on with, where it was cut; more of it than the fade takes, for a next change
    // to line up against should this run hand out nothing (at the end of the stream, or after a seek).
    struct Run {
        double start = 0;
        double rate = 1.0;
        uint64_t read = 0;
        double drift = 0;
        std::vector<float> fadeFrom;
    };
    Run m_run;
    // A change applied but not yet started: after `after` more frames of the run, at `rate`, from the frame
    // pull() returned at count `start`, fading in from `fadeFrom`. The last `played` of the run's frames before
    // it are the run's continuation, played on to line the junction up; `pulled` is the count pull() was at when
    // it was planned. Both are for undoing it.
    struct Change {
        uint64_t after = 0;
        double rate = 1.0;
        double start = 0;
        std::vector<float> fadeFrom;
        size_t played = 0;
        uint64_t pulled = 0;
    };
    std::optional<Change> m_change;
};

}  // namespace wallpaperx
