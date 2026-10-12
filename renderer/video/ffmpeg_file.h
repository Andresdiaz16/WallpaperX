// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// How the renderer's decoders -- a video's pictures and its sound -- open a wallpaper file for
// FFmpeg and say why one failed, so the two refuse the same files in the same words. Renderer only:
// it throws the renderer's error types. The app's audio probe shares ffmpeg_fd_io.h instead.

#include <cstdint>
#include <string>

struct AVFormatContext;
struct AVIOContext;
struct AVPacket;

namespace wallpaperx::ffmpeg_file {

std::string avErrorString(int err);

[[noreturn]] void throwOutOfMemory(const char* component);

// An FFmpeg error that is not the GPU's: ResourceExhausted for memory, otherwise WallpaperContentError
// naming the file.
[[noreturn]] void throwFileFailure(const char* component, const std::string& reason, int err,
                                   const std::string& file);

// A wallpaper file opened through openWallpaperFile() and the I/O context FFmpeg reads it through.
// Not movable: the context holds the descriptor's address.
class Reader {
public:
    Reader() = default;
    ~Reader() { close(); }
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    // Throws as throwFileFailure() does, and WallpaperContentError for a file openWallpaperFile() refuses.
    void open(const std::string& file, const char* component);
    // The I/O context before the file it reads.
    void close() noexcept;
    // Null until open() succeeds.
    AVIOContext* io() const { return m_io; }

private:
    int m_fd = -1;
    AVIOContext* m_io = nullptr;
};

// Opens the container `io` reads from its current position into `format`, which must be null, so
// that it reads nothing but `io`, and reads its streams' parameters. Whatever `format` holds after a
// throw is the caller's to close, as after a return. A container that is a list of other files is
// refused as one. Forgets any read failure `io` kept, as seekFrame() does.
void openContainer(AVFormatContext*& format, AVIOContext* io, const std::string& file, const char* component);

// av_read_frame(), with a failed read told from the end of the file. avio reports both as the end,
// and a demuxer may hand over what a failed read left as a packet, so the failure is reported by
// whichever read comes next to fail: avio keeps it in the I/O context for the context's life.
int readPacket(AVFormatContext& format, AVPacket* packet);

// av_seek_frame() and avformat_seek_file(), forgetting any read failure kept from before: it belonged
// to the position left, which a reader recovering from it leaves by seeking or by openContainer().
int seekFrame(AVFormatContext& format, int stream, int64_t timestamp, int flags);
int seekFile(AVFormatContext& format, int stream, int64_t earliest, int64_t timestamp, int64_t latest, int flags);

}  // namespace wallpaperx::ffmpeg_file
