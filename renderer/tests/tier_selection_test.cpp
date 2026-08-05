#include "vulkan/tier_selection.h"

#include "support/check.h"

#include <iostream>
#include <optional>

namespace {

using wallpaperx::testing::check;

using wallpaperx::DescriptorTier;
using wallpaperx::GpuVendor;
using wallpaperx::TierOverride;

void testVendorFromId() {
    check(wallpaperx::vendorFromId(0x10DE) == GpuVendor::NVIDIA, "0x10DE maps to NVIDIA");
    check(wallpaperx::vendorFromId(0x1002) == GpuVendor::AMD, "0x1002 maps to AMD");
    check(wallpaperx::vendorFromId(0x8086) == GpuVendor::Intel, "0x8086 maps to Intel");
    check(wallpaperx::vendorFromId(0xDEAD) == GpuVendor::Other, "unknown vendor ID maps to Other");
}

void testParseTierOverride() {
    check(wallpaperx::parseTierOverride(nullptr) == std::nullopt, "null env value parses to nullopt");
    check(wallpaperx::parseTierOverride("") == std::nullopt, "empty env value parses to nullopt");
    check(wallpaperx::parseTierOverride("heap") == std::nullopt, "\"heap\" parses to nullopt (already top priority)");
    check(wallpaperx::parseTierOverride("garbage") == std::nullopt, "unrecognized value parses to nullopt");
    check(wallpaperx::parseTierOverride("buffer") == TierOverride::Buffer, "\"buffer\" parses to TierOverride::Buffer");
    check(wallpaperx::parseTierOverride("classic") == TierOverride::Classic, "\"classic\" parses to TierOverride::Classic");
}

void testSelectTierNoOverride() {
    check(wallpaperx::selectTier(true, true, std::nullopt) == DescriptorTier::Heap,
          "heap+buffer supported, no override -> Heap");
    check(wallpaperx::selectTier(false, true, std::nullopt) == DescriptorTier::Buffer,
          "buffer only, no override -> Buffer");
    check(wallpaperx::selectTier(false, false, std::nullopt) == DescriptorTier::Classic,
          "neither supported, no override -> Classic");
}

void testSelectTierWithOverride() {
    check(wallpaperx::selectTier(true, true, TierOverride::Buffer) == DescriptorTier::Buffer,
          "heap+buffer supported, forced buffer -> Buffer");
    check(wallpaperx::selectTier(true, false, TierOverride::Buffer) == DescriptorTier::Classic,
          "heap only, forced buffer -> falls to Classic (buffer unsupported)");
    check(wallpaperx::selectTier(true, true, TierOverride::Classic) == DescriptorTier::Classic,
          "heap+buffer supported, forced classic -> Classic");
    check(wallpaperx::selectTier(false, false, TierOverride::Classic) == DescriptorTier::Classic,
          "nothing supported, forced classic -> Classic");
}

}

int main() {
    testVendorFromId();
    testParseTierOverride();
    testSelectTierNoOverride();
    testSelectTierWithOverride();

    if (wallpaperx::testing::reportFailures("tier_selection_test") == 0) {
        std::cout << "tier_selection_test: all checks passed" << std::endl;
        return 0;
    }
    return 1;
}
