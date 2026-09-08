// SPDX-License-Identifier: GPL-2.0-or-later

#include "descriptor_heap.h"

#include "gpu_fault.h"

#include <algorithm>
#include <stdexcept>

namespace wallpaperx {

namespace {
// Rounds up to a multiple of a true power-of-two alignment (Vulkan alignments
// are always powers of two). Do NOT use this for arbitrary sizes -- the bitmask
// is only correct when `alignment` is a power of two.
VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment) {
    return alignment == 0 ? value : (value + alignment - 1) & ~(alignment - 1);
}

// Rounds up to a multiple of an arbitrary (not necessarily power-of-two) size,
// e.g. a descriptor size in bytes, which the spec does not guarantee is a power
// of two.
VkDeviceSize roundUpToMultiple(VkDeviceSize value, VkDeviceSize multiple) {
    return multiple == 0 ? value : ((value + multiple - 1) / multiple) * multiple;
}
}

DescriptorHeapAllocator::DescriptorHeapAllocator(Device& device)
    : m_physicalDevice(device.physicalDevice()),
      m_device(device.device()),
      m_memory(device.memory()),
      m_imageSlots(device, kMaxDescriptorSlots, "DescriptorHeapAllocator: image"),
      m_uniformSlots(device, kMaxDescriptorSlots, "DescriptorHeapAllocator: uniform") {
    loadFunctions();

    m_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT;
    VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    props2.pNext = &m_properties;
    vkGetPhysicalDeviceProperties2(m_physicalDevice, &props2);

    const VkDeviceSize imageRegionSize = static_cast<VkDeviceSize>(m_properties.imageDescriptorSize) * kMaxDescriptorSlots;

    // Uniform region must start after the image region ends in bytes (no physical
    // overlap) and at an index >= kMaxDescriptorSlots (no numeric collision with
    // image handle indices, which occupy [0, kMaxDescriptorSlots)). The offset is
    // aligned twice: first to bufferDescriptorAlignment (the driver's placement
    // rule for writing a buffer-type descriptor, per
    // VkPhysicalDeviceDescriptorHeapPropertiesEXT), then to bufferDescriptorSize
    // (so m_uniformIndexBase divides evenly — required for the GPU's own
    // index*bufferDescriptorSize array addressing to land on the same byte offset
    // this code writes to). bufferDescriptorSize is assumed to itself be a
    // multiple of bufferDescriptorAlignment, which holds for descriptor-heap-style
    // extensions where "size" already accounts for placement padding.
    m_uniformRegionOffset = std::max(
        roundUpToMultiple(alignUp(imageRegionSize, m_properties.bufferDescriptorAlignment),
                          m_properties.bufferDescriptorSize),
        static_cast<VkDeviceSize>(m_properties.bufferDescriptorSize) * kMaxDescriptorSlots);
    m_uniformIndexBase = static_cast<uint32_t>(m_uniformRegionOffset / m_properties.bufferDescriptorSize);
    const VkDeviceSize uniformRegionSize = static_cast<VkDeviceSize>(m_properties.bufferDescriptorSize) * kMaxDescriptorSlots;

    // Descriptor storage occupies [0, descriptorRegion); the driver-required
    // reserved range is appended after it so the two never overlap. heapRange in
    // bind() spans the whole grown buffer.
    const VkDeviceSize resourceDescriptorRegion =
        alignUp(m_uniformRegionOffset + uniformRegionSize, m_properties.resourceHeapAlignment);
    m_resourceReservedOffset = resourceDescriptorRegion;
    m_resourceReservedSize = alignUp(m_properties.minResourceHeapReservedRange, m_properties.resourceHeapAlignment);
    m_resourceHeapSize = m_resourceReservedOffset + m_resourceReservedSize;

    const VkDeviceSize samplerDescriptorRegion = alignUp(
        static_cast<VkDeviceSize>(m_properties.samplerDescriptorSize) * kMaxDescriptorSlots,
        m_properties.samplerHeapAlignment);
    m_samplerReservedOffset = samplerDescriptorRegion;
    m_samplerReservedSize = alignUp(m_properties.minSamplerHeapReservedRange, m_properties.samplerHeapAlignment);
    m_samplerHeapSize = m_samplerReservedOffset + m_samplerReservedSize;

    // If the second createBuffer (or anything after) throws, ~DescriptorHeapAllocator
    // never runs, so the first heap would leak -- and, being registered in Memory's
    // live-allocation set, would later trip its teardown abort. Clean up on throw.
    try {
        m_resourceHeap = m_memory.createBuffer(
            m_resourceHeapSize,
            VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            HostAccess::Write, "descriptor-heap-resource");
        m_samplerHeap = m_memory.createBuffer(
            m_samplerHeapSize,
            VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            HostAccess::Write, "descriptor-heap-sampler");

        VkBufferDeviceAddressInfo resourceAddrInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        resourceAddrInfo.buffer = m_resourceHeap.buffer;
        m_resourceHeapAddress = vkGetBufferDeviceAddress(m_device, &resourceAddrInfo);

        VkBufferDeviceAddressInfo samplerAddrInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        samplerAddrInfo.buffer = m_samplerHeap.buffer;
        m_samplerHeapAddress = vkGetBufferDeviceAddress(m_device, &samplerAddrInfo);
    } catch (...) {
        m_memory.destroyBuffer(m_resourceHeap);
        m_memory.destroyBuffer(m_samplerHeap);
        throw;
    }
}

