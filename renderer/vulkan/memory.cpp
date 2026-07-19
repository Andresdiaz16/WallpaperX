#define VMA_IMPLEMENTATION
#include "memory.h"
#include "../third_party/vk_mem_alloc.h"

#include <cstdio>
#include <stdexcept>
#include <string>

namespace wallpaperx {

void Memory::init(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device, uint32_t apiVersion, bool bufferDeviceAddressEnabled) {
    VmaAllocatorCreateInfo info{};
    info.instance = instance;
    info.physicalDevice = physicalDevice;
    info.device = device;
    info.vulkanApiVersion = apiVersion;
    if (bufferDeviceAddressEnabled) {
        info.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    }

    if (vmaCreateAllocator(&info, &m_allocator) != VK_SUCCESS) {
        throw std::runtime_error("Memory: vmaCreateAllocator failed");
    }
}

void Memory::shutdown() {
    if (m_allocator) {
        // vmaDestroyAllocator() hard-asserts (SIGABRT, uncatchable) if any
        // allocation is still outstanding -- that assert gives no indication of
        // which allocation leaked or from where. Naming it here first, before
        // that assert inevitably fires, turns an opaque crash into an
        // attributable one.
        if (!m_liveAllocations.empty()) {
            std::fprintf(stderr, "Memory::shutdown: %zu GPU allocation(s) were never freed before teardown:\n",
                         m_liveAllocations.size());
            for (const auto& [allocation, name] : m_liveAllocations) {
                std::fprintf(stderr, "  - %s\n", name.c_str());
            }
            std::fflush(stderr);
        }
        vmaDestroyAllocator(m_allocator);
        m_allocator = nullptr;
    }
}

AllocatedBuffer Memory::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, HostAccess hostAccess, const char* debugName) {
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    if (hostAccess == HostAccess::Write) {
        // Upload-only: SEQUENTIAL_WRITE lets VMA pick fast write-combined memory.
        allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    } else if (hostAccess == HostAccess::Read) {
        // Readback: RANDOM forces host-cached memory so CPU reads are coherent.
        allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }

    AllocatedBuffer result;
    VmaAllocationInfo outInfo{};
    if (vmaCreateBuffer(m_allocator, &bufferInfo, &allocInfo, &result.buffer, &result.allocation, &outInfo) != VK_SUCCESS) {
        throw std::runtime_error(std::string("Memory: failed to create buffer ") + (debugName ? debugName : ""));
    }
    result.mapped = outInfo.pMappedData;
    m_liveAllocations[result.allocation] = debugName ? debugName : "(unnamed buffer)";
    return result;
}

void Memory::destroyBuffer(AllocatedBuffer& buffer) {
    if (buffer.buffer != VK_NULL_HANDLE) {
        m_liveAllocations.erase(buffer.allocation);
        vmaDestroyBuffer(m_allocator, buffer.buffer, buffer.allocation);
        buffer.buffer = VK_NULL_HANDLE;
        buffer.allocation = nullptr;
        buffer.mapped = nullptr;
    }
}

void Memory::invalidateForHostRead(const AllocatedBuffer& buffer) const {
    // vmaInvalidateAllocation checks the memory's coherence flags internally: it
    // issues vkInvalidateMappedMemoryRanges only for non-coherent memory and is a
    // no-op otherwise, so this is always safe to call before a host read.
    vmaInvalidateAllocation(m_allocator, buffer.allocation, 0, VK_WHOLE_SIZE);
}

void Memory::flushForHostWrite(const AllocatedBuffer& buffer) const {
    // Symmetric with invalidateForHostRead: vmaFlushAllocation issues
    // vkFlushMappedMemoryRanges only for non-coherent memory and no-ops otherwise,
    // so this is always safe to call after a host write and before GPU submission.
    vmaFlushAllocation(m_allocator, buffer.allocation, 0, VK_WHOLE_SIZE);
}

AllocatedImage Memory::createExportableImage(const VkImageCreateInfo& imageInfoIn, VkExternalMemoryHandleTypeFlagBits exportHandleType, const char* debugName) {
    VkExternalMemoryImageCreateInfo externalImageInfo{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    externalImageInfo.handleTypes = exportHandleType;

    VkImageCreateInfo imageInfo = imageInfoIn;
    imageInfo.pNext = &externalImageInfo;

    VkExportMemoryAllocateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
    exportInfo.handleTypes = exportHandleType;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

    AllocatedImage result;
    if (vmaCreateDedicatedImage(m_allocator, &imageInfo, &allocInfo, &exportInfo, &result.image, &result.allocation, nullptr) != VK_SUCCESS) {
        throw std::runtime_error(std::string("Memory: failed to create exportable image ") + (debugName ? debugName : ""));
    }
    m_liveAllocations[result.allocation] = debugName ? debugName : "(unnamed exportable image)";
    return result;
}

AllocatedImage Memory::createImage(const VkImageCreateInfo& imageInfo, const char* debugName) {
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;

    AllocatedImage result;
    if (vmaCreateImage(m_allocator, &imageInfo, &allocInfo, &result.image, &result.allocation, nullptr) != VK_SUCCESS) {
        throw std::runtime_error(std::string("Memory: failed to create image ") + (debugName ? debugName : ""));
    }
    m_liveAllocations[result.allocation] = debugName ? debugName : "(unnamed image)";
    return result;
}

void Memory::destroyImage(AllocatedImage& image) {
    if (image.image != VK_NULL_HANDLE) {
        m_liveAllocations.erase(image.allocation);
        vmaDestroyImage(m_allocator, image.image, image.allocation);
        image.image = VK_NULL_HANDLE;
        image.allocation = nullptr;
    }
}

void Memory::requireLiveAllocation(VmaAllocation allocation, const char* context) const {
    // Best-effort: VMA may recycle a freed VmaAllocation handle value, so a
    // reused handle can read as "live". This reliably catches the common
    // use-after-free (freed and not yet reused) and turns it into a clean,
    // catchable error instead of a driver-side crash -- it is not a proof of
    // validity.
    if (m_liveAllocations.find(allocation) == m_liveAllocations.end()) {
        throw std::runtime_error(std::string(context) +
                                 ": allocation is not live (already destroyed or never allocated)");
    }
}

VkDeviceMemory Memory::imageDeviceMemory(const AllocatedImage& image) const {
    requireLiveAllocation(image.allocation, "Memory::imageDeviceMemory");
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(m_allocator, image.allocation, &info);
    return info.deviceMemory;
}

VkDeviceSize Memory::imageAllocationSize(const AllocatedImage& image) const {
    requireLiveAllocation(image.allocation, "Memory::imageAllocationSize");
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(m_allocator, image.allocation, &info);
    return info.size;
}

}
