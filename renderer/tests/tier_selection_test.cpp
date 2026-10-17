// SPDX-License-Identifier: GPL-2.0-or-later

#include "vulkan/tier_selection.h"

#include "support/check.h"

#include <iostream>
#include <optional>

namespace {

using wallpaperx::testing::check;

using wallpaperx::DescriptorTier;
using wallpaperx::GpuVendor;
using wallpaperx::TierOverride;

// GOAL: S2.4 -- each known PCI vendor id maps to its vendor and anything else to Other.
// CONTROL: renderer/vulkan/tier_selection.cpp vendorFromId, the cases changed to 0x10DF and 0x1003
//       and the default to Intel -> "FAIL: 0x10DE maps to NVIDIA", "FAIL: 0x1002 maps to AMD",
//       "FAIL: unknown vendor ID maps to Other". Reverted. The Intel line has no control of its own yet.
void testVendorFromId() {
    check(wallpaperx::vendorFromId(0x10DE) == GpuVendor::NVIDIA, "0x10DE maps to NVIDIA");
    check(wallpaperx::vendorFromId(0x1002) == GpuVendor::AMD, "0x1002 maps to AMD");
    check(wallpaperx::vendorFromId(0x8086) == GpuVendor::Intel, "0x8086 maps to Intel");
    check(wallpaperx::vendorFromId(0xDEAD) == GpuVendor::Other, "unknown vendor ID maps to Other");
}

// GOAL: S2.6 -- only the exact strings "buffer" and "classic" are overrides; null and anything
//       else are no override.
// CONTROL: renderer/vulkan/tier_selection.cpp parseTierOverride, null mapped to Classic, the two
//       strings swapped and the fall-through returning Buffer -> all four lines red, e.g. "FAIL: null
//       env value parses to nullopt", "FAIL: \"buffer\" parses to TierOverride::Buffer". Reverted.
void testParseTierOverride() {
    check(wallpaperx::parseTierOverride(nullptr) == std::nullopt, "null env value parses to nullopt");
    check(wallpaperx::parseTierOverride("garbage") == std::nullopt, "unrecognized value parses to nullopt");
    check(wallpaperx::parseTierOverride("buffer") == TierOverride::Buffer, "\"buffer\" parses to TierOverride::Buffer");
    check(wallpaperx::parseTierOverride("classic") == TierOverride::Classic, "\"classic\" parses to TierOverride::Classic");
}

// GOAL: S2.5 -- with no override the highest supported tier wins: Heap > Buffer > Classic.
// CONTROL: renderer/vulkan/tier_selection.cpp selectTier, each a separate break, each reverted:
//       the heap branch returning Buffer -> "FAIL: heap+buffer supported, no override -> Heap";
//       the buffer branch returning Classic -> "FAIL: buffer only, no override -> Buffer";
//       the final return made Buffer (with the Classic-override line deleted, the break named
//       below) -> "FAIL: neither supported, no override -> Classic".
void testSelectTierNoOverride() {
    check(wallpaperx::selectTier(true, true, std::nullopt) == DescriptorTier::Heap,
          "heap+buffer supported, no override -> Heap");
    check(wallpaperx::selectTier(false, true, std::nullopt) == DescriptorTier::Buffer,
          "buffer only, no override -> Buffer");
    check(wallpaperx::selectTier(false, false, std::nullopt) == DescriptorTier::Classic,
          "neither supported, no override -> Classic");
}

// GOAL: S2.6 -- a forced tier is honoured, and forcing buffer never yields Heap even where Heap exists.
// CONTROL: renderer/vulkan/tier_selection.cpp selectTier, each reverted: the
//       `override == TierOverride::Buffer` line deleted -> "FAIL: heap+buffer supported, forced buffer
//       -> Buffer" and "FAIL: heap only, forced buffer -> falls to Classic (buffer unsupported)"; the
//       Classic-override line deleted with the final return made Buffer -> "FAIL: heap+buffer
//       supported, forced classic -> Classic".
void testSelectTierWithOverride() {
    check(wallpaperx::selectTier(true, true, TierOverride::Buffer) == DescriptorTier::Buffer,
          "heap+buffer supported, forced buffer -> Buffer");
    check(wallpaperx::selectTier(true, false, TierOverride::Buffer) == DescriptorTier::Classic,
          "heap only, forced buffer -> falls to Classic (buffer unsupported)");
    check(wallpaperx::selectTier(true, true, TierOverride::Classic) == DescriptorTier::Classic,
          "heap+buffer supported, forced classic -> Classic");
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
