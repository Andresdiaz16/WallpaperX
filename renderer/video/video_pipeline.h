// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "../renderers/video_renderer.h"
#include "decode_ahead.h"
#include "frame_converter.h"
#include "picture_source.h"
#include "video_decoder.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>

namespace wallpaperx {

class Device;
class OffscreenTargetManager;
struct OffscreenTarget;

// Takes one video file onto offscreen targets a picture per call: decode, convert, blit. Paces
// nothing -- each call shows the next picture whenever it is made. A looping pipeline shows the
// first picture in the call after the last, on the same stage when their shape matches.
//
// Each picture the decoder returns is shown once, in its output order; the decoder is what keeps
// a hardware failure from returning one twice. The picture after a failure comes once software has
// caught up, and the target keeps the last picture shown until then.
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
// The first picture is decoded on the calling thread. From the second on, a stream the device
// decodes is decoded on a thread of its own, a few pictures ahead (DecodeAhead), when the device's
// queues are internally synchronised and the thread can be had; otherwise every picture is decoded
// on the calling thread. Only takeReady() and decodeWithin() set what that thread's decodes skip.
//
// GPU thread only, destruction included. Must be destroyed before the Device. Destruction waits
// for the decode thread to leave the decode call it is in.
class VideoPipeline {
public:
    using Playback = PictureSource::Playback;
    using PictureTiming = wallpaperx::PictureTiming;

    // What the caller lends the decode thread. Both are optional; what they refer to must outlive
    // this pipeline.
    struct DecodeHooks {
        // Wraps each decode the thread makes, as DecodeAhead::Around: whatever names the work for
        // whoever has to know which file the thread is decoding.
        DecodeAhead::Around around;
        // As DecodeAhead::Ready: a takeReady() that answered Pending is worth calling again.
        DecodeAhead::Ready ready;
    };

    // Opens `file`; nothing is decoded until the first call, which shows the picture at `startAt`
    // when one is given, as VideoDecoder's constructor describes. Throws what that constructor throws.
    VideoPipeline(Device& device, const std::string& file, const std::string& shaderDir,
                  Playback playback = Playback::Once, std::optional<StreamPosition> startAt = std::nullopt,
                  DecodeHooks hooks = {});

    VideoPipeline(const VideoPipeline&) = delete;
    VideoPipeline& operator=(const VideoPipeline&) = delete;

    // Renders the next picture not yet shown onto `target`, which `targets` owns, laid out as Cover.
    // False, with nothing rendered, once the stream is exhausted; when looping, only when the stream has shown
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
    // draw() shows the picture decoded last laid out as `placement` says, converting it first if it
    // is not yet, throws std::logic_error when there is none, std::invalid_argument for a placement
    // VideoRenderer::place() refuses, and otherwise throws as renderNext() does.
    bool decodeNext();
    void draw(OffscreenTargetManager& targets, const OffscreenTarget& target, const Placement& placement);

    // decodeNext() without the conversion, that gives up for now once the decoder has dropped
    // `dropLimit` pictures on its way to the next one it returns, as VideoDecoder::next(dropLimit)
    // does. Dropping leaves the picture decoded last, and its timing and position, as they were; the
    // next call carries on. Throws as decodeNext() does, but for the conversion. `skip` is as
    // PictureSkip: a skipped picture is never returned, so the next one's timing spans it. Once a
    // decode thread runs, the dropping is the thread's, and this waits for the next picture.
    //
    // Pending is takeReady()'s alone.
    enum class Decode { Decoded, Exhausted, Dropping, Pending };
    Decode decodeWithin(uint32_t dropLimit, PictureSkip skip = PictureSkip::Nothing);

    // decodeWithin(1, skip) that never waits for a decode thread: Pending when it has no picture
    // ready, and the ready hook is called once it has one.
    Decode takeReady(PictureSkip skip);

    // Converts the picture decoded last unless it already is. A caller that draws under a lock calls
    // it first, so the lock waits on a blit alone. Throws as decodeNext() does.
    void convert();

    // When the picture draw() shows belongs on screen, as the decoder reported it. Throws
    // std::logic_error before the first picture.
    PictureTiming timing() const;

    // As VideoDecoder::deepestSkip(), as of the picture decoded last.
    PictureSkip deepestSkip() const { return m_deepest; }

    // Where the picture draw() last showed lies in the file, for a pipeline opened on it later to
    // start there. Empty before the first draw and when the decoder could not find it again.
    std::optional<StreamPosition> shownPosition() const { return m_shownPosition; }

private:
    enum class Waiting { ForPicture, Never };
    // No `skip` decodes the whole stream on this thread and leaves a decode thread's level as it is.
    Decode decode(std::optional<uint32_t> dropLimit, std::optional<PictureSkip> skip, Waiting waiting);
    Decode accept(DecodeAhead::Item item);
    void hold(PictureSource::Picture picture);
    void decodeAheadIfSafe();

    // Built and replaced together, so the renderer never outlives the image it shows.
    struct Stage {
        Stage(Device& device, const DecodedFrame& first, const std::string& shaderDir);
        FrameConverter converter;
        VideoRenderer renderer;
    };

    Device& m_device;
    std::string m_shaderDir;
    DecodeHooks m_hooks;
    // Who decodes: this thread, until a decode thread takes the source over.
    std::variant<std::unique_ptr<PictureSource>, std::unique_ptr<DecodeAhead>> m_decoding;
    // The system had no thread to give, so this thread decodes for good.
    bool m_noDecodeThread = false;
    PictureSkip m_deepest;
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
