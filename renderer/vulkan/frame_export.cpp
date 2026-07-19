#include "frame_export.h"

#include "device.h"
#include "memory_export.h"
#include "offscreen.h"

#include <stdexcept>

namespace wallpaperx {

ExportedFrame exportFrame(Device& device, const OffscreenTarget& target) {
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
    frame.type = (handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT) ? ExportedFrame::HandleType::OpaqueFd
                                                                               : ExportedFrame::HandleType::DmaBuf;

    // drmFormat/stride/modifier are deliberately left at their unset sentinels
    // (see ExportedFrame's field docs and PLAN.md Step 5's scope decision). The
    // DMA-BUF (AMD/Intel) branch is structurally present and produces a valid fd,
    // but the layout metadata a compositor needs to import it requires the image
    // to be created with an explicit DRM format modifier -- deferred. Leaving
    // modifier as kDrmModifierInvalid (not 0 = LINEAR) keeps that unambiguous.

    return frame;
}

}
