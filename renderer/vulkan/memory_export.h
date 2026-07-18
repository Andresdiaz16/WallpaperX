#pragma once

#include "tier_selection.h"

#include <vulkan/vulkan.h>

namespace wallpaperx {

VkExternalMemoryHandleTypeFlagBits externalMemoryHandleType(GpuVendor vendor, bool hasExternalMemoryDmaBuf);

}
