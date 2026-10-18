// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_pipeline.h"

#include "../vulkan/device.h"
#include "../vulkan/offscreen.h"

#include <stdexcept>
#include <utility>
#include <variant>

namespace wallpaperx {

VideoPipeline::Stage::Stage(Device& device, const DecodedFrame& first, const std::string& shaderDir)
    : converter(device, first, shaderDir), renderer(device, converter.frame()) {}

VideoPipeline::VideoPipeline(Device& device, const std::string& file, const std::string& shaderDir,
                             Playback playback, std::optional<StreamPosition> startAt)
    : m_device(device), m_shaderDir(shaderDir), m_playback(playback), m_decoder(device, file, startAt) {}

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
    if (decode(std::nullopt, PictureSkip::Nothing) != Decode::Decoded) return false;
    convert();
    return true;
}

VideoPipeline::Decode VideoPipeline::decodeWithin(uint32_t dropLimit, PictureSkip skip) {
    m_device.assertGpuThread("VideoPipeline::decodeWithin");
    return decode(dropLimit, skip);
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

VideoPipeline::Decode VideoPipeline::decode(std::optional<uint32_t> dropLimit, PictureSkip skip) {
    std::optional<DecodedFrame> frame;
    if (dropLimit) {
        auto step = m_decoder.next(*dropLimit, skip);
        if (std::holds_alternative<VideoDecoder::Dropping>(step)) return Decode::Dropping;
        frame = std::move(std::get<std::optional<DecodedFrame>>(step));
    } else {
        frame = m_decoder.next(skip);
    }
    bool startsPass = !m_timing && !m_decoder.startedMidStream();
    std::optional<std::chrono::nanoseconds> passEnded;
    // No timing means nothing was ever decoded, and a stream with nothing to show must not spin here.
    if (!frame && m_playback == Playback::Loop && m_timing) {
        passEnded = m_decoder.readThrough();
        m_decoder.restart();
        // Unbounded: a restarted pass has returned nothing to resume after, so it drops nothing.
        frame = m_decoder.next(skip);
        startsPass = true;
    }
    if (!frame) return Decode::Exhausted;

    m_timing = PictureTiming{frame->presentationTime(), frame->duration(), startsPass, frame->skip(), passEnded,
                             frame->keptThrough(), frame->readSince()};
    m_unconverted = std::move(*frame);
    return Decode::Decoded;
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
