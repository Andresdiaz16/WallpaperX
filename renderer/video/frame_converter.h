// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "../renderers/video_renderer.h"
#include "../vulkan/device.h"
#include "../vulkan/memory.h"
#include "video_decoder.h"

#include <array>
#include <cstdint>
#include <string>

namespace wallpaperx {

// The conversion pass's push constants, defined with the shader's layout in the .cpp.
struct ConvertPush;

// Turns decoded Y'CbCr pictures into one B8G8R8A8_UNORM image that VideoRenderer can show:
// single-plane, optimal-tiled, graphics-family owned, in TRANSFER_SRC_OPTIMAL from the moment
// this converter exists (black until the first conversion), and cropped to the visible
// rectangle.
//
// Every Vulkan object is created once, for the picture the first frame describes. A frame's
// planes reach the pass by copy -- plane by plane out of the decoder's multi-planar image, or
// up from host memory -- into single-plane images this converter owns, and a fragment pass
// samples those. Converting a frame therefore creates no Vulkan object, writes no descriptor,
// and allocates no host memory of this converter's own; the semaphore lists that
// DecodedFrame::submit and Device::submit build for a device frame are theirs. The device path
// never waits; the host path waits only when the upload from two frames ago has still not run.
// The plane images hold only the samples the visible rectangle covers, so filtering never reads
// the codec's padding.
//
// Any planar Y'CbCr layout ffmpeg describes is read from host memory -- any chroma subsampling,
// two or three chroma planes, 8 to 16 bits. A device image is read only where readsDeviceImage()
// says so, and the decoder asks that before decoding on the device. The conversion matrix,
// range and chroma siting are read off each frame. Primaries, transfer and matrix are each
// accepted only from a named set -- BT.709-class gamuts, SDR gamma curves, plain Y'CbCr
// matrices other than BT.2020's -- and a picture tagged with anything else is refused rather
// than shown wrong: this pass does no gamut, transfer or tone conversion.
//
// GPU thread only, destruction included. Must be destroyed before the Device. The image is
// lent, in VideoRenderer's sense: the batch that must outlive it is the last render that
// blitted from it, and this converter's destructor asks the Device whether that has happened.
class FrameConverter {
public:
    // What a frame must match to be converted here; anything else needs a new converter.
    struct Picture {
        DecodedFrame::Storage storage = DecodedFrame::Storage::HostPlanes;
        AVPixelFormat format = AV_PIX_FMT_NONE;
        VkExtent2D extent{};
        VkRect2D visible{};

        bool operator==(const Picture& other) const {
            return storage == other.storage && format == other.format && extent.width == other.extent.width &&
                   extent.height == other.extent.height && visible.offset.x == other.visible.offset.x &&
                   visible.offset.y == other.visible.offset.y && visible.extent.width == other.visible.extent.width &&
                   visible.extent.height == other.visible.extent.height;
        }
    };
    static Picture pictureOf(const DecodedFrame& frame);

    // Whether this pass reads a decoder's multi-planar image of `swFormat` held in `imageFormat`
    // on `device`. A stream this refuses must be decoded in software, whose frames this pass
    // reads in more layouts; it says nothing about the picture's colour or size.
    static bool readsDeviceImage(const Device& device, AVPixelFormat swFormat, VkFormat imageFormat);

    // Throws WallpaperContentError for a picture it cannot convert, std::logic_error for a
    // device frame readsDeviceImage() refuses, GpuFailure and std::runtime_error as the Device
    // and the shader bundle do.
    FrameConverter(Device& device, const DecodedFrame& first, const std::string& shaderDir);
    ~FrameConverter();

    FrameConverter(const FrameConverter&) = delete;
    FrameConverter& operator=(const FrameConverter&) = delete;

    // The image every convert() writes, as VideoRenderer::show() takes it. Constant for the
    // life of this converter.
    const VideoFrame& frame() const { return m_frame; }
    const Picture& picture() const { return m_picture; }

    // Records and submits one batch that writes the image from `frame`, ordered after the
    // decoder's write of a device frame through DecodedFrame::submit -- which is what advances
    // the frame's semaphore value, hence the mutable frame. Throws std::invalid_argument for a
    // frame whose picture differs from picture(), WallpaperContentError for colour it cannot
    // render, and what Device::submit throws; nothing is submitted on a throw.
    Device::SubmitTicket convert(DecodedFrame& frame);

private:
    struct Plane {
        AllocatedImage image;
        // Where in the decoded plane this image's texels come from.
        VkRect2D region{};
        VkFormat format = VK_FORMAT_UNDEFINED;
        uint32_t texelBytes = 0;
    };
    // Which plane image a fragment binding samples, and through which swizzle.
    struct Binding {
        uint32_t plane = 0;
        VkComponentSwizzle r = VK_COMPONENT_SWIZZLE_IDENTITY;
        VkImageView view = VK_NULL_HANDLE;
    };
    struct Staging {
        AllocatedBuffer buffer;
        Device::SubmitTicket lastUpload;
    };

    void createPlanes();
    void createOutput();
    void createSampling();
    void createPipeline(const std::string& shaderDir);
    void clearOutput();
    // Frees every object without asking whether the GPU is done with it.
    void releaseObjects() noexcept;

    ConvertPush pushFor(const DecodedFrame& frame) const;
    void fillStaging(const DecodedFrame& frame, Staging& staging) const;
    // The two transitions every conversion takes the plane images through.
    enum class PlaneTransition { ToCopyTarget, ToSampled };
    // Writes one barrier per plane, from `out` on.
    void planeBarriers(PlaneTransition transition, VkImageMemoryBarrier* out) const;
    void recordDevice(VkCommandBuffer cmd, const DecodedFrame::DeviceImage& source, const ConvertPush& push);
    void recordHost(VkCommandBuffer cmd, const Staging& staging, const ConvertPush& push);
    void recordPass(VkCommandBuffer cmd, const ConvertPush& push);

    Device& m_device;
    Picture m_picture;
    uint32_t m_planeCount = 0;
    // Multiplies a sampled plane value back to its code value: 255 for 8-bit samples; for a
    // code stored in the top bits of a 16-bit sample, the largest code (1023 for 10-bit);
    // 65535 for one stored in the bottom bits.
    float m_sampleToCode = 0.0f;

    std::array<Plane, 3> m_planes{};
    std::array<Binding, 3> m_bindings{};
    VkSampler m_sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_set = VK_NULL_HANDLE;

    AllocatedImage m_output;
    VkImageView m_outputView = VK_NULL_HANDLE;
    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    VkFramebuffer m_framebuffer = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VideoFrame m_frame;

    // Host frames only, made on the first one: two uploads in flight is all one queue can use.
    std::array<Staging, 2> m_staging{};
    uint32_t m_nextStaging = 0;
};

}
