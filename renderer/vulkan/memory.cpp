// SPDX-License-Identifier: GPL-2.0-or-later

#define VMA_IMPLEMENTATION
#include "memory.h"

#include "drm_formats.h"

#include "../third_party/vk_mem_alloc.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace wallpaperx {

void Memory::init(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device, uint32_t apiVersion, bool bufferDeviceAddressEnabled, bool drmFormatModifierSupported) {
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
    m_physicalDevice = physicalDevice;
    m_drmFormatModifierSupported = drmFormatModifierSupported;
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

std::vector<VkDrmFormatModifierPropertiesEXT> queryDrmModifierList(VkPhysicalDevice physicalDevice, VkFormat format) {
    VkDrmFormatModifierPropertiesListEXT modifierList{VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
    VkFormatProperties2 formatProps{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
    formatProps.pNext = &modifierList;
    vkGetPhysicalDeviceFormatProperties2(physicalDevice, format, &formatProps);

    std::vector<VkDrmFormatModifierPropertiesEXT> all(modifierList.drmFormatModifierCount);
    if (all.empty()) {
        return {};
    }
    modifierList.pDrmFormatModifierProperties = all.data();
    vkGetPhysicalDeviceFormatProperties2(physicalDevice, format, &formatProps);

    // drmFormatModifierCount is in/out: the second call overwrites it with how many
    // entries were actually written, which may be fewer than the first call reported.
    // Without this resize the tail stays value-initialised and reads as modifier 0 --
    // a *valid* DRM_FORMAT_MOD_LINEAR with planeCount 0, not an obvious empty slot.
    all.resize(modifierList.drmFormatModifierCount);
    return all;
}

std::vector<uint64_t> usableDrmModifiers(VkPhysicalDevice physicalDevice, VkFormat format, VkImageType imageType,
                                         VkImageUsageFlags usage, VkExtent3D extent,
                                         VkExternalMemoryFeatureFlags requiredFeatures) {
    const std::vector<VkDrmFormatModifierPropertiesEXT> all = queryDrmModifierList(physicalDevice, format);

    // A modifier appearing in the format's list only means the driver understands the
    // layout -- not that it can render to it, sample it, or export it as a dma_buf.
    // Each has to be re-checked against the actual usage, or vkCreateImage fails later
    // with no indication of which requirement was the problem.
    std::vector<uint64_t> usable;
    for (const auto& candidate : all) {
        // Asked before the capability queries, because this one is about our own importer rather
        // than the driver: a layout it cannot describe is not usable however well the GPU
        // supports it. See kMaxExportedPlanes.
        if (candidate.drmFormatModifierPlaneCount > kMaxExportedPlanes) {
            continue;
        }

        VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifierInfo{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT};
        modifierInfo.drmFormatModifier = candidate.drmFormatModifier;
        modifierInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VkPhysicalDeviceExternalImageFormatInfo externalInfo{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
        externalInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        externalInfo.pNext = &modifierInfo;

        VkPhysicalDeviceImageFormatInfo2 imageFormatInfo{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
        imageFormatInfo.format = format;
        imageFormatInfo.type = imageType;
        imageFormatInfo.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        imageFormatInfo.usage = usage;
        imageFormatInfo.pNext = &externalInfo;

        VkExternalImageFormatProperties externalProps{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 imageFormatProps{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
        imageFormatProps.pNext = &externalProps;

        if (vkGetPhysicalDeviceImageFormatProperties2(physicalDevice, &imageFormatInfo, &imageFormatProps) !=
                VK_SUCCESS ||
            (externalProps.externalMemoryProperties.externalMemoryFeatures & requiredFeatures) !=
                requiredFeatures) {
            continue;
        }
        // The extent is not part of the query, so the returned maxExtent has to be
        // checked by hand. A tiled or compressed modifier can cap out below a large
        // display's resolution; offering it anyway just moves the failure to
        // vkCreateImage, where the error says nothing about which modifier was at
        // fault and the whole list fails with it.
        const VkExtent3D& maxExtent = imageFormatProps.imageFormatProperties.maxExtent;
        if (extent.width > maxExtent.width || extent.height > maxExtent.height ||
            extent.depth > maxExtent.depth) {
            continue;
        }
        usable.push_back(candidate.drmFormatModifier);
    }
    return usable;
}

uint64_t queryImageDrmModifier(VkDevice device, VkImage image) {
    auto pfnGetModifierProps = reinterpret_cast<PFN_vkGetImageDrmFormatModifierPropertiesEXT>(
        vkGetDeviceProcAddr(device, "vkGetImageDrmFormatModifierPropertiesEXT"));
    if (!pfnGetModifierProps) {
        throw std::runtime_error("queryImageDrmModifier: vkGetImageDrmFormatModifierPropertiesEXT entry point missing");
    }

    VkImageDrmFormatModifierPropertiesEXT props{VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT};
    if (pfnGetModifierProps(device, image, &props) != VK_SUCCESS) {
        throw std::runtime_error("queryImageDrmModifier: failed to query the image's DRM format modifier");
    }
    if (props.drmFormatModifier == kDrmModifierInvalid) {
        throw std::runtime_error("queryImageDrmModifier: driver reported DRM_FORMAT_MOD_INVALID for an "
                                 "explicit-modifier image");
    }
    return props.drmFormatModifier;
}

AllocatedImage Memory::createExportableImage(const VkImageCreateInfo& imageInfoIn, VkExternalMemoryHandleTypeFlagBits exportHandleType, const char* debugName) {
    VkExternalMemoryImageCreateInfo externalImageInfo{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    externalImageInfo.handleTypes = exportHandleType;

    VkImageCreateInfo imageInfo = imageInfoIn;
    imageInfo.pNext = &externalImageInfo;

    // Kept alive until vmaCreateDedicatedImage() consumes the pNext chain below.
    std::vector<uint64_t> modifiers;
    VkImageDrmFormatModifierListCreateInfoEXT modifierListInfo{
        VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT};

    bool drmModifierTiled = false;
    if (exportHandleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) {
        // A dma_buf with no explicit modifier is refused rather than degraded. There is
        // no legal consumer for one: an importer handed a dma_buf and no modifier cannot
        // interpret the bytes at all -- that is the entire reason
        // VK_EXT_image_drm_format_modifier exists -- so a frame exported this way is not
        // "worse", it is unusable, and the sentinels it would carry invite the plugin to
        // guess. The earlier degrade-don't-fail policy also produced a branch that no
        // machine here can reach: every ICD available to this project (NVIDIA 3080 Ti and
        // Mesa Lavapipe) supports the extension, so that code would first have executed
        // on a stranger's GPU, having never been run by anyone.
        if (!m_drmFormatModifierSupported) {
            throw std::runtime_error(
                std::string("Memory: cannot export ") + (debugName ? debugName : "(unnamed exportable image)") +
                " as a dma_buf -- VK_EXT_image_drm_format_modifier is unavailable, and a dma_buf without a "
                "DRM format modifier cannot be interpreted by any importer");
        }

        // EXPORTABLE *and* IMPORTABLE. Exportability alone is the wrong question: the
        // artifact this produces is defined by whether a consumer can interpret it, and a
        // driver may advertise a modifier it can hand out but not take back. Offering
        // such a modifier lets the driver pick it, after which exportFrame() publishes it
        // over DBus, the compositor's vkCreateImage/EGL_LINUX_DMA_BUF rejects it, and the
        // wallpaper never appears while GetStatus still reports has_frame=true with no
        // last_error. Narrowing the set trades that for a failure here, where it is
        // attributable and local.
        modifiers = usableDrmModifiers(
            m_physicalDevice, imageInfo.format, imageInfo.imageType, imageInfo.usage, imageInfo.extent,
            VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT);
        if (modifiers.empty()) {
            throw std::runtime_error(
                std::string("Memory: no single-plane DRM format modifier is both exportable and importable for ") +
                std::to_string(imageInfo.extent.width) + "x" + std::to_string(imageInfo.extent.height) +
                " with the requested usage for " + (debugName ? debugName : "(unnamed exportable image)"));
        }

        // Narrowed to what the consumer reported, once it has. Checked after the list above is
        // known non-empty so "this GPU offers nothing" and "we agree on nothing" stay distinct
        // diagnoses. Vulkan's extension leaves this to the application deliberately: its
        // importable bit answers for Vulkan, and the importer here is EGL in another process.
        if (!m_consumerModifiers.empty()) {
            std::vector<uint64_t> agreed;
            for (uint64_t candidate : modifiers) {
                if (std::find(m_consumerModifiers.begin(), m_consumerModifiers.end(), candidate) !=
                    m_consumerModifiers.end()) {
                    agreed.push_back(candidate);
                }
            }
            if (agreed.empty()) {
                throw std::runtime_error(
                    std::string("Memory: this GPU and the frame consumer share no DRM format modifier for ") +
                    (debugName ? debugName : "(unnamed exportable image)") + " (" +
                    std::to_string(modifiers.size()) + " usable here, " +
                    std::to_string(m_consumerModifiers.size()) + " accepted there)");
            }
            modifiers = std::move(agreed);
        }

        // Offering the driver every usable modifier lets it pick its most efficient
        // layout; the one it actually chose is read back right after creation via
        // queryImageDrmModifier (see OffscreenTargetManager::makeTarget).
        modifierListInfo.drmFormatModifierCount = static_cast<uint32_t>(modifiers.size());
        modifierListInfo.pDrmFormatModifiers = modifiers.data();
        modifierListInfo.pNext = &externalImageInfo;

        imageInfo.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        imageInfo.pNext = &modifierListInfo;
        drmModifierTiled = true;
    } else {
        // The same question the modifier loop above asks, for the path that has no modifiers to
        // loop over. VUID-VkImageCreateInfo-pNext-00990 requires every handle type in
        // handleTypes to be reported supported for THIS format/type/tiling/usage combination;
        // nothing checked it here, so an unsupported combination created the image happily and
        // failed later inside vkGetMemoryFdKHR, blaming the export for a decision made here.
        VkPhysicalDeviceExternalImageFormatInfo externalInfo{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
        externalInfo.handleType = exportHandleType;

        VkPhysicalDeviceImageFormatInfo2 formatInfo{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
        formatInfo.format = imageInfo.format;
        formatInfo.type = imageInfo.imageType;
        formatInfo.tiling = imageInfo.tiling;
        formatInfo.usage = imageInfo.usage;
        formatInfo.flags = imageInfo.flags;
        formatInfo.pNext = &externalInfo;

        VkExternalImageFormatProperties externalProps{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 formatProps{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
        formatProps.pNext = &externalProps;

        // EXPORTABLE only, not IMPORTABLE: an opaque fd is imported by a GL context on the same
        // device through GL_EXT_memory_object_fd, which Vulkan's importability bit says nothing
        // about. Requiring it here would refuse combinations that work.
        if (vkGetPhysicalDeviceImageFormatProperties2(m_physicalDevice, &formatInfo, &formatProps) != VK_SUCCESS ||
            (externalProps.externalMemoryProperties.externalMemoryFeatures &
             VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) == 0) {
            throw std::runtime_error(std::string("Memory: this device cannot export ") +
                                     (debugName ? debugName : "(unnamed exportable image)") +
                                     " as an opaque fd with the requested format, tiling and usage");
        }

        // The extent is not part of the query, so the answer has to be checked against it -- the
        // same reason usableDrmModifiers() checks it. Skipping it moves the failure to
        // vkCreateImage, whose error names neither the size nor the export as the cause.
        const VkExtent3D& maxExtent = formatProps.imageFormatProperties.maxExtent;
        if (imageInfo.extent.width > maxExtent.width || imageInfo.extent.height > maxExtent.height ||
            imageInfo.extent.depth > maxExtent.depth) {
            throw std::runtime_error(std::string("Memory: ") + (debugName ? debugName : "(unnamed exportable image)") +
                                     " is larger than this device can export as an opaque fd (" +
                                     std::to_string(imageInfo.extent.width) + "x" +
                                     std::to_string(imageInfo.extent.height) + "x" +
                                     std::to_string(imageInfo.extent.depth) + " exceeds " +
                                     std::to_string(maxExtent.width) + "x" + std::to_string(maxExtent.height) + "x" +
                                     std::to_string(maxExtent.depth) + ")");
        }
    }

    VkExportMemoryAllocateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
    exportInfo.handleTypes = exportHandleType;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

    AllocatedImage result;
    if (vmaCreateDedicatedImage(m_allocator, &imageInfo, &allocInfo, &exportInfo, &result.image, &result.allocation, nullptr) != VK_SUCCESS) {
        throw std::runtime_error(std::string("Memory: failed to create exportable image ") + (debugName ? debugName : ""));
    }
    result.drmModifierTiled = drmModifierTiled;
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
