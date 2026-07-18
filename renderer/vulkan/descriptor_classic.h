#pragma once

#include "descriptor_allocator.h"

namespace wallpaperx {

class ClassicDescriptorAllocator final : public IDescriptorAllocator {
public:
    explicit ClassicDescriptorAllocator(VkDevice device);
    ~ClassicDescriptorAllocator() override;

    ClassicDescriptorAllocator(const ClassicDescriptorAllocator&) = delete;
    ClassicDescriptorAllocator& operator=(const ClassicDescriptorAllocator&) = delete;

    DescriptorHandle allocate(VkImageView view, VkSampler sampler) override;
    DescriptorHandle allocateUniform(VkBuffer buffer, VkDeviceSize size) override;
    void bind(VkCommandBuffer cmd, VkPipelineBindPoint bindPoint) override;
    void free(DescriptorHandle handle) override;
    DescriptorTier tier() const override { return DescriptorTier::Classic; }

    VkDescriptorSetLayout setLayout() const { return m_setLayout; }

private:
    static constexpr uint32_t kImageBinding = 0;
    static constexpr uint32_t kUniformBinding = 1;

    VkDevice m_device = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkDescriptorSet m_set = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    bool m_imageOccupied = false;
    bool m_uniformOccupied = false;
};

}
