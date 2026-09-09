// SPDX-License-Identifier: GPL-2.0-or-later

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
    // therefore has a queryable DRM modifier and plane layout. Always true for a
    // DMA_BUF export (creation now throws rather than producing an image without one)
    // and always false for OPAQUE_FD; querying a modifier from a non-modifier image is
    // a spec violation, so the export path still checks it rather than inferring.
    bool drmModifierTiled = false;
};

// Every DRM format modifier the driver understands for `format`, with its plane
// count. Shared with the export path, which needs the plane count for the modifier
// the driver actually chose -- the two-call query idiom is subtle enough (the count
// is in/out and shrinks on the second call) that duplicating it invites drift.
std::vector<VkDrmFormatModifierPropertiesEXT> queryDrmModifierList(VkPhysicalDevice physicalDevice, VkFormat format);

// Every DRM format modifier this driver can use for a dma_buf image with
// `format`/`imageType`/`usage` at `extent`, restricted to those advertising every bit
// in `requiredFeatures` and, in their own tiling features, every bit in
// `requiredTilingFeatures`. Empty if none qualify.
//
// Strictly narrower than queryDrmModifierList: appearing in the format's list only
// means the driver understands the layout, not that it can render to it, sample it, or
// hand it across a process boundary at this size.
//
// `requiredTilingFeatures` is for what `usage` does not imply. Image creation already holds a
// modifier to the features each usage bit maps to; a blit destination maps from none, so a
// caller that will blit into the image asks for VK_FORMAT_FEATURE_BLIT_DST_BIT here.
//
// `requiredFeatures` is explicit, with no default, because export and import are
// different questions with different answers. VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT
// is what the export path needs; a caller that intends to *import* a modifier must ask
// for IMPORTABLE_BIT as well, and a driver may advertise one without the other. This
// parameter used to be hardcoded to EXPORTABLE while a test used the result to pick a
// modifier it then imported -- so a modifier that was exportable-only got rejected at
// vkCreateImage, and the test blamed the round trip for a capability mismatch.
std::vector<uint64_t> usableDrmModifiers(VkPhysicalDevice physicalDevice, VkFormat format, VkImageType imageType,
                                         VkImageUsageFlags usage, VkExtent3D extent,
                                         VkExternalMemoryFeatureFlags requiredFeatures,
                                         VkFormatFeatureFlags requiredTilingFeatures);

// Which modifier the driver picked from the list an image was offered. Only legal for an image
// created with VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT -- see AllocatedImage::drmModifierTiled.
uint64_t queryImageDrmModifier(VkDevice device, VkImage image);

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
    // THROWS if a DMA_BUF image cannot be given an explicit DRM format modifier --
    // either because VK_EXT_image_drm_format_modifier is unavailable or because no
    // modifier satisfies the format, usage and extent. This used to degrade to the
    // caller's tiling with a warning, which was wrong twice over: a dma_buf with no
    // modifier cannot be interpreted by any importer, so the "degraded" frame is not
    // worse but unusable; and the branch was unreachable on every ICD available to this
    // project, so it would have first executed on a user's machine having never run
    // once here.
    //
    // OPAQUE_FD always keeps the caller's tiling: that handle type is only importable
    // by another Vulkan/GL context on the same device, which needs no layout metadata.
    //
    // `requiredTilingFeatures` narrows the modifiers offered to those whose tiling supports
    // them -- see usableDrmModifiers. Not consulted on the OPAQUE_FD path, whose tiling is
    // the caller's and whose features the caller can query directly.
    AllocatedImage createExportableImage(const VkImageCreateInfo& imageInfo,
                                         VkExternalMemoryHandleTypeFlagBits exportHandleType,
                                         VkFormatFeatureFlags requiredTilingFeatures, const char* debugName = nullptr);

    // What the consumer that will import these frames says it can take. Vulkan cannot answer
    // this: its importable bit describes Vulkan, and the consumer here is EGL in another
    // process, so the only correct source is the consumer itself. Until one reports, the set
    // is empty and only the plane limit applies.
    //
    // Images already created keep the modifier they were made with -- the caller must destroy
    // and re-create them for a change to take effect. GPU thread only.
    void setConsumerModifiers(std::vector<uint64_t> modifiers) { m_consumerModifiers = std::move(modifiers); }
    const std::vector<uint64_t>& consumerModifiers() const { return m_consumerModifiers; }
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
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;  // for the modifier queries above
    bool m_drmFormatModifierSupported = false;
    std::vector<uint64_t> m_consumerModifiers;

    // Tracks the debug name of every live allocation. Used both for shutdown()'s
    // leaked-allocation diagnostic and by requireLiveAllocation() to reject use of
    // a freed allocation before it reaches VMA/the driver.
    std::unordered_map<VmaAllocation, std::string> m_liveAllocations;
};

}
