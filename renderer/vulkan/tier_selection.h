#pragma once

#include "descriptor_allocator.h"

#include <cstdint>
#include <optional>

namespace wallpaperx {

enum class GpuVendor { NVIDIA, AMD, Intel, Other };

const char* toString(GpuVendor vendor);
GpuVendor vendorFromId(uint32_t vendorId);

enum class TierOverride { Buffer, Classic };

std::optional<TierOverride> parseTierOverride(const char* envValue);

DescriptorTier selectTier(bool hasDescriptorHeap, bool hasDescriptorBuffer, std::optional<TierOverride> override);

}
