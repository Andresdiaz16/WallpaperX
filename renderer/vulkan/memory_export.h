// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "tier_selection.h"

#include <vulkan/vulkan.h>

namespace wallpaperx {

// How this GPU's memory should be handed to another process.
//
// The two-argument form reads the WALLPAPERX_FORCE_HANDLE_TYPE environment variable
// ("dmabuf" or "opaque"); the three-argument form takes that value directly and is the
// pure decision function, kept separate so it can be unit-tested without a process-wide
// setenv -- the same split tier_selection uses for WALLPAPERX_FORCE_TIER.
VkExternalMemoryHandleTypeFlagBits externalMemoryHandleType(GpuVendor vendor, bool hasExternalMemoryDmaBuf);
VkExternalMemoryHandleTypeFlagBits externalMemoryHandleType(GpuVendor vendor, bool hasExternalMemoryDmaBuf,
                                                            const char* forceHandleType);

}
