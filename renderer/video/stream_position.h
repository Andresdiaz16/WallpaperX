// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>

namespace wallpaperx {

class VideoDecoder;

// Where one decoded picture lies in its file, for a decoder opened later on the same file to start
// there. Only a VideoDecoder makes one, and only for a picture it can find again. Apart from
// video_decoder.h so state that outlives a decoder can hold one without ffmpeg's headers.
class StreamPosition {
public:
    bool operator==(const StreamPosition&) const = default;

private:
    friend class VideoDecoder;
    explicit StreamPosition(int64_t timestamp) : m_timestamp(timestamp) {}
    // The picture's timestamp, in its stream's time base.
    int64_t m_timestamp;
};

}
