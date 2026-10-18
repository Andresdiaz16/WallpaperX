// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "renderer_base.h"
#include "../vulkan/descriptor_allocator.h"
#include "../vulkan/memory.h"

#include "wallpaper_position.h"

#include <sys/types.h>

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

namespace wallpaperx {

class Device;
class DescriptorHeapAllocator;

// A still image decoded and uploaded once, for any number of StaticRenderers to draw. It remembers
// the file it was read from, so a later draw can find that file unchanged without decoding it again.
// Its destructor waits for the GPU as StaticRenderer's does, so it must be released on the GPU thread.
class StillImage {
public:
    // Opens `imagePath` once, by descriptor, as every read of a wallpaper is. When `kept` was read
    // from that same file and nothing has changed it since, `kept` is returned and nothing is
    // decoded. Otherwise `kept` is released before the decode, so this call never holds two copies
    // at once. Throws what a decode or an upload throws.
    static std::shared_ptr<const StillImage> load(Device& device, const std::string& imagePath,
                                                  std::shared_ptr<const StillImage> kept = nullptr);
    ~StillImage();

    StillImage(const StillImage&) = delete;
    StillImage& operator=(const StillImage&) = delete;

    VkExtent2D extent() const { return m_extent; }
    VkImageView view() const { return m_view; }

private:
    // Equal only for the same file with no write, truncation, rename onto it or change of its
    // attributes in between: the change time moves on every one of them and cannot be set back.
    struct FileIdentity {
        dev_t device = 0;
        ino_t inode = 0;
        off_t size = 0;
        timespec modified{};
        timespec changed{};
        bool operator==(const FileIdentity& other) const;
    };

    StillImage(Device& device, FILE* file, const std::string& imagePath, const FileIdentity& identity);
    void upload(FILE* file, const std::string& imagePath);
    void cleanup();

    Device& m_device;
    DescriptorHeapAllocator* m_heapAllocator = nullptr;
    FileIdentity m_file;
    AllocatedImage m_texture;
    VkExtent2D m_extent{};
    VkImageView m_view = VK_NULL_HANDLE;
};

class StaticRenderer final : public IRenderer {
public:
    // Decodes `imagePath` for this renderer alone.
    StaticRenderer(Device& device, VkRenderPass renderPass, const std::string& imagePath, const std::string& shaderDir);
    StaticRenderer(Device& device, VkRenderPass renderPass, std::shared_ptr<const StillImage> image,
                   const std::string& shaderDir);
    ~StaticRenderer() override;

    StaticRenderer(const StaticRenderer&) = delete;
    StaticRenderer& operator=(const StaticRenderer&) = delete;

    // How every later record() lays the image on its target; Cover until called. Throws
    // std::invalid_argument for a placement placementAllowed() refuses.
    void place(const Placement& placement);

    [[nodiscard]] VkImageLayout record(VkCommandBuffer cmd, const OffscreenTarget& target) override;

private:
    void createSampler();
    void createQuad();
    void createPipeline(VkRenderPass renderPass, const std::string& shaderDir);
    void cleanup();

    Device& m_device;
    DescriptorHeapAllocator* m_heapAllocator = nullptr;

    std::shared_ptr<const StillImage> m_image;  // never null
    VkSampler m_sampler = VK_NULL_HANDLE;

    AllocatedBuffer m_vertexBuffer;

    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;

    DescriptorHandle m_descriptorHandle;

    Placement m_placement;
};

}
