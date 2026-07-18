#include "vulkan/memory_export.h"

#include <iostream>
#include <stdexcept>

namespace {

int g_failures = 0;

void check(bool condition, const char* description) {
    if (!condition) {
        std::cerr << "FAIL: " << description << std::endl;
        ++g_failures;
    }
}

}

int main() {
    using wallpaperx::GpuVendor;

    check(wallpaperx::externalMemoryHandleType(GpuVendor::NVIDIA, /*hasExternalMemoryDmaBuf=*/false) == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
          "NVIDIA uses OPAQUE_FD regardless of the dma_buf capability flag");
    check(wallpaperx::externalMemoryHandleType(GpuVendor::AMD, /*hasExternalMemoryDmaBuf=*/true) == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
          "AMD with dma_buf support uses DMA_BUF");
    check(wallpaperx::externalMemoryHandleType(GpuVendor::Intel, /*hasExternalMemoryDmaBuf=*/true) == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
          "Intel with dma_buf support uses DMA_BUF");
    check(wallpaperx::externalMemoryHandleType(GpuVendor::Other, /*hasExternalMemoryDmaBuf=*/true) == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
          "Other vendor with dma_buf support falls back to DMA_BUF");

    try {
        wallpaperx::externalMemoryHandleType(GpuVendor::AMD, /*hasExternalMemoryDmaBuf=*/false);
        check(false, "AMD without dma_buf support should throw, not silently return an unusable handle type");
    } catch (const std::runtime_error&) {
    }

    if (g_failures == 0) {
        std::cout << "memory_export_test: all checks passed" << std::endl;
        return 0;
    }
    std::cerr << "memory_export_test: " << g_failures << " check(s) failed" << std::endl;
    return 1;
}
