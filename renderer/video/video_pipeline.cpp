// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_pipeline.h"

#include "../vulkan/device.h"
#include "../vulkan/offscreen.h"

#include <stdexcept>

namespace wallpaperx {

VideoPipeline::Stage::Stage(Device& device, const DecodedFrame& first, const std::string& shaderDir)
    : converter(device, first, shaderDir), renderer(device, converter.frame()) {}

VideoPipeline::VideoPipeline(Device& device, const std::string& file, const std::string& shaderDir,
                             Playback playback)
    : m_device(device), m_shaderDir(shaderDir), m_playback(playback), m_decoder(device, file) {}

bool VideoPipeline::renderNext(OffscreenTargetManager& targets, const OffscreenTarget& target) {
    // Asserted here as well as in the two halves: the abort has to name the entry point the
    // caller used, not the one it delegates to.
    m_device.assertGpuThread("VideoPipeline::renderNext");
    if (!decodeNext()) return false;
    draw(targets, target);
    return true;
}

bool VideoPipeline::decodeNext() {
    m_device.assertGpuThread("VideoPipeline::decodeNext");
    std::optional<DecodedFrame> frame = m_decoder.next();
    bool startsPass = !m_stage;
    // No stage means nothing was ever shown, and a stream with nothing to show must not spin here.
    if (!frame && m_playback == Playback::Loop && m_stage) {
        m_decoder.restart();
        frame = m_decoder.next();
        startsPass = true;
    }
    if (!frame) return false;

    // The new stage is complete before the old one is freed, so a picture it refuses leaves the
    // old stage and the target as they were. The old converter's destructor then finds its
    // readers finished: the new one's constructor waited for a batch submitted after them.
    if (!m_stage || FrameConverter::pictureOf(*frame) != m_stage->converter.picture()) {
        m_stage = std::make_unique<Stage>(m_device, *frame, m_shaderDir);
    }
    static_cast<void>(m_stage->converter.convert(*frame));
    m_timing = PictureTiming{frame->presentationTime(), frame->duration(), startsPass};
    return true;
}

VideoPipeline::PictureTiming VideoPipeline::timing() const {
    if (!m_stage) throw std::logic_error("VideoPipeline::timing: no picture has been decoded");
    return m_timing;
}

void VideoPipeline::draw(OffscreenTargetManager& targets, const OffscreenTarget& target) {
    m_device.assertGpuThread("VideoPipeline::draw");
    // A stage exists exactly once a picture has been converted into it, so this is the one state
    // that has nothing to show.
    if (!m_stage) throw std::logic_error("VideoPipeline::draw: no picture has been decoded");
    targets.render(m_stage->renderer, target);
}

}
