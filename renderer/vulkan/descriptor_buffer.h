// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "descriptor_allocator.h"
#include "descriptor_slots.h"
#include "memory.h"

namespace wallpaperx {

class DescriptorBufferAllocator final : public IDescriptorAllocator {
public:
    // Built from Device mid-construction, so it may only use what is ready by then: the
    // physical device, the logical device and Memory.
    explicit DescriptorBufferAllocator(Device& device);
    ~DescriptorBufferAllocator() override;

    DescriptorBufferAllocator(const DescriptorBufferAllocator&) = delete;
    DescriptorBufferAllocator& operator=(const DescriptorBufferAllocator&) = delete;

    DescriptorHandle allocate(VkImageView view, VkSampler sampler) override;
    DescriptorHandle allocateUniform(VkBuffer buffer, VkDeviceSize size) override;
    void bind(VkCommandBuffer cmd, VkPipelineBindPoint bindPoint) override;
    void free(DescriptorHandle handle) override;
    DescriptorTier tier() const override { return DescriptorTier::Buffer; }

    VkDescriptorSetLayout setLayout() const { return m_setLayout; }

private:
    static constexpr uint32_t kImageBinding = 0;
    static constexpr uint32_t kUniformBinding = 1;

    void loadFunctions();

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    Memory& m_memory;

    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    AllocatedBuffer m_buffer;
    VkDeviceAddress m_bufferAddress = 0;
    VkDeviceSize m_imageOffset = 0;
    VkDeviceSize m_uniformOffset = 0;

    PFN_vkGetDescriptorSetLayoutSizeEXT m_pfnGetLayoutSize = nullptr;
    PFN_vkGetDescriptorSetLayoutBindingOffsetEXT m_pfnGetBindingOffset = nullptr;
    PFN_vkGetDescriptorEXT m_pfnGetDescriptor = nullptr;
    PFN_vkCmdBindDescriptorBuffersEXT m_pfnCmdBindBuffers = nullptr;
    PFN_vkCmdSetDescriptorBufferOffsetsEXT m_pfnCmdSetOffsets = nullptr;

    VkPhysicalDeviceDescriptorBufferPropertiesEXT m_properties{};

    // One binding each, at a fixed offset in m_buffer, so re-allocating rewrites the exact
    // bytes the last draw read. Capacity 1 makes that a wait for the previous batch rather
    // than a race -- this tier has nowhere else to put a descriptor.
    DescriptorSlotPool m_imageSlots;
    DescriptorSlotPool m_uniformSlots;
};

}
