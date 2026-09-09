// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "renderer_base.h"

#include <cstdint>

namespace wallpaperx {

class Device;

// What the owner of a set of frames declares about them, once: everything a blit needs of its
// source that a VkImage handle cannot be asked for. Vulkan reads none of this back, so the
// owner is the only authority; the renderer checks what it can against the device and trusts
// the rest. Frames from one decoder pool share one of these.
struct VideoSource {
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL;
    uint32_t width = 0;
    uint32_t height = 0;
    // Must include VK_IMAGE_USAGE_TRANSFER_SRC_BIT.
    VkImageUsageFlags usage = 0;

    bool operator==(const VideoSource&) const = default;
};

// One decoded frame, lent by its owner for as long as it is shown: mip 0, layer 0 of a
// single-plane colour image that `source` describes.
//
// The image must be owned by the graphics queue family, or shared concurrently, when a batch
// that shows it runs. A frame another queue family or another device produced has to be
// released to the graphics family by its owner first, with whatever ordering that needs:
// render() submits without semaphores, so nothing here can wait for a producer.
struct VideoFrame {
    VideoSource source;
    VkImage image = VK_NULL_HANDLE;

    // The layout the image is in when a batch that shows it runs, and the layout that batch
    // returns it to. Every record() leaves the frame exactly as it found it, so the owner's own
    // record of the layout never has to be told what happened here.
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

// Puts a caller-owned decoded frame onto an offscreen target by blitting it: scaled to cover
// the target and cropped centrally to the target's aspect, never stretched or letterboxed.
//
// Owns no Vulkan object at all -- no pipeline, sampler or descriptor -- so it is cheap to
// build and destroy, works identically on every descriptor tier, and record() costs two
// barriers and one blit.
//
// The frame's owner keeps every lifetime obligation. A shown frame is read by every render()
// recorded while it is shown -- one per monitor, per display frame -- so the batch to outlive
// is the last of those: Device::lastSubmission() taken after the final render() before show()
// replaced the frame or this renderer was destroyed. Its pixels must not change until that
// batch has completed either.
//
// Whatever decodes into a multi-planar Y'CbCr image (NV12, P010) has to convert to a colour
// format first: a blit cannot read those, and show() refuses them.
class VideoRenderer final : public IRenderer {
public:
    // Aborts, like every entry to the Device, unless called on its GPU thread; that is checked
    // before anything else, so nothing below is reached from the wrong thread. Then show(first),
    // with its exceptions.
    VideoRenderer(Device& device, const VideoFrame& first);

    // Makes `frame` the one every later record() draws; the previous one is no longer referenced
    // on the host, though a submitted batch may still be reading it. GPU thread only.
    //
    // A frame whose source differs from the shown one's is checked against the device before it
    // is adopted, so a decoder that reconfigures mid-stream needs no new renderer, and a refusal
    // leaves the previous frame shown. Throws WallpaperContentError for a format the target
    // cannot take verbatim -- one that is not a single-plane UNORM colour format, or that this
    // device cannot blit from with linear filtering in its tiling -- and std::invalid_argument
    // for a declaration that is wrong on its face: a null image, an extent that is empty or past
    // what this device can hold, a usage without TRANSFER_SRC, or a layout the frame cannot be
    // returned to -- UNDEFINED has no pixels to show, and no barrier transitions back into
    // PREINITIALIZED.
    void show(const VideoFrame& frame);

    [[nodiscard]] VkImageLayout record(VkCommandBuffer cmd, const OffscreenTarget& target) override;

private:
    void requireBlittable(const VideoSource& source) const;

    Device& m_device;
    // A null image until the first frame is adopted, since show() never accepts one. That is
    // what tells "nothing shown yet" from a shown frame whose declaration happens to be all
    // defaults; the source alone cannot, as a caller can supply that value too.
    VideoFrame m_frame;
};

}
