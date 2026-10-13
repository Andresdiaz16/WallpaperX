// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "../renderers/video_renderer.h"
#include "frame_converter.h"
#include "video_decoder.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace wallpaperx {

class Device;
class OffscreenTargetManager;
struct OffscreenTarget;

// Takes one video file onto offscreen targets a picture per call: decode, convert, blit. Paces
// nothing -- each call shows the next picture whenever it is made. A looping pipeline shows the
// first picture in the call after the last, on the same stage when their shape matches.
//
// Each picture the decoder returns is shown once, in its output order; the decoder is what keeps
// a hardware failure from returning one twice. The call that meets the failure returns once
// software has caught up, and the target keeps the last picture shown until then.
//
// A picture of a different shape -- a new size or crop, or host planes after device images --
// gets a new converter, built before the old one is freed, in the call that shows it. The target
// goes from the last picture of one shape straight to the first of the next and is never shown
// black. That rebuild waits for the GPU; a steady picture adds no wait beyond its parts' own.
//
// A picture decodeWithin() returns is held as decoded until it is converted, by convert() or the
// draw() that shows it, or until the next decode replaces it unconverted -- so a picture decoded
// only to be passed over costs no conversion. The conversion copies it out, and its batch is
// ordered against the decoder's next write of that image by the frame's semaphore. The blit
// needs nothing beyond submission order: both batches go to the graphics queue, blit second.
//
// GPU thread only, destruction included. Must be destroyed before the Device.
class VideoPipeline {
public:
    enum class Playback { Once, Loop };

    // Opens `file`; nothing is decoded until the first call, which shows the picture at `startAt`
    // when one is given, as VideoDecoder's constructor describes. Throws what that constructor throws.
    VideoPipeline(Device& device, const std::string& file, const std::string& shaderDir,
                  Playback playback = Playback::Once, std::optional<StreamPosition> startAt = std::nullopt);

    VideoPipeline(const VideoPipeline&) = delete;
    VideoPipeline& operator=(const VideoPipeline&) = delete;

    // Renders the next picture not yet shown onto `target`, which `targets` owns. False, with
    // nothing rendered, once the stream is exhausted; when looping, only when the stream has shown
    // nothing or has no picture to start again from. Throws WallpaperContentError for a file or
    // picture this pipeline cannot show, GpuFailure for a GPU that has stopped answering, and
    // std::runtime_error and std::logic_error as the decoder, converter and renderer do. After a
    // throw this pipeline is not usable.
    bool renderNext(OffscreenTargetManager& targets, const OffscreenTarget& target);

    // The two halves of that call, for a caller that must decide between them whether this picture
    // may be shown at all: decodeNext() touches no target, so a caller holding a picture may still
    // abandon it, and draw() of a converted picture is one blit.
    //
    // decodeNext() decodes and converts, and answers as renderNext() does and throws as it does.
    // draw() shows the picture decoded last, converting it first if it is not yet, throws
    // std::logic_error when there is none, and otherwise throws as renderNext() does.
    bool decodeNext();
    void draw(OffscreenTargetManager& targets, const OffscreenTarget& target);

    // decodeNext() without the conversion, that gives up for now once the decoder has dropped
    // `dropLimit` pictures on its way to the next one it returns, as VideoDecoder::next(dropLimit)
    // does. Dropping leaves the picture decoded last, and its timing and position, as they were; the
    // next call carries on. Throws as decodeNext() does, but for the conversion. `skip` is as
    // PictureSkip: a skipped picture is never returned, so the next one's timing spans it.
    enum class Decode { Decoded, Exhausted, Dropping };
    Decode decodeWithin(uint32_t dropLimit, PictureSkip skip = PictureSkip::Nothing);

    // Converts the picture decoded last unless it already is. A caller that draws under a lock calls
    // it first, so the lock waits on a blit alone. Throws as decodeNext() does.
    void convert();

    // When the picture draw() shows belongs on screen, as the decoder reported it. What paces the
    // pictures is the caller's; this only says where each one falls.
    struct PictureTiming {
        std::optional<std::chrono::nanoseconds> presentation;
        std::optional<std::chrono::nanoseconds> duration;
        // The first picture of a pass through the file: the first this pipeline showed, unless it
        // started part way in, or the first after a loop. Its presentation time says nothing about
        // its distance from the picture before it.
        bool startsPass = false;
    };
    // Throws std::logic_error before the first picture.
    PictureTiming timing() const;

    // As VideoDecoder::averageFrameInterval().
    std::optional<std::chrono::nanoseconds> averageFrameInterval() const { return m_decoder.averageFrameInterval(); }

    // Where the picture draw() last showed lies in the file, for a pipeline opened on it later to
    // start there. Empty before the first draw and when the decoder could not find it again.
    std::optional<StreamPosition> shownPosition() const { return m_shownPosition; }

private:
    Decode decode(std::optional<uint32_t> dropLimit, PictureSkip skip);

    // Built and replaced together, so the renderer never outlives the image it shows.
    struct Stage {
        Stage(Device& device, const DecodedFrame& first, const std::string& shaderDir);
        FrameConverter converter;
        VideoRenderer renderer;
    };

    Device& m_device;
    std::string m_shaderDir;
    Playback m_playback;
    VideoDecoder m_decoder;
    // Null until the first picture is converted.
    std::unique_ptr<Stage> m_stage;
    // The picture decoded last, while it is not converted yet.
    std::optional<DecodedFrame> m_unconverted;
    // The picture decoded last's; empty before the first.
    std::optional<PictureTiming> m_timing;
    std::optional<StreamPosition> m_convertedPosition;
    std::optional<StreamPosition> m_shownPosition;
};

}