DescriptorHeapAllocator::~DescriptorHeapAllocator() {
    m_memory.destroyBuffer(m_resourceHeap);
    m_memory.destroyBuffer(m_samplerHeap);
}

void DescriptorHeapAllocator::loadFunctions() {
    m_pfnWriteResource = reinterpret_cast<PFN_vkWriteResourceDescriptorsEXT>(
        vkGetDeviceProcAddr(m_device, "vkWriteResourceDescriptorsEXT"));
    m_pfnWriteSampler = reinterpret_cast<PFN_vkWriteSamplerDescriptorsEXT>(
        vkGetDeviceProcAddr(m_device, "vkWriteSamplerDescriptorsEXT"));
    m_pfnCmdBindResourceHeap = reinterpret_cast<PFN_vkCmdBindResourceHeapEXT>(
        vkGetDeviceProcAddr(m_device, "vkCmdBindResourceHeapEXT"));
    m_pfnCmdBindSamplerHeap = reinterpret_cast<PFN_vkCmdBindSamplerHeapEXT>(
        vkGetDeviceProcAddr(m_device, "vkCmdBindSamplerHeapEXT"));
    m_pfnCmdPushData = reinterpret_cast<PFN_vkCmdPushDataEXT>(
        vkGetDeviceProcAddr(m_device, "vkCmdPushDataEXT"));

    if (!m_pfnWriteResource || !m_pfnWriteSampler || !m_pfnCmdBindResourceHeap || !m_pfnCmdBindSamplerHeap ||
        !m_pfnCmdPushData) {
        // This tier is only selected when the extension was reported and enabled, so a
        // missing entry point is the driver withholding what it advertised.
        throw GpuFailure("DescriptorHeapAllocator: VK_EXT_descriptor_heap entry points missing");
    }
}

void DescriptorHeapAllocator::registerImageView(VkImageView view, const VkImageViewCreateInfo& createInfo) {
    m_registeredViews[view] = createInfo;
}

void DescriptorHeapAllocator::registerSampler(VkSampler sampler, const VkSamplerCreateInfo& createInfo) {
    m_registeredSamplers[sampler] = createInfo;
}

void DescriptorHeapAllocator::unregisterImageView(VkImageView view) {
    m_registeredViews.erase(view);
}

void DescriptorHeapAllocator::unregisterSampler(VkSampler sampler) {
    m_registeredSamplers.erase(sampler);
}

DescriptorHandle DescriptorHeapAllocator::allocate(VkImageView view, VkSampler sampler) {
    auto viewIt = m_registeredViews.find(view);
    if (viewIt == m_registeredViews.end()) {
        throw std::runtime_error(
            "DescriptorHeapAllocator::allocate: image view was never registered via registerImageView()");
    }
    auto samplerIt = m_registeredSamplers.find(sampler);
    if (samplerIt == m_registeredSamplers.end()) {
        throw std::runtime_error(
            "DescriptorHeapAllocator::allocate: sampler was never registered via registerSampler()");
    }

    // After the registration checks: acquiring can wait for the GPU, and a caller that was
    // going to be rejected anyway should not pay for that.
    const uint32_t slot = m_imageSlots.acquire();

    VkImageDescriptorInfoEXT imageDescInfo{VK_STRUCTURE_TYPE_IMAGE_DESCRIPTOR_INFO_EXT};
    imageDescInfo.pView = &viewIt->second;
    imageDescInfo.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkResourceDescriptorInfoEXT resourceInfo{VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT};
    resourceInfo.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    resourceInfo.data.pImage = &imageDescInfo;

    VkHostAddressRangeEXT resourceDst{};
    resourceDst.address = static_cast<uint8_t*>(m_resourceHeap.mapped) + slot * m_properties.imageDescriptorSize;
    resourceDst.size = m_properties.imageDescriptorSize;
    // Released as unwritten on both failure paths: the slot was acquired inside this call, so
    // no handle naming it ever escaped and no submitted batch can be reading it. Stamping it
    // with the latest submission instead would gate its reuse on a batch that never saw it.
    if (const VkResult r = m_pfnWriteResource(m_device, 1, &resourceInfo, &resourceDst); r != VK_SUCCESS) {
        m_imageSlots.releaseUnwritten(slot);
        throwVkFailure("DescriptorHeapAllocator: vkWriteResourceDescriptorsEXT", r);
    }

    VkHostAddressRangeEXT samplerDst{};
    samplerDst.address = static_cast<uint8_t*>(m_samplerHeap.mapped) + slot * m_properties.samplerDescriptorSize;
    samplerDst.size = m_properties.samplerDescriptorSize;
    if (const VkResult r = m_pfnWriteSampler(m_device, 1, &samplerIt->second, &samplerDst); r != VK_SUCCESS) {
        m_imageSlots.releaseUnwritten(slot);
        throwVkFailure("DescriptorHeapAllocator: vkWriteSamplerDescriptorsEXT", r);
    }

    m_memory.flushForHostWrite(m_resourceHeap);
    m_memory.flushForHostWrite(m_samplerHeap);

    return DescriptorHandle{slot};
}

