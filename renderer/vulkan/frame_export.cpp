// SPDX-License-Identifier: GPL-2.0-or-later

#include "frame_export.h"

#include "device.h"
#include "memory_export.h"
#include "offscreen.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

namespace wallpaperx {

uint32_t drmFourccForVkFormat(VkFormat format) {
    // The X (ignore-alpha) variants are deliberate. A wallpaper is the bottom-most
    // surface on the desktop, so it must be opaque -- but nothing in the render path
    // forces alpha to 1, and StaticRenderer writes a source PNG's alpha through
    // verbatim with blending disabled. Reporting ARGB would make a compositor
    // alpha-blend a transparent-background image into whatever is behind it (black),
    // instead of showing it. Telling the compositor to ignore alpha is what actually
    // matches the intent; the A variants stay defined for a future surface that wants
    // real transparency. This applies to the 10-bit entry for the same reason -- the
    // cause is the render path, not the pixel format.
    switch (format) {
        case VK_FORMAT_B8G8R8A8_UNORM:           return kDrmFormatXrgb8888;
        case VK_FORMAT_B8G8R8A8_SRGB:            return kDrmFormatXrgb8888;
        case VK_FORMAT_R8G8B8A8_UNORM:           return kDrmFormatXbgr8888;
        case VK_FORMAT_R8G8B8A8_SRGB:            return kDrmFormatXbgr8888;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return kDrmFormatXbgr2101010;
        default:                                 return 0;
    }
}

namespace {

// Describes where each memory plane sits inside the exported allocation. Without this an
// importer only has an fd and no way to interpret it.
//
// Valid only for an image built with VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT: asking
// vkGetImageSubresourceLayout for a VK_IMAGE_ASPECT_MEMORY_PLANE_n_BIT_EXT aspect requires
// modifier tiling. createExportableImage() guarantees it, and the call site checks
// target.image.drmModifierTiled before getting here.
void populateDmaBufLayout(Device& device, const OffscreenTarget& target, ExportedFrame& frame) {
    // Recorded when the image was created; an image's modifier cannot change afterwards.
    const uint64_t chosen = target.drmModifier;

    // How many memory planes that modifier uses is a property of the modifier, not
    // the format, so it has to come back from the same list the image was created
    // from. Export is a cold path (once per SetWallpaper), so re-querying here is
    // cheaper than threading the plane count through OffscreenTarget.
    const std::vector<VkDrmFormatModifierPropertiesEXT> all =
        queryDrmModifierList(device.physicalDevice(), OffscreenTargetManager::kFormat);

    uint32_t planeCount = 0;
    for (const auto& candidate : all) {
        if (candidate.drmFormatModifier == chosen) {
            planeCount = candidate.drmFormatModifierPlaneCount;
            break;
        }
    }
    if (planeCount == 0) {
        throw std::runtime_error("exportFrame: driver reported a DRM format modifier it does not list as supported");
    }

    // The driver picks from the list it was offered; nothing until here checks what it picked.
    // A pick outside the consumer's reported set exports cleanly and fails every import, with
    // has_frame true and no error to explain it.
    const std::vector<uint64_t>& agreed = device.memory().consumerModifiers();
    if (!agreed.empty() &&
        std::find(agreed.begin(), agreed.end(), chosen) == agreed.end()) {
        throw std::runtime_error("exportFrame: the driver chose DRM format modifier " +
                                 std::to_string(chosen) +
                                 ", which the frame consumer did not report as importable");
    }
    if (planeCount > kMaxExportedPlanes) {
        throw std::runtime_error("exportFrame: the driver chose a DRM format modifier spanning " +
                                 std::to_string(planeCount) + " memory planes, more than the importer describes");
    }

    constexpr VkImageAspectFlagBits kMemoryPlaneAspects[] = {
        VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT, VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT,
        VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT, VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT};
    if (planeCount > std::size(kMemoryPlaneAspects)) {
        throw std::runtime_error("exportFrame: DRM format modifier uses more memory planes than Vulkan defines");
    }

    frame.planes.reserve(planeCount);
    for (uint32_t plane = 0; plane < planeCount; ++plane) {
        VkImageSubresource subresource{};
        subresource.aspectMask = kMemoryPlaneAspects[plane];
        VkSubresourceLayout layout{};
        vkGetImageSubresourceLayout(device.device(), target.image.image, &subresource, &layout);
        frame.planes.push_back(ExportedFramePlane{layout.offset, layout.rowPitch});
    }

    frame.modifier = chosen;
}

// The pipeline stage and access scope that produced the image's current contents,
// inferred from the layout a renderer left it in. The release barrier has to make
// *those* writes available; declaring a colour-attachment write for what was actually
// a transfer or compute write means the release orders nothing, and the consumer sees
// the previous frame. Conservative by default -- a too-wide source scope costs a
// little on a once-per-frame barrier, a too-narrow one corrupts.
std::pair<VkPipelineStageFlags, VkAccessFlags> sourceScopeFor(VkImageLayout layout) {
    switch (layout) {
        case VK_IMAGE_LAYOUT_UNDEFINED:
            return {VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0};  // nothing was written
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            return {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            return {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT};
        default:
            return {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT};
    }
}

// Hands the image over to a consumer outside this Vulkan device: transitions it to
// VK_IMAGE_LAYOUT_GENERAL and releases queue-family ownership. Without this the image
// is still owned by the graphics queue in whatever layout the renderer left, and what
// an external importer sees is formally undefined -- on a compressed modifier (AMD
// DCC, Intel CCS) the metadata we report then describes bytes the compositor cannot
// read. Lavapipe picks LINEAR and never compresses, so it cannot witness that; the
// visible failure is AMD/Intel-only.
//
// Applies to both handle types. An opaque fd is imported by a second Vulkan device,
// which must acquire from VK_QUEUE_FAMILY_EXTERNAL -- an acquire with no matching
// release is an incomplete transfer, so skipping this on the NVIDIA path would leave
// the same defect there. OffscreenTargetManager::render() issues the paired acquire
// before the next render.
void releaseToExternalConsumer(Device& device, const OffscreenTarget& target, ExportedFrame::HandleType handleType) {
    // Keyed on ownership, not on the layout being GENERAL: GENERAL is also a layout a
    // renderer may legitimately leave the image in, and treating it as "already
    // released" would skip the release entirely for such a frame.
    if (target.externalOwner != VK_QUEUE_FAMILY_IGNORED) {
        return;  // already released and not re-rendered since
    }

    // FOREIGN is for a consumer outside Vulkan entirely (a compositor importing a
    // dma_buf); EXTERNAL (core since 1.1) is for another Vulkan instance, which is
    // what an opaque fd is always imported by. FOREIGN also degrades to EXTERNAL when
    // VK_EXT_queue_family_foreign is unavailable.
    const bool useForeign = handleType == ExportedFrame::HandleType::DmaBuf && device.info().hasQueueFamilyForeign;
    const uint32_t dstQueueFamily = useForeign ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_EXTERNAL;

    const auto [srcStage, srcAccess] = sourceScopeFor(target.exportLayout);

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = 0;  // ownership release: the acquire side does the rest
    // Read from the target rather than assumed: exportFrame is reachable on a target
    // that was never rendered into, whose image is still UNDEFINED, and a renderer may
    // leave it in a layout other than the shared render pass's finalLayout.
    barrier.oldLayout = target.exportLayout;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = device.graphicsQueueFamily();
    barrier.dstQueueFamilyIndex = dstQueueFamily;
    barrier.image = target.image.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    device.immediateSubmit([&](VkCommandBuffer cmd) {
        vkCmdPipelineBarrier(cmd, srcStage, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &barrier);
    });

    target.releasedFromLayout = barrier.oldLayout;
    target.exportLayout = VK_IMAGE_LAYOUT_GENERAL;
    target.externalOwner = dstQueueFamily;
}

}  // namespace

ExportedFrame exportFrame(Device& device, const OffscreenTarget& target) {
    // The export submits an ownership-release barrier, so it is GPU work like any other
    // and belongs on the same thread as the renders it interleaves with. In the daemon
    // this is what makes GetFrameFd's asynchronous handling load-bearing rather than
    // stylistic: answering it inline on the DBus thread would trip this.
    device.assertGpuThread("exportFrame");

    auto pfnGetMemoryFd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(
        vkGetDeviceProcAddr(device.device(), "vkGetMemoryFdKHR"));
    if (!pfnGetMemoryFd) {
        throw std::runtime_error("exportFrame: vkGetMemoryFdKHR entry point missing");
    }

    const VkExternalMemoryHandleTypeFlagBits handleType =
        externalMemoryHandleType(device.info().vendor, device.info().hasExternalMemoryDmaBuf);

    // Resolve everything that can throw (both do a live-allocation check) BEFORE
    // acquiring the fd, so no code path can leak an already-open fd on a later throw.
    VkMemoryGetFdInfoKHR getFdInfo{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
    getFdInfo.memory = device.memory().imageDeviceMemory(target.image);
    getFdInfo.handleType = handleType;
    const VkDeviceSize memorySize = device.memory().imageAllocationSize(target.image);
    const uint32_t drmFormat = drmFourccForVkFormat(OffscreenTargetManager::kFormat);
    if (drmFormat == 0) {
        throw std::runtime_error("exportFrame: no DRM fourcc mapping for the offscreen target's format -- "
                                 "before changing kFormat, add a case to drmFourccForVkFormat AND to "
                                 "FrameImport::import's format switch, which refuses what it cannot lay out");
    }

    ExportedFrame frame;
    // Validate the fd itself, not just the return code: some drivers (observed on
    // Lavapipe's DMA_BUF path under fd exhaustion) return VK_SUCCESS while writing
    // fd = -1, which would otherwise be handed back as a valid-looking frame with a
    // dead fd. exportFrame's contract is "a fresh valid fd, or throw".
    if (pfnGetMemoryFd(device.device(), &getFdInfo, &frame.fd) != VK_SUCCESS || frame.fd < 0) {
        throw std::runtime_error("exportFrame: vkGetMemoryFdKHR failed to produce a valid fd");
    }

    frame.width = target.width;
    frame.height = target.height;
    frame.memorySize = memorySize;
    // Reported for BOTH handle types, not just dma_buf. An opaque fd carries no layout
    // metadata, but it still has a CHANNEL ORDER, and an importer that guesses gets red and
    // blue the wrong way round -- which is exactly what the first cross-process frame looked
    // like.
    frame.drmFormat = drmFormat;
    frame.deviceUuid = device.info().deviceUuid;
    frame.bufferId = target.bufferId;
    frame.type = (handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT) ? ExportedFrame::HandleType::OpaqueFd
                                                                               : ExportedFrame::HandleType::DmaBuf;

    try {
        // Gated on how the image was actually created, not on device capability:
        // querying a modifier from a non-modifier image is a spec violation. The two
        // must agree -- createExportableImage now refuses to produce a DMA_BUF image
        // without an explicit modifier, so a dma_buf frame always carries real layout
        // metadata and never the "layout unknown" sentinels.
        if (frame.type == ExportedFrame::HandleType::DmaBuf) {
            if (!target.image.drmModifierTiled) {
                throw std::runtime_error(
                    "exportFrame: dma_buf export of an image created without an explicit DRM format modifier -- "
                    "no importer can interpret it");
            }
            populateDmaBufLayout(device, target, frame);
        }
        releaseToExternalConsumer(device, target, frame.type);
    } catch (...) {
        // The fd is already open at this point and nothing else will close it.
        close(frame.fd);
        throw;
    }

    return frame;
}

}
