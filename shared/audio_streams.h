// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace wallpaperx {

// Whether a file holds an audio stream this build's FFmpeg can decode -- the same library the
// daemon plays it with. Unknown when the file could not be read as media to the end of the
// question: a read error, an open FFmpeg refused, no memory.
enum class AudioStreams { Unknown, Absent, Present };

} // namespace wallpaperx
