// SPDX-License-Identifier: GPL-2.0-or-later

#include "ffmpeg_file.h"

#include "ffmpeg_fd_io.h"

#include "../vulkan/gpu_fault.h"
#include "../wallpaper_file.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avstring.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
}

#include <unistd.h>

#include <cerrno>

namespace wallpaperx::ffmpeg_file {

namespace {

// ffmpeg's own default for a file it opens, and what its probes are sized against.
constexpr int kIoBufferSize = 32768;

// throwFileFailure() for opening a container. One that is a list of other files fails because
// openContainer() lets ffmpeg open nothing by itself, and ffmpeg's reason ("Invalid argument") says
// nothing of that.
[[noreturn]] void throwOpenFailure(const char* component, const std::string& reason, int err,
                                   const std::string& file, const AVInputFormat* format) {
    if (err != AVERROR(ENOMEM) && format != nullptr && av_match_name(format->name, "concat,hls,dash,imf")) {
        throw WallpaperContentError(describeRejection(
            component,
            "the wallpaper is a list of other files to play (a playlist or an ffconcat script); only a single "
            "self-contained video file can be shown",
            file));
    }
    throwFileFailure(component, reason, err, file);
}

}  // namespace

std::optional<std::chrono::nanoseconds> inNanoseconds(int64_t value, AVRational base) {
    if (value == AV_NOPTS_VALUE || base.num <= 0 || base.den <= 0) return std::nullopt;
    return std::chrono::nanoseconds(av_rescale_q(value, base, AVRational{1, 1'000'000'000}));
}

std::string avErrorString(int err) {
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(err, text, sizeof text);
    return text;
}

void throwOutOfMemory(const char* component) { throw ResourceExhausted(std::string(component) + ": out of memory"); }

void throwFileFailure(const char* component, const std::string& reason, int err, const std::string& file) {
    if (err == AVERROR(ENOMEM)) throwOutOfMemory(component);
    throw WallpaperContentError(describeRejection(component, reason + " (" + avErrorString(err) + ")", file));
}

void Reader::open(const std::string& file, const char* component) {
    close();
    m_fd = openWallpaperFile(file, component);
    if (m_fd < 0) {
        // Taken before the message is built: building one allocates, an allocator may leave errno
        // changed behind it, and a reason that reads as out of memory would send the daemon off
        // freeing another monitor's decoder to retry this open.
        const int why = errno;
        throwFileFailure(component, "cannot open the wallpaper", AVERROR(why), file);
    }
    m_io = ffmpeg_fd::openReader(&m_fd, kIoBufferSize);
    if (m_io == nullptr) throwOutOfMemory(component);
}

void Reader::close() noexcept {
    if (m_io != nullptr) av_freep(&m_io->buffer);
    avio_context_free(&m_io);
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
}

void openContainer(AVFormatContext*& format, AVIOContext* io, const std::string& file, const char* component) {
    io->error = 0;
    format = avformat_alloc_context();
    if (format == nullptr || !ffmpeg_fd::readOnlyThrough(format, io)) throwOutOfMemory(component);
    // Frees the context on failure, never a caller's I/O context.
    if (const int err = avformat_open_input(&format, file.c_str(), nullptr, nullptr); err < 0) {
        // The context and the demuxer it found are gone, so the file is probed again to say what it
        // was -- on this path only, so a file that opens is opened exactly as before.
        const AVInputFormat* probed = nullptr;
        if (avio_seek(io, 0, SEEK_SET) < 0 || av_probe_input_buffer2(io, &probed, file.c_str(), nullptr, 0, 0) < 0) {
            probed = nullptr;
        }
        throwOpenFailure(component, "cannot open the wallpaper", err, file, probed);
    }
    if (const int err = avformat_find_stream_info(format, nullptr); err < 0) {
        throwOpenFailure(component, "cannot read the wallpaper's streams", err, file, format->iformat);
    }
}

int readPacket(AVFormatContext& format, AVPacket* packet) {
    const int err = av_read_frame(&format, packet);
    if (err < 0 && format.pb != nullptr && format.pb->error < 0) return format.pb->error;
    return err;
}

int seekFrame(AVFormatContext& format, int stream, int64_t timestamp, int flags) {
    if (format.pb != nullptr) format.pb->error = 0;
    return av_seek_frame(&format, stream, timestamp, flags);
}

int seekFile(AVFormatContext& format, int stream, int64_t earliest, int64_t timestamp, int64_t latest, int flags) {
    if (format.pb != nullptr) format.pb->error = 0;
    return avformat_seek_file(&format, stream, earliest, timestamp, latest, flags);
}

}  // namespace wallpaperx::ffmpeg_file
