// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "descriptor_allocator.h"
#include "descriptor_slots.h"
#include "memory.h"

#include <unordered_map>

namespace wallpaperx {

// VK_EXT_descriptor_heap writes descriptors from VkImageViewCreateInfo /
// VkSamplerCreateInfo, not from handles — callers must register() first.
class DescriptorHeapAllocator final : public IDescriptorAllocator {
public:
    // Built from Device mid-construction, so it may only use what is ready by then: the
    // physical device, the logical device and Memory.
    explicit DescriptorHeapAllocator(Device& device);
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
    void pushData(VkCommandBuffer cmd, uint32_t offset, const void* data, size_t size) override;
    DescriptorTier tier() const override { return DescriptorTier::Heap; }

private:
    void loadFunctions();

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

    // The driver requires each bound heap to carry a reserved range of at least
    // min*HeapReservedRange bytes (VkBindHeapInfoEXT). We carve it out of dedicated
    // space at the end of each heap, past all descriptor storage, so descriptor
    // offsets are unaffected.
    VkDeviceSize m_resourceReservedOffset = 0;
    VkDeviceSize m_resourceReservedSize = 0;
    VkDeviceSize m_samplerReservedOffset = 0;
    VkDeviceSize m_samplerReservedSize = 0;

    // Which slots are live, and the only record of it: the heap bytes themselves say nothing
    // about whether a slot is owned, and a second table saying so could only drift from this.
    DescriptorSlotPool m_imageSlots;
    DescriptorSlotPool m_uniformSlots;

    std::unordered_map<VkImageView, VkImageViewCreateInfo> m_registeredViews;
    std::unordered_map<VkSampler, VkSamplerCreateInfo> m_registeredSamplers;

    PFN_vkWriteResourceDescriptorsEXT m_pfnWriteResource = nullptr;
    PFN_vkWriteSamplerDescriptorsEXT m_pfnWriteSampler = nullptr;
    PFN_vkCmdBindResourceHeapEXT m_pfnCmdBindResourceHeap = nullptr;
    PFN_vkCmdBindSamplerHeapEXT m_pfnCmdBindSamplerHeap = nullptr;
    PFN_vkCmdPushDataEXT m_pfnCmdPushData = nullptr;
};

}
