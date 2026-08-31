// SPDX-License-Identifier: GPL-2.0-or-later

#include "memory_export.h"

#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

namespace wallpaperx {

VkExternalMemoryHandleTypeFlagBits externalMemoryHandleType(GpuVendor vendor, bool hasExternalMemoryDmaBuf,
                                                            const char* forceHandleType) {
    // Compared with strcmp against the exact strings, matching WALLPAPERX_FORCE_TIER:
    // a truthiness check would make "=0" mean "forced", the opposite of what anyone
    // typing it intends. An unrecognised value falls through to the default policy
    // rather than failing, so a typo degrades to normal behaviour instead of bricking
    // the daemon -- the override exists for investigation, not for configuration.
    if (forceHandleType != nullptr) {
        if (std::strcmp(forceHandleType, "opaque") == 0) {
            return VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        }
        if (std::strcmp(forceHandleType, "dmabuf") == 0) {
            // Forced, but not unconditionally: a dma_buf handle from a driver without
            // the extension is not a degraded export, it is an invalid one. Failing here
            // names the reason; letting it through fails later inside vkGetMemoryFdKHR
            // with nothing to attribute it to.
            if (!hasExternalMemoryDmaBuf) {
                throw std::runtime_error(
                    "externalMemoryHandleType: WALLPAPERX_FORCE_HANDLE_TYPE=dmabuf, but "
                    "VK_EXT_external_memory_dma_buf is not supported on this GPU/driver");
            }
            return VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        }
    }

    // NVIDIA's proprietary driver is the reason this function exists. The policy is a
    // choice rather than a capability limit -- the driver does advertise dma_buf and DRM
    // format modifiers, and exporting one works (verified on an RTX 3080 Ti: seven
    // modifiers for B8G8R8A8_UNORM, six of them block-linear). OPAQUE_FD stays the
    // default because it is the path NVIDIA supports best and the KDE plugin's NVIDIA
    // branch is built around it; WALLPAPERX_FORCE_HANDLE_TYPE=dmabuf is how the other
    // path gets exercised on this hardware, which is what modifier_roundtrip_probe does.
    if (vendor == GpuVendor::NVIDIA) {
        return VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    }
    if (!hasExternalMemoryDmaBuf) {
        throw std::runtime_error("externalMemoryHandleType: VK_EXT_external_memory_dma_buf not supported on this GPU/driver");
    }
    return VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
}

VkExternalMemoryHandleTypeFlagBits externalMemoryHandleType(GpuVendor vendor, bool hasExternalMemoryDmaBuf) {
    // Read once, on first use, and never again.
    //
    // Re-reading getenv on every call would let the handle type change mid-process: a
    // target created while the variable was set exports as dma_buf with an explicit
    // modifier, and a later export of that same image reads the variable as unset and
    // asks for an OPAQUE_FD handle from modifier-tiled memory. The image's tiling is
    // fixed at creation and cannot follow. Resolving once means the whole process
    // agrees, which is the only useful meaning for a policy override.
    //
    // Function-local static: initialised on first call, thread-safe since C++11. Every
    // caller resolves it long after main() has had its chance to setenv.
    static const std::string forced = [] {
        const char* env = std::getenv("WALLPAPERX_FORCE_HANDLE_TYPE");
        return env != nullptr ? std::string(env) : std::string();
    }();
    // forced.c_str() unconditionally: an unset variable yields "", and "" is an
    // unrecognised override that falls through to the vendor policy -- byte for byte the
    // behaviour nullptr produces. The empty-string-to-nullptr conversion that used to sit
    // here was therefore unobservable through this function's public behaviour, which
    // means no test could cover it while one claimed to. Removing it deletes the
    // distinction rather than leaving an untestable branch behind.
    return externalMemoryHandleType(vendor, hasExternalMemoryDmaBuf, forced.c_str());
}

uint32_t externalConsumerQueueFamily(VkExternalMemoryHandleTypeFlagBits handleType, bool hasQueueFamilyForeign) {
    // FOREIGN is for a consumer outside Vulkan entirely (a compositor importing a dma_buf);
    // EXTERNAL (core since 1.1) covers another Vulkan or OpenGL context on the same device
    // and driver, which is what an opaque fd is always imported by.
    const bool foreign = handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT && hasQueueFamilyForeign;
    return foreign ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_EXTERNAL;
}

}
