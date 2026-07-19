#pragma once

#include "memory.h"

#include <cstdint>

namespace wallpaperx {

class Device;
struct OffscreenTarget;

struct ExportedFrame {
    enum class HandleType { OpaqueFd, DmaBuf };

    int fd = -1;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t memorySize = 0;
    HandleType type = HandleType::OpaqueFd;

    // DMA-BUF only (AMD/Intel) -- zeroed/unused on the OpaqueFd (NVIDIA) path.
    uint32_t drmFormat = 0;
    uint32_t stride = 0;
    uint64_t modifier = 0;
};

// Exports target.image's backing memory as a new file descriptor. Every call
// produces a fresh fd via a fresh vkGetMemoryFdKHR call (Vulkan's own contract
// -- see Key Gotchas: "Reusing fd after import" in PLAN.md); the caller owns
// the returned fd and must close() it if it isn't handed off to an importer.
ExportedFrame exportFrame(Device& device, const OffscreenTarget& target);

}
