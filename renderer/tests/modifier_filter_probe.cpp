// SPDX-License-Identifier: GPL-2.0-or-later
//
// usableDrmModifiers()'s tiling-feature filter, and that Memory::createExportableImage applies it
// to dma_buf exports only. An offscreen target's export code is what asks for those features.

#include "vulkan/device.h"
#include "vulkan/memory.h"
#include "vulkan/offscreen.h"

#include "support/check.h"
#include "support/validation_grade.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

namespace {

using wallpaperx::AllocatedImage;
using wallpaperx::Device;
using wallpaperx::OffscreenTargetManager;
using wallpaperx::testing::check;
using wallpaperx::testing::fail;

constexpr int kNotVerifiedExitCode = 77;
bool g_notVerified = false;

// ------------------------------------------------------------------------- //
// The modifier filter's tiling-feature requirement.
// ------------------------------------------------------------------------- //

struct FeatureBit {
    VkFormatFeatureFlags bit;
    const char* name;
};

// The core format features a DRM modifier's tiling can advertise. Enumerated rather than
// looped over all 32 bit positions so a failure names something, and because the reserved
// positions would add nothing but noise.
constexpr FeatureBit kFeatureBits[] = {
    {VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT, "SAMPLED_IMAGE"},
    {VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT, "STORAGE_IMAGE"},
    {VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT, "STORAGE_IMAGE_ATOMIC"},
    {VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT, "COLOR_ATTACHMENT"},
    {VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT, "COLOR_ATTACHMENT_BLEND"},
    {VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT, "DEPTH_STENCIL_ATTACHMENT"},
    {VK_FORMAT_FEATURE_BLIT_SRC_BIT, "BLIT_SRC"},
    {VK_FORMAT_FEATURE_BLIT_DST_BIT, "BLIT_DST"},
    {VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT, "SAMPLED_IMAGE_FILTER_LINEAR"},
    {VK_FORMAT_FEATURE_TRANSFER_SRC_BIT, "TRANSFER_SRC"},
    {VK_FORMAT_FEATURE_TRANSFER_DST_BIT, "TRANSFER_DST"},
    {VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT, "MIDPOINT_CHROMA_SAMPLES"},
    {VK_FORMAT_FEATURE_DISJOINT_BIT, "DISJOINT"},
    {VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT, "COSITED_CHROMA_SAMPLES"},
};

constexpr VkExtent3D kModifierExtent{64, 64, 1};
constexpr VkExternalMemoryFeatureFlags kBothWays =
    VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;

std::vector<uint64_t> askUsable(VkPhysicalDevice physicalDevice, VkFormatFeatureFlags requiredTilingFeatures) {
    return wallpaperx::usableDrmModifiers(physicalDevice, OffscreenTargetManager::kFormat, VK_IMAGE_TYPE_2D,
                                          OffscreenTargetManager::kUsage, kModifierExtent, kBothWays,
                                          requiredTilingFeatures);
}

std::string hex(uint64_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0x%016llx", static_cast<unsigned long long>(value));
    return buffer;
}

VkImageCreateInfo targetLikeImageInfo() {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = OffscreenTargetManager::kFormat;
    info.extent = kModifierExtent;
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = OffscreenTargetManager::kUsage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    return info;
}

void runModifierFilterChecks(Device& device) {
    if (!device.info().hasImageDrmFormatModifier) {
        g_notVerified = true;
        std::printf("modifier_filter_probe: NOT VERIFIED -- this driver has no DRM format modifiers, so there is no "
                    "modifier list to filter\n");
        return;
    }

    const std::vector<VkDrmFormatModifierPropertiesEXT> all =
        wallpaperx::queryDrmModifierList(device.physicalDevice(), OffscreenTargetManager::kFormat);
    const std::vector<uint64_t> base = askUsable(device.physicalDevice(), 0);
    std::printf("modifier_filter_probe: %s reports %zu modifier(s) for the target format, %zu of them usable\n",
                device.info().gpuName.c_str(), all.size(), base.size());
    for (const VkDrmFormatModifierPropertiesEXT& properties : all) {
        std::printf("  %s tilingFeatures=0x%08x\n", hex(properties.drmFormatModifier).c_str(),
                    properties.drmFormatModifierTilingFeatures);
    }

    if (base.empty()) {
        g_notVerified = true;
        std::printf("modifier_filter_probe: NOT VERIFIED -- no modifier is usable for a target-shaped image here, so "
                    "there is nothing for the tiling-feature filter to keep or drop\n");
        return;
    }

    auto featuresOf = [&](uint64_t modifier) -> VkFormatFeatureFlags {
        for (const VkDrmFormatModifierPropertiesEXT& properties : all) {
            if (properties.drmFormatModifier == modifier) return properties.drmFormatModifierTilingFeatures;
        }
        return 0;
    };
    bool everyUsableIsListed = true;
    for (uint64_t modifier : base) {
        bool found = false;
        for (const VkDrmFormatModifierPropertiesEXT& properties : all) {
            if (properties.drmFormatModifier == modifier) found = true;
        }
        everyUsableIsListed = everyUsableIsListed && found;
    }
    // The expectations below are built out of the driver's own feature report, so a usable
    // modifier missing from that report would silently be expected to be dropped.
    // CONTROL: the queryDrmModifierList() call above asked for VK_FORMAT_R4G4_UNORM_PACK8, a
    // format with no modifiers, so the report covers none of the usable ones -> "FAIL: every
    // usable modifier appears in the driver's modifier report" and the phase abandoned rather
    // than grading seven modifiers against an empty table. Reverted.
    check(everyUsableIsListed, "every usable modifier appears in the driver's modifier report");
    if (!everyUsableIsListed) return;

    // GOAL: S13.29 -- usableDrmModifiers() drops exactly those modifiers whose own tiling features lack
    //       the caller's required ones -- so requiring a feature no modifier advertises leaves
    //       none, and requiring one only some advertise drops precisely the others.
    // WHY-THIS-SHAPE: a check that asked for VK_FORMAT_FEATURE_BLIT_DST_BIT (what production
    //       asks for) and counted survivors would establish nothing: measured here, every
    //       modifier advertises it -- seven of seven on the RTX 3080 Ti, one of one on
    //       Lavapipe -- so that check passes identically with the filter deleted. What has to
    //       be shown is a *drop*, so the required feature is chosen from the driver's own
    //       report: one bit that no usable modifier has (nothing may survive) and, where the
    //       driver offers one, a bit that splits the set (exactly the advertisers survive).
    //       Every bit in the table is swept rather than only those two, because the two
    //       interesting bits differ per driver and a sweep needs no per-driver constant.
    // CONTROL: the `if ((candidate.drmFormatModifierTilingFeatures & requiredTilingFeatures) !=
    //       requiredTilingFeatures) continue;` block deleted from usableDrmModifiers
    //       (renderer/vulkan/memory.cpp) -> on the RTX 3080 Ti, six per-bit diagnostics ("
    //       requiring COLOR_ATTACHMENT: 6 modifier(s) advertise it but 7 survived the filter",
    //       and five bits nothing advertises where all 7 survived) followed by "FAIL: for every
    //       required tiling feature, the modifiers that survive are exactly those advertising
    //       it", "FAIL: at least one required tiling feature actually removed a modifier" and
    //       "FAIL: requiring a tiling feature no modifier advertises leaves no usable modifier
    //       at all". Reverted.
    // COVERAGE-ASSERT: the number of bits that actually caused a modifier to be dropped is
    //       asserted -- a sweep run on a driver where the filter can never bite fails instead of
    //       passing quietly.
    int mismatches = 0;
    int bitsThatDropped = 0;
    bool sawEverythingDropped = false;
    bool sawPartialDrop = false;
    const char* partialBitName = nullptr;
    for (const FeatureBit& feature : kFeatureBits) {
        std::vector<uint64_t> expected;
        for (uint64_t modifier : base) {
            if ((featuresOf(modifier) & feature.bit) == feature.bit) expected.push_back(modifier);
        }
        std::vector<uint64_t> got = askUsable(device.physicalDevice(), feature.bit);

        std::vector<uint64_t> sortedExpected = expected;
        std::sort(sortedExpected.begin(), sortedExpected.end());
        std::sort(got.begin(), got.end());
        if (sortedExpected != got) {
            ++mismatches;
            std::fprintf(stderr,
                         "  requiring %s: %zu modifier(s) advertise it but %zu survived the filter\n", feature.name,
                         expected.size(), got.size());
            continue;
        }
        if (expected.size() == base.size()) continue;
        ++bitsThatDropped;
        if (expected.empty()) {
            sawEverythingDropped = true;
        } else {
            sawPartialDrop = true;
            partialBitName = feature.name;
        }
    }

    check(mismatches == 0,
          "for every required tiling feature, the modifiers that survive are exactly those advertising it");
    check(bitsThatDropped > 0,
          "at least one required tiling feature actually removed a modifier, so the filter was observed doing "
          "something");
    check(sawEverythingDropped,
          "requiring a tiling feature no modifier advertises leaves no usable modifier at all");

    if (sawPartialDrop) {
        std::printf("modifier_filter_probe: requiring %s kept some modifiers and dropped others\n", partialBitName);
    } else {
        // Scoped to this claim alone, and it is the claim the real GPU carries: Lavapipe offers
        // one modifier, so nothing there can distinguish "the filter drops what lacks the
        // feature" from "the filter drops everything". Reporting the whole entry as verified
        // would be claiming a measurement this driver cannot make.
        g_notVerified = true;
        std::printf("modifier_filter_probe: NOT VERIFIED -- no format feature splits this driver's usable modifiers "
                    "(%zu of them), so a filter that kept the advertisers and dropped the rest cannot be told from "
                    "one that drops everything\n",
                    base.size());
    }

    if (!device.info().hasExternalMemoryDmaBuf) {
        g_notVerified = true;
        std::printf("modifier_filter_probe: NOT VERIFIED -- this driver cannot export a dma_buf, so image creation "
                    "never reaches the modifier filter\n");
        return;
    }

    // GOAL: S13.30 -- the requirement reaches image creation. A dma_buf image whose required tiling
    //       features no modifier satisfies is refused, rather than created with a layout the
    //       use it is made for cannot target.
    // WHY-THIS-SHAPE: graded through createExportableImage rather than only through the free
    //       function, because a caller passing the argument and a callee using it are separate
    //       failures -- the free-function sweep above stays green if createExportableImage
    //       hands the filter a zero. The impossible feature is taken from the sweep, so this
    //       cannot be a bit the driver happens to advertise. The opposite direction is asserted
    //       beside it: with what production actually asks for, the image is still created, so
    //       this cannot pass by refusing everything.
    // CONTROL: two, both in the usableDrmModifiers() call inside Memory::createExportableImage
    //       (renderer/vulkan/memory.cpp), both reverted. (a) `requiredTilingFeatures` replaced
    //       by `0`, a callee that ignores what it was given -> "FAIL: a dma_buf image is refused
    //       when no modifier has the required tiling features" as the run's only failure, with
    //       the sweep above still green -- which is the measurement showing this check is not
    //       redundant with it. (b) replaced by VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT, a
    //       callee that demands more than it was given -> "FAIL: a dma_buf image is still
    //       created for the tiling features an offscreen target asks for", so the pair cannot be
    //       satisfied by a build that refuses everything. Both re-run 2026-10-26 (G6), RTX 3080 Ti, with
    //       OffscreenTargetManager::kFormatFeatures now 0: the same two lines.
    // With kFormatFeatures 0 the production-features image asks for nothing, so it no longer shows the
    // filter keeping a modifier that meets a real requirement. The satisfiable check below does.
    VkFormatFeatureFlags impossible = 0;
    for (const FeatureBit& feature : kFeatureBits) {
        bool advertisedBySomeone = false;
        for (uint64_t modifier : base) {
            if ((featuresOf(modifier) & feature.bit) == feature.bit) advertisedBySomeone = true;
        }
        if (!advertisedBySomeone) {
            impossible = feature.bit;
            break;
        }
    }
    if (impossible == 0) {
        g_notVerified = true;
        std::printf("modifier_filter_probe: NOT VERIFIED -- every format feature in the table is advertised by some "
                    "modifier here, so image creation cannot be asked for one that is impossible\n");
        return;
    }

    const VkImageCreateInfo info = targetLikeImageInfo();
    bool refusedImpossible = false;
    try {
        AllocatedImage image = device.memory().createExportableImage(
            info, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, impossible, "modifier-probe-impossible");
        device.memory().destroyImage(image);
    } catch (const std::exception&) {
        refusedImpossible = true;
    }
    check(refusedImpossible, "a dma_buf image is refused when no modifier has the required tiling features");

    bool createdWithProductionFeatures = false;
    try {
        AllocatedImage image =
            device.memory().createExportableImage(info, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                                                  OffscreenTargetManager::kFormatFeatures,
                                                  "modifier-probe-target-features");
        createdWithProductionFeatures = image.image != VK_NULL_HANDLE;
        device.memory().destroyImage(image);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "  creating a dma_buf image with the target's own tiling features threw: %s\n",
                     e.what());
    }
    check(createdWithProductionFeatures,
          "a dma_buf image is still created for the tiling features an offscreen target asks for");

