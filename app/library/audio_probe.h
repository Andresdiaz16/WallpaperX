// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace wallpaperx {

// Whether a file holds an audio stream this build's FFmpeg can decode -- the same library the
// daemon plays it with. Unknown when the file could not be read as media to the end of the
// question: a read error, an open FFmpeg refused, no memory.
enum class AudioStreams { Unknown, Absent, Present };

// Probes the media file open as `fd`, which stays the caller's. Reads from the file's start and
// leaves its offset anywhere. Opens no other file: a container naming one (a playlist, a
// reference) is read without it.
AudioStreams probeAudioStreams(int fd);

} // namespace wallpaperx
