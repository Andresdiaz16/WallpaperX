#pragma once

#include "memory.h"

#include <cstdint>

namespace wallpaperx {

class Device;
struct OffscreenTarget;

// DRM_FORMAT_MOD_INVALID (from drm_fourcc.h, redefined here to avoid a libdrm
// dependency): the well-known "no/unknown modifier" sentinel. Used to mark
// ExportedFrame::modifier as not-populated -- distinct from 0, which is the
// valid DRM_FORMAT_MOD_LINEAR and would otherwise be mistaken for a real layout.
inline constexpr uint64_t kDrmModifierInvalid = (1ULL << 56) - 1;

struct ExportedFrame {
    enum class HandleType { OpaqueFd, DmaBuf };

    int fd = -1;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t memorySize = 0;
    HandleType type = HandleType::OpaqueFd;

    // DMA-BUF layout metadata (AMD/Intel). Currently NOT populated on either
    // path: the OpaqueFd (NVIDIA) path doesn't need it, and the DmaBuf path is a
    // structural stub -- the offscreen image is VK_IMAGE_TILING_OPTIMAL with no
    // explicit DRM format modifier, so stride/modifier are driver-private and
    // unknowable here. These sentinels (drmFormat 0, stride 0, modifier INVALID)
    // mean "unset": a DmaBuf frame is not yet importable by a compositor. Filling
    // these in requires creating the image with VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT
    // (deferred -- see PLAN.md Step 5 scope decision).
    uint32_t drmFormat = 0;
    uint32_t stride = 0;
    uint64_t modifier = kDrmModifierInvalid;
};

// Exports target.image's backing memory as a new file descriptor. Every call
// produces a fresh fd via a fresh vkGetMemoryFdKHR call (Vulkan's own contract
// -- see Key Gotchas: "Reusing fd after import" in PLAN.md); the caller owns
// the returned fd and must close() it if it isn't handed off to an importer.
ExportedFrame exportFrame(Device& device, const OffscreenTarget& target);

}
