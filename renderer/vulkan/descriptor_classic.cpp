// SPDX-License-Identifier: GPL-2.0-or-later

#include "descriptor_classic.h"

#include "gpu_fault.h"

#include <array>

namespace wallpaperx {

ClassicDescriptorAllocator::ClassicDescriptorAllocator(Device& device)
    : m_device(device),
      m_imageSlots(device, 1, "ClassicDescriptorAllocator: image"),
      m_uniformSlots(device, 1, "ClassicDescriptorAllocator: uniform") {
    const VkDevice vk = m_device.device();

    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    bindings[0].binding = kImageBinding;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    bindings[1].binding = kUniformBinding;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    // Cleanup on throw: ~ClassicDescriptorAllocator won't run if the constructor
    // throws partway, so the layout/pool created before the throw point would leak.
    try {
        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();
        if (const VkResult r = vkCreateDescriptorSetLayout(vk, &layoutInfo, nullptr, &m_setLayout);
            r != VK_SUCCESS) {
            throwVkFailure("ClassicDescriptorAllocator: vkCreateDescriptorSetLayout", r);
        }

        std::array<VkDescriptorPoolSize, 2> poolSizes{
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
        };

        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        if (const VkResult r = vkCreateDescriptorPool(vk, &poolInfo, nullptr, &m_pool); r != VK_SUCCESS) {
            throwVkFailure("ClassicDescriptorAllocator: vkCreateDescriptorPool", r);
        }

        VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocInfo.descriptorPool = m_pool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &m_setLayout;
        if (const VkResult r = vkAllocateDescriptorSets(vk, &allocInfo, &m_set); r != VK_SUCCESS) {
            throwVkFailure("ClassicDescriptorAllocator: vkAllocateDescriptorSets", r);
        }

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &m_setLayout;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &kPushConstantRange;
        if (const VkResult r = vkCreatePipelineLayout(vk, &pipelineLayoutInfo, nullptr, &m_pipelineLayout);
            r != VK_SUCCESS) {
            throwVkFailure("ClassicDescriptorAllocator: vkCreatePipelineLayout", r);
        }
    } catch (...) {
        if (m_pipelineLayout) vkDestroyPipelineLayout(vk, m_pipelineLayout, nullptr);
        if (m_pool) vkDestroyDescriptorPool(vk, m_pool, nullptr);
        if (m_setLayout) vkDestroyDescriptorSetLayout(vk, m_setLayout, nullptr);
        throw;
    }
}

ClassicDescriptorAllocator::~ClassicDescriptorAllocator() {
    const VkDevice vk = m_device.device();
    if (m_pipelineLayout) vkDestroyPipelineLayout(vk, m_pipelineLayout, nullptr);
    if (m_pool) vkDestroyDescriptorPool(vk, m_pool, nullptr);
    if (m_setLayout) vkDestroyDescriptorSetLayout(vk, m_setLayout, nullptr);
}

void ClassicDescriptorAllocator::waitUntilSetIdle() {
    if (!m_setMayBeInFlight) return;
    // Every batch, not the last one: nothing here records which of them bound the set, and
    // fences carry no guarantee of signalling in submission order.
    m_device.waitForSubmissions();
    m_setMayBeInFlight = false;
}

DescriptorHandle ClassicDescriptorAllocator::allocate(VkImageView view, VkSampler sampler) {
    waitUntilSetIdle();
    m_imageSlots.acquire();

    VkDescriptorImageInfo imageInfo{};
    imageInfo.sampler = sampler;
    imageInfo.imageView = view;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = m_set;
    write.dstBinding = kImageBinding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;

    vkUpdateDescriptorSets(m_device.device(), 1, &write, 0, nullptr);
    return DescriptorHandle{kImageBinding};
}

DescriptorHandle ClassicDescriptorAllocator::allocateUniform(VkBuffer buffer, VkDeviceSize size) {
    waitUntilSetIdle();
    m_uniformSlots.acquire();

    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = buffer;
    bufferInfo.offset = 0;
    bufferInfo.range = size;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = m_set;
    write.dstBinding = kUniformBinding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.pBufferInfo = &bufferInfo;

    vkUpdateDescriptorSets(m_device.device(), 1, &write, 0, nullptr);
    return DescriptorHandle{kUniformBinding};
}

void ClassicDescriptorAllocator::bind(VkCommandBuffer cmd, VkPipelineBindPoint bindPoint) {
    vkCmdBindDescriptorSets(cmd, bindPoint, m_pipelineLayout, 0, 1, &m_set, 0, nullptr);
    m_setMayBeInFlight = true;
}

void ClassicDescriptorAllocator::pushData(VkCommandBuffer cmd, uint32_t offset, const void* data, size_t size) {
    vkCmdPushConstants(cmd, m_pipelineLayout, kPushConstantRange.stageFlags, offset, static_cast<uint32_t>(size),
                       data);
}

void ClassicDescriptorAllocator::free(DescriptorHandle handle) {
    if (handle.index == kImageBinding) {
        m_imageSlots.release(0);
    } else if (handle.index == kUniformBinding) {
        m_uniformSlots.release(0);
    }
}

}
