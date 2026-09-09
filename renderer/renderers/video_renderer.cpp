// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_renderer.h"

#include "../vulkan/device.h"
#include "../vulkan/gpu_fault.h"
#include "../vulkan/memory_export.h"
#include "../vulkan/offscreen.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>

namespace wallpaperx {

namespace {

constexpr VkImageSubresourceRange kWholeImage{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
constexpr VkImageSubresourceLayers kBaseLayer{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
constexpr VkImageLayout kBlitSourceLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

// What vkCmdBlitImage with VK_FILTER_LINEAR needs of its source format. A Y'CbCr format has
// neither, so this is what turns NV12 away.
constexpr VkFormatFeatureFlags kSourceFeatures =
    VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;

// The single-plane colour formats a blit moves into a UNORM target byte-for-byte in meaning.
// The target holds whatever encoding the source had and every consumer treats it as-is, so a
// source the blit would decode on read -- an sRGB format -- or one with no encoding to keep
// -- a float -- would land with the wrong gamma. Being on this list is also what makes the
// COLOR aspect the image's only one.
bool isUnormColour(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
        case VK_FORMAT_R8G8B8_UNORM:
        case VK_FORMAT_B8G8R8_UNORM:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
        case VK_FORMAT_R16G16B16A16_UNORM:
        case VK_FORMAT_R16G16B16_UNORM:
            return true;
        default:
            return false;
    }
}

// The formats a decoder or a conversion pass plausibly hands over. Anything else is named by
// number, which is still a lookup away rather than nothing.
std::string formatName(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
        case VK_FORMAT_A8B8G8R8_UNORM_PACK32: return "A8B8G8R8_UNORM_PACK32";
        case VK_FORMAT_R8G8B8_UNORM: return "R8G8B8_UNORM";
        case VK_FORMAT_B8G8R8_UNORM: return "B8G8R8_UNORM";
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "A2B10G10R10_UNORM_PACK32";
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32: return "A2R10G10B10_UNORM_PACK32";
        case VK_FORMAT_R16G16B16A16_UNORM: return "R16G16B16A16_UNORM";
        case VK_FORMAT_R16G16B16_UNORM: return "R16G16B16_UNORM";
        case VK_FORMAT_R8G8B8A8_SRGB: return "R8G8B8A8_SRGB";
        case VK_FORMAT_B8G8R8A8_SRGB: return "B8G8R8A8_SRGB";
        case VK_FORMAT_R16G16B16A16_SFLOAT: return "R16G16B16A16_SFLOAT";
        case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM: return "G8_B8R8_2PLANE_420_UNORM (NV12)";
        case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16: return "G10X6_B10X6R10X6_2PLANE_420_UNORM (P010)";
        case VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM: return "G8_B8_R8_3PLANE_420_UNORM (YUV420P)";
        default: return "VkFormat(" + std::to_string(static_cast<int>(format)) + ")";
    }
}

std::string tilingName(VkImageTiling tiling) {
    switch (tiling) {
        case VK_IMAGE_TILING_OPTIMAL: return "optimal";
        case VK_IMAGE_TILING_LINEAR: return "linear";
        case VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT: return "DRM format modifier";
        default: return "VkImageTiling(" + std::to_string(static_cast<int>(tiling)) + ")";
    }
}

VkFormatFeatureFlags tilingFeatures(VkPhysicalDevice physicalDevice, VkFormat format, VkImageTiling tiling) {
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &properties);
    return tiling == VK_IMAGE_TILING_LINEAR ? properties.linearTilingFeatures : properties.optimalTilingFeatures;
}

uint32_t maxImageDimension2D(VkPhysicalDevice physicalDevice) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physicalDevice, &properties);
    return properties.limits.maxImageDimension2D;
}

VkImageMemoryBarrier layoutBarrier(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                                   VkAccessFlags srcAccess, VkAccessFlags dstAccess) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = kWholeImage;
    return barrier;
}

struct Crop {
    int32_t x0, y0, x1, y1;
};

// The centred region of a frame with the target's aspect ratio. Products rather than divided
// ratios so the comparison is exact; the region is never larger than the frame and never
// empty, whatever the two shapes are. Both extents are within maxImageDimension2D, so the
// narrowing to VkOffset3D's int32_t cannot wrap.
Crop coverCrop(uint32_t frameWidth, uint32_t frameHeight, uint32_t targetWidth, uint32_t targetHeight) {
    const uint64_t frameWide = static_cast<uint64_t>(frameWidth) * targetHeight;
    const uint64_t targetWide = static_cast<uint64_t>(targetWidth) * frameHeight;

    uint64_t width = frameWidth;
    uint64_t height = frameHeight;
    if (frameWide > targetWide) {
        width = std::max<uint64_t>(1, static_cast<uint64_t>(frameHeight) * targetWidth / targetHeight);
    } else if (frameWide < targetWide) {
        height = std::max<uint64_t>(1, static_cast<uint64_t>(frameWidth) * targetHeight / targetWidth);
    }

    const uint64_t x0 = (frameWidth - width) / 2;
    const uint64_t y0 = (frameHeight - height) / 2;
    return {static_cast<int32_t>(x0), static_cast<int32_t>(y0), static_cast<int32_t>(x0 + width),
            static_cast<int32_t>(y0 + height)};
}

