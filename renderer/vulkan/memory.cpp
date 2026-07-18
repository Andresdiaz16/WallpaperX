#define VMA_IMPLEMENTATION
#include "memory.h"
#include "../third_party/vk_mem_alloc.h"

#include <stdexcept>
#include <string>

namespace wallpaperx {

void Memory::init(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device, uint32_t apiVersion) {
    VmaAllocatorCreateInfo info{};
    info.instance = instance;
    info.physicalDevice = physicalDevice;
    info.device = device;
    info.vulkanApiVersion = apiVersion;
    info.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;

    if (vmaCreateAllocator(&info, &m_allocator) != VK_SUCCESS) {
        throw std::runtime_error("Memory: vmaCreateAllocator failed");
    }
}

void Memory::shutdown() {
    if (m_allocator) {
        vmaDestroyAllocator(m_allocator);
        m_allocator = nullptr;
    }
}

AllocatedBuffer Memory::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible, const char* debugName) {
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    if (hostVisible) {
        allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }

    AllocatedBuffer result;
    VmaAllocationInfo outInfo{};
    if (vmaCreateBuffer(m_allocator, &bufferInfo, &allocInfo, &result.buffer, &result.allocation, &outInfo) != VK_SUCCESS) {
        throw std::runtime_error(std::string("Memory: failed to create buffer ") + (debugName ? debugName : ""));
    }
    result.mapped = outInfo.pMappedData;
    return result;
}

void Memory::destroyBuffer(AllocatedBuffer& buffer) {
    if (buffer.buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(m_allocator, buffer.buffer, buffer.allocation);
        buffer.buffer = VK_NULL_HANDLE;
        buffer.allocation = nullptr;
        buffer.mapped = nullptr;
    }
}

}
