// SPDX-License-Identifier: GPL-2.0-or-later

#include "offscreen.h"

#include "../renderers/renderer_base.h"

#include "descriptor_heap.h"
#include "device.h"
#include "memory_export.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <stdexcept>
#include <utility>

namespace wallpaperx {

namespace {

// Process-wide rather than per manager: see OffscreenTarget::bufferId. Starts at 1 so 0 stays "no buffer".
std::atomic<uint64_t> g_nextBufferId{1};

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
    static_cast<void>(destroyTargets(all));

    // Every render batch's command buffer names this render pass, so it is under the same rule
    // as the images those batches draw into: a GPU that has not finished is still reading it.
    // Asked again rather than inferred from destroyTargets(), which drains nothing when the
    // map is empty. A drain that failed once answers immediately, so this costs one poll on
    // the ordinary path.
    if (!m_renderPass) return;
    if (!m_device.safeToFree("the render pass every render batch names")) return;
    vkDestroyRenderPass(m_device.device(), m_renderPass, nullptr);
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

    if (const VkResult r = vkCreateRenderPass(m_device.device(), &renderPassInfo, nullptr, &m_renderPass);
        r != VK_SUCCESS) {
        throwVkFailure("OffscreenTargetManager: vkCreateRenderPass", r);
    }
}

void OffscreenTargetManager::render(IRenderer& renderer, const OffscreenTarget& target) {
    // Derived from the same handle-type decision exportFrame() reports to the consumer,
    // so the family a release names cannot disagree with the handle that is imported.
    const uint32_t consumer = externalConsumerQueueFamily(
        externalMemoryHandleType(m_device.info().vendor, m_device.info().hasExternalMemoryDmaBuf),
        m_device.info().hasQueueFamilyForeign);

    VkImageLayout renderedLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // Nothing is recorded on the target until the batch has been accepted by the queue.
    // Neither barrier takes effect unless it is, so a throw leaves ownership exactly where it
    // was: with the consumer if a frame was published, with us if not.
    //
    // Accepted, not completed: this does not wait. Whatever this monitor renders next is
    // ordered after it by submission order -- the acquire barrier and the render pass's own
    // external dependency both name everything submitted earlier on this queue -- and a
    // consumer is ordered by the semaphore exportFrameAcquireFd signals, which is the only
    // ordering a host wait here never provided.
    m_device.submit([&](VkCommandBuffer cmd) {
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
    makeCurrent(target);
}

void OffscreenTargetManager::makeCurrent(const OffscreenTarget& target) {
    for (auto& [id, ring] : m_targets) {
        for (size_t slot = 0; slot < ring.images.size(); ++slot) {
            if (&ring.images[slot] == &target) {
                ring.current = slot;
                return;
            }
        }
    }
}

void OffscreenTargetManager::reclaim(const OffscreenTarget& target) {
    if (!target.published()) {
        throw std::runtime_error("OffscreenTargetManager::reclaim: the target has no published frame");
    }
    m_device.submit([&](VkCommandBuffer cmd) { recordAcquire(cmd, target, m_device.graphicsQueueFamily()); });
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
    // it already had. Nothing built here has been submitted against, so on either exit that
    // undoes the build the new images are dropped without a drain.
    Ring ring;
    ring.images.reserve(depth);
    try {
        for (uint32_t slot = 0; slot < depth; ++slot) {
            ring.images.push_back(makeTarget(width, height));
        }
    } catch (...) {
        for (OffscreenTarget& image : ring.images) dropTarget(image);
        throw;
    }

    if (auto existing = m_targets.find(monitorId); existing != m_targets.end()) {
        std::vector<OffscreenTarget*> old;
        for (OffscreenTarget& image : existing->second.images) old.push_back(&image);
        if (!destroyTargets(old)) {
            // The old ring stays recorded: its images still exist and the GPU is still reading
            // them, and this monitor's entry is the only record of that.
            for (OffscreenTarget& image : ring.images) dropTarget(image);
            throw GpuFailure("OffscreenTargetManager: the GPU has not finished reading this monitor's images, so "
                             "they cannot be replaced");
        }
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
    target.bufferId = g_nextBufferId.fetch_add(1, std::memory_order_relaxed);
    target.width = width;
    target.height = height;
    target.image = m_device.memory().createExportableImage(
        imageInfo,
        externalMemoryHandleType(m_device.info().vendor, m_device.info().hasExternalMemoryDmaBuf),
        kFormatFeatures, "offscreen-target");

    if (target.image.drmModifierTiled) {
        try {
            target.drmModifier = queryImageDrmModifier(m_device.device(), target.image.image);
        } catch (...) {
            dropTarget(target);
            throw;
        }
    }

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = target.image.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = kFormat;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    if (const VkResult r = vkCreateImageView(m_device.device(), &viewInfo, nullptr, &target.view); r != VK_SUCCESS) {
        dropTarget(target);
        throwVkFailure("OffscreenTargetManager: vkCreateImageView", r);
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

    if (const VkResult r = vkCreateFramebuffer(m_device.device(), &fbInfo, nullptr, &target.framebuffer);
        r != VK_SUCCESS) {
        dropTarget(target);
        throwVkFailure("OffscreenTargetManager: vkCreateFramebuffer", r);
    }

    return target;
}

void OffscreenTargetManager::destroy(int monitorId) {
    auto it = m_targets.find(monitorId);
    if (it == m_targets.end()) return;
    std::vector<OffscreenTarget*> ring;
    for (OffscreenTarget& target : it->second.images) ring.push_back(&target);
    if (destroyTargets(ring)) m_targets.erase(it);
}

// The images may still be referenced by in-flight GPU work, so the GPU has to finish before
// any of them is freed. One wait for the whole batch: per target, dropping a ring cost a full
// device stall per slot, which any client able to trigger a rebuild could repeat.
//
// Every path that frees an image a render may have drawn into funnels through here --
// destroy(), create()'s replace, and the destructor -- so the answer to a GPU that has not
// finished has to be one a destructor can take: the images are left behind rather than freed
// under a GPU still reading them. Device records that and refuses further work, so the wedge
// is reported by the next render rather than swallowed here.
bool OffscreenTargetManager::destroyTargets(const std::vector<OffscreenTarget*>& images) {
    m_device.assertGpuThread("OffscreenTargetManager::destroyTargets");
    if (images.empty()) return true;

    // No batch to name: nothing records which render last drew into which image, so any
    // pending one may still be reading these.
    if (!m_device.safeToFree("offscreen images, their views and their framebuffers")) return false;
    for (OffscreenTarget* image : images) dropTarget(*image);
    return true;
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

const OffscreenTarget* OffscreenTargetManager::nextUnheld(int monitorId, const std::vector<uint64_t>& held) const {
    const Ring& ring = m_targets.at(monitorId);
    const size_t count = ring.images.size();

    // A ring whose current slot has never been drawn into starts there; every other case
    // starts one past the published frame, so the image a consumer was most recently told
    // about is the last slot considered rather than the first.
    const size_t first = ring.images[ring.current].published() ? 1 : 0;
    for (size_t step = first; step < first + count; ++step) {
        const OffscreenTarget& candidate = ring.images[(ring.current + step) % count];
        if (std::find(held.begin(), held.end(), candidate.bufferId) == held.end()) return &candidate;
    }
    return nullptr;
}

}
