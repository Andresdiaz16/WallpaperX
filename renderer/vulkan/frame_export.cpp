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
    if (pfnGetMemoryFd(device.device(), &getFdInfo, &frame.fd) != VK_SUCCESS) {
        throw std::runtime_error("exportFrame: vkGetMemoryFdKHR failed");
    }

    frame.width = target.width;
    frame.height = target.height;
    frame.memorySize = memorySize;
    frame.type = (handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT) ? ExportedFrame::HandleType::OpaqueFd
                                                                               : ExportedFrame::HandleType::DmaBuf;

    // drmFormat/stride/modifier intentionally left zeroed -- see PLAN.md Step 5's
    // scope decision. The DMA-BUF (AMD/Intel) path is structurally present but
    // not verified on real hardware; correct compositor import additionally
    // needs the offscreen image created with an explicit DRM format modifier,
    // which is deferred to a follow-up step.

    return frame;
}

}
