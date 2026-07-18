#include "offscreen.h"

#include "descriptor_heap.h"
#include "device.h"
#include "memory_export.h"

#include <array>
#include <stdexcept>
#include <utility>

namespace wallpaperx {

OffscreenTargetManager::OffscreenTargetManager(Device& device) : m_device(device) {
    createRenderPass();
}

OffscreenTargetManager::~OffscreenTargetManager() {
    for (auto& [id, target] : m_targets) {
        destroyTarget(target);
    }
    if (m_renderPass) vkDestroyRenderPass(m_device.device(), m_renderPass, nullptr);
}

void OffscreenTargetManager::createRenderPass() {
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = kFormat;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    std::array<VkSubpassDependency, 2> dependencies{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    VkRenderPassCreateInfo renderPassInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &colorAttachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = static_cast<uint32_t>(dependencies.size());
    renderPassInfo.pDependencies = dependencies.data();

    if (vkCreateRenderPass(m_device.device(), &renderPassInfo, nullptr, &m_renderPass) != VK_SUCCESS) {
        throw std::runtime_error("OffscreenTargetManager: failed to create render pass");
    }
}

const OffscreenTarget& OffscreenTargetManager::create(int monitorId, uint32_t width, uint32_t height) {
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = kFormat;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    OffscreenTarget target;
    target.width = width;
    target.height = height;
    target.image = m_device.memory().createExportableImage(
        imageInfo,
        externalMemoryHandleType(m_device.info().vendor, m_device.info().hasExternalMemoryDmaBuf),
        "offscreen-target");

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = target.image.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = kFormat;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    if (vkCreateImageView(m_device.device(), &viewInfo, nullptr, &target.view) != VK_SUCCESS) {
        destroyTarget(target);
        throw std::runtime_error("OffscreenTargetManager: failed to create image view");
    }

    if (auto* heap = dynamic_cast<DescriptorHeapAllocator*>(&m_device.descriptorAllocator())) {
        heap->registerImageView(target.view, viewInfo);
    }

    VkFramebufferCreateInfo fbInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fbInfo.renderPass = m_renderPass;
    fbInfo.attachmentCount = 1;
    fbInfo.pAttachments = &target.view;
    fbInfo.width = width;
    fbInfo.height = height;
    fbInfo.layers = 1;

    if (vkCreateFramebuffer(m_device.device(), &fbInfo, nullptr, &target.framebuffer) != VK_SUCCESS) {
        destroyTarget(target);
        throw std::runtime_error("OffscreenTargetManager: failed to create framebuffer");
    }

    if (auto existing = m_targets.find(monitorId); existing != m_targets.end()) {
        destroyTarget(existing->second);
    }

    auto [it, inserted] = m_targets.insert_or_assign(monitorId, std::move(target));
    return it->second;
}

void OffscreenTargetManager::destroy(int monitorId) {
    auto it = m_targets.find(monitorId);
    if (it == m_targets.end()) return;
    destroyTarget(it->second);
    m_targets.erase(it);
}

void OffscreenTargetManager::destroyTarget(OffscreenTarget& target) {
    if (auto* heap = dynamic_cast<DescriptorHeapAllocator*>(&m_device.descriptorAllocator())) {
        heap->unregisterImageView(target.view);
    }
    vkDestroyFramebuffer(m_device.device(), target.framebuffer, nullptr);
    vkDestroyImageView(m_device.device(), target.view, nullptr);
    m_device.memory().destroyImage(target.image);
}

bool OffscreenTargetManager::has(int monitorId) const {
    return m_targets.find(monitorId) != m_targets.end();
}

const OffscreenTarget& OffscreenTargetManager::get(int monitorId) const {
    return m_targets.at(monitorId);
}

}
