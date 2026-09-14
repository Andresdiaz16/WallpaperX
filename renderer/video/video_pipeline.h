// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "../renderers/video_renderer.h"
#include "frame_converter.h"
#include "video_decoder.h"

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
// No decoded frame is held between calls. The conversion copies it out, and its batch is
// ordered against the decoder's next write of that image by the frame's semaphore. The blit
// needs nothing beyond submission order: both batches go to the graphics queue, blit second.
//
// GPU thread only, destruction included. Must be destroyed before the Device.
class VideoPipeline {
public:
    enum class Playback { Once, Loop };

    // Opens `file`; nothing is decoded until the first call. Throws what VideoDecoder's
    // constructor throws.
    VideoPipeline(Device& device, const std::string& file, const std::string& shaderDir,
                  Playback playback = Playback::Once);

    VideoPipeline(const VideoPipeline&) = delete;
    VideoPipeline& operator=(const VideoPipeline&) = delete;

    // Renders the next picture not yet shown onto `target`, which `targets` owns. False, with
    // nothing rendered, once the stream is exhausted; when looping, only when the stream has shown
    // nothing or has no picture to start again from. Throws WallpaperContentError for a file or
    // picture this pipeline cannot show, GpuFailure for a GPU that has stopped answering, and
    // std::runtime_error and std::logic_error as the decoder, converter and renderer do. After a
    // throw this pipeline is not usable.
    bool renderNext(OffscreenTargetManager& targets, const OffscreenTarget& target);

private:
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
    // Null until the first picture.
    std::unique_ptr<Stage> m_stage;
};

}