    // GOAL: S13.30, the other direction -- a non-zero requirement that some modifier meets still
    //       creates the dma_buf image. False for a createExportableImage that refuses any requirement.
    // WHY-THIS-SHAPE: asks for what a target's own use needs of its tiling, colour attachment and
    //       sampling, when a modifier here advertises both. The check above asks for kFormatFeatures,
    //       now 0, which a filter that refuses every non-zero requirement also passes.
    // CONTROL: renderer/vulkan/memory.cpp createExportableImage(), the filter handed
    //       STORAGE_IMAGE_ATOMIC in place of any non-zero requirement, through control_batch.sh: "FAIL:
    //       a dma_buf image is created for a non-zero tiling requirement some modifier meets", with the
    //       two checks above green. Held.
    constexpr VkFormatFeatureFlags kTargetUse =
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    bool satisfiable = false;
    for (uint64_t modifier : base) {
        if ((featuresOf(modifier) & kTargetUse) == kTargetUse) satisfiable = true;
    }
    if (!satisfiable) {
        g_notVerified = true;
        std::printf("modifier_filter_probe: NOT VERIFIED -- no modifier here advertises colour attachment and "
                    "sampling together, so a satisfiable non-zero requirement cannot be asked for\n");
    } else {
        bool createdForTargetUse = false;
        try {
            AllocatedImage image = device.memory().createExportableImage(
                info, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, kTargetUse, "modifier-probe-satisfiable");
            createdForTargetUse = image.image != VK_NULL_HANDLE;
            device.memory().destroyImage(image);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "  creating a dma_buf image for colour attachment and sampling threw: %s\n",
                         e.what());
        }
        check(createdForTargetUse, "a dma_buf image is created for a non-zero tiling requirement some modifier meets");
    }

    // GOAL: S13.31 -- the opaque-fd path does not consult the requirement, as its documentation says --
    //       there is no modifier list there, and the caller can query the tiling it chose.
    // WHY-THIS-SHAPE: the same impossible feature that refuses the dma_buf path above. Without
    //       this, moving the filter above the handle-type branch -- the obvious "simplification"
    //       -- would break every opaque-fd export on the project's target GPU with nothing
    //       failing here.
    // CONTROL: the requirement applied above the handle-type branch, which is what "hoisting the
    //       filter" would amount to -- a vkGetPhysicalDeviceFormatProperties call at the top of
    //       Memory::createExportableImage (renderer/vulkan/memory.cpp) throwing when the
    //       format's optimal tiling features lack requiredTilingFeatures -> "creating an
    //       opaque-fd image threw: control: the required tiling features are unsupported" and
    //       "FAIL: an opaque-fd image ignores the tiling features the dma_buf path filters on",
    //       with the two dma_buf checks above still green. Reverted.
    bool opaqueIgnoredIt = false;
    try {
        AllocatedImage image = device.memory().createExportableImage(
            info, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT, impossible, "modifier-probe-opaque");
        opaqueIgnoredIt = image.image != VK_NULL_HANDLE;
        device.memory().destroyImage(image);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "  creating an opaque-fd image threw: %s\n", e.what());
    }
    check(opaqueIgnoredIt, "an opaque-fd image ignores the tiling features the dma_buf path filters on");
}

}  // namespace

int main() {
    const bool graded = wallpaperx::testing::gradedRun("modifier_filter_probe", [&](auto& grade) {
        try {
            Device device(/*enableValidation=*/true);
            grade.watch(device);
            std::printf("modifier_filter_probe: on %s\n", device.info().gpuName.c_str());
            runModifierFilterChecks(device);
        } catch (const std::exception& e) {
            // Reported, not rethrown: an escaping exception would skip the validation grade in
            // gradedRun's frame and print no verdict for the checks that did run.
            fail(std::string("modifier_filter_probe threw: ") + e.what());
        }
        return wallpaperx::testing::failureCount() == 0;
    });

    // Counted before the verdict is combined, so the summary prints even when the grade failed.
    const int failures = wallpaperx::testing::reportFailures("modifier_filter_probe");
    if (!graded || failures > 0) return 1;
    if (g_notVerified) {
        std::printf("modifier_filter_probe: NOT VERIFIED on this driver -- reported to ctest as a skip, not a pass\n");
        return kNotVerifiedExitCode;
    }
    std::printf("modifier_filter_probe: all checks passed, zero validation warnings\n");
    return 0;
}
