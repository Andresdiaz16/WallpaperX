// SPDX-License-Identifier: GPL-2.0-or-later

#include "audio_probe.h"

#include "../../renderer/video/ffmpeg_fd_io.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
}

#include <unistd.h>

namespace wallpaperx {

namespace {

constexpr int kBufferBytes = 64 * 1024;
// A container that names streams only as their packets arrive (FLV, for one) is read this far
// for an audio one: an ordinary interleave meets every stream long before, and the bound keeps a
// file with no audio from being read to its end on every library scan.
constexpr int kPacketsForLateStreams = 256;

bool holdsDecodableAudio(const AVFormatContext& format)
{
    for (unsigned i = 0; i < format.nb_streams; ++i) {
        const AVCodecParameters* stream = format.streams[i]->codecpar;
        if (stream->codec_type == AVMEDIA_TYPE_AUDIO && avcodec_find_decoder(stream->codec_id) != nullptr) {
            return true;
        }
    }
    return false;
}

// The format context before the I/O context it reads through, which FFmpeg never frees for us.
struct Probe {
    AVIOContext* io = nullptr;
    AVFormatContext* format = nullptr;
    AVPacket* packet = nullptr;

    ~Probe()
    {
        av_packet_free(&packet);
        avformat_close_input(&format);
        if (io != nullptr) {
            av_freep(&io->buffer);
        }
        avio_context_free(&io);
    }
};

} // namespace

AudioStreams probeAudioStreams(int fd)
{
    if (::lseek(fd, 0, SEEK_SET) < 0) {
        return AudioStreams::Unknown;
    }
    Probe probe;
    probe.io = ffmpeg_fd::openReader(&fd, kBufferBytes);
    if (probe.io == nullptr) {
        return AudioStreams::Unknown;
    }
    probe.format = avformat_alloc_context();
    if (probe.format == nullptr || !ffmpeg_fd::readOnlyThrough(probe.format, probe.io)) {
        return AudioStreams::Unknown;
    }
    // Frees the format context on failure, never the I/O context.
    if (avformat_open_input(&probe.format, "", nullptr, nullptr) < 0) {
        return AudioStreams::Unknown;
    }
    if (holdsDecodableAudio(*probe.format)) {
        return AudioStreams::Present;
    }
    if ((probe.format->ctx_flags & AVFMTCTX_NOHEADER) == 0) {
        return AudioStreams::Absent;
    }
    probe.packet = av_packet_alloc();
    if (probe.packet == nullptr) {
        return AudioStreams::Unknown;
    }
    for (int read = 0; read < kPacketsForLateStreams; ++read) {
        if (const int error = av_read_frame(probe.format, probe.packet); error < 0) {
            return error == AVERROR_EOF ? AudioStreams::Absent : AudioStreams::Unknown;
        }
        av_packet_unref(probe.packet);
        if (holdsDecodableAudio(*probe.format)) {
            return AudioStreams::Present;
        }
    }
    return AudioStreams::Absent;
}

} // namespace wallpaperx
