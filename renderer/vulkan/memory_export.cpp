#include "memory_export.h"

#include <stdexcept>

namespace wallpaperx {

VkExternalMemoryHandleTypeFlagBits externalMemoryHandleType(GpuVendor vendor, bool hasExternalMemoryDmaBuf) {
    if (vendor == GpuVendor::NVIDIA) {
        return VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    }
    if (!hasExternalMemoryDmaBuf) {
        throw std::runtime_error("externalMemoryHandleType: VK_EXT_external_memory_dma_buf not supported on this GPU/driver");
    }
    return VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
}

}
