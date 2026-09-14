// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_pipeline.h"

#include "../vulkan/device.h"
#include "../vulkan/offscreen.h"

namespace wallpaperx {

VideoPipeline::Stage::Stage(Device& device, const DecodedFrame& first, const std::string& shaderDir)
    : converter(device, first, shaderDir), renderer(device, converter.frame()) {}

VideoPipeline::VideoPipeline(Device& device, const std::string& file, const std::string& shaderDir,
                             Playback playback)
    : m_device(device), m_shaderDir(shaderDir), m_playback(playback), m_decoder(device, file) {}

bool VideoPipeline::renderNext(OffscreenTargetManager& targets, const OffscreenTarget& target) {
    m_device.assertGpuThread("VideoPipeline::renderNext");
    std::optional<DecodedFrame> frame = m_decoder.next();
    // No stage means nothing was ever shown, and a stream with nothing to show must not spin here.
    if (!frame && m_playback == Playback::Loop && m_stage) {
        m_decoder.restart();
        frame = m_decoder.next();
    }
    if (!frame) return false;

    // The new stage is complete before the old one is freed, so a picture it refuses leaves the
    // old stage and the target as they were. The old converter's destructor then finds its
    // readers finished: the new one's constructor waited for a batch submitted after them.
    if (!m_stage || FrameConverter::pictureOf(*frame) != m_stage->converter.picture()) {
        m_stage = std::make_unique<Stage>(m_device, *frame, m_shaderDir);
    }
    static_cast<void>(m_stage->converter.convert(*frame));
    targets.render(m_stage->renderer, target);
    return true;
}

}
