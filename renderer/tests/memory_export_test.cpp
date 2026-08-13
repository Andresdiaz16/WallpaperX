// SPDX-License-Identifier: GPL-2.0-or-later

#include "vulkan/memory_export.h"

#include "support/check.h"

#include <iostream>
#include <stdexcept>

namespace {

using wallpaperx::testing::check;

}

int main() {
    using wallpaperx::GpuVendor;

    // Every policy check below goes through the three-argument form with an explicit
    // nullptr override, never the two-argument one.
    //
    // The two-argument overload reads WALLPAPERX_FORCE_HANDLE_TYPE. Using it here would
    // make this file's verdict depend on the tester's shell: a developer who exported
    // that variable to exercise the dma_buf path -- which is exactly what
    // modifier_roundtrip_probe documents doing -- would get a SIGABRT from the throwing
    // case below, or four FAIL lines naming code that is correct. Passing the override
    // explicitly is what keeps these assertions about the policy rather than about the
    // environment they happen to run in.
    constexpr const char* kNoOverride = nullptr;

    check(wallpaperx::externalMemoryHandleType(GpuVendor::NVIDIA, /*hasExternalMemoryDmaBuf=*/false, kNoOverride) == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
          "NVIDIA uses OPAQUE_FD regardless of the dma_buf capability flag");
    check(wallpaperx::externalMemoryHandleType(GpuVendor::AMD, /*hasExternalMemoryDmaBuf=*/true, kNoOverride) == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
          "AMD with dma_buf support uses DMA_BUF");
    check(wallpaperx::externalMemoryHandleType(GpuVendor::Intel, /*hasExternalMemoryDmaBuf=*/true, kNoOverride) == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
          "Intel with dma_buf support uses DMA_BUF");
    check(wallpaperx::externalMemoryHandleType(GpuVendor::Other, /*hasExternalMemoryDmaBuf=*/true, kNoOverride) == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
          "Other vendor with dma_buf support falls back to DMA_BUF");

    try {
        wallpaperx::externalMemoryHandleType(GpuVendor::AMD, /*hasExternalMemoryDmaBuf=*/false, kNoOverride);
        check(false, "AMD without dma_buf support should throw, not silently return an unusable handle type");
    } catch (const std::runtime_error&) {
    }

    // WALLPAPERX_FORCE_HANDLE_TYPE, exercised the same way: values are passed in, never
    // set in the environment, so nothing here is order- or shell-dependent.
    check(wallpaperx::externalMemoryHandleType(GpuVendor::NVIDIA, /*hasExternalMemoryDmaBuf=*/true, "dmabuf") ==
              VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
          "forcing dmabuf overrides the NVIDIA OPAQUE_FD policy when the driver supports it");
    check(wallpaperx::externalMemoryHandleType(GpuVendor::AMD, /*hasExternalMemoryDmaBuf=*/true, "opaque") ==
              VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
          "forcing opaque overrides the dma_buf default");

    // A forced dma_buf on a driver that cannot do it must fail loudly. Silently handing
    // back OPAQUE_FD would make the override look like it worked and send the caller
    // down the modifier path for a handle that has none.
    try {
        wallpaperx::externalMemoryHandleType(GpuVendor::NVIDIA, /*hasExternalMemoryDmaBuf=*/false, "dmabuf");
        check(false, "forcing dmabuf without dma_buf support should throw");
    } catch (const std::runtime_error&) {
    }

    // An unrecognised value is not a third mode. It falls through to the normal policy,
    // so a typo degrades to default behaviour instead of changing it.
    check(wallpaperx::externalMemoryHandleType(GpuVendor::NVIDIA, /*hasExternalMemoryDmaBuf=*/true, "dma-buf") ==
              VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
          "an unrecognised WALLPAPERX_FORCE_HANDLE_TYPE value falls through to the default policy");
    check(wallpaperx::externalMemoryHandleType(GpuVendor::NVIDIA, /*hasExternalMemoryDmaBuf=*/true, "") ==
              VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
          "an empty WALLPAPERX_FORCE_HANDLE_TYPE falls through to the default policy");

    if (wallpaperx::testing::reportFailures("memory_export_test") == 0) {
        std::cout << "memory_export_test: all checks passed" << std::endl;
        return 0;
    }
    return 1;
}
