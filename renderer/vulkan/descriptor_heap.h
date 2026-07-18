#pragma once

#include "descriptor_allocator.h"
#include "memory.h"

#include <unordered_map>
#include <vector>

namespace wallpaperx {

// VK_EXT_descriptor_heap writes descriptors from VkImageViewCreateInfo /
// VkSamplerCreateInfo, not from handles — callers must register() first.
class DescriptorHeapAllocator final : public IDescriptorAllocator {
public:
    DescriptorHeapAllocator(VkPhysicalDevice physicalDevice, VkDevice device, Memory& memory);
    ~DescriptorHeapAllocator() override;

    DescriptorHeapAllocator(const DescriptorHeapAllocator&) = delete;
    DescriptorHeapAllocator& operator=(const DescriptorHeapAllocator&) = delete;

    void registerImageView(VkImageView view, const VkImageViewCreateInfo& createInfo);
    void registerSampler(VkSampler sampler, const VkSamplerCreateInfo& createInfo);

    DescriptorHandle allocate(VkImageView view, VkSampler sampler) override;
    DescriptorHandle allocateUniform(VkBuffer buffer, VkDeviceSize size) override;
    void bind(VkCommandBuffer cmd, VkPipelineBindPoint bindPoint) override;
    void free(DescriptorHandle handle) override;
    DescriptorTier tier() const override { return DescriptorTier::Heap; }

private:
    void loadFunctions();
    uint32_t acquireSlot();

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    Memory& m_memory;

    VkPhysicalDeviceDescriptorHeapPropertiesEXT m_properties{};
    VkDeviceSize m_resourceStride = 0;

    AllocatedBuffer m_resourceHeap;
    AllocatedBuffer m_samplerHeap;
    VkDeviceAddress m_resourceHeapAddress = 0;
    VkDeviceAddress m_samplerHeapAddress = 0;
    VkDeviceSize m_resourceHeapSize = 0;
    VkDeviceSize m_samplerHeapSize = 0;

    std::vector<uint32_t> m_freeSlots;
    uint32_t m_nextSlot = 0;

    std::unordered_map<VkImageView, VkImageViewCreateInfo> m_registeredViews;
    std::unordered_map<VkSampler, VkSamplerCreateInfo> m_registeredSamplers;

    PFN_vkWriteResourceDescriptorsEXT m_pfnWriteResource = nullptr;
    PFN_vkWriteSamplerDescriptorsEXT m_pfnWriteSampler = nullptr;
    PFN_vkCmdBindResourceHeapEXT m_pfnCmdBindResourceHeap = nullptr;
    PFN_vkCmdBindSamplerHeapEXT m_pfnCmdBindSamplerHeap = nullptr;
};

}
