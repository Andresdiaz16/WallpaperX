#pragma once

#include <vulkan/vulkan.h>

#include <string>
#include <unordered_map>

typedef struct VmaAllocator_T* VmaAllocator;
typedef struct VmaAllocation_T* VmaAllocation;

namespace wallpaperx {

// How the CPU will touch a buffer's memory. Read requires host-cached (RANDOM)
// memory: Write may land in write-combined memory, which is fine for upload-only
// buffers but gives stale/uncached results when read back.
enum class HostAccess { Write, Read };

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

    AllocatedBuffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, HostAccess hostAccess, const char* debugName = nullptr);
    void destroyBuffer(AllocatedBuffer& buffer);

    // Makes a mapped host-Read buffer's contents visible to the CPU after a device
    // write (invalidates the cache for non-coherent memory; a no-op otherwise).
    // Call after the copy's fence has signalled and before reading buffer.mapped.
    void invalidateForHostRead(const AllocatedBuffer& buffer) const;

    // Makes host writes to a mapped buffer visible to the device (flushes the
    // cache for non-coherent memory; a no-op otherwise). Call after writing
    // through buffer.mapped and before the GPU work that reads it is submitted.
    void flushForHostWrite(const AllocatedBuffer& buffer) const;

    AllocatedImage createExportableImage(const VkImageCreateInfo& imageInfo, VkExternalMemoryHandleTypeFlagBits exportHandleType, const char* debugName = nullptr);
    AllocatedImage createImage(const VkImageCreateInfo& imageInfo, const char* debugName = nullptr);
    void destroyImage(AllocatedImage& image);
    VkDeviceMemory imageDeviceMemory(const AllocatedImage& image) const;
    VkDeviceSize imageAllocationSize(const AllocatedImage& image) const;

    VmaAllocator handle() const { return m_allocator; }

private:
    // Throws std::runtime_error if `allocation` is not currently tracked as live
    // (already destroyed, or never allocated by this Memory). Callers that hand a
    // freed allocation to VMA/the driver otherwise get undefined behaviour -- e.g.
    // exporting a frame from a destroyed OffscreenTarget crashes inside the driver.
    void requireLiveAllocation(VmaAllocation allocation, const char* context) const;

    VmaAllocator m_allocator = nullptr;

    // Tracks the debug name of every live allocation. Used both for shutdown()'s
    // leaked-allocation diagnostic and by requireLiveAllocation() to reject use of
    // a freed allocation before it reaches VMA/the driver.
    std::unordered_map<VmaAllocation, std::string> m_liveAllocations;
};

}
