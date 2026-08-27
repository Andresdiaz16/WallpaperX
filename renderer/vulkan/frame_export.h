// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "drm_formats.h"
#include "memory.h"

#include <array>
#include <cstdint>
#include <vector>

namespace wallpaperx {

class Device;
struct OffscreenTarget;

// The DRM fourcc naming a compositor needs for `format`, or 0 if this format has no
// mapping here. Returning 0 rather than guessing is deliberate: an importer told the
// wrong fourcc renders swapped channels or garbage, whereas 0 means "unknown" and
// makes the export fail loudly. Callers must treat 0 as an error, not a default.
uint32_t drmFourccForVkFormat(VkFormat format);

// Where one plane of a DMA-BUF frame lives within the exported memory. Multi-planar
// modifiers (common on Intel, e.g. compressed layouts with a separate metadata plane)
// need one of these per plane, which is why this is a list rather than a single
// stride -- a scalar stride silently describes only single-plane layouts.
struct ExportedFramePlane {
    uint64_t offset = 0;
    uint64_t rowPitch = 0;
};

struct ExportedFrame {
    enum class HandleType { OpaqueFd, DmaBuf };

    int fd = -1;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t memorySize = 0;
    HandleType type = HandleType::OpaqueFd;

    // The channel order, reported on BOTH paths: an opaque fd carries no layout but its bytes
    // still have an order, and an importer that guesses draws red and blue transposed.
    uint32_t drmFormat = 0;

    // Which GPU allocated this memory. Reported on both paths, enforced only on the opaque
    // one, where importing on any other device cannot work.
    std::array<uint8_t, VK_UUID_SIZE> deviceUuid{};

    // DMA-BUF layout metadata (AMD/Intel), describing how to interpret the bytes
    // behind `fd`. Populated on the DmaBuf path only; the OpaqueFd (NVIDIA) path
    // leaves them at their unset sentinels because an opaque fd is importable only
    // by another Vulkan/GL context on the same device, which already knows the
    // layout. Note that modifier's sentinel is kDrmModifierInvalid rather than 0 --
    // 0 is the valid DRM_FORMAT_MOD_LINEAR, so a zero default would be
    // indistinguishable from a genuine linear layout.
    uint64_t modifier = kDrmModifierInvalid;
    std::vector<ExportedFramePlane> planes;
};

// Exports target.image's backing memory as a new file descriptor. Every call
// produces a fresh fd via a fresh vkGetMemoryFdKHR call (Vulkan's own contract
// -- see Key Gotchas: "Reusing fd after import" in PLAN.md); the caller owns
// the returned fd and must close() it if it isn't handed off to an importer.
ExportedFrame exportFrame(Device& device, const OffscreenTarget& target);

}
