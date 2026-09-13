// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vulkan/vulkan.h>

#include <string>

namespace wallpaperx {

// What `format` supports under `tiling`, through the 64-bit query every Device has, since it
// requires Vulkan 1.3. The low 31 bits match VkFormatFeatureFlags, so either flag type tests
// against it. Any tiling but LINEAR reads the optimal features.
VkFormatFeatureFlags2 formatFeatures(VkPhysicalDevice physicalDevice, VkFormat format, VkImageTiling tiling);

// The Vulkan name of a format the renderer or a decoder plausibly handles, otherwise its number.
std::string formatName(VkFormat format);

}
