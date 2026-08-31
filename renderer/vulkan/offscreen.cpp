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

namespace {

// Both halves of an ownership transfer name the same image and subresource; built here so
// they cannot be given different ones.
VkImageMemoryBarrier transferOf(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout, uint32_t srcFamily,
                                uint32_t dstFamily) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = srcFamily;
    barrier.dstQueueFamilyIndex = dstFamily;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return barrier;
}

// Takes a published frame back onto the graphics queue: the same layout pair the release
// named, in the same direction. A transfer performs its transition once, on whichever side
// the driver picks, so halves that disagree leave the conversion undone.
//
// The destination scope covers everything on purpose. IRenderer::record may write the image
// as a colour attachment, a compute store or a blit, and this is recorded before record()
// runs -- nothing here can know which. A scope too narrow for what follows makes neither the
// acquire nor the transition visible to it, and no validation layer says so: an ownership
// transfer's scopes are not checked. One barrier per frame, so the width costs nothing.
void recordAcquire(VkCommandBuffer cmd, const OffscreenTarget& target, uint32_t graphicsFamily) {
    VkImageMemoryBarrier acquire = transferOf(target.image.image, target.releasedFromLayout,
                                              VK_IMAGE_LAYOUT_GENERAL, target.externalOwner, graphicsFamily);
    acquire.srcAccessMask = 0;  // the release side made the writes available
    acquire.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &acquire);
}

}  // namespace

OffscreenTargetManager::OffscreenTargetManager(Device& device) : m_device(device) {
    createRenderPass();
}

OffscreenTargetManager::~OffscreenTargetManager() {
    std::vector<OffscreenTarget*> all;
    for (auto& [id, ring] : m_targets) {
        for (OffscreenTarget& target : ring.images) all.push_back(&target);
    }
    destroyTargets(all);
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

    // These are what chain the pass's automatic layout transitions to the barriers around
    // it -- the acquire before and the ownership release after, both in the same command
    // buffer as the pass. Neither of those runs in a framebuffer-space stage, so naming one
    // (FRAGMENT_SHADER, say) leaves the transition unordered against it, and BY_REGION would
    // promise a per-region dependency the whole-image barriers do not honour.
    std::array<VkSubpassDependency, 2> dependencies{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;

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
    // Derived from the same handle-type decision exportFrame() reports to the consumer,
    // so the family a release names cannot disagree with the handle that is imported.
    const uint32_t consumer = externalConsumerQueueFamily(
        externalMemoryHandleType(m_device.info().vendor, m_device.info().hasExternalMemoryDmaBuf),
        m_device.info().hasQueueFamilyForeign);

    VkImageLayout renderedLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // Nothing is recorded on the target until the submit has returned. Neither barrier takes
    // effect unless it succeeds, so a throw leaves ownership exactly where it was: with the
    // consumer if a frame was published, with us if not.
    m_device.immediateSubmit([&](VkCommandBuffer cmd) {
        // Take ownership back before writing. The previous render released it to an
        // external consumer, and a release with no matching acquire is where a driver's
        // compressed-modifier metadata resolve is undefined.
        if (target.published()) recordAcquire(cmd, target, m_device.graphicsQueueFamily());

        renderedLayout = renderer.record(cmd, target);

        // Hand the finished frame over: transition it to GENERAL and release queue-family
        // ownership. Without this the image is still owned by the graphics queue in
        // whatever layout the renderer left, and what an external importer sees is
        // formally undefined -- on a compressed modifier (AMD DCC, Intel CCS) the metadata
        // the export reports then describes bytes the compositor cannot read. Lavapipe
        // picks LINEAR and never compresses, so it cannot witness that; the visible
        // failure is AMD/Intel-only.
        //
        // In the same submit as the render, not a later one: a frame that is drawn and
        // never fetched must still leave the target's recorded state describing the image,
        // and a second submit per frame is what a display-rate producer cannot afford.
        //
        // The source scope covers everything the renderer could have done rather than
        // being inferred from the layout it returned: SHADER_READ_ONLY_OPTIMAL is reached
        // by a colour attachment write and by a compute store alike, and a release that
        // names the wrong one orders nothing -- the consumer then sees the previous frame,
        // with no validation warning. This command buffer holds only the pass and these
        // two barriers, so there is nothing for a narrower scope to overlap with.
        VkImageMemoryBarrier release = transferOf(target.image.image, renderedLayout, VK_IMAGE_LAYOUT_GENERAL,
                                                  m_device.graphicsQueueFamily(), consumer);
        release.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        release.dstAccessMask = 0;  // ownership release: the acquire side does the rest
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &release);
    });
    target.notifyPublished(consumer, renderedLayout);
}

