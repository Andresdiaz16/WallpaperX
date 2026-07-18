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

    m_resourceStride = std::max(m_properties.imageDescriptorSize, m_properties.bufferDescriptorSize);

    m_resourceHeapSize = alignUp(m_resourceStride * kMaxDescriptorSlots, m_properties.resourceHeapAlignment);
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

    if (!m_pfnWriteResource || !m_pfnWriteSampler || !m_pfnCmdBindResourceHeap || !m_pfnCmdBindSamplerHeap) {
        throw std::runtime_error("DescriptorHeapAllocator: VK_EXT_descriptor_heap entry points missing");
    }
}

void DescriptorHeapAllocator::registerImageView(VkImageView view, const VkImageViewCreateInfo& createInfo) {
    m_registeredViews[view] = createInfo;
}

void DescriptorHeapAllocator::registerSampler(VkSampler sampler, const VkSamplerCreateInfo& createInfo) {
    m_registeredSamplers[sampler] = createInfo;
}

uint32_t DescriptorHeapAllocator::acquireSlot() {
    if (!m_freeSlots.empty()) {
        uint32_t slot = m_freeSlots.back();
        m_freeSlots.pop_back();
        return slot;
    }
    if (m_nextSlot >= kMaxDescriptorSlots) {
        throw std::runtime_error("DescriptorHeapAllocator: heap exhausted");
    }
    return m_nextSlot++;
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

    const uint32_t slot = acquireSlot();

    VkImageDescriptorInfoEXT imageDescInfo{VK_STRUCTURE_TYPE_IMAGE_DESCRIPTOR_INFO_EXT};
    imageDescInfo.pView = &viewIt->second;
    imageDescInfo.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkResourceDescriptorInfoEXT resourceInfo{VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT};
    resourceInfo.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    resourceInfo.data.pImage = &imageDescInfo;

    VkHostAddressRangeEXT resourceDst{};
    resourceDst.address = static_cast<uint8_t*>(m_resourceHeap.mapped) + slot * m_resourceStride;
    resourceDst.size = m_properties.imageDescriptorSize;
    if (m_pfnWriteResource(m_device, 1, &resourceInfo, &resourceDst) != VK_SUCCESS) {
        throw std::runtime_error("DescriptorHeapAllocator: vkWriteResourceDescriptorsEXT failed");
    }

    VkHostAddressRangeEXT samplerDst{};
    samplerDst.address = static_cast<uint8_t*>(m_samplerHeap.mapped) + slot * m_properties.samplerDescriptorSize;
    samplerDst.size = m_properties.samplerDescriptorSize;
    if (m_pfnWriteSampler(m_device, 1, &samplerIt->second, &samplerDst) != VK_SUCCESS) {
        throw std::runtime_error("DescriptorHeapAllocator: vkWriteSamplerDescriptorsEXT failed");
    }

    return DescriptorHandle{slot};
}

DescriptorHandle DescriptorHeapAllocator::allocateUniform(VkBuffer buffer, VkDeviceSize size) {
    const uint32_t slot = acquireSlot();

    VkBufferDeviceAddressInfo addrInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    addrInfo.buffer = buffer;

    VkDeviceAddressRangeKHR range{};
    range.address = vkGetBufferDeviceAddress(m_device, &addrInfo);
    range.size = size;

    VkResourceDescriptorInfoEXT resourceInfo{VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT};
    resourceInfo.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    resourceInfo.data.pAddressRange = &range;

    VkHostAddressRangeEXT dst{};
    dst.address = static_cast<uint8_t*>(m_resourceHeap.mapped) + slot * m_resourceStride;
    dst.size = m_properties.bufferDescriptorSize;
    if (m_pfnWriteResource(m_device, 1, &resourceInfo, &dst) != VK_SUCCESS) {
        throw std::runtime_error("DescriptorHeapAllocator: vkWriteResourceDescriptorsEXT (uniform) failed");
    }

    return DescriptorHandle{slot};
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
    if (handle.valid()) {
        m_freeSlots.push_back(handle.index);
    }
}

}
