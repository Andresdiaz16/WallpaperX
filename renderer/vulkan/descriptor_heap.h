#pragma once

#include "descriptor_allocator.h"
#include "memory.h"

#include <unordered_map>
#include <unordered_set>
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
    void unregisterImageView(VkImageView view);
    void unregisterSampler(VkSampler sampler);

    DescriptorHandle allocate(VkImageView view, VkSampler sampler) override;
    DescriptorHandle allocateUniform(VkBuffer buffer, VkDeviceSize size) override;
    void bind(VkCommandBuffer cmd, VkPipelineBindPoint bindPoint) override;
    void free(DescriptorHandle handle) override;
    DescriptorTier tier() const override { return DescriptorTier::Heap; }

    void pushData(VkCommandBuffer cmd, uint32_t offset, const void* data, size_t size);

private:
    void loadFunctions();
    uint32_t acquireImageSlot();
    uint32_t acquireUniformSlot();

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    Memory& m_memory;

    VkPhysicalDeviceDescriptorHeapPropertiesEXT m_properties{};

    // Images and uniform buffers have different native descriptor sizes, so each
    // gets its own byte region of m_resourceHeap (addressed at its own stride) and
    // its own slot free-list. m_uniformIndexBase is chosen so uniform handle
    // indices never numerically collide with image handle indices (see free()).
    VkDeviceSize m_uniformRegionOffset = 0;
    uint32_t m_uniformIndexBase = 0;

    AllocatedBuffer m_resourceHeap;
    AllocatedBuffer m_samplerHeap;
    VkDeviceAddress m_resourceHeapAddress = 0;
    VkDeviceAddress m_samplerHeapAddress = 0;
    VkDeviceSize m_resourceHeapSize = 0;
    VkDeviceSize m_samplerHeapSize = 0;

    std::vector<uint32_t> m_freeImageSlots;
    uint32_t m_nextImageSlot = 0;
    std::vector<uint32_t> m_freeUniformSlots;
    uint32_t m_nextUniformSlot = 0;

    std::unordered_map<VkImageView, VkImageViewCreateInfo> m_registeredViews;
    std::unordered_map<VkSampler, VkSamplerCreateInfo> m_registeredSamplers;

    std::unordered_map<uint32_t, VkImageView> m_slotView;
    std::unordered_map<uint32_t, VkSampler> m_slotSampler;
    std::unordered_set<uint32_t> m_activeUniformSlots;

    PFN_vkWriteResourceDescriptorsEXT m_pfnWriteResource = nullptr;
    PFN_vkWriteSamplerDescriptorsEXT m_pfnWriteSampler = nullptr;
    PFN_vkCmdBindResourceHeapEXT m_pfnCmdBindResourceHeap = nullptr;
    PFN_vkCmdBindSamplerHeapEXT m_pfnCmdBindSamplerHeap = nullptr;
    PFN_vkCmdPushDataEXT m_pfnCmdPushData = nullptr;
};

}
