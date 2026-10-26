// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "../renderers/renderer_base.h"
#include "../vulkan/device.h"
#include "../vulkan/memory.h"
#include "video_decoder.h"
#include "ycbcr_planes.h"

#include "wallpaper_position.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace wallpaperx {

// Draws decoded Y'CbCr pictures straight onto offscreen targets: one draw per target samples each
// plane through a view of its own, scales, converts colour and writes the target, laid out as
// place() says. Scaling filters the planes before converting, so a scaled picture can differ from
// converting at full size and then scaling, at sharp saturated edges; at 1:1 the two agree.
//
// A device picture is read where the decoder wrote it, which must be an optimally tiled image.
// Each image of the decoder's pool gets plane views and a descriptor set the first time it is
// shown, kept until a picture of another pool is shown: a new pool of the same image size keeps
// this renderer and drops only those. The picture shown is held until show() replaces it or this
// renderer is destroyed, and every draw of it waits on and advances its timeline semaphore, so the
// decoder writes that image again only after every draw that read it -- and a redraw of it waits
// for whatever the decoder has submitted against that image since, references included.
//
// A host picture is uploaded into plane images of this renderer's own when it is shown, which
// waits only when the upload from two pictures ago has still not run. Every draw after reads those
// planes, ordered by this queue alone.
//
// Host pictures in any planar Y'CbCr layout ycbcr::layoutFor() describes; device pictures only where
// readsDeviceImage() says so. Colour tags are refused unless ycbcr::colourMapFor() accepts them:
// there is no gamut, transfer or tone conversion.
//
// GPU thread only, destruction included. Must be destroyed before the Device; destruction asks the
// Device whether the GPU has finished with what this renderer made, and leaks it otherwise.
class YcbcrRenderer final : public IRenderer {
public:
    // What a picture must match to be shown here; anything else needs a new renderer.
    struct Shape {
        DecodedFrame::Storage storage = DecodedFrame::Storage::HostPlanes;
        AVPixelFormat format = AV_PIX_FMT_NONE;
        VkExtent2D extent{};
        VkRect2D visible{};
        // DecodedFrame::imageExtent(); zero for host pictures.
        VkExtent2D imageExtent{};

        bool operator==(const Shape& other) const {
            return storage == other.storage && format == other.format && extent.width == other.extent.width &&
                   extent.height == other.extent.height && visible.offset.x == other.visible.offset.x &&
                   visible.offset.y == other.visible.offset.y && visible.extent.width == other.visible.extent.width &&
                   visible.extent.height == other.visible.extent.height &&
                   imageExtent.width == other.imageExtent.width && imageExtent.height == other.imageExtent.height;
        }
    };
    static Shape shapeOf(const DecodedFrame& frame);

    // Whether a decoder's multi-planar image of `swFormat` held in `imageFormat` can be drawn from
    // on `device`. A stream this refuses must be decoded in software; it says nothing about the
    // picture's colour or size.
    static bool readsDeviceImage(const Device& device, AVPixelFormat swFormat, VkFormat imageFormat);

    // Builds for `first`'s shape and shows it, as show() does. Throws WallpaperContentError for a
    // picture it cannot show, std::logic_error for a device picture readsDeviceImage() refuses or
    // whose image is not one optimally tiled, sampleable image of the format it checked,
    // GpuFailure, ResourceExhausted and std::runtime_error as the Device and the shader bundle do.
    // `first` is left as it was on a throw.
    YcbcrRenderer(Device& device, DecodedFrame&& first, const std::string& shaderDir);
    ~YcbcrRenderer() override;

    YcbcrRenderer(const YcbcrRenderer&) = delete;
    YcbcrRenderer& operator=(const YcbcrRenderer&) = delete;

    const Shape& shape() const { return m_shape; }

