// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "picture_skip.h"
#include "stream_position.h"
#include "video_decoder.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <variant>

namespace wallpaperx {

class Device;

// When a picture belongs on screen, as the decoder reported it. What paces the pictures is the
// caller's; this only says where each one falls.
struct PictureTiming {
    std::optional<std::chrono::nanoseconds> presentation;
    std::optional<std::chrono::nanoseconds> duration;
    // The first picture of a pass through the file: the first returned, unless the decoder
    // started part way in, or the first after a loop. Its presentation time says nothing about
    // its distance from the picture before it.
    bool startsPass = false;
    // As DecodedFrame::skip().
    PictureSkip skip = PictureSkip::Nothing;
    // For the first picture after a loop, where the pass before it ended, as
    // VideoDecoder::readThrough(): past its last picture shown when that one's successors were skipped.
    std::optional<std::chrono::nanoseconds> passEnded;
    // As DecodedFrame::keptThrough() and readSince().
    PictureSkip keptThrough = PictureSkip::Nothing;
    uint32_t readSince = 1;
    // How long the decoder worked to return this picture since it returned the one before:
    // the time spent in its calls, dropped pictures and a loop's restart included, not the time
    // anyone waited for it nor the time it waited for an answer from another thread.
    std::chrono::nanoseconds decodeTime{};
    // As DecodedFrame::thinnedUnder().
    std::optional<uint8_t> thinnedUnder;
};

// One file's VideoDecoder, played through: started again at its end when looping, and each
// picture handed out with its timing. Used by one thread at a time, as VideoDecoder::next() is.
class PictureSource {
public:
    enum class Playback { Once, Loop };

    // Throws what VideoDecoder's constructor throws.
    PictureSource(Device& device, const std::string& file, Playback playback, std::optional<StreamPosition> startAt);

    struct Picture {
        DecodedFrame frame;
        PictureTiming timing;
        // As VideoDecoder::deepestSkip() when this picture was returned.
        PictureSkip deepest = PictureSkip::Nothing;
    };
    // The stream is exhausted; when looping, it has shown nothing or has no picture to start again from.
    struct End {};
    // As VideoDecoder::Dropping.
    struct Dropping {};
    using Step = std::variant<Picture, End, Dropping>;

    // The next picture, as VideoDecoder::next() returns it with `dropLimit` or without one. Throws
    // what that throws; the source is not usable after a throw.
    Step next(std::optional<uint32_t> dropLimit, PictureSkip skip);

    // As VideoDecoder::askDeviceThrough(). Time spent in `ask` is waiting on another thread, so it
    // is no picture's decodeTime.
    void askDeviceThrough(std::function<bool()> ask);

    VideoDecoder& decoder() { return m_decoder; }
    const VideoDecoder& decoder() const { return m_decoder; }

private:
    std::variant<std::optional<DecodedFrame>, VideoDecoder::Dropping> decode(std::optional<uint32_t> dropLimit,
                                                                            PictureSkip skip);

    VideoDecoder m_decoder;
    Playback m_playback;
    bool m_returnedAny = false;
    // Decoder time spent since the last picture was returned.
    std::chrono::nanoseconds m_spent{};
    // Time spent in the ask hook during the current next().
    std::chrono::nanoseconds m_asking{};
};

}
