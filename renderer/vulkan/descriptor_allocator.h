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

class IDescriptorAllocator {
public:
    virtual ~IDescriptorAllocator() = default;

    virtual DescriptorHandle allocate(VkImageView view, VkSampler sampler) = 0;
    virtual DescriptorHandle allocateUniform(VkBuffer buffer, VkDeviceSize size) = 0;

    // No handle argument: binds the tier's whole backing resource (set /
    // buffer / heap), not one specific allocation.
    virtual void bind(VkCommandBuffer cmd, VkPipelineBindPoint bindPoint) = 0;

    virtual void free(DescriptorHandle handle) = 0;
    virtual DescriptorTier tier() const = 0;
};

}
