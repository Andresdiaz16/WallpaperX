#pragma once

#include <vulkan/vulkan.h>

#include <string>
#include <unordered_map>

typedef struct VmaAllocator_T* VmaAllocator;
typedef struct VmaAllocation_T* VmaAllocation;

namespace wallpaperx {

struct AllocatedBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    void* mapped = nullptr;
};

struct AllocatedImage {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
};

class Memory {
public:
    void init(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device, uint32_t apiVersion, bool bufferDeviceAddressEnabled);
    void shutdown();

    AllocatedBuffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible, const char* debugName = nullptr);
    void destroyBuffer(AllocatedBuffer& buffer);

    AllocatedImage createExportableImage(const VkImageCreateInfo& imageInfo, VkExternalMemoryHandleTypeFlagBits exportHandleType, const char* debugName = nullptr);
    AllocatedImage createImage(const VkImageCreateInfo& imageInfo, const char* debugName = nullptr);
    void destroyImage(AllocatedImage& image);
    VkDeviceMemory imageDeviceMemory(const AllocatedImage& image) const;

    VmaAllocator handle() const { return m_allocator; }

private:
    VmaAllocator m_allocator = nullptr;

    // Debug-name tracking only, for a clear diagnostic if shutdown() finds
    // outstanding allocations -- not consulted anywhere else.
    std::unordered_map<VmaAllocation, std::string> m_liveAllocations;
};

}