    // Makes `frame` the picture every later record() draws: a device picture is moved from and held,
    // a host picture is uploaded and left to the caller. A device picture of another pool than the
    // one shown waits for every batch submitted so far, which has almost always run by then.
    // Throws std::invalid_argument for a frame of another shape, std::logic_error as the
    // constructor does for an image it cannot read, WallpaperContentError for colour it cannot
    // render, and what the Device throws waiting, making a pool image's views or uploading; `frame`
    // and the picture shown are left as they were on a throw.
    void show(DecodedFrame&& frame);

    // How every later record() lays the picture on its target; Cover until called. Throws
    // std::invalid_argument for a mode that does not show video, and for a placement
    // placementAllowed() refuses.
    void place(const Placement& placement);

    // For a device picture, only inside submit(), which is what render() calls it from.
    [[nodiscard]] VkImageLayout record(VkCommandBuffer cmd, const OffscreenTarget& target) override;
    void submit(Device& device, const std::function<void(VkCommandBuffer)>& recordBatch) override;

private:
    // One sampled picture's views, one per binding, and the set that binds them.
    struct Source {
        VkImage image = VK_NULL_HANDLE;
        std::array<VkImageView, 3> views{};
        VkDescriptorPool pool = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
    };
    // Which plane a binding samples, and through which swizzle.
    struct Binding {
        uint32_t plane = 0;
        VkComponentSwizzle r = VK_COMPONENT_SWIZZLE_IDENTITY;
    };
    // The host path's own plane images and the uploads that fill them.
    struct UploadPlane {
        AllocatedImage image;
        // Where in the decoded plane this image's texels come from.
        VkRect2D region{};
        uint32_t texelBytes = 0;
    };
    struct Staging {
        AllocatedBuffer buffer;
        Device::SubmitTicket lastUpload;
    };

    void checkDecoderImage(const DecodedFrame::DeviceImage& image) const;
    void createSampling();
    // ycbcr_draw.frag's specialization constants: what the shape fixes, so each renderer's
    // pipeline bakes its own.
    struct Constants {
        float lumaMax[2];
        float lumaShift[2];
        float lumaInv[2];
        float chromaScale[2];
        float chromaMin[2];
        float chromaMax[2];
        float chromaInv[2];
    };
    void createPipeline(const std::string& shaderDir, const Constants& constants);
    void createUploadPlanes();
    // Views of `image`'s planes -- aspect planes of a decoder's image, or this renderer's own
    // single-plane images -- and a set binding them. Frees whatever it made on a throw.
    Source makeSource(VkImage image, bool decoderImage);
    const Source& sourceFor(VkImage image);
    // Replaces every source with one for `image`, of a pool none of them is from.
    const Source& startPool(VkImage image);
    void destroySource(Source& source) noexcept;
    void upload(const DecodedFrame& frame);
    void releaseObjects() noexcept;

    Device& m_device;
    Shape m_shape;
    ycbcr::Planes m_planes;
    std::array<Binding, 3> m_bindings{};
    // The chroma texels the visible picture uses, in the chroma plane's coordinates.
    VkRect2D m_chromaRegion{};
    // Where the image sampled for chroma starts in the chroma plane.
    std::array<float, 2> m_chromaOrigin{};

    VkSampler m_sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;

    // Device shapes: one per image of the held picture's pool shown so far. Host shapes: one, of
    // the upload planes.
    std::vector<Source> m_sources;
    std::vector<UploadPlane> m_uploadPlanes;
    std::array<Staging, 2> m_staging{};
    uint32_t m_nextStaging = 0;

    // What record() draws: the shown picture's set, its colour, and its chroma siting as
    // ycbcr_draw.frag's chromaShift.
    VkDescriptorSet m_shownSet = VK_NULL_HANDLE;
    ycbcr::ColourMap m_colour{};
    std::array<float, 2> m_chromaShift{};
    Placement m_placement;
    // The device picture shown; empty for a host shape.
    std::optional<DecodedFrame> m_held;
    // The image as its lock reads it, while submit() records a batch.
    const DecodedFrame::DeviceImage* m_recording = nullptr;
};

}