void requireShowable(const VideoFrame& frame) {
    if (frame.image == VK_NULL_HANDLE) {
        throw std::invalid_argument("VideoRenderer: the frame has no image");
    }
    if (frame.layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        throw std::invalid_argument("VideoRenderer: a frame in VK_IMAGE_LAYOUT_UNDEFINED has no pixels to show");
    }
    if (frame.layout == VK_IMAGE_LAYOUT_PREINITIALIZED) {
        throw std::invalid_argument(
            "VideoRenderer: a frame cannot be returned to VK_IMAGE_LAYOUT_PREINITIALIZED; transition it to "
            "GENERAL first");
    }
}

}  // namespace

VideoRenderer::VideoRenderer(Device& device, const VideoFrame& first) : m_device(device) {
    m_device.assertGpuThread("VideoRenderer::VideoRenderer");

    // A target's tiling follows the handle type its memory is exported with: a dma_buf target
    // carries a modifier usableDrmModifiers() has already held to kFormatFeatures, and an
    // opaque-fd one is optimal-tiled, which is the one tiling left to ask about.
    const bool opaque = externalMemoryHandleType(m_device.info().vendor, m_device.info().hasExternalMemoryDmaBuf) ==
                        VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    if (opaque) {
        const VkFormatFeatureFlags targetFeatures =
            tilingFeatures(m_device.physicalDevice(), OffscreenTargetManager::kFormat, VK_IMAGE_TILING_OPTIMAL);
        if ((targetFeatures & OffscreenTargetManager::kFormatFeatures) != OffscreenTargetManager::kFormatFeatures) {
            throw GpuFailure("VideoRenderer: " + device.info().gpuName + " cannot blit into an optimal-tiled " +
                             formatName(OffscreenTargetManager::kFormat) + " image, which every offscreen target is");
        }
    }

    show(first);
}

void VideoRenderer::requireBlittable(const VideoSource& source) const {
    const uint32_t limit = maxImageDimension2D(m_device.physicalDevice());
    if (source.width == 0 || source.height == 0 || source.width > limit || source.height > limit) {
        throw std::invalid_argument("VideoRenderer: the source is " + std::to_string(source.width) + "x" +
                                    std::to_string(source.height) + ", which " + m_device.info().gpuName +
                                    " cannot hold as an image (1 to " + std::to_string(limit) + " on a side)");
    }
    if ((source.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0) {
        throw std::invalid_argument(
            "VideoRenderer: the source was created without VK_IMAGE_USAGE_TRANSFER_SRC_BIT, so it cannot be blitted "
            "from");
    }
    if (!isUnormColour(source.format)) {
        throw WallpaperContentError("VideoRenderer: " + formatName(source.format) +
                                    " is not a single-plane UNORM colour format, so a blit would not carry its "
                                    "pixels into the target unchanged; a Y'CbCr, sRGB or float frame needs converting "
                                    "first");
    }
    if (source.tiling != VK_IMAGE_TILING_OPTIMAL && source.tiling != VK_IMAGE_TILING_LINEAR) {
        throw WallpaperContentError("VideoRenderer: a source's blit support is only known for optimal or linear "
                                    "tiling, not " +
                                    tilingName(source.tiling));
    }

    const VkFormatFeatureFlags features = tilingFeatures(m_device.physicalDevice(), source.format, source.tiling);
    if ((features & kSourceFeatures) != kSourceFeatures) {
        throw WallpaperContentError("VideoRenderer: " + formatName(source.format) + " with " +
                                    tilingName(source.tiling) + " tiling cannot be blitted with linear filtering on " +
                                    m_device.info().gpuName);
    }
}

void VideoRenderer::show(const VideoFrame& frame) {
    m_device.assertGpuThread("VideoRenderer::show");
    requireShowable(frame);
    // Checked on the first frame and whenever the declaration changes: a decoder hands over
    // frames from one pool far more often than it rebuilds the pool. Checked before anything is
    // adopted, so a refused frame leaves the previous one shown.
    if (m_frame.image == VK_NULL_HANDLE || frame.source != m_frame.source) requireBlittable(frame.source);
    m_frame = frame;
}

VkImageLayout VideoRenderer::record(VkCommandBuffer cmd, const OffscreenTarget& target) {
    // The blit covers every pixel of the target, so whatever it held is discarded. The frame's
    // device writes are made visible from whichever stage produced them -- a decode, a copy, a
    // draw -- since nothing here knows which; host writes need only have been flushed before
    // the submit, which orders them itself.
    const std::array<VkImageMemoryBarrier, 2> before{
        layoutBarrier(target.image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT),
        layoutBarrier(m_frame.image, m_frame.layout, kBlitSourceLayout, VK_ACCESS_MEMORY_WRITE_BIT,
                      VK_ACCESS_TRANSFER_READ_BIT),
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, static_cast<uint32_t>(before.size()), before.data());

    const Crop crop = coverCrop(m_frame.source.width, m_frame.source.height, target.width, target.height);

    VkImageBlit blit{};
    blit.srcSubresource = kBaseLayer;
    blit.srcOffsets[0] = {crop.x0, crop.y0, 0};
    blit.srcOffsets[1] = {crop.x1, crop.y1, 1};
    blit.dstSubresource = kBaseLayer;
    blit.dstOffsets[0] = {0, 0, 0};
    blit.dstOffsets[1] = {static_cast<int32_t>(target.width), static_cast<int32_t>(target.height), 1};

    vkCmdBlitImage(cmd, m_frame.image, kBlitSourceLayout, target.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &blit, VK_FILTER_LINEAR);

    // Back to the owner's layout, ordered before anything it submits next. The batch either runs
    // whole or not at all, so the frame is in that layout on both sides of every render and no
    // record of it can drift.
    const VkImageMemoryBarrier after = layoutBarrier(m_frame.image, kBlitSourceLayout, m_frame.layout, 0,
                                                     VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &after);

    return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
}

}
