#include "descriptor_heap.h"

#include <algorithm>
#include <stdexcept>

namespace wallpaperx {

namespace {
VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment) {
    return alignment == 0 ? value : (value + alignment - 1) & ~(alignment - 1);
}
}

DescriptorHeapAllocator::DescriptorHeapAllocator(VkPhysicalDevice physicalDevice, VkDevice device, Memory& memory)
    : m_physicalDevice(physicalDevice), m_device(device), m_memory(memory) {
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
        alignUp(alignUp(imageRegionSize, m_properties.bufferDescriptorAlignment), m_properties.bufferDescriptorSize),
        static_cast<VkDeviceSize>(m_properties.bufferDescriptorSize) * kMaxDescriptorSlots);
    m_uniformIndexBase = static_cast<uint32_t>(m_uniformRegionOffset / m_properties.bufferDescriptorSize);
    const VkDeviceSize uniformRegionSize = static_cast<VkDeviceSize>(m_properties.bufferDescriptorSize) * kMaxDescriptorSlots;

    m_resourceHeapSize =
        alignUp(m_uniformRegionOffset + uniformRegionSize, m_properties.resourceHeapAlignment);
    m_samplerHeapSize = alignUp(
        static_cast<VkDeviceSize>(m_properties.samplerDescriptorSize) * kMaxDescriptorSlots,
        m_properties.samplerHeapAlignment);

    m_resourceHeap = m_memory.createBuffer(
        m_resourceHeapSize,
        VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        /*hostVisible=*/true, "descriptor-heap-resource");
    m_samplerHeap = m_memory.createBuffer(
        m_samplerHeapSize,
        VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        /*hostVisible=*/true, "descriptor-heap-sampler");

    VkBufferDeviceAddressInfo resourceAddrInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    resourceAddrInfo.buffer = m_resourceHeap.buffer;
    m_resourceHeapAddress = vkGetBufferDeviceAddress(m_device, &resourceAddrInfo);

    VkBufferDeviceAddressInfo samplerAddrInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    samplerAddrInfo.buffer = m_samplerHeap.buffer;
    m_samplerHeapAddress = vkGetBufferDeviceAddress(m_device, &samplerAddrInfo);
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
        throw std::runtime_error("DescriptorHeapAllocator: VK_EXT_descriptor_heap entry points missing");
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

uint32_t DescriptorHeapAllocator::acquireImageSlot() {
    if (!m_freeImageSlots.empty()) {
        uint32_t slot = m_freeImageSlots.back();
        m_freeImageSlots.pop_back();
        return slot;
    }
    if (m_nextImageSlot >= kMaxDescriptorSlots) {
        throw std::runtime_error("DescriptorHeapAllocator: image heap exhausted");
    }
    return m_nextImageSlot++;
}

uint32_t DescriptorHeapAllocator::acquireUniformSlot() {
    if (!m_freeUniformSlots.empty()) {
        uint32_t slot = m_freeUniformSlots.back();
        m_freeUniformSlots.pop_back();
        return slot;
    }
    if (m_nextUniformSlot >= kMaxDescriptorSlots) {
        throw std::runtime_error("DescriptorHeapAllocator: uniform heap exhausted");
    }
    return m_nextUniformSlot++;
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

    const uint32_t slot = acquireImageSlot();

    VkImageDescriptorInfoEXT imageDescInfo{VK_STRUCTURE_TYPE_IMAGE_DESCRIPTOR_INFO_EXT};
    imageDescInfo.pView = &viewIt->second;
    imageDescInfo.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkResourceDescriptorInfoEXT resourceInfo{VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT};
    resourceInfo.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    resourceInfo.data.pImage = &imageDescInfo;

    VkHostAddressRangeEXT resourceDst{};
    resourceDst.address = static_cast<uint8_t*>(m_resourceHeap.mapped) + slot * m_properties.imageDescriptorSize;
    resourceDst.size = m_properties.imageDescriptorSize;
    if (m_pfnWriteResource(m_device, 1, &resourceInfo, &resourceDst) != VK_SUCCESS) {
        m_freeImageSlots.push_back(slot);
        throw std::runtime_error("DescriptorHeapAllocator: vkWriteResourceDescriptorsEXT failed");
    }

    VkHostAddressRangeEXT samplerDst{};
    samplerDst.address = static_cast<uint8_t*>(m_samplerHeap.mapped) + slot * m_properties.samplerDescriptorSize;
    samplerDst.size = m_properties.samplerDescriptorSize;
    if (m_pfnWriteSampler(m_device, 1, &samplerIt->second, &samplerDst) != VK_SUCCESS) {
        m_freeImageSlots.push_back(slot);
        throw std::runtime_error("DescriptorHeapAllocator: vkWriteSamplerDescriptorsEXT failed");
    }

    m_slotView[slot] = view;
    m_slotSampler[slot] = sampler;

    return DescriptorHandle{slot};
}

