// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// A B8G8R8A8 image filled from host bytes and lent to a VideoRenderer, the way a decoder lends
// one. Test-facing only.

#include "renderers/video_renderer.h"
#include "vulkan/device.h"
#include "vulkan/memory.h"

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace wallpaperx::testing {

class LentFrame {
public:
    static constexpr VkFormat kFormat = VK_FORMAT_B8G8R8A8_UNORM;
    // SAMPLED makes SHADER_READ_ONLY_OPTIMAL a legal layout to declare; TRANSFER_DST is how the
    // bytes get in.
    static constexpr VkImageUsageFlags kUsage =
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    LentFrame(Device& device, uint32_t width, uint32_t height, VkImageLayout declaredLayout,
              const std::vector<unsigned char>& bgra, const char* name)
        : m_device(device), m_layout(declaredLayout) {
        if (bgra.size() != static_cast<size_t>(width) * height * 4) {
            throw std::invalid_argument("LentFrame: pixel buffer does not match the extent");
        }
        m_source.format = kFormat;
        m_source.tiling = VK_IMAGE_TILING_OPTIMAL;
        m_source.width = width;
        m_source.height = height;
        m_source.usage = kUsage;

        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = kFormat;
        info.extent = {width, height, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = kUsage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        m_image = m_device.memory().createImage(info, name);
        try {
            upload(bgra);
        } catch (...) {
            release();
            throw;
        }
    }

    LentFrame(const LentFrame&) = delete;
    LentFrame& operator=(const LentFrame&) = delete;

    ~LentFrame() { release(); }

    VideoFrame frame() const {
        VideoFrame frame;
        frame.source = m_source;
        frame.image = m_image.image;
        frame.layout = m_layout;
        return frame;
    }

    VkImage image() const { return m_image.image; }
    uint32_t width() const { return m_source.width; }
    uint32_t height() const { return m_source.height; }
    VkImageLayout declaredLayout() const { return m_layout; }

private:
    // safeToFree(), not waitForSubmissions(): the wait throws on a wedged GPU, and from a
    // destructor that is std::terminate and the probe's verdict is lost.
    void release() {
        if (m_image.image == VK_NULL_HANDLE) return;
        if (!m_device.safeToFree("a probe's lent video frame")) return;
        m_device.memory().destroyImage(m_image);
    }

    void upload(const std::vector<unsigned char>& bgra) {
        AllocatedBuffer staging = m_device.memory().createBuffer(bgra.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                                 HostAccess::Write, "lent-frame-upload");
        struct Guard {
            Device& device;
            AllocatedBuffer& buffer;
            ~Guard() { device.memory().destroyBuffer(buffer); }
        } guard{m_device, staging};

        std::memcpy(staging.mapped, bgra.data(), bgra.size());
        m_device.memory().flushForHostWrite(staging);

        m_device.immediateSubmit([&](VkCommandBuffer cmd) {
            VkImageMemoryBarrier toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toDst.image = m_image.image;
            toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &toDst);

            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {m_source.width, m_source.height, 1};
            vkCmdCopyBufferToImage(cmd, staging.buffer, m_image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                   &region);

            VkImageMemoryBarrier toDeclared = toDst;
            toDeclared.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toDeclared.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
            toDeclared.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toDeclared.newLayout = m_layout;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &toDeclared);
        });
    }

    Device& m_device;
    VideoSource m_source;
    AllocatedImage m_image;
    VkImageLayout m_layout;
};

}  // namespace wallpaperx::testing
