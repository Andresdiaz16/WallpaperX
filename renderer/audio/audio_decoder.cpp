// SPDX-License-Identifier: GPL-2.0-or-later

#include "audio_decoder.h"

#include "../video/ffmpeg_file.h"
#include "../vulkan/gpu_fault.h"
#include "../wallpaper_file.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <numbers>

namespace wallpaperx {

namespace {

constexpr const char* kComponent = "AudioDecoder";
// Silence put before a stream that starts after the time asked for. Past this the timestamps are
// not believed: a gap that long is a broken file, not an intro.
constexpr uint64_t kMaxLeadingSilence = 60 * AudioDecoder::kRate;
constexpr std::chrono::nanoseconds kSeekPreroll = std::chrono::milliseconds(100);

[[noreturn]] void throwFailure(const std::string& reason, int err, const std::string& file) {
    ffmpeg_file::throwFileFailure(kComponent, reason, err, file);
}

[[noreturn]] void throwOutOfMemory() { ffmpeg_file::throwOutOfMemory(kComponent); }

// Throws for a stretcher call that failed: `err` below 0.
void checkStretch(int err, const std::string& file) {
    if (err == AVERROR(ENOMEM)) throwOutOfMemory();
    if (err < 0) throwFailure("cannot stretch the sound", err, file);
}

// Frames at kRate in `span`, rounded to the nearest.
int64_t framesIn(std::chrono::nanoseconds span) {
    return av_rescale(span.count(), AudioDecoder::kRate, 1'000'000'000);
}

bool stretches(double rate) { return rate != 1.0; }

// Two channels fed beside the sound and dropped once stretched. atempo aligns its fragments on the
// loudest channel; through silence every alignment scores nothing and it takes the earliest, which plays
// the sound after a silence up to half its window early. These are never silent, and too quiet to align on
// anything heard. The first also counts the frames fed above kGuide, so each frame out says which frame it
// was made from -- a blend of two fragments says their weighted mean -- once divided by the second, plain
// kGuide, which says what the blend's weights sum to: not quite one, and kGuide magnifies the difference.
constexpr float kGuide = 0x1p-10f;
// One count is kGuide's last place: below kGuideCounts every count keeps kGuide's exponent, so is exact.
constexpr float kGuideStep = 0x1p-33f;
constexpr int64_t kGuideCounts = int64_t{1} << 22;
static_assert(kGuide + (kGuideCounts - 1) * double{kGuideStep} < 2.0 * kGuide);
constexpr int kStretchChannels = 4;
// atempo's fragment at kRate: kRate / 24 rounded up to a power of two.
constexpr size_t kStretchWindow = 2048;
// At most this much of the sound before a new stretcher's run is fed to it first, what it makes from that dropped.
// Started cold, atempo plays its first fragment at 1 and takes the next several to steer back onto the rate; led
// in by this much, it is on the rate by the run's first frame. Within kSeekPreroll, so a seek decodes it anyway.
constexpr uint64_t kLeadFrames = 2 * kStretchWindow;
// What pull() keeps of what it returned before its latest call, for unpull(): more than dropChange() hands back,
// which is what one change of rate pulls to make its run's first frames.
constexpr uint64_t kKeepFrames = 8 * kStretchWindow;
// Where one stretch gives way to the next their waveforms need not meet, which clicks. 2 ms of raised cosine
// from where the old would have gone on leaves no click; a fade through silence instead would dip at every
// change, and so does this one where the two meet in opposite phase, unless lined up first (alignShift).
constexpr size_t kCrossfadeFrames = AudioDecoder::kRate * 2 / 1000;
// How far the junction may move to line the two waveforms up first: half a wave of anything above 500 Hz.
constexpr size_t kAlignFrames = AudioDecoder::kRate / 1000;
// What each side of a junction is read ahead for: the crossfade at any shift.
static_assert(AudioDecoder::kJoinFrames == kCrossfadeFrames + kAlignFrames);

// The new stretch's weight at frame k of a crossfade `span` frames long.
double fadeWeight(size_t k, size_t span) {
    return 0.5 - 0.5 * std::cos(std::numbers::pi * (static_cast<double>(k) + 0.5) / static_cast<double>(span));
}

// How far to move `head` against `onward` before crossfading them: positive, the old stretch plays that many
// frames of `onward` first; negative, the new one's first frames are dropped. The shift kept is the one whose
// crossfade loses the least: a crossfade's level depends on the shift only through the two waves' product, each
// frame's weighted as much as the fade mixes them there, so the largest weighted product wins. Ties keep the
// smaller shift; with too little of either to score, nothing moves.
ptrdiff_t alignShift(const std::vector<float>& onward, const std::vector<float>& head) {
    constexpr size_t kChannels = AudioDecoder::kChannels;
    const size_t onwardFrames = onward.size() / kChannels;
    const size_t headFrames = head.size() / kChannels;
    if (onwardFrames < kCrossfadeFrames || headFrames < kCrossfadeFrames) return 0;
    const auto mixed = [&](size_t played, size_t dropped) {
        double sum = 0;
        for (size_t k = 0; k < kCrossfadeFrames; ++k) {
            const double in = fadeWeight(k, kCrossfadeFrames);
            double product = 0;
            for (size_t c = 0; c < kChannels; ++c) {
                product += double{onward[(played + k) * kChannels + c]} * head[(dropped + k) * kChannels + c];
            }
            sum += in * (1.0 - in) * product;
        }
        return sum;
    };
    ptrdiff_t best = 0;
    double bestMixed = mixed(0, 0);
    for (size_t d = 1; d <= kAlignFrames; ++d) {
        if (d + kCrossfadeFrames <= onwardFrames) {
            if (const double m = mixed(d, 0); m > bestMixed) {
                bestMixed = m;
                best = static_cast<ptrdiff_t>(d);
            }
        }
        if (d + kCrossfadeFrames <= headFrames) {
            if (const double m = mixed(0, d); m > bestMixed) {
                bestMixed = m;
                best = -static_cast<ptrdiff_t>(d);
            }
        }
    }
    return best;
}

// A tempo as atempo's option parser reads it whatever the locale: a ratio of whole numbers.
std::string tempoArgument(double tempo) { return std::to_string(std::llround(tempo * 1e6)) + "/1000000"; }

}  // namespace

AudioDecoder::AudioDecoder(std::string file) : m_file(std::move(file)) {}

std::unique_ptr<AudioDecoder> AudioDecoder::open(const std::string& file) {
    std::unique_ptr<AudioDecoder> decoder(new AudioDecoder(file));
    decoder->openContainer();
    if (!decoder->openCodec()) return nullptr;
    return decoder;
}

AudioDecoder::~AudioDecoder() { closeAll(); }

void AudioDecoder::closeAll() noexcept {
    freeStretcher();
    av_frame_free(&m_stretchFrame);
    swr_free(&m_resampler);
    av_frame_free(&m_frame);
    av_packet_free(&m_packet);
    avcodec_free_context(&m_codec);
    avformat_close_input(&m_format);
    m_reader.close();
}

void AudioDecoder::openContainer() {
    m_reader.open(m_file, kComponent);
    ffmpeg_file::openContainer(m_format, m_reader.io(), m_file, kComponent);
}

bool AudioDecoder::openCodec() {
    const AVCodec* codec = nullptr;
    const int stream = av_find_best_stream(m_format, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (stream == AVERROR_STREAM_NOT_FOUND || stream == AVERROR_DECODER_NOT_FOUND) return false;
    if (stream < 0) throwFailure("cannot pick an audio stream", stream, m_file);
    m_stream = stream;
    // The rest are still read off the disk but dropped before any parsing.
    for (unsigned i = 0; i < m_format->nb_streams; ++i) {
        m_format->streams[i]->discard = static_cast<int>(i) == m_stream ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
    }

    m_codec = avcodec_alloc_context3(codec);
    if (m_codec == nullptr) throwOutOfMemory();
    const AVStream& audio = *m_format->streams[m_stream];
    if (const int err = avcodec_parameters_to_context(m_codec, audio.codecpar); err < 0) {
        throwFailure("cannot read the audio stream's parameters", err, m_file);
    }
    m_codec->pkt_timebase = audio.time_base;
    m_codec->thread_count = 1;
    if (const int err = avcodec_open2(m_codec, codec, nullptr); err < 0) {
        throwFailure("cannot open the audio decoder", err, m_file);
    }
    m_packet = av_packet_alloc();
    m_frame = av_frame_alloc();
    if (m_packet == nullptr || m_frame == nullptr) throwOutOfMemory();
    return true;
}

void AudioDecoder::matchResampler(const AVFrame& frame) {
    const int channels = frame.ch_layout.nb_channels;
    if (m_resampler != nullptr && frame.sample_rate == m_resamplerRate && frame.format == m_resamplerFormat &&
        channels == m_resamplerChannels) {
        return;
    }
    swr_free(&m_resampler);
    m_resamplerRate = 0;
    if (frame.sample_rate <= 0 || channels <= 0) {
        throw WallpaperContentError(
            describeRejection(kComponent, "the audio has no sample rate or no channels", m_file));
    }
    AVChannelLayout in{};
    const int copied = frame.ch_layout.order == AV_CHANNEL_ORDER_UNSPEC
                           ? (av_channel_layout_default(&in, channels), 0)
                           : av_channel_layout_copy(&in, &frame.ch_layout);
    if (copied < 0) throwFailure("cannot read the audio's channel layout", copied, m_file);
    const AVChannelLayout out = AV_CHANNEL_LAYOUT_STEREO;
    int err = swr_alloc_set_opts2(&m_resampler, &out, AV_SAMPLE_FMT_FLT, kRate, &in,
                                  static_cast<AVSampleFormat>(frame.format), frame.sample_rate, 0, nullptr);
    av_channel_layout_uninit(&in);
    if (err >= 0) err = swr_init(m_resampler);
    if (err < 0) {
        swr_free(&m_resampler);
        throwFailure("cannot convert the audio to 48 kHz stereo", err, m_file);
    }
    m_resamplerRate = frame.sample_rate;
    m_resamplerFormat = frame.format;
    m_resamplerChannels = channels;
}

void AudioDecoder::decodeMore() {
    constexpr int kMaxFailuresInARow = 16;
    int failedInARow = 0;
    const auto pendingFrames = [this] { return m_pending.size() / kChannels - m_pendingAt; };
    while (pendingFrames() == 0 && m_silence == 0 && !m_drained) {
        m_pending.clear();
        m_pendingAt = 0;
        int err = avcodec_receive_frame(m_codec, m_frame);
        if (err == AVERROR_EOF) {
            // What the resampler still holds is the stream's last few frames.
            if (m_resampler != nullptr) {
                const int tail = swr_get_out_samples(m_resampler, 0);
                if (tail > 0) {
                    m_pending.resize(static_cast<size_t>(tail) * kChannels);
                    auto* out = reinterpret_cast<uint8_t*>(m_pending.data());
                    const int got = swr_convert(m_resampler, &out, tail, nullptr, 0);
                    m_pending.resize(static_cast<size_t>(std::max(got, 0)) * kChannels);
                }
            }
            m_drained = true;
            break;
        }
        if (err == AVERROR(EAGAIN)) {
            if (m_sentEnd) {
                m_drained = true;
                break;
            }
            err = ffmpeg_file::readPacket(*m_format, m_packet);
            if (err < 0) {
                // A demuxer's own error at the end of the file is taken as the end, a failed read never.
                if (m_format->pb->error < 0 || (err != AVERROR_EOF && !avio_feof(m_format->pb))) {
                    throwFailure("cannot read the wallpaper's audio", err, m_file);
                }
                avcodec_send_packet(m_codec, nullptr);
                m_sentEnd = true;
                continue;
            }
            if (m_packet->stream_index == m_stream) err = avcodec_send_packet(m_codec, m_packet);
            av_packet_unref(m_packet);
            if (err == AVERROR(ENOMEM)) throwOutOfMemory();
            failedInARow = 0;
            continue;
        }
        if (err == AVERROR(ENOMEM)) throwOutOfMemory();
        // A packet that does not decode is skipped, as the video skips one. Errors with no packet
        // read between them are the decoder failing, not the file's packets.
        if (err < 0) {
            if (++failedInARow > kMaxFailuresInARow) throwFailure("cannot decode the wallpaper's audio", err, m_file);
            continue;
        }
        failedInARow = 0;

        matchResampler(*m_frame);
        if (m_alignTo) {
            const AVStream& audio = *m_format->streams[m_stream];
            if (const auto start = ffmpeg_file::inNanoseconds(m_frame->best_effort_timestamp, audio.time_base)) {
                const int64_t gap = framesIn(*start - *m_alignTo);
                if (gap < 0) m_dropFrames = static_cast<uint64_t>(-gap);
                if (gap > 0) m_silence = std::min(static_cast<uint64_t>(gap), kMaxLeadingSilence);
            }
            m_alignTo.reset();
        }
        const int room = swr_get_out_samples(m_resampler, m_frame->nb_samples);
        if (room < 0) throwFailure("cannot convert the audio", room, m_file);
        m_pending.resize(static_cast<size_t>(room) * kChannels);
        auto* out = reinterpret_cast<uint8_t*>(m_pending.data());
        const int got = swr_convert(m_resampler, &out, room, const_cast<const uint8_t**>(m_frame->extended_data),
                                    m_frame->nb_samples);
        av_frame_unref(m_frame);
        if (got < 0) throwFailure("cannot convert the audio", got, m_file);
        m_pending.resize(static_cast<size_t>(got) * kChannels);
        const uint64_t dropped = std::min<uint64_t>(m_dropFrames, pendingFrames());
        const auto from = m_pending.begin() + static_cast<ptrdiff_t>(m_pendingAt * kChannels);
        m_before.insert(m_before.end(), from, from + static_cast<ptrdiff_t>(dropped * kChannels));
        if (const size_t over = m_before.size() / kChannels; over > kLeadFrames) {
            const auto excess = static_cast<ptrdiff_t>((over - kLeadFrames) * kChannels);
            m_before.erase(m_before.begin(), m_before.begin() + excess);
        }
        m_pendingAt += dropped;
        m_dropFrames -= dropped;
    }
}

size_t AudioDecoder::fetch(size_t frames) {
    for (;;) {
        if (m_silence > 0) {
            const size_t take = static_cast<size_t>(std::min<uint64_t>(m_silence, frames));
            m_history.resize(m_history.size() + take * kChannels, 0.0f);
            m_silence -= take;
            return take;
        }
        if (const size_t available = m_pending.size() / kChannels - m_pendingAt; available > 0) {
            const size_t take = std::min(available, frames);
            const auto from = m_pending.begin() + static_cast<ptrdiff_t>(m_pendingAt * kChannels);
            m_history.insert(m_history.end(), from, from + static_cast<ptrdiff_t>(take * kChannels));
            m_pendingAt += take;
            return take;
        }
        if (!m_drained) {
            decodeMore();
            continue;
        }
        if (!wrapsAtEnd()) return 0;
        const uint64_t at = heldEnd();
        const std::chrono::nanoseconds from = m_loop ? *m_loop : *m_next;
        seekStream(from);
        m_passes.push_back(Pass{at, from, m_loop ? Begun::Wrap : Begun::Waits});
    }
}

bool AudioDecoder::wrapsAtEnd() const {
    return (m_loop || m_next) && !(m_passes.back().begun != Begun::Seek && m_passes.back().from == heldEnd());
}

size_t AudioDecoder::pull(float* out, size_t frames) {
    if (m_pulled - m_historyFrom > 2 * kKeepFrames) {
        const uint64_t drop = m_pulled - kKeepFrames - m_historyFrom;
        m_history.erase(m_history.begin(), m_history.begin() + static_cast<ptrdiff_t>(drop * kChannels));
        m_historyFrom += drop;
    }
    size_t written = 0;
    while (written < frames) {
        const uint64_t at = m_pulled + written;
        if (at == heldEnd() && fetch(frames - written) == 0) break;
        const size_t take = static_cast<size_t>(std::min<uint64_t>(heldEnd() - at, frames - written));
        std::memcpy(out + written * kChannels, m_history.data() + (at - m_historyFrom) * kChannels,
                    take * kChannels * sizeof(float));
        written += take;
    }
    m_pulled += written;
    return written;
}

size_t AudioDecoder::passOf(double frame) const {
    size_t pass = 0;
    while (pass + 1 < m_passes.size() && static_cast<double>(m_passes[pass + 1].from) <= frame) ++pass;
    return pass;
}

std::optional<size_t> AudioDecoder::nextPass() const {
    if (stretchedFrames() > 0) return passOf(m_stretchedFrom[m_stretchedAt]);
    if (!stretches(m_applied)) return passOf(static_cast<double>(m_pulled));
    return std::nullopt;
}

void AudioDecoder::seek(std::chrono::nanoseconds at) {
    // read() never hands out a Waits pass, so standing in one is standing at its start.
    if (const std::optional<size_t> next = nextPass();
        next && m_passes[*next].begun == Begun::Waits && m_passes[*next].at == at && m_rate == m_applied && !m_change) {
        m_passes[*next].begun = Begun::Wrap;
        return;
    }
    seekAnew(at);
}

void AudioDecoder::seekAnew(std::chrono::nanoseconds at) {
    freeStretcher();
    m_stretched.clear();
    m_stretchedFrom.clear();
    m_stretchedAt = 0;
    m_change.reset();
    m_applied = m_rate;
    seekStream(at);
    // Decoded up to `at` now: what lies before it leads a stretch in from there. Nothing stands in for sound before
    // the stream's start: led in by silence, atempo can settle whole periods of a tone off the rate.
    decodeMore();
    m_history = std::move(m_before);
    m_before.clear();
    m_historyFrom = kLeadFrames - m_history.size() / kChannels;
    m_pulled = kLeadFrames;
    m_passes.assign(1, Pass{kLeadFrames, at});
    m_run = Run{static_cast<double>(kLeadFrames), m_rate, 0, 0};
}

void AudioDecoder::seekStream(std::chrono::nanoseconds at) {
    const AVStream& audio = *m_format->streams[m_stream];
    // Decoding starts this far before `at` and drops what lies before it: a frame decoded without
    // the one before it is wrong (AAC overlaps each with the last), and Opus says how many it needs.
    // From a stream's start that also takes in the packet whose frames the encoder's delay discards.
    std::chrono::nanoseconds preroll = kSeekPreroll;
    if (audio.codecpar->seek_preroll > 0 && audio.codecpar->sample_rate > 0) {
        preroll += std::chrono::nanoseconds(
            av_rescale(audio.codecpar->seek_preroll, 1'000'000'000, audio.codecpar->sample_rate));
    }
    const AVRational nanoseconds{1, 1'000'000'000};
    const int64_t target = av_rescale_q((at - preroll).count(), nanoseconds, audio.time_base);
    // Anywhere up to `at` will do, or up to the stream's start for an `at` before it: a demuxer with
    // nothing indexed at or before the preroll -- the start of an AVI or FLV file -- fails a seek
    // bounded by it.
    const int64_t start = audio.start_time != AV_NOPTS_VALUE ? audio.start_time : 0;
    const int64_t latest = std::max({target, start, av_rescale_q(at.count(), nanoseconds, audio.time_base)});
    if (const int err = ffmpeg_file::seekFile(*m_format, m_stream, INT64_MIN, target, latest, 0); err < 0) {
        throwFailure("cannot seek in the wallpaper's audio", err, m_file);
    }
    avcodec_flush_buffers(m_codec);
    m_before.clear();
    // Rebuilt at the next frame, without the delay line holding frames from before the seek.
    swr_free(&m_resampler);
    m_resamplerRate = 0;
    m_pending.clear();
    m_pendingAt = 0;
    m_silence = 0;
    m_dropFrames = 0;
    m_alignTo = at;
    m_sentEnd = false;
    m_drained = false;
}

void AudioDecoder::seek(std::chrono::nanoseconds at, std::vector<float> onward, double onwardRate) {
    seekAnew(at);
    if (onward.empty()) return;
    onward.resize(std::min(onward.size(), kJoinFrames * kChannels));
    // The run before: what is played on from `onward` is placed at its rate.
    m_run.rate = onwardRate;
    planChange(std::move(onward));
}

bool AudioDecoder::pulledAll() const {
    return m_pulled == heldEnd() && m_drained && m_silence == 0 && m_pending.size() / kChannels == m_pendingAt &&
           !wrapsAtEnd();
}

bool AudioDecoder::ended() const {
    return pulledAll() && stretchedFrames() == 0 && (m_graph == nullptr || m_stretchEnd);
}

void AudioDecoder::setRate(double rate) { m_rate = std::clamp(rate, kMinRate, kMaxRate); }

void AudioDecoder::applyRate() {
    // The old stretch is drained and cut at the next frame, so the new run starts there exactly, but for any
    // frames alignShift drops. Retuning atempo where it stands instead steps the place of what it gives by
    // over 10 ms.
    std::vector<float> onward = runOnward();
    freeStretcher();
    // A run that hands out nothing leaves the fade from the one before it standing.
    if (m_run.read == 0 && stretchedFrames() == 0) onward = std::move(m_run.fadeFrom);
    planChange(std::move(onward));
}

std::vector<float> AudioDecoder::dropChange() {
    Change change = std::move(*m_change);
    m_change.reset();
    freeStretcher();
    // The run's frames end where the change starts; of the continuation it played on into, what is not yet
    // read goes back to the front of the rest.
    const size_t end = m_stretchedAt + static_cast<size_t>(change.after);
    const size_t cut = end - std::min<size_t>(change.played, static_cast<size_t>(change.after));
    change.fadeFrom.insert(change.fadeFrom.begin(), m_stretched.begin() + static_cast<ptrdiff_t>(cut * kChannels),
                           m_stretched.begin() + static_cast<ptrdiff_t>(end * kChannels));
    m_stretched.resize(cut * kChannels);
    m_stretchedFrom.resize(cut);
    unpull(static_cast<size_t>(m_pulled - change.pulled));
    m_applied = m_run.rate;
    return std::move(change.fadeFrom);
}

void AudioDecoder::planChange(std::vector<float> onward) {
    const size_t old = stretchedFrames();
    m_applied = m_rate;
    const uint64_t pulled = m_pulled;
    double start = static_cast<double>(m_pulled);
    const ptrdiff_t shift = alignShift(onward, runHead(old, onward.empty()));
    const auto boundary = static_cast<ptrdiff_t>(m_stretchedAt + old);
    size_t played = 0;
    if (shift > 0) {
        // The old run plays on into what it went on with, placed as it places its own frames.
        played = static_cast<size_t>(shift);
        const auto end = onward.begin() + static_cast<ptrdiff_t>(played * kChannels);
        m_stretched.insert(m_stretched.begin() + boundary * kChannels, onward.begin(), end);
        onward.erase(onward.begin(), end);
        std::vector<double> from(played);
        for (size_t i = 0; i < played; ++i) {
            from[i] = m_run.start + static_cast<double>(m_run.read + old + i) * m_run.rate + m_run.drift;
        }
        m_stretchedFrom.insert(m_stretchedFrom.begin() + boundary, from.begin(), from.end());
    } else if (shift < 0) {
        const auto dropped = static_cast<size_t>(-shift);
        if (stretches(m_applied)) {
            m_stretched.erase(m_stretched.begin() + boundary * kChannels,
                              m_stretched.begin() + (boundary - shift) * kChannels);
            m_stretchedFrom.erase(m_stretchedFrom.begin() + boundary, m_stretchedFrom.begin() + boundary - shift);
            start += static_cast<double>(dropped) * m_applied;
        } else {
            std::vector<float> skipped(dropped * kChannels);
            pull(skipped.data(), dropped);
            start = static_cast<double>(m_pulled);
        }
    }
    m_change = Change{old + played, m_rate, start, std::move(onward), played, pulled};
}

std::vector<float> AudioDecoder::peekJoin() {
    std::vector<float> join(kJoinFrames * kChannels);
    const size_t got = pull(join.data(), kJoinFrames);
    join.resize(got * kChannels);
    unpull(got);
    return join;
}

std::vector<float> AudioDecoder::runHead(size_t old, bool leadIn) {
    if (!stretches(m_applied)) return peekJoin();
    while (stretchedFrames() < old + kJoinFrames && stretchMore(leadIn)) {
    }
    const size_t got = std::min(stretchedFrames() - old, kJoinFrames);
    const auto from = m_stretched.begin() + static_cast<ptrdiff_t>((m_stretchedAt + old) * kChannels);
    return {from, from + static_cast<ptrdiff_t>(got * kChannels)};
}

std::vector<float> AudioDecoder::runOnward() {
    if (!stretches(m_applied)) return peekJoin();
    const uint64_t next = m_pulled;
    std::vector<float> onward;
    if (m_graph == nullptr || m_stretchEnd) return onward;
    // Fed on past `next` far enough to give kJoinFrames wherever within its window the stretcher aligns them, then
    // handed back to pull() for the next run.
    const size_t ahead = static_cast<size_t>(std::ceil(kJoinFrames * m_applied)) + kStretchWindow;
    std::vector<float> fed(ahead * kChannels);
    const size_t got = pull(fed.data(), ahead);
    feedStretcher(fed.data(), got, next);
    endStretcher();
    takeStretched();
    unpull(got);
    size_t cut = m_stretchedAt;
    while (cut < m_stretchedFrom.size() && m_stretchedFrom[cut] < static_cast<double>(next)) ++cut;
    const size_t kept = std::min(m_stretchedFrom.size() - cut, kJoinFrames);
    const auto from = m_stretched.begin() + static_cast<ptrdiff_t>(cut * kChannels);
    onward.assign(from, from + static_cast<ptrdiff_t>(kept * kChannels));
    m_stretched.resize(cut * kChannels);
    m_stretchedFrom.resize(cut);
    return onward;
}

void AudioDecoder::fadeIn(float* out, size_t frames) const {
    const size_t span = std::min(m_run.fadeFrom.size() / kChannels, kCrossfadeFrames);
    for (size_t i = 0; i < frames && m_run.read + i < span; ++i) {
        const size_t k = static_cast<size_t>(m_run.read) + i;
        const double in = fadeWeight(k, span);
        for (size_t c = 0; c < kChannels; ++c) {
            const float old = m_run.fadeFrom[k * kChannels + c];
            out[i * kChannels + c] = static_cast<float>(old + (out[i * kChannels + c] - old) * in);
        }
    }
}

void AudioDecoder::startRun() {
    Change change = std::move(*m_change);
    m_change.reset();
    m_run = Run{change.start, change.rate, 0, 0, std::move(change.fadeFrom)};
}

size_t AudioDecoder::read(float* out, size_t frames, Position* first) {
    // How many frames the drift is averaged over: a few of the stretcher's fragments, so it follows the
    // stretch and not each fragment.
    constexpr double kDriftFrames = 8192;
    if (m_rate != m_applied) {
        if (m_change) {
            planChange(dropChange());
        } else {
            applyRate();
        }
    }
    size_t written = 0;
    // Taken at the first frame: a change of rate reached before it starts a new run.
    std::optional<uint64_t> firstRead;
    double driftBefore = 0;
    // The frame the last stretched frame handed out was made from.
    std::optional<double> lastFrom;
    // The pass the first frame lies in, which the rest must too, and the count its next starts at.
    std::optional<size_t> pass;
    const auto passEnd = [this, &pass] {
        return *pass + 1 < m_passes.size() ? static_cast<double>(m_passes[*pass + 1].from) : HUGE_VAL;
    };
    while (written < frames) {
        if (m_change && m_change->after == 0) {
            if (written > 0) break;
            startRun();
            continue;
        }
        const size_t limit = std::min<uint64_t>(frames - written, m_change ? m_change->after : UINT64_MAX);
        if (!firstRead) {
            firstRead = m_run.read;
            driftBefore = m_run.drift;
        }
        size_t take = 0;
        if (const size_t available = stretchedFrames(); available > 0) {
            const double* from = m_stretchedFrom.data() + m_stretchedAt;
            if (!pass) pass = passOf(from[0]);
            if (m_passes[*pass].begun == Begun::Waits) break;
            const double end = passEnd();
            while (take < std::min(available, limit) && from[take] < end) ++take;
            if (take == 0) break;
            std::memcpy(out + written * kChannels, m_stretched.data() + m_stretchedAt * kChannels,
                        take * kChannels * sizeof(float));
            double off = 0;
            for (size_t i = 0; i < take; ++i) {
                const double planned = m_run.start + static_cast<double>(m_run.read + i) * m_run.rate;
                off += m_stretchedFrom[m_stretchedAt + i] - planned;
            }
            const double weight = std::min(1.0, static_cast<double>(take) / kDriftFrames);
            m_run.drift += (off / static_cast<double>(take) - m_run.drift) * weight;
            lastFrom = m_stretchedFrom[m_stretchedAt + take - 1];
            m_stretchedAt += take;
        } else if (stretches(m_applied)) {
            if (!stretchMore(true)) break;
            continue;
        } else {
            const uint64_t at = m_pulled;
            take = pull(out + written * kChannels, limit);
            // After the pull, which may have begun the pass `at` starts.
            if (!pass) pass = passOf(static_cast<double>(at));
            if (m_passes[*pass].begun == Begun::Waits) {
                unpull(take);
                break;
            }
            const double end = passEnd();
            if (static_cast<double>(at + take) > end) {
                const size_t past = take - static_cast<size_t>(std::max(end - static_cast<double>(at), 0.0));
                unpull(past);
                take -= past;
            }
            if (take == 0) break;
            lastFrom.reset();
        }
        fadeIn(out + written * kChannels, take);
        written += take;
        m_run.read += take;
        if (m_change) m_change->after -= take;
    }
    if (written > 0 && lastFrom) glideToRunEnd(*firstRead, written, driftBefore, *lastFrom);
    if (first != nullptr && written > 0) {
        const double frame = m_run.start + static_cast<double>(*firstRead) * m_run.rate + m_run.drift;
        const Pass& in = m_passes[*pass];
        *first = Position{in.at + spanOf(frame - static_cast<double>(in.from)), m_run.rate};
    }
    // A pass wholly before what is kept is never handed out again.
    while (m_passes.size() > 1 && m_passes[1].from <= m_historyFrom) m_passes.erase(m_passes.begin());
    return written;
}

void AudioDecoder::glideToRunEnd(uint64_t firstRead, size_t written, double driftBefore, double lastFrom) {
    // The frames the run still hands out, when every one of them is already made.
    size_t rest = 0;
    if (m_change) {
        rest = static_cast<size_t>(m_change->after);
    } else if (m_stretchEnd) {
        rest = stretchedFrames();
    } else {
        return;
    }
    const double runLast = rest > 0 ? m_stretchedFrom[m_stretchedAt + rest - 1] : lastFrom;
    // Where the line put this read's first frame, and where its last now goes: on the straight line from there
    // to the run's last frame, so the read's frames, placed `rate` apart, never pass that frame.
    const double from = m_run.start + static_cast<double>(firstRead) * m_run.rate + driftBefore;
    const auto span = static_cast<double>(written - 1);
    const double to = rest > 0 ? from + (runLast - from) * span / (span + static_cast<double>(rest)) : runLast;
    m_run.drift = to - (m_run.start + static_cast<double>(firstRead + written - 1) * m_run.rate);
}

void AudioDecoder::buildStretcher(bool leadIn) {
    const auto fail = [this](int err) {
        freeStretcher();
        checkStretch(err, m_file);
    };
    m_graph = avfilter_graph_alloc();
    if (m_stretchFrame == nullptr) m_stretchFrame = av_frame_alloc();
    if (m_graph == nullptr || m_stretchFrame == nullptr) fail(AVERROR(ENOMEM));
    // Run on this thread: the graph's own pool would start threads for a filter this small.
    m_graph->nb_threads = 1;
    avfilter_graph_set_auto_convert(m_graph, AVFILTER_AUTO_CONVERT_NONE);
    const std::string format = "sample_rate=" + std::to_string(kRate) +
                               ":sample_fmt=flt:channel_layout=4.0:time_base=1/" + std::to_string(kRate);
    int err = avfilter_graph_create_filter(&m_graphIn, avfilter_get_by_name("abuffer"), "in", format.c_str(), nullptr,
                                           m_graph);
    if (err < 0) fail(err);
    err = avfilter_graph_create_filter(&m_graphOut, avfilter_get_by_name("abuffersink"), "out", nullptr, nullptr,
                                       m_graph);
    if (err < 0) fail(err);
    const std::string tempoOptions = "tempo=" + tempoArgument(m_applied);
    AVFilterContext* tempo = nullptr;
    err = avfilter_graph_create_filter(&tempo, avfilter_get_by_name("atempo"), "tempo", tempoOptions.c_str(), nullptr,
                                       m_graph);
    if (err < 0) fail(err);
    err = avfilter_link(m_graphIn, 0, tempo, 0);
    if (err >= 0) err = avfilter_link(tempo, 0, m_graphOut, 0);
    if (err >= 0) err = avfilter_graph_config(m_graph, nullptr);
    if (err < 0) fail(err);
    m_stretchEnd.reset();
    // Led in by less than a window, atempo's start reaches the run's first frames out of order, and clicks.
    const uint64_t held = m_pulled - m_historyFrom;
    const uint64_t lead = leadIn && held >= kStretchWindow ? std::min(kLeadFrames, held) : 0;
    m_lastFrom = static_cast<double>(m_pulled - lead);
    m_coldHead = ColdHead{m_pulled - lead};
    if (lead > 0) {
        feedStretcher(m_history.data() + (m_pulled - lead - m_historyFrom) * kChannels, static_cast<size_t>(lead),
                      m_pulled - lead);
        m_leadUntil = m_pulled;
    }
}

void AudioDecoder::freeStretcher() noexcept {
    avfilter_graph_free(&m_graph);
    m_graphIn = nullptr;
    m_graphOut = nullptr;
    m_stretchEnd.reset();
    m_leadUntil.reset();
    m_coldHead.reset();
}

bool AudioDecoder::stretchMore(bool leadIn) {
    if (m_graph == nullptr) buildStretcher(leadIn);
    if (m_stretchedAt > 0) {
        m_stretched.erase(m_stretched.begin(), m_stretched.begin() + static_cast<ptrdiff_t>(m_stretchedAt * kChannels));
        m_stretchedFrom.erase(m_stretchedFrom.begin(), m_stretchedFrom.begin() + static_cast<ptrdiff_t>(m_stretchedAt));
        m_stretchedAt = 0;
    }
    const size_t before = m_stretchedFrom.size();
    if (!m_stretchEnd) {
        // About one of the stretcher's fragments at a time, so little is stretched ahead of a change of rate.
        constexpr size_t kChunk = 1024;
        const uint64_t first = m_pulled;
        m_input.resize(kChunk * kChannels);
        const size_t got = pull(m_input.data(), kChunk);
        feedStretcher(m_input.data(), got, first);
        if (got < kChunk) endStretcher();
    }
    takeStretched();
    return !m_stretchEnd || m_stretchedFrom.size() > before;
}

void AudioDecoder::feedStretcher(const float* in, size_t frames, uint64_t first) {
    if (frames == 0) return;
    av_frame_unref(m_stretchFrame);
    m_stretchFrame->format = AV_SAMPLE_FMT_FLT;
    m_stretchFrame->sample_rate = kRate;
    m_stretchFrame->nb_samples = static_cast<int>(frames);
    const AVChannelLayout fed = AV_CHANNEL_LAYOUT_4POINT0;
    int err = av_channel_layout_copy(&m_stretchFrame->ch_layout, &fed);
    if (err >= 0) err = av_frame_get_buffer(m_stretchFrame, 0);
    checkStretch(err, m_file);
    m_stretchFrame->pts = static_cast<int64_t>(first);
    auto* to = reinterpret_cast<float*>(m_stretchFrame->data[0]);
    for (size_t i = 0; i < frames; ++i) {
        to[i * kStretchChannels] = in[i * kChannels];
        to[i * kStretchChannels + 1] = in[i * kChannels + 1];
        to[i * kStretchChannels + 2] =
            kGuide + static_cast<float>(static_cast<int64_t>((first + i) % kGuideCounts)) * kGuideStep;
        to[i * kStretchChannels + 3] = kGuide;
    }
    err = av_buffersrc_add_frame(m_graphIn, m_stretchFrame);
    av_frame_unref(m_stretchFrame);
    checkStretch(err, m_file);
}

void AudioDecoder::endStretcher() {
    // atempo's flush leaves out up to about half a window of the sound's end. A window of silence after it
    // carries that out, and takeStretched() drops what is made from the silence.
    const std::vector<float> padding(kStretchWindow * kChannels, 0.0f);
    feedStretcher(padding.data(), kStretchWindow, m_pulled);
    const int err = av_buffersrc_add_frame(m_graphIn, nullptr);
    checkStretch(err, m_file);
    m_stretchEnd = m_pulled;
}

void AudioDecoder::takeStretched() {
    for (;;) {
        const int err = av_buffersink_get_frame(m_graphOut, m_stretchFrame);
        if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) break;
        checkStretch(err, m_file);
        const bool asFed =
            m_stretchFrame->format == AV_SAMPLE_FMT_FLT && m_stretchFrame->ch_layout.nb_channels == kStretchChannels;
        if (asFed) {
            const auto* from = reinterpret_cast<const float*>(m_stretchFrame->data[0]);
            for (int i = 0; i < m_stretchFrame->nb_samples; ++i) {
                // The count as fed, taken back past each wrap of kGuideCounts. A blend of padding alone
                // (weight 0) or across a wrap says nothing, nor can a frame come from further off than the
                // stretcher's window: such a one is placed on from the frame before at the rate. Written so
                // a NaN or an infinity fails the test too.
                const double counted = from[i * kStretchChannels + 2];
                const double weight = from[i * kStretchChannels + 3];
                double frame = m_lastFrom + m_applied;
                if (weight > 0.0) {
                    const double count = (counted - weight) / kGuideStep / (weight / kGuide);
                    const double wraps = std::round((m_lastFrom - count) / static_cast<double>(kGuideCounts));
                    const double placed = count + wraps * static_cast<double>(kGuideCounts);
                    if (std::fabs(placed - m_lastFrom) <= 4096.0) frame = placed;
                }
                float made[kChannels] = {from[i * kStretchChannels], from[i * kStretchChannels + 1]};
                if (m_coldHead) {
                    ColdHead& head = *m_coldHead;
                    if (head.faded == 0 && frame == static_cast<double>(head.next)) {
                        ++head.next;
                    } else if (const uint64_t at = head.next + head.faded; at >= m_historyFrom && at < heldEnd()) {
                        const double in = fadeWeight(head.faded, kCrossfadeFrames);
                        for (size_t c = 0; c < kChannels; ++c) {
                            const float copied = m_history[(at - m_historyFrom) * kChannels + c];
                            made[c] = static_cast<float>(copied + (made[c] - copied) * in);
                        }
                        if (++head.faded == kCrossfadeFrames) m_coldHead.reset();
                    } else {
                        m_coldHead.reset();
                    }
                }
                if (m_leadUntil) {
                    if (frame < static_cast<double>(*m_leadUntil)) {
                        m_lastFrom = frame;
                        continue;
                    }
                    m_leadUntil.reset();
                }
                // Made from the silence endStretcher() fed after the sound.
                if (m_stretchEnd && frame >= static_cast<double>(*m_stretchEnd)) continue;
                m_stretched.push_back(made[0]);
                m_stretched.push_back(made[1]);
                m_stretchedFrom.push_back(frame);
                m_lastFrom = frame;
            }
        }
        av_frame_unref(m_stretchFrame);
        if (!asFed) throwFailure("the sound's stretch changed its format", AVERROR_BUG, m_file);
    }
}

}  // namespace wallpaperx