DescriptorHandle DescriptorHeapAllocator::allocateUniform(VkBuffer buffer, VkDeviceSize size) {
    const uint32_t slot = acquireUniformSlot();

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
    if (m_pfnWriteResource(m_device, 1, &resourceInfo, &dst) != VK_SUCCESS) {
        m_freeUniformSlots.push_back(slot);
        throw std::runtime_error("DescriptorHeapAllocator: vkWriteResourceDescriptorsEXT (uniform) failed");
    }

    m_activeUniformSlots.insert(slot);
    return DescriptorHandle{m_uniformIndexBase + slot};
}

void DescriptorHeapAllocator::bind(VkCommandBuffer cmd, VkPipelineBindPoint) {
    VkBindHeapInfoEXT resourceBind{VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT};
    resourceBind.heapRange.address = m_resourceHeapAddress;
    resourceBind.heapRange.size = m_resourceHeapSize;
    m_pfnCmdBindResourceHeap(cmd, &resourceBind);

    VkBindHeapInfoEXT samplerBind{VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT};
    samplerBind.heapRange.address = m_samplerHeapAddress;
    samplerBind.heapRange.size = m_samplerHeapSize;
    m_pfnCmdBindSamplerHeap(cmd, &samplerBind);
}

void DescriptorHeapAllocator::free(DescriptorHandle handle) {
    if (!handle.valid()) return;

    // Image indices occupy [0, kMaxDescriptorSlots); uniform indices occupy
    // [m_uniformIndexBase, m_uniformIndexBase + kMaxDescriptorSlots), and the
    // constructor guarantees m_uniformIndexBase >= kMaxDescriptorSlots, so these
    // ranges never overlap. Both branches reject a handle that isn't currently
    // active (already freed, or never allocated) rather than pushing it onto the
    // free-list anyway -- doing that silently would hand the same slot out to two
    // independent, simultaneously-live allocations.
    if (handle.index >= m_uniformIndexBase) {
        const uint32_t local = handle.index - m_uniformIndexBase;
        if (m_activeUniformSlots.erase(local) == 0) {
            throw std::runtime_error(
                "DescriptorHeapAllocator::free: uniform handle already freed or never allocated");
        }
        m_freeUniformSlots.push_back(local);
        return;
    }

    auto viewIt = m_slotView.find(handle.index);
    if (viewIt == m_slotView.end()) {
        throw std::runtime_error("DescriptorHeapAllocator::free: image handle already freed or never allocated");
    }

    // A registered view/sampler may back more than one live slot (the same
    // texture allocated into two heap indices), so only drop its registration
    // once no other slot still references it — otherwise freeing one slot would
    // unregister a view/sampler a different, still-live slot depends on.
    const VkImageView view = viewIt->second;
    m_slotView.erase(viewIt);
    const bool viewStillReferenced = std::any_of(
        m_slotView.begin(), m_slotView.end(), [view](const auto& entry) { return entry.second == view; });
    if (!viewStillReferenced) {
        m_registeredViews.erase(view);
    }

    // allocate() always populates m_slotView and m_slotSampler together, so a
    // slot found in one but missing from the other means that invariant broke
    // somewhere -- fail loud rather than silently proceeding on inconsistent
    // internal state.
    auto samplerIt = m_slotSampler.find(handle.index);
    if (samplerIt == m_slotSampler.end()) {
        throw std::runtime_error("DescriptorHeapAllocator::free: internal inconsistency -- slot had a view but no sampler");
    }
    const VkSampler sampler = samplerIt->second;
    m_slotSampler.erase(samplerIt);
    const bool samplerStillReferenced = std::any_of(
        m_slotSampler.begin(), m_slotSampler.end(), [sampler](const auto& entry) { return entry.second == sampler; });
    if (!samplerStillReferenced) {
        m_registeredSamplers.erase(sampler);
    }

    m_freeImageSlots.push_back(handle.index);
}

void DescriptorHeapAllocator::pushData(VkCommandBuffer cmd, uint32_t offset, const void* data, size_t size) {
    VkPushDataInfoEXT info{VK_STRUCTURE_TYPE_PUSH_DATA_INFO_EXT};
    info.offset = offset;
    info.data.address = data;
    info.data.size = size;
    m_pfnCmdPushData(cmd, &info);
}

}
