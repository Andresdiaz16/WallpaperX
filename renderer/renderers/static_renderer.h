#pragma once

#include "renderer_base.h"
#include "../vulkan/descriptor_allocator.h"
#include "../vulkan/memory.h"

#include <cstdint>
#include <string>
#include <vector>

namespace wallpaperx {

class Device;
class DescriptorHeapAllocator;

class StaticRenderer final : public IRenderer {
public:
    StaticRenderer(Device& device, VkRenderPass renderPass, const std::string& imagePath, const std::string& shaderDir);
    ~StaticRenderer() override;

    StaticRenderer(const StaticRenderer&) = delete;
    StaticRenderer& operator=(const StaticRenderer&) = delete;

    void render(VkCommandBuffer cmd, VkFramebuffer framebuffer, uint32_t width, uint32_t height) override;

private:
    void loadAndUploadTexture(const std::string& imagePath);
    void createSampler();
    void createQuad();
    void createPipeline(VkRenderPass renderPass, const std::string& shaderDir);
    void cleanup();

    static std::vector<uint32_t> readSpirv(const std::string& path);
    static VkShaderModule createShaderModule(VkDevice device, const std::vector<uint32_t>& spirv);

    Device& m_device;
    DescriptorHeapAllocator* m_heapAllocator = nullptr;

    AllocatedImage m_texture;
    VkImageView m_textureView = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;

    AllocatedBuffer m_vertexBuffer;

    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;

    DescriptorHandle m_descriptorHandle;
};

}
