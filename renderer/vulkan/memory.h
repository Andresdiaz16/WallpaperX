#pragma once

#include <vulkan/vulkan.h>

typedef struct VmaAllocator_T* VmaAllocator;
typedef struct VmaAllocation_T* VmaAllocation;

namespace wallpaperx {

struct AllocatedBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    void* mapped = nullptr;
};

class Memory {
public:
    void init(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device, uint32_t apiVersion);
    void shutdown();

    AllocatedBuffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible, const char* debugName = nullptr);
    void destroyBuffer(AllocatedBuffer& buffer);

    VmaAllocator handle() const { return m_allocator; }

private:
    VmaAllocator m_allocator = nullptr;
};

}
