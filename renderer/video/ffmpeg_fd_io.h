// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// How FFmpeg reads a media file here: through a descriptor already opened and checked, never by a
// path or a protocol of its own. Shared by the daemon's decoder and the app's audio probe, so the
// two refuse exactly the same things. Header-only and free of Vulkan and Qt: the app includes it
// without linking the renderer.

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
}

#include <cerrno>
#include <cstdint>

#include <sys/stat.h>
#include <unistd.h>

namespace wallpaperx::ffmpeg_fd {

// The AVIOContext callbacks; `opaque` is an `int*` to the descriptor.
inline int readFd(void* opaque, uint8_t* buffer, int size) {
    const int fd = *static_cast<const int*>(opaque);
    ssize_t got = 0;
    do {
        got = ::read(fd, buffer, static_cast<size_t>(size));
    } while (got < 0 && errno == EINTR);
    if (got < 0) return AVERROR(errno);
    return got == 0 ? AVERROR_EOF : static_cast<int>(got);
}

inline int64_t seekFd(void* opaque, int64_t offset, int whence) {
    const int fd = *static_cast<const int*>(opaque);
    // Asked for the file's length rather than for a position; AVSEEK_FORCE is advice about how
    // dear a seek is, which costs a regular file nothing.
    if ((whence & ~AVSEEK_FORCE) == AVSEEK_SIZE) {
        struct stat info {};
        if (::fstat(fd, &info) != 0) return AVERROR(errno);
        return info.st_size;
    }
    const off_t at = ::lseek(fd, static_cast<off_t>(offset), whence & ~AVSEEK_FORCE);
    return at == static_cast<off_t>(-1) ? AVERROR(errno) : at;
}

// The io_open hook: refuses every open FFmpeg would make of its own accord.
inline int refuseOpen(AVFormatContext*, AVIOContext**, const char*, int, AVDictionary**) {
    return AVERROR(EACCES);
}

// A read-only I/O context over `*fd`, which must outlive it; null when out of memory. Its buffer
// is the caller's to free (av_freep(&io->buffer)) before avio_context_free(), which leaves it.
inline AVIOContext* openReader(int* fd, int bufferBytes) {
    auto* buffer = static_cast<unsigned char*>(av_malloc(static_cast<size_t>(bufferBytes)));
    if (buffer == nullptr) return nullptr;
    AVIOContext* io = avio_alloc_context(buffer, bufferBytes, /*write_flag=*/0, fd, &readFd, nullptr, &seekFd);
    if (io == nullptr) av_free(buffer);
    return io;
}

// Makes `format` read only through `io` and open nothing else. A container whose content names
// further files -- a playlist, a concat script, an image sequence, a QuickTime external data
// reference -- would have them opened by a path nobody checked, so a pipe among them parks the
// thread. Both refusals are needed: io_open is the hook a demuxer opens a file through, while one
// that opens a container of its own (concat does) builds a context that inherits the protocol list
// and nothing else. A protocol list naming none there is, because an empty one means unrestricted.
// Such a container then fails to open, which is the intended loss. False when out of memory.
[[nodiscard]] inline bool readOnlyThrough(AVFormatContext* format, AVIOContext* io) {
    format->pb = io;
    format->flags |= AVFMT_FLAG_CUSTOM_IO;
    format->io_open = &refuseOpen;
    format->protocol_whitelist = av_strdup("none");
    return format->protocol_whitelist != nullptr;
}

}  // namespace wallpaperx::ffmpeg_fd
