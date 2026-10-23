// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_pipeline.h"

#include "../vulkan/device.h"
#include "../vulkan/offscreen.h"

#include <exception>
#include <new>
#include <stdexcept>
#include <utility>
#include <variant>

namespace wallpaperx {

VideoPipeline::Stage::Stage(Device& device, const DecodedFrame& first, const std::string& shaderDir)
    : converter(device, first, shaderDir), renderer(device, converter.frame()) {}

VideoPipeline::VideoPipeline(Device& device, const std::string& file, const std::string& shaderDir,
                             Playback playback, std::optional<StreamPosition> startAt, DecodeHooks hooks)
    : m_device(device),
      m_shaderDir(shaderDir),
      m_hooks(std::move(hooks)),
      m_decoding(std::make_unique<PictureSource>(device, file, playback, startAt)),
      m_deepest(std::get<std::unique_ptr<PictureSource>>(m_decoding)->decoder().deepestSkip()),
      m_origin(std::get<std::unique_ptr<PictureSource>>(m_decoding)->decoder().timelineStart()) {
    decodeAheadIfSafe();
}

bool VideoPipeline::renderNext(OffscreenTargetManager& targets, const OffscreenTarget& target) {
    // Asserted here as well as in the two halves: the abort has to name the entry point the
    // caller used, not the one it delegates to.
    m_device.assertGpuThread("VideoPipeline::renderNext");
    if (!decodeNext()) return false;
    draw(targets, target, Placement{});
    return true;
}

bool VideoPipeline::decodeNext() {
    m_device.assertGpuThread("VideoPipeline::decodeNext");
    if (decode(std::nullopt, std::nullopt, Waiting::ForPicture) != Decode::Decoded) return false;
    convert();
    return true;
}

VideoPipeline::Decode VideoPipeline::decodeWithin(uint32_t dropLimit, PictureSkip skip) {
    m_device.assertGpuThread("VideoPipeline::decodeWithin");
    return decode(dropLimit, skip, Waiting::ForPicture);
}

VideoPipeline::Decode VideoPipeline::takeReady(PictureSkip skip) {
    m_device.assertGpuThread("VideoPipeline::takeReady");
    return decode(1, skip, Waiting::Never);
}

void VideoPipeline::convert() {
    m_device.assertGpuThread("VideoPipeline::convert");
    if (!m_unconverted) return;
    // The new stage is complete before the old one is freed, so a picture it refuses leaves the
    // old stage and the target as they were. The old converter's destructor then finds its
    // readers finished: the new one's constructor waited for a batch submitted after them.
    if (!m_stage || FrameConverter::pictureOf(*m_unconverted) != m_stage->converter.picture()) {
        m_stage = std::make_unique<Stage>(m_device, *m_unconverted, m_shaderDir);
    }
    static_cast<void>(m_stage->converter.convert(*m_unconverted));
    m_convertedPosition = m_unconverted->position();
    m_unconverted.reset();
}

VideoPipeline::Decode VideoPipeline::decode(std::optional<uint32_t> dropLimit, std::optional<PictureSkip> skip,
                                            Waiting waiting) {
    if (auto* ahead = std::get_if<std::unique_ptr<DecodeAhead>>(&m_decoding)) {
        if (waiting == Waiting::ForPicture) return accept((*ahead)->wait(skip));
        std::optional<DecodeAhead::Item> item = (*ahead)->take(skip.value_or(PictureSkip::Nothing));
        return item ? accept(std::move(*item)) : Decode::Pending;
    }
    PictureSource::Step step =
        std::get<std::unique_ptr<PictureSource>>(m_decoding)->next(dropLimit, skip.value_or(PictureSkip::Nothing));
    if (std::holds_alternative<PictureSource::Dropping>(step)) return Decode::Dropping;
    if (std::holds_alternative<PictureSource::End>(step)) return Decode::Exhausted;
    hold(std::move(std::get<PictureSource::Picture>(step)));
    decodeAheadIfSafe();
    return Decode::Decoded;
}

VideoPipeline::Decode VideoPipeline::accept(DecodeAhead::Item item) {
    if (auto* picture = std::get_if<PictureSource::Picture>(&item)) {
        hold(std::move(*picture));
        return Decode::Decoded;
    }
    if (std::holds_alternative<PictureSource::End>(item)) return Decode::Exhausted;
    std::rethrow_exception(std::get<std::exception_ptr>(item));
}

void VideoPipeline::hold(PictureSource::Picture picture) {
    m_timing = picture.timing;
    m_deepest = picture.deepest;
    m_unconverted = std::move(picture.frame);
}

// After a picture, which settles whether the device decodes the stream; or before the first, when the
// decoder has a start to make that reads the file up to its position, whatever its path. A stream decoded
// in software from its first picture stays on this thread; one the decode thread already holds keeps
// decoding there if it settles in software, which touches no queue. So does a stream on a system that had
// no thread to give, which is told once. Out of memory leaves the source on this thread, to try again
// after the next picture.
void VideoPipeline::decodeAheadIfSafe() {
    std::unique_ptr<PictureSource>& source = std::get<std::unique_ptr<PictureSource>>(m_decoding);
    const VideoDecoder& decoder = source->decoder();
    const bool eligible = decoder.path() == VideoDecoder::Path::Hardware || decoder.startPending();
    if (m_noDecodeThread || !eligible || !m_device.queuesInternallySynchronized()) return;
    std::unique_ptr<DecodeAhead> ahead;
    try {
        ahead = DecodeAhead::start(m_device, source, m_hooks.around, m_hooks.ready);
    } catch (const std::bad_alloc&) {
        return;
    }
    if (ahead) {
        m_decoding = std::move(ahead);
    } else {
        m_noDecodeThread = true;
    }
}

void VideoPipeline::leaveUnshown(std::span<const std::chrono::nanoseconds> spacings, uint8_t number) {
    m_device.assertGpuThread("VideoPipeline::leaveUnshown");
    const std::optional<NumberedGrids> grids =
        spacings.empty() ? std::nullopt : std::optional(NumberedGrids{ShowGrids(m_origin, spacings), number});
    if (auto* ahead = std::get_if<std::unique_ptr<DecodeAhead>>(&m_decoding)) {
        (*ahead)->leaveUnshown(grids);
    } else {
        std::get<std::unique_ptr<PictureSource>>(m_decoding)->decoder().leaveUnshown(grids);
    }
}

bool VideoPipeline::shows(std::span<const std::chrono::nanoseconds> spacings) const {
    const PictureTiming& picture = timing();
    if (!picture.presentation || !picture.duration || spacings.empty()) return true;
    return ShowGrids(m_origin, spacings).shows(*picture.presentation, *picture.duration);
}

VideoPipeline::PictureTiming VideoPipeline::timing() const {
    if (!m_timing) throw std::logic_error("VideoPipeline::timing: no picture has been decoded");
    return *m_timing;
}

void VideoPipeline::draw(OffscreenTargetManager& targets, const OffscreenTarget& target, const Placement& placement) {
    m_device.assertGpuThread("VideoPipeline::draw");
    convert();
    // A stage exists exactly once a picture has been converted into it, so this is the one state
    // that has nothing to show.
    if (!m_stage) throw std::logic_error("VideoPipeline::draw: no picture has been decoded");
    m_stage->renderer.place(placement);
    targets.render(m_stage->renderer, target);
    m_shownPosition = m_convertedPosition;
}

}
