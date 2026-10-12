// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "../video/ffmpeg_file.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;
struct SwrContext;

namespace wallpaperx {

// One file's audio as 48 kHz interleaved float stereo, read from any point on the file's timeline.
// It has a demuxer and a descriptor of its own, opened the way VideoDecoder opens the file, so the
// picture's seeks and scans never move it. Software decode only. One thread at a time.
class AudioDecoder {
public:
    static constexpr int kRate = 48000;
    static constexpr int kChannels = 2;

    // Nothing when the file holds no audio stream this FFmpeg can decode. Throws
    // WallpaperContentError for a file that cannot be opened or read, and ResourceExhausted when
    // out of memory or descriptors.
    static std::unique_ptr<AudioDecoder> open(const std::string& file);
    ~AudioDecoder();

    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;

    // The next frame read() returns is the one at `at`, on the timeline DecodedFrame::presentationTime()
    // uses, so audio and pictures of one file line up. Silence stands in for any time before the stream's
    // first sample. Throws as open() does.
    void seek(std::chrono::nanoseconds at);

    // Writes up to `frames` frames into `out`, fewer only at the end of the stream and none after it.
    // Throws as open() does; a packet that does not decode is skipped.
    size_t read(float* out, size_t frames);

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
    bool m_sentEnd = false;
    bool m_drained = false;
};

}  // namespace wallpaperx
