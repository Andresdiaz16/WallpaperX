// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

namespace wallpaperx {

enum class DescriptorTier { Heap, Buffer, Classic };

const char* toString(DescriptorTier tier);

struct DescriptorHandle {
    uint32_t index = UINT32_MAX;

    bool valid() const { return index != UINT32_MAX; }
};

inline constexpr uint32_t kMaxDescriptorSlots = 64;

// The push data every tier offers a pipeline drawing against its descriptors. The heap tier's is
// native; the classic and buffer tiers declare this range in the pipeline layout bind() uses.
inline constexpr uint32_t kPushDataBytes = 32;
inline constexpr VkPushConstantRange kPushConstantRange{
    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, kPushDataBytes};

class IDescriptorAllocator {
public:
    virtual ~IDescriptorAllocator() = default;

    virtual DescriptorHandle allocate(VkImageView view, VkSampler sampler) = 0;
    virtual DescriptorHandle allocateUniform(VkBuffer buffer, VkDeviceSize size) = 0;

    // No handle argument: binds the tier's whole backing resource (set /
    // buffer / heap), not one specific allocation.
    virtual void bind(VkCommandBuffer cmd, VkPipelineBindPoint bindPoint) = 0;

    // Visible to every stage. After the pipeline is bound; `offset + size` within kPushDataBytes.
    virtual void pushData(VkCommandBuffer cmd, uint32_t offset, const void* data, size_t size) = 0;

    virtual void free(DescriptorHandle handle) = 0;
    virtual DescriptorTier tier() const = 0;
};

}
