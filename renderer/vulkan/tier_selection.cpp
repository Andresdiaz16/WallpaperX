// SPDX-License-Identifier: GPL-2.0-or-later

#include "tier_selection.h"

#include <cstring>

namespace wallpaperx {

const char* toString(GpuVendor vendor) {
    switch (vendor) {
        case GpuVendor::NVIDIA: return "NVIDIA";
        case GpuVendor::AMD: return "AMD";
        case GpuVendor::Intel: return "Intel";
        case GpuVendor::Other: return "Other";
    }
    return "Unknown";
}

GpuVendor vendorFromId(uint32_t vendorId) {
    switch (vendorId) {
        case 0x10DE: return GpuVendor::NVIDIA;
        case 0x1002: return GpuVendor::AMD;
        case 0x8086: return GpuVendor::Intel;
        default: return GpuVendor::Other;
    }
}

std::optional<TierOverride> parseTierOverride(const char* envValue) {
    if (!envValue) return std::nullopt;
    if (std::strcmp(envValue, "buffer") == 0) return TierOverride::Buffer;
    if (std::strcmp(envValue, "classic") == 0) return TierOverride::Classic;
    return std::nullopt;
}

DescriptorTier selectTier(bool hasDescriptorHeap, bool hasDescriptorBuffer, std::optional<TierOverride> override) {
    if (override == TierOverride::Classic) return DescriptorTier::Classic;
    if (override == TierOverride::Buffer) hasDescriptorHeap = false;

    if (hasDescriptorHeap) return DescriptorTier::Heap;
    if (hasDescriptorBuffer) return DescriptorTier::Buffer;
    return DescriptorTier::Classic;
}

}