DescriptorHandle DescriptorHeapAllocator::allocateUniform(VkBuffer buffer, VkDeviceSize size) {
    const uint32_t slot = m_uniformSlots.acquire();

    VkBufferDeviceAddressInfo addrInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    addrInfo.buffer = buffer;

    VkDeviceAddressRangeKHR range{};
    range.address = vkGetBufferDeviceAddress(m_device, &addrInfo);
    range.size = size;

    VkResourceDescriptorInfoEXT resourceInfo{VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT};
    resourceInfo.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    resourceInfo.data.pAddressRange = &range;

    VkHostAddressRangeEXT dst{};
    dst.address =
        static_cast<uint8_t*>(m_resourceHeap.mapped) + m_uniformRegionOffset + slot * m_properties.bufferDescriptorSize;
    dst.size = m_properties.bufferDescriptorSize;
    // Unwritten, as in allocate(): nothing outside this call has ever named this slot.
    if (const VkResult r = m_pfnWriteResource(m_device, 1, &resourceInfo, &dst); r != VK_SUCCESS) {
        m_uniformSlots.releaseUnwritten(slot);
        throwVkFailure("DescriptorHeapAllocator: vkWriteResourceDescriptorsEXT (uniform)", r);
    }

    m_memory.flushForHostWrite(m_resourceHeap);

    return DescriptorHandle{m_uniformIndexBase + slot};
}

void DescriptorHeapAllocator::bind(VkCommandBuffer cmd, VkPipelineBindPoint) {
    VkBindHeapInfoEXT resourceBind{VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT};
    resourceBind.heapRange.address = m_resourceHeapAddress;
    resourceBind.heapRange.size = m_resourceHeapSize;
    resourceBind.reservedRangeOffset = m_resourceReservedOffset;
    resourceBind.reservedRangeSize = m_resourceReservedSize;
    m_pfnCmdBindResourceHeap(cmd, &resourceBind);

    VkBindHeapInfoEXT samplerBind{VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT};
    samplerBind.heapRange.address = m_samplerHeapAddress;
    samplerBind.heapRange.size = m_samplerHeapSize;
    samplerBind.reservedRangeOffset = m_samplerReservedOffset;
    samplerBind.reservedRangeSize = m_samplerReservedSize;
    m_pfnCmdBindSamplerHeap(cmd, &samplerBind);
}

void DescriptorHeapAllocator::free(DescriptorHandle handle) {
    if (!handle.valid()) return;

    // Image indices occupy [0, kMaxDescriptorSlots); uniform indices occupy
    // [m_uniformIndexBase, m_uniformIndexBase + kMaxDescriptorSlots), and the
    // constructor guarantees m_uniformIndexBase >= kMaxDescriptorSlots, so these
    // ranges never overlap.
    //
    // The view and sampler registrations are deliberately NOT dropped here. Their lifetime
    // belongs to register/unregisterImageView/Sampler alone: a caller may register once and
    // reuse across many allocate()/free() cycles, and unregistering here would make the next
    // allocate() with the same still-live sampler fail as "never registered".
    if (handle.index >= m_uniformIndexBase) {
        m_uniformSlots.release(handle.index - m_uniformIndexBase);
        return;
    }
    m_imageSlots.release(handle.index);
}

void DescriptorHeapAllocator::pushData(VkCommandBuffer cmd, uint32_t offset, const void* data, size_t size) {
    VkPushDataInfoEXT info{VK_STRUCTURE_TYPE_PUSH_DATA_INFO_EXT};
    info.offset = offset;
    info.data.address = data;
    info.data.size = size;
    m_pfnCmdPushData(cmd, &info);
}

}
