// SPDX-License-Identifier: GPL-2.0-or-later

#include "frame_export.h"

#include "device.h"
#include "memory_export.h"
#include "offscreen.h"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
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
    // A driver contradicting its own modifier list is the driver going wrong. The two checks
    // below are not: one is a negotiation outcome and one is this importer's limit, so they
    // stay unclassified and leave the GPU's recorded health alone.
    if (planeCount == 0) {
        throw GpuFailure("exportFrame: driver reported a DRM format modifier it does not list as supported");
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

}  // namespace

ExportedFrame exportFrame(Device& device, const OffscreenTarget& target) {
    // Memory's live-allocation map is a plain container written by every image create and
    // destroy, and the queries below read it -- so this reads GPU-owned state and belongs
    // on the same thread as the renders it interleaves with. In the daemon this is what
    // makes GetFrameFd's asynchronous handling load-bearing rather than stylistic:
    // answering it inline on the DBus thread would trip this.
    device.assertGpuThread("exportFrame");

    // Renders do not wait, so without this the descriptor names memory the GPU is still
    // writing, and its receiver -- a host mmap above all -- has nothing to wait with.
    device.waitForSubmissions();

    auto pfnGetMemoryFd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(
        vkGetDeviceProcAddr(device.device(), "vkGetMemoryFdKHR"));
    if (!pfnGetMemoryFd) {
        // Device selection refuses a GPU without VK_KHR_external_memory_fd and the device is
        // created with it enabled, so a missing entry point is the driver withholding what it
        // advertised -- a GPU fault, not a configuration one.
        throw GpuFailure("exportFrame: vkGetMemoryFdKHR entry point missing");
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
    const VkResult fdResult = pfnGetMemoryFd(device.device(), &getFdInfo, &frame.fd);
    if (fdResult != VK_SUCCESS || frame.fd < 0) {
        // The two results this call may return are both host limits, and so is a VK_SUCCESS
        // with no descriptor to show for it: the cause is fd exhaustion, usually a consumer
        // leaking the ones GetFrameFd hands it. Calling that a GPU fault points an operator
        // at their driver for a problem in RLIMIT_NOFILE. Only an unpermitted result is the
        // driver going wrong.
        const bool hostLimit = fdResult == VK_SUCCESS || fdResult == VK_ERROR_TOO_MANY_OBJECTS ||
                               fdResult == VK_ERROR_OUT_OF_HOST_MEMORY;
        if (hostLimit) {
            throw std::runtime_error("exportFrame: vkGetMemoryFdKHR produced no file descriptor for the frame -- "
                                     "the process is out of file descriptors, or a consumer is leaking them");
        }
        throw GpuFailure("exportFrame: vkGetMemoryFdKHR returned " + std::to_string(static_cast<int>(fdResult)) +
                         ", which it is not permitted to return");
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
    } catch (...) {
        // The fd is already open at this point and nothing else will close it.
        close(frame.fd);
        throw;
    }

    return frame;
}

int exportFrameAcquireFd(Device& device, const OffscreenTarget& target) {
    device.assertGpuThread("exportFrameAcquireFd");

    if (!target.published()) {
        throw std::runtime_error("exportFrameAcquireFd: the target has no published frame to acquire");
    }
    if (!device.info().hasExternalSemaphoreFd) {
        // exportFrame's rule again, for the caller that re-fetches an acquire without
        // re-exporting: nothing leaves naming a frame the GPU has not finished.
        device.waitForSubmissions();
        return -1;
    }

    auto pfnGetSemaphoreFd = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(
        vkGetDeviceProcAddr(device.device(), "vkGetSemaphoreFdKHR"));
    if (!pfnGetSemaphoreFd) {
        // The device was created with the extension enabled, so a missing entry point is the
        // driver withholding what it advertised.
        throw GpuFailure("exportFrameAcquireFd: vkGetSemaphoreFdKHR entry point missing");
    }

    VkExportSemaphoreCreateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
    exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    semaphoreInfo.pNext = &exportInfo;

    VkSemaphore semaphore = VK_NULL_HANDLE;
    if (const VkResult r = vkCreateSemaphore(device.device(), &semaphoreInfo, nullptr, &semaphore); r != VK_SUCCESS) {
        throwVkFailure("exportFrameAcquireFd: vkCreateSemaphore", r);
    }

    // Destroyed on every path where the GPU is not still signalling it, including the throws out
    // of submit() and the fd query. The exported descriptor holds its own reference to the
    // payload -- measured on this driver, not assumed -- so the signal survives this object by
    // exactly as long as the consumer needs it.
    struct Cleanup {
        Device& device;
        VkSemaphore semaphore;
        // Names the batch that signals this semaphore for as long as that batch may still be
        // running: unset before submit() accepts it, and again once the wait below returns.
        // Those are the only two states in which destroying the semaphore is legal.
        std::optional<Device::SubmitTicket> inFlight;

        ~Cleanup() {
            // A bounded drain rather than another wait(): a destructor cannot throw, and the
            // path that gets here is the one where the wait below already expired with the
            // batch still holding this semaphore. Leaked rather than destroyed then, as
            // everything else abandoned to a wedged GPU is.
            if (inFlight && !device.drainForTeardown()) {
                std::fprintf(stderr,
                             "exportFrameAcquireFd: the GPU has not finished the batch that signals this "
                             "frame's acquire semaphore, so it is leaked rather than destroyed\n");
                return;
            }
            vkDestroySemaphore(device.device(), semaphore, nullptr);
        }
    } cleanup{device, semaphore, std::nullopt};

    // Empty command buffer: the batch exists only to carry the signal, whose first
    // synchronization scope covers everything submitted earlier on this queue -- the render
    // that published this frame included. Waited on, so the descriptor comes from a payload
    // already signalled and a producer that dies next leaves nothing to wait on forever. That
    // wait is transitively the render's: the export pays for the frame, the render does not.
    cleanup.inFlight = device.submit([](VkCommandBuffer) {}, {{}, {}, {semaphore}});
    device.wait(*cleanup.inFlight);
    cleanup.inFlight.reset();

    int fd = -1;
    VkSemaphoreGetFdInfoKHR getFdInfo{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
    getFdInfo.semaphore = semaphore;
    getFdInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    const VkResult r = pfnGetSemaphoreFd(device.device(), &getFdInfo, &fd);
    if (r != VK_SUCCESS || fd < 0) {
        // Classified as vkGetMemoryFdKHR's results are: running out of descriptors is the host's
        // limit and usually a consumer leaking them, and blaming the GPU for it points an operator
        // at their driver over RLIMIT_NOFILE.
        if (r == VK_SUCCESS || r == VK_ERROR_TOO_MANY_OBJECTS || r == VK_ERROR_OUT_OF_HOST_MEMORY) {
            throw std::runtime_error("exportFrameAcquireFd: vkGetSemaphoreFdKHR produced no file descriptor -- "
                                     "the process is out of file descriptors, or a consumer is leaking them");
        }
        throw GpuFailure("exportFrameAcquireFd: vkGetSemaphoreFdKHR returned " +
                         std::to_string(static_cast<int>(r)) + ", which it is not permitted to return");
    }
    return fd;
}

}
