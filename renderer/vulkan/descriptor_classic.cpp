// SPDX-License-Identifier: GPL-2.0-or-later

#include "descriptor_classic.h"

#include <array>
#include <stdexcept>

namespace wallpaperx {

ClassicDescriptorAllocator::ClassicDescriptorAllocator(VkDevice device) : m_device(device) {
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
        if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_setLayout) != VK_SUCCESS) {
            throw std::runtime_error("ClassicDescriptorAllocator: failed to create descriptor set layout");
        }

        std::array<VkDescriptorPoolSize, 2> poolSizes{
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
        };

        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_pool) != VK_SUCCESS) {
            throw std::runtime_error("ClassicDescriptorAllocator: failed to create descriptor pool");
        }

        VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocInfo.descriptorPool = m_pool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &m_setLayout;
        if (vkAllocateDescriptorSets(m_device, &allocInfo, &m_set) != VK_SUCCESS) {
            throw std::runtime_error("ClassicDescriptorAllocator: failed to allocate descriptor set");
        }

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &m_setLayout;
        if (vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
            throw std::runtime_error("ClassicDescriptorAllocator: failed to create pipeline layout");
        }
    } catch (...) {
        if (m_pipelineLayout) vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
        if (m_pool) vkDestroyDescriptorPool(m_device, m_pool, nullptr);
        if (m_setLayout) vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
        throw;
    }
}

ClassicDescriptorAllocator::~ClassicDescriptorAllocator() {
    if (m_pipelineLayout) vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
    if (m_pool) vkDestroyDescriptorPool(m_device, m_pool, nullptr);
    if (m_setLayout) vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
}

DescriptorHandle ClassicDescriptorAllocator::allocate(VkImageView view, VkSampler sampler) {
    if (m_imageOccupied) {
        throw std::runtime_error(
            "ClassicDescriptorAllocator::allocate: image slot already in use — this tier supports only "
            "one live image at a time; call free() on the previous handle first");
    }

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

    vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
    m_imageOccupied = true;
    return DescriptorHandle{kImageBinding};
}

DescriptorHandle ClassicDescriptorAllocator::allocateUniform(VkBuffer buffer, VkDeviceSize size) {
    if (m_uniformOccupied) {
        throw std::runtime_error(
            "ClassicDescriptorAllocator::allocateUniform: uniform slot already in use — this tier supports "
            "only one live uniform buffer at a time; call free() on the previous handle first");
    }

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

    vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
    m_uniformOccupied = true;
    return DescriptorHandle{kUniformBinding};
}

void ClassicDescriptorAllocator::bind(VkCommandBuffer cmd, VkPipelineBindPoint bindPoint) {
    vkCmdBindDescriptorSets(cmd, bindPoint, m_pipelineLayout, 0, 1, &m_set, 0, nullptr);
}

void ClassicDescriptorAllocator::free(DescriptorHandle handle) {
    if (handle.index == kImageBinding) {
        if (!m_imageOccupied) {
            throw std::runtime_error("ClassicDescriptorAllocator::free: image handle already freed or never allocated");
        }
        m_imageOccupied = false;
    } else if (handle.index == kUniformBinding) {
        if (!m_uniformOccupied) {
            throw std::runtime_error(
                "ClassicDescriptorAllocator::free: uniform handle already freed or never allocated");
        }
        m_uniformOccupied = false;
    }
}

}