void OffscreenTargetManager::reclaim(const OffscreenTarget& target) {
    if (!target.published()) {
        throw std::runtime_error("OffscreenTargetManager::reclaim: the target has no published frame");
    }
    m_device.immediateSubmit(
        [&](VkCommandBuffer cmd) { recordAcquire(cmd, target, m_device.graphicsQueueFamily()); });
    target.notifyReclaimed();
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
    ring.images.reserve(depth);
    try {
        for (uint32_t slot = 0; slot < depth; ++slot) {
            ring.images.push_back(makeTarget(width, height));
        }
    } catch (...) {
        std::vector<OffscreenTarget*> built;
        for (OffscreenTarget& image : ring.images) built.push_back(&image);
        destroyTargets(built);
        throw;
    }

    if (auto existing = m_targets.find(monitorId); existing != m_targets.end()) {
        std::vector<OffscreenTarget*> old;
        for (OffscreenTarget& image : existing->second.images) old.push_back(&image);
        destroyTargets(old);
    }

    auto [it, inserted] = m_targets.insert_or_assign(monitorId, std::move(ring));
    return it->second.images[it->second.current];
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
    target.bufferId = m_nextBufferId++;
    target.width = width;
    target.height = height;
    target.image = m_device.memory().createExportableImage(
        imageInfo,
        externalMemoryHandleType(m_device.info().vendor, m_device.info().hasExternalMemoryDmaBuf),
        "offscreen-target");

    if (target.image.drmModifierTiled) {
        try {
            target.drmModifier = queryImageDrmModifier(m_device.device(), target.image.image);
        } catch (...) {
            destroyTarget(target);
            throw;
        }
    }

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
    std::vector<OffscreenTarget*> ring;
    for (OffscreenTarget& target : it->second.images) ring.push_back(&target);
    destroyTargets(ring);
    m_targets.erase(it);
}

void OffscreenTargetManager::destroyTarget(OffscreenTarget& target) {
    std::vector<OffscreenTarget*> one{&target};
    destroyTargets(one);
}

// The images may still be referenced by in-flight GPU work, so the device has to go idle
// before any of them is freed. One wait for the whole batch: per target, dropping a ring cost
// a full device stall per slot, which any client able to trigger a rebuild could repeat.
//
// Every destruction path funnels through here -- destroy(), create()'s replace and error
// paths, and the destructor. The wait's result is ignored: this runs from a destructor where
// throwing is not an option, and a lost device is being torn down anyway.
void OffscreenTargetManager::destroyTargets(const std::vector<OffscreenTarget*>& images) {
    m_device.assertGpuThread("OffscreenTargetManager::destroyTargets");
    if (images.empty()) return;

    vkDeviceWaitIdle(m_device.device());
    for (OffscreenTarget* image : images) dropTarget(*image);
}

void OffscreenTargetManager::dropTarget(OffscreenTarget& target) {
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
    return ring.images[ring.current];
}

const OffscreenTarget& OffscreenTargetManager::advance(int monitorId) {
    Ring& ring = m_targets.at(monitorId);
    ring.current = (ring.current + 1) % ring.images.size();
    return ring.images[ring.current];
}

uint32_t OffscreenTargetManager::depth(int monitorId) const {
    return static_cast<uint32_t>(m_targets.at(monitorId).images.size());
}

}
