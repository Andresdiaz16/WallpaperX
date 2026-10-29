// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "decode_ahead.h"
#include "picture_source.h"
#include "video_decoder.h"
#include "ycbcr_renderer.h"

#include "wallpaper_position.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>

namespace wallpaperx {

class Device;
class OffscreenTargetManager;
struct OffscreenTarget;

// Takes one video file onto offscreen targets a picture per call: decode, then one draw per target. Paces
// nothing -- each call shows the next picture whenever it is made. A looping pipeline shows the
// first picture in the call after the last, on the same renderer when their shape matches.
//
// Each picture the decoder returns is shown once, in its output order; the decoder is what keeps
// a hardware failure from returning one twice. The picture after a failure comes once software has
// caught up, and the target keeps the last picture shown until then.
//
// A picture of a different shape -- a new size or crop, decode images of another size, or host
// planes after device images -- gets a new renderer, built before the old one is freed, in the call
// that shows it. The target goes from the last picture of one shape straight to the first of the
// next and is never shown black. That rebuild waits for the GPU, as does the first picture of a new
// decode-image pool of the same size, which keeps the renderer; a steady picture adds no wait
// beyond its parts' own.
//
// A picture decodeWithin() returns is held as decoded until it is converted, by convert() or the
// draw() that shows it, or until the next decode replaces it unconverted -- so a picture decoded
// only to be passed over costs nothing on the GPU. Converting a device picture makes it the one
// shown and keeps it until the next is converted; every draw of it is ordered against the
// decoder's next write of that image by the frame's semaphore. Converting a host picture uploads
// its planes, ordered before the draws by submission order on the graphics queue.
//
// Pictures are decoded on a thread of its own, a few ahead (DecodeAhead), when the device's queues are
// internally synchronised and the thread can be had: from the second picture on for a stream the device
// decodes, and from the first, whatever the path, for a decoder whose start reads the file up to its
// position first (VideoDecoder::startPending()). Such a stream stays on that thread if it then settles in
// software. Every other picture is decoded on the calling thread. A start's read is unbounded by any drop
// limit, and a call that waits for a picture -- renderNext(), decodeNext(), decodeWithin() -- holds the
// calling thread, and every screen it draws, for the whole read, with or without the thread: a late start
// in a large FLV or NUT file is a long hold. Only takeReady() leaves the calling thread free during it, and
// only where the thread runs. Only takeReady(), decodeWithin() and leaveUnshown() set what that thread's
// decodes skip.
//
// GPU thread only, destruction included. Must be destroyed before the Device. Destruction waits
// for the decode thread to leave the decode call it is in; a start's read is given up at its next packet.
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

    // Opens `file`; nothing is decoded before the first call, but by a decode thread started here for a start
    // that reads the file first, as above. That call shows the picture at `startAt` when one is given, as
    // VideoDecoder's constructor describes. Throws what that constructor throws.
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
    // abandon it, and draw() of a converted picture is one draw.
    //
    // decodeNext() decodes and converts, and answers as renderNext() does and throws as it does.
    // draw() shows the picture decoded last laid out as `placement` says, converting it first if it
    // is not yet, throws std::logic_error when there is none, std::invalid_argument for a placement
    // YcbcrRenderer::place() refuses, and otherwise throws as renderNext() does.
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
    // it first, so the lock waits on the draws alone. Throws as decodeNext() does, leaving the
    // picture and what is shown as they were.
    void convert();

    // When the picture draw() shows belongs on screen, as the decoder reported it. Throws
    // std::logic_error before the first picture.
    PictureTiming timing() const;

    // As VideoDecoder::deepestSkip(), as of the picture decoded last.
    PictureSkip deepestSkip() const { return m_deepest; }

    // Lets the decoder leave out the pictures nothing references that no display taking one picture every
    // one of `spacings` of the file's own time would show, from its next decode on, as
    // VideoDecoder::leaveUnshown() does with ShowGrids laid from the file's start; none leaves none out.
    // Standing, unlike a PictureSkip, and not a promise: a picture decoded ahead, or one something
    // references, is still returned. A picture thinned by them reports `number` as
    // PictureTiming::thinnedUnder. Throws as ShowGrids' constructor does.
    void leaveUnshown(std::span<const std::chrono::nanoseconds> spacings, uint8_t number);

    // Whether any of those displays would show the picture decoded last; true for one without a time or
    // a duration, and for no spacing. Throws std::logic_error before the first picture, and as ShowGrids'
    // constructor does.
    bool shows(std::span<const std::chrono::nanoseconds> spacings) const;

    // Where the picture draw() last showed lies in the file, for a pipeline opened on it later to
    // start there. Empty before the first draw and when the decoder could not find it again.
    std::optional<StreamPosition> shownPosition() const { return m_shownPosition; }

    // Whether the picture draw() showed last was decoded in software. Throws std::logic_error before the first.
    bool shownFromSoftware() const;

    // As VideoDecoder::softwareFromSetup(), as of the picture decoded last. It never turns false again, so
    // when true every picture shown was decoded in software.
    bool softwareFromSetup() const { return m_softwareFromSetup; }

    // As VideoDecoder's, read when the pipeline is opened.
    std::chrono::nanoseconds timelineStart() const { return m_origin; }
    std::optional<std::chrono::nanoseconds> declaredEnd() const { return m_declaredEnd; }

private:
    enum class Waiting { ForPicture, Never };
    // No `skip` decodes the whole stream on this thread and leaves a decode thread's level as it is.
    Decode decode(std::optional<uint32_t> dropLimit, std::optional<PictureSkip> skip, Waiting waiting);
    Decode accept(DecodeAhead::Item item);
    void hold(PictureSource::Picture picture);
    void decodeAheadIfSafe();

    Device& m_device;
    std::string m_shaderDir;
    DecodeHooks m_hooks;
    // Who decodes: this thread, until a decode thread takes the source over.
    std::variant<std::unique_ptr<PictureSource>, std::unique_ptr<DecodeAhead>> m_decoding;
    // The system had no thread to give, so this thread decodes for good.
    bool m_noDecodeThread = false;
    PictureSkip m_deepest;
    bool m_softwareFromSetup = false;
    // Where every ShowGrid for this file is laid from, read before a decode thread owns the decoder.
    std::chrono::nanoseconds m_origin;
    std::optional<std::chrono::nanoseconds> m_declaredEnd;
    // Null until the first picture is converted.
    std::unique_ptr<YcbcrRenderer> m_renderer;
    // The picture decoded last, while it is not converted yet.
    std::optional<DecodedFrame> m_unconverted;
    // The picture decoded last's; empty before the first.
    std::optional<PictureTiming> m_timing;
    std::optional<StreamPosition> m_convertedPosition;
    std::optional<StreamPosition> m_shownPosition;
};

}
