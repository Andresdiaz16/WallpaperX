// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "descriptor_allocator.h"
#include "descriptor_slots.h"

namespace wallpaperx {

class ClassicDescriptorAllocator final : public IDescriptorAllocator {
public:
    // Built from Device mid-construction, so it may only use the logical device.
    explicit ClassicDescriptorAllocator(Device& device);
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

    // Blocks until no command buffer that bound m_set is still pending, which every write to
    // it must do first. Unlike the other two tiers a write here is not a byte range: the set
    // is the unit the spec protects, so writing a binding that pending work never reads --
    // even one never written before -- is as illegal as overwriting the one it samples
    // (VUID-vkUpdateDescriptorSets-None-03047).
    void waitUntilSetIdle();

    Device& m_device;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkDescriptorSet m_set = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;

    // Set by bind() and cleared by the drain. Only ever wrong in the safe direction: a bind
    // whose submit then failed leaves it true, costing one drain of an empty queue.
    bool m_setMayBeInFlight = false;

    // Ownership of the two bindings -- who holds them, and whether a free is a double free.
    // The delay a pool would impose is redundant here, since waitUntilSetIdle() has already
    // drained everything by the time either is acquired.
    DescriptorSlotPool m_imageSlots;
    DescriptorSlotPool m_uniformSlots;
};

}
