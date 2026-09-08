// SPDX-License-Identifier: GPL-2.0-or-later

#include "descriptor_buffer.h"

#include "gpu_fault.h"

#include <array>

namespace wallpaperx {

DescriptorBufferAllocator::DescriptorBufferAllocator(Device& device)
    : m_physicalDevice(device.physicalDevice()),
      m_device(device.device()),
      m_memory(device.memory()),
      m_imageSlots(device, 1, "DescriptorBufferAllocator: image"),
      m_uniformSlots(device, 1, "DescriptorBufferAllocator: uniform") {
    loadFunctions();

    m_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT;
    VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    props2.pNext = &m_properties;
    vkGetPhysicalDeviceProperties2(m_physicalDevice, &props2);

    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    bindings[0].binding = kImageBinding;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    bindings[1].binding = kUniformBinding;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    // Everything acquired below is released only by ~DescriptorBufferAllocator,
    // which never runs if the constructor throws -- so clean up on throw to avoid
    // leaking the set layout / pipeline layout / descriptor buffer (the latter
    // would also trip Memory's teardown leak abort).
    try {
        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
        layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();
        if (const VkResult r = vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_setLayout);
            r != VK_SUCCESS) {
            throwVkFailure("DescriptorBufferAllocator: vkCreateDescriptorSetLayout", r);
        }

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &m_setLayout;
        if (const VkResult r = vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout);
            r != VK_SUCCESS) {
            throwVkFailure("DescriptorBufferAllocator: vkCreatePipelineLayout", r);
        }

        VkDeviceSize layoutSize = 0;
        m_pfnGetLayoutSize(m_device, m_setLayout, &layoutSize);
        m_pfnGetBindingOffset(m_device, m_setLayout, kImageBinding, &m_imageOffset);
        m_pfnGetBindingOffset(m_device, m_setLayout, kUniformBinding, &m_uniformOffset);

        const VkDeviceSize alignment = m_properties.descriptorBufferOffsetAlignment;
        const VkDeviceSize alignedSize = (layoutSize + alignment - 1) & ~(alignment - 1);

        m_buffer = m_memory.createBuffer(
            alignedSize,
            VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
                VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            HostAccess::Write,
            "descriptor-buffer-tier-set");

        VkBufferDeviceAddressInfo addrInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        addrInfo.buffer = m_buffer.buffer;
        m_bufferAddress = vkGetBufferDeviceAddress(m_device, &addrInfo);
    } catch (...) {
        m_memory.destroyBuffer(m_buffer);
        if (m_pipelineLayout) vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
        if (m_setLayout) vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
        throw;
    }
}

DescriptorBufferAllocator::~DescriptorBufferAllocator() {
    m_memory.destroyBuffer(m_buffer);
    if (m_pipelineLayout) vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
    if (m_setLayout) vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
}

void DescriptorBufferAllocator::loadFunctions() {
    m_pfnGetLayoutSize = reinterpret_cast<PFN_vkGetDescriptorSetLayoutSizeEXT>(
        vkGetDeviceProcAddr(m_device, "vkGetDescriptorSetLayoutSizeEXT"));
    m_pfnGetBindingOffset = reinterpret_cast<PFN_vkGetDescriptorSetLayoutBindingOffsetEXT>(
        vkGetDeviceProcAddr(m_device, "vkGetDescriptorSetLayoutBindingOffsetEXT"));
    m_pfnGetDescriptor = reinterpret_cast<PFN_vkGetDescriptorEXT>(
        vkGetDeviceProcAddr(m_device, "vkGetDescriptorEXT"));
    m_pfnCmdBindBuffers = reinterpret_cast<PFN_vkCmdBindDescriptorBuffersEXT>(
        vkGetDeviceProcAddr(m_device, "vkCmdBindDescriptorBuffersEXT"));
    m_pfnCmdSetOffsets = reinterpret_cast<PFN_vkCmdSetDescriptorBufferOffsetsEXT>(
        vkGetDeviceProcAddr(m_device, "vkCmdSetDescriptorBufferOffsetsEXT"));

    if (!m_pfnGetLayoutSize || !m_pfnGetBindingOffset || !m_pfnGetDescriptor ||
        !m_pfnCmdBindBuffers || !m_pfnCmdSetOffsets) {
        // This tier is only selected when the extension was reported and enabled, so a
        // missing entry point is the driver withholding what it advertised.
        throw GpuFailure("DescriptorBufferAllocator: VK_EXT_descriptor_buffer entry points missing");
    }
}

DescriptorHandle DescriptorBufferAllocator::allocate(VkImageView view, VkSampler sampler) {
    m_imageSlots.acquire();

    VkDescriptorImageInfo imageInfo{};
    imageInfo.sampler = sampler;
    imageInfo.imageView = view;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorGetInfoEXT getInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
    getInfo.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    getInfo.data.pCombinedImageSampler = &imageInfo;

    m_pfnGetDescriptor(m_device, &getInfo, m_properties.combinedImageSamplerDescriptorSize,
                        static_cast<uint8_t*>(m_buffer.mapped) + m_imageOffset);
    m_memory.flushForHostWrite(m_buffer);
    return DescriptorHandle{kImageBinding};
}

DescriptorHandle DescriptorBufferAllocator::allocateUniform(VkBuffer buffer, VkDeviceSize size) {
    m_uniformSlots.acquire();

    VkBufferDeviceAddressInfo addrInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    addrInfo.buffer = buffer;

    VkDescriptorAddressInfoEXT addressInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT};
    addressInfo.address = vkGetBufferDeviceAddress(m_device, &addrInfo);
    addressInfo.range = size;
    addressInfo.format = VK_FORMAT_UNDEFINED;

    VkDescriptorGetInfoEXT getInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
    getInfo.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    getInfo.data.pUniformBuffer = &addressInfo;

    m_pfnGetDescriptor(m_device, &getInfo, m_properties.uniformBufferDescriptorSize,
                        static_cast<uint8_t*>(m_buffer.mapped) + m_uniformOffset);
    m_memory.flushForHostWrite(m_buffer);
    return DescriptorHandle{kUniformBinding};
}

void DescriptorBufferAllocator::bind(VkCommandBuffer cmd, VkPipelineBindPoint bindPoint) {
    VkDescriptorBufferBindingInfoEXT bindingInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT};
    bindingInfo.address = m_bufferAddress;
    bindingInfo.usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
                         VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT;

    m_pfnCmdBindBuffers(cmd, 1, &bindingInfo);

    const uint32_t bufferIndex = 0;
    const VkDeviceSize offset = 0;
    m_pfnCmdSetOffsets(cmd, bindPoint, m_pipelineLayout, 0, 1, &bufferIndex, &offset);
}

void DescriptorBufferAllocator::free(DescriptorHandle handle) {
    if (handle.index == kImageBinding) {
        m_imageSlots.release(0);
    } else if (handle.index == kUniformBinding) {
        m_uniformSlots.release(0);
    }
}

}
