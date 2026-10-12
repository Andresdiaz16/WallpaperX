// SPDX-License-Identifier: GPL-2.0-or-later

#include "audio_decoder.h"

#include "../video/ffmpeg_file.h"
#include "../vulkan/gpu_fault.h"
#include "../wallpaper_file.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cerrno>
#include <cstring>

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

// Frames at kRate in `span`, rounded to the nearest.
int64_t framesIn(std::chrono::nanoseconds span) {
    return av_rescale(span.count(), AudioDecoder::kRate, 1'000'000'000);
}

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
            const int64_t timestamp = m_frame->best_effort_timestamp;
            if (timestamp != AV_NOPTS_VALUE) {
                const std::chrono::nanoseconds start(
                    av_rescale_q(timestamp, audio.time_base, AVRational{1, 1'000'000'000}));
                const int64_t gap = framesIn(start - *m_alignTo);
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
        m_pendingAt += dropped;
        m_dropFrames -= dropped;
    }
}

size_t AudioDecoder::read(float* out, size_t frames) {
    size_t written = 0;
    while (written < frames) {
        if (m_silence > 0) {
            const size_t take = static_cast<size_t>(std::min<uint64_t>(m_silence, frames - written));
            std::fill_n(out + written * kChannels, take * kChannels, 0.0f);
            m_silence -= take;
            written += take;
            continue;
        }
        const size_t available = m_pending.size() / kChannels - m_pendingAt;
        if (available == 0) {
            if (m_drained) break;
            decodeMore();
            continue;
        }
        const size_t take = std::min(available, frames - written);
        std::memcpy(out + written * kChannels, m_pending.data() + m_pendingAt * kChannels,
                    take * kChannels * sizeof(float));
        m_pendingAt += take;
        written += take;
    }
    return written;
}

void AudioDecoder::seek(std::chrono::nanoseconds at) {
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

}  // namespace wallpaperx
