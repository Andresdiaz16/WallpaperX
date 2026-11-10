// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "audio_streams.h"

namespace wallpaperx {

// Probes the media file open as `fd`, which stays the caller's. Reads from the file's start and
// leaves its offset anywhere. Opens no other file: a container naming one (a playlist, a
// reference) is read without it.
AudioStreams probeAudioStreams(int fd);

} // namespace wallpaperx
