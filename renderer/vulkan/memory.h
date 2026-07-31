#pragma once

#include <vulkan/vulkan.h>

#include <string>
#include <unordered_map>
#include <vector>

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

    // True when the image was created with VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT and
    // therefore has a queryable DRM modifier and plane layout. The export path must
    // consult this rather than the device's extension support: creation falls back to
    // the caller's tiling when no modifier qualifies, and querying a modifier from a
    // non-modifier image is a spec violation.
    bool drmModifierTiled = false;
};

// Every DRM format modifier the driver understands for `format`, with its plane
// count. Shared with the export path, which needs the plane count for the modifier
// the driver actually chose -- the two-call query idiom is subtle enough (the count
// is in/out and shrinks on the second call) that duplicating it invites drift.
std::vector<VkDrmFormatModifierPropertiesEXT> queryDrmModifierList(VkPhysicalDevice physicalDevice, VkFormat format);

class Memory {
public:
    void init(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device, uint32_t apiVersion, bool bufferDeviceAddressEnabled, bool drmFormatModifierSupported);
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

    // Creates an image whose memory can be handed to another process as an fd.
    //
    // For DMA_BUF, and only when the device supports VK_EXT_image_drm_format_modifier,
    // the caller's imageInfo.tiling is IGNORED and replaced with
    // VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT, chained to the modifiers this driver
    // supports for the requested format, usage and extent. This is not a detail: an
    // OPTIMAL-tiled image has a vendor-private layout with no modifier to report, so
    // its fd cannot be described to an importer, and querying one is a spec violation
    // (VUID-vkGetImageDrmFormatModifierPropertiesEXT-image-02272).
    //
    // NEVER throws for want of a usable modifier. Both degraded cases -- the extension
    // absent, and the extension present but no modifier satisfying the format, usage
    // and extent -- keep the caller's tiling, warn to stderr, and leave
    // AllocatedImage::drmModifierTiled false; the frame is still rendered and still
    // exportable to a same-device consumer, it just carries no layout metadata. That
    // beats refusing to draw anything at all, but it is silent to callers by design:
    // check drmModifierTiled (or the exported frame's modifier) rather than relying on
    // an exception to detect that a compositor will not be able to import the result.
    //
    // OPAQUE_FD always keeps the caller's tiling: that handle type is only importable
    // by another Vulkan/GL context on the same device, which needs no layout metadata.
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

    // Every DRM format modifier this driver can use for `format` with `usage`, as an
    // exportable dma_buf image. Empty if the combination is unsupported.
    std::vector<uint64_t> supportedDrmModifiers(VkFormat format, VkImageType imageType,
                                                VkImageUsageFlags usage, VkExtent3D extent) const;

    VmaAllocator m_allocator = nullptr;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;  // for the modifier queries above
    bool m_drmFormatModifierSupported = false;

    // Tracks the debug name of every live allocation. Used both for shutdown()'s
    // leaked-allocation diagnostic and by requireLiveAllocation() to reject use of
    // a freed allocation before it reaches VMA/the driver.
    std::unordered_map<VmaAllocation, std::string> m_liveAllocations;
};

}
