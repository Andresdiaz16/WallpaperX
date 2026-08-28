// SPDX-License-Identifier: GPL-2.0-or-later

#include "offscreen.h"

#include "../renderers/renderer_base.h"

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
    for (auto& [id, ring] : m_targets) {
        for (OffscreenTarget& target : ring.slots) destroyTarget(target);
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
    colorAttachment.finalLayout = kRenderedLayout;

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

void OffscreenTargetManager::render(IRenderer& renderer, const OffscreenTarget& target) {
    VkImageLayout resultingLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool recorded = false;
    bool acquired = false;
    try {
        m_device.immediateSubmit([&](VkCommandBuffer cmd) {
            recorded = true;

            // Take ownership back before writing. exportFrame() released it to an
            // external consumer, and a release with no matching acquire is where a
            // driver's compressed-modifier metadata resolve is undefined -- so the
            // second and later renders of a target must re-acquire, not just overwrite.
            if (target.externalOwner != VK_QUEUE_FAMILY_IGNORED) {
                VkImageMemoryBarrier acquire{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                acquire.srcAccessMask = 0;  // the release side made the writes available
                acquire.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                // The same pair the release named. A transfer performs its transition once,
                // on whichever side the driver picks, so halves that disagree leave the
                // conversion undone. The render pass discards from GENERAL anyway.
                acquire.oldLayout = target.releasedFromLayout;
                acquire.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                acquire.srcQueueFamilyIndex = target.externalOwner;
                acquire.dstQueueFamilyIndex = m_device.graphicsQueueFamily();
                acquire.image = target.image.image;
                acquire.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                     &acquire);
                acquired = true;
            }

            resultingLayout = renderer.record(cmd, target);
        });
    } catch (...) {
        // Only invalidate if commands were actually recorded. immediateSubmit can also
        // throw from vkAllocateCommandBuffers/vkBeginCommandBuffer before reaching the
        // lambda, in which case nothing touched the image -- stamping UNDEFINED there
        // would discard a still-valid layout and let the next export's barrier license
        // the driver to throw away a frame the compositor is currently displaying.
        //
        // externalOwner is deliberately NOT touched here. The acquire barrier only takes
        // effect if the submit succeeds, so on failure the external consumer still owns
        // the image and the next render must issue the acquire again. Clearing it at
        // record time (as this did originally) desynchronised ownership permanently for
        // any caller that survives a failed submit -- harmless for a probe that exits on
        // throw, silent corruption for a long-lived daemon.
        if (recorded) {
            target.notifyLayout(VK_IMAGE_LAYOUT_UNDEFINED);
        }
        throw;
    }
    if (acquired) {
        target.externalOwner = VK_QUEUE_FAMILY_IGNORED;
    }
    target.notifyLayout(resultingLayout);
}

const OffscreenTarget& OffscreenTargetManager::create(int monitorId, uint32_t width, uint32_t height,
                                                      uint32_t depth) {
    m_device.assertGpuThread("OffscreenTargetManager::create");
    if (depth == 0) {
        throw std::runtime_error("OffscreenTargetManager: a monitor needs at least one target");
    }

    // Every slot is built before anything existing is destroyed. Replacing first would leave a
    // monitor with no image at all if the second allocation failed, where this leaves the one
    // it already had.
    Ring ring;
    ring.slots.reserve(depth);
    try {
        for (uint32_t slot = 0; slot < depth; ++slot) {
            ring.slots.push_back(makeTarget(width, height));
        }
    } catch (...) {
        for (OffscreenTarget& built : ring.slots) destroyTarget(built);
        throw;
    }

    if (auto existing = m_targets.find(monitorId); existing != m_targets.end()) {
        for (OffscreenTarget& old : existing->second.slots) destroyTarget(old);
    }

    auto [it, inserted] = m_targets.insert_or_assign(monitorId, std::move(ring));
    return it->second.slots[it->second.current];
}

OffscreenTarget OffscreenTargetManager::makeTarget(uint32_t width, uint32_t height) {
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = kFormat;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = kUsage;
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

    return target;
}

void OffscreenTargetManager::destroy(int monitorId) {
    auto it = m_targets.find(monitorId);
    if (it == m_targets.end()) return;
    for (OffscreenTarget& target : it->second.slots) destroyTarget(target);
    m_targets.erase(it);
}

void OffscreenTargetManager::destroyTarget(OffscreenTarget& target) {
    m_device.assertGpuThread("OffscreenTargetManager::destroyTarget");

    // The image/view/framebuffer may still be referenced by in-flight GPU work
    // (e.g. a monitor resized while its previous frame is still executing). Destroying
    // them under the GPU is a use-after-free, so wait for the device to go idle first.
    // All destruction paths (destroy(), the create() replace path, the destructor, and
    // create()'s own error cleanup) funnel through here, so this is the one guard point.
    // Ignore the result: this runs in teardown paths (including a destructor) where
    // throwing is not an option, and a lost device is being torn down anyway.
    vkDeviceWaitIdle(m_device.device());

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
    const Ring& ring = m_targets.at(monitorId);
    return ring.slots[ring.current];
}

const OffscreenTarget& OffscreenTargetManager::advance(int monitorId) {
    Ring& ring = m_targets.at(monitorId);
    ring.current = (ring.current + 1) % ring.slots.size();
    return ring.slots[ring.current];
}

uint32_t OffscreenTargetManager::depth(int monitorId) const {
    return static_cast<uint32_t>(m_targets.at(monitorId).slots.size());
}

}
