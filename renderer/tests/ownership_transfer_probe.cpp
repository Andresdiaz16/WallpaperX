// SPDX-License-Identifier: GPL-2.0-or-later

// Grades the queue-family ownership transfer that exportFrame() and
// OffscreenTargetManager::render() perform between them: the release that hands the
// exported image to a consumer outside this device, and the acquire that takes it back
// before the next render writes to it.
//
// WHY THIS PROBE EXISTS. Vulkan requires the two halves of an ownership transfer to name
// the *same* oldLayout and the *same* newLayout, because the layout transition is
// performed exactly once and the driver may choose either side to perform it on. Halves
// that disagree leave the conversion undone; on a compressed modifier (AMD DCC, Intel
// CCS) what the consumer then reads is the unresolved metadata rather than the frame.
//
// Nothing in the suite could see that. The defect was restored deliberately -- the
// acquire reading `exportLayout` instead of `releasedFromLayout`, giving GENERAL->GENERAL
// against a release of SHADER_READ_ONLY_OPTIMAL->GENERAL -- and all 72 renderer checks
// stayed green on both NVIDIA and Lavapipe. The reasons are structural, not accidental:
//
//   * The validation layer cannot see it. The two halves are in separate submissions a
//     frame apart, and the layer's own layout tracking has already recorded GENERAL from
//     the release, so the *defective* acquire is the one that agrees with its map and the
//     correct one is the one that would disagree. It has nothing to compare against.
//   * A pixel round trip cannot see it. Neither available driver has a compressed
//     modifier -- NVIDIA's block-linear layout and Lavapipe's LINEAR both survive an
//     unpaired transfer intact -- so the pixels come back correct either way. This is
//     recorded in modifier_roundtrip_probe's header as the queue-family line it cannot
//     establish, and in GOALS.md as S5.10 (DEFERRED).
//
// So the only oracle left is the barrier itself: what the two submissions actually
// declared. This probe reads them directly, by interposing vkCmdPipelineBarrier (see
// below), and compares the pair. That is a weaker claim than "the driver resolved the
// metadata" -- it cannot say the transfer works on hardware nobody here has -- but it is
// the exact property the spec states and the exact one the defect violates, and it holds
// identically on every driver because it never asks a driver anything.

#include "vulkan/device.h"
#include "vulkan/frame_export.h"
#include "vulkan/memory.h"
#include "vulkan/memory_export.h"
#include "vulkan/offscreen.h"
#include "renderers/static_renderer.h"

#include "support/check.h"
#include "support/test_image.h"
#include "support/validation_grade.h"

#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

namespace {

using wallpaperx::testing::check;

constexpr uint32_t kWidth = 68;
constexpr uint32_t kHeight = 36;

// ctest's own convention for "the host could not be put in the state this grades";
// SKIP_RETURN_CODE in CMakeLists.txt is what makes ctest act on it.
constexpr int kNotVerifiedExitCode = 77;
bool g_notVerified = false;

// One recorded image barrier that transfers queue-family ownership. Only the fields the
// spec's pairing rule constrains are kept.
struct TransferBarrier {
    VkImage image = VK_NULL_HANDLE;
    uint32_t srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    uint32_t dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    VkImageLayout oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout newLayout = VK_IMAGE_LAYOUT_UNDEFINED;
};

// Guarded because vkCmdPipelineBarrier is interposed process-wide and a future probe
// linking this file might record from more than one thread. The renderer records on one
// thread today (Device pins itself), so this is never contended.
std::mutex g_recordedMutex;
std::vector<TransferBarrier> g_recorded;
bool g_recording = false;

void startRecording() {
    std::lock_guard lock(g_recordedMutex);
    g_recorded.clear();
    g_recording = true;
}

std::vector<TransferBarrier> stopRecording() {
    std::lock_guard lock(g_recordedMutex);
    g_recording = false;
    return g_recorded;
}

const char* layoutName(VkImageLayout layout) {
    switch (layout) {
        case VK_IMAGE_LAYOUT_UNDEFINED:                return "UNDEFINED";
        case VK_IMAGE_LAYOUT_GENERAL:                  return "GENERAL";
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return "COLOR_ATTACHMENT_OPTIMAL";
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return "SHADER_READ_ONLY_OPTIMAL";
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:     return "TRANSFER_SRC_OPTIMAL";
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:     return "TRANSFER_DST_OPTIMAL";
        case VK_IMAGE_LAYOUT_PREINITIALIZED:           return "PREINITIALIZED";
        default:                                       return "other";
    }
}

std::string familyName(uint32_t family) {
    if (family == VK_QUEUE_FAMILY_IGNORED) return "IGNORED";
    if (family == VK_QUEUE_FAMILY_EXTERNAL) return "EXTERNAL";
    if (family == VK_QUEUE_FAMILY_FOREIGN_EXT) return "FOREIGN";
    return std::to_string(family);
}

std::string describe(const TransferBarrier& barrier) {
    return familyName(barrier.srcQueueFamilyIndex) + "->" + familyName(barrier.dstQueueFamilyIndex) + " " +
           layoutName(barrier.oldLayout) + "->" + layoutName(barrier.newLayout);
}

}  // namespace

// GOAL: the two halves of the exported image's ownership transfer name the same layout
//       pair, so the single transition the spec allows is actually performed.
// WHY-THIS-SHAPE: symbol interposition, not a hook in the renderer. X.6 forbids adding a
//                 test-only seam to production, and the barriers are local variables
//                 inside immediateSubmit lambdas that no public interface exposes.
//                 Interposing the loader entry point the renderer already calls reads
//                 exactly what was submitted, changes no production line, and works on
//                 both ICDs. The obvious alternative -- asserting on OffscreenTarget's
//                 releasedFromLayout/exportLayout members -- cannot distinguish the
//                 defect: those members hold the right values in both the correct and the
//                 defective build, because the bug is in which member the acquire *reads*.
// CONTROL: see the block above the pairing check in run().
//
// The executable's definition of this symbol takes precedence over libvulkan's for every
// call site linked into it, including those inside wallpaperx-renderer.a; the real entry
// point is reached through RTLD_NEXT. Forwarding is unconditional, so the renderer's
// behaviour is identical whether or not recording is on.
extern "C" void vkCmdPipelineBarrier(VkCommandBuffer commandBuffer, VkPipelineStageFlags srcStageMask,
                                     VkPipelineStageFlags dstStageMask, VkDependencyFlags dependencyFlags,
                                     uint32_t memoryBarrierCount, const VkMemoryBarrier* pMemoryBarriers,
                                     uint32_t bufferMemoryBarrierCount,
                                     const VkBufferMemoryBarrier* pBufferMemoryBarriers,
                                     uint32_t imageMemoryBarrierCount,
                                     const VkImageMemoryBarrier* pImageMemoryBarriers) {
    {
        std::lock_guard lock(g_recordedMutex);
        if (g_recording) {
            for (uint32_t i = 0; i < imageMemoryBarrierCount; ++i) {
                const VkImageMemoryBarrier& barrier = pImageMemoryBarriers[i];
                // Only ownership transfers. A same-family barrier (both IGNORED) is an
                // ordinary layout transition and the pairing rule says nothing about it.
                if (barrier.srcQueueFamilyIndex == barrier.dstQueueFamilyIndex) continue;
                g_recorded.push_back(TransferBarrier{barrier.image, barrier.srcQueueFamilyIndex,
                                                     barrier.dstQueueFamilyIndex, barrier.oldLayout,
                                                     barrier.newLayout});
            }
        }
    }

    using Fn = void (*)(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags, uint32_t,
                        const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*, uint32_t,
                        const VkImageMemoryBarrier*);
    // Resolved once. A failure here is fatal rather than silent: skipping the forward
    // would drop every barrier in the process and leave the probe grading a run whose
    // GPU work was never synchronised.
    static const Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "vkCmdPipelineBarrier"));
    if (real == nullptr) {
        std::fprintf(stderr, "ownership_transfer_probe: cannot resolve the real vkCmdPipelineBarrier\n");
        std::abort();
    }
    real(commandBuffer, srcStageMask, dstStageMask, dependencyFlags, memoryBarrierCount, pMemoryBarriers,
         bufferMemoryBarrierCount, pBufferMemoryBarriers, imageMemoryBarrierCount, pImageMemoryBarriers);
}

namespace {

// Two monitors, two export/render cycles each: four transfers, eight barriers.
//
// Two monitors rather than one, and interleaved rather than sequential, so the barriers
// arrive release(A) release(B) acquire(A) acquire(B). That ordering is what forces the
// pairing below to match on the image handle. With one monitor every release is
// immediately followed by its own acquire, index arithmetic pairs them correctly by
// accident, and an acquire naming the wrong image would still be graded as matching.
constexpr int kMonitors[] = {0, 1};
constexpr size_t kCycles = 2;
constexpr size_t kExpectedTransfers = std::size(kMonitors) * kCycles;

bool run(wallpaperx::testing::ValidationGrade& grade) {
    const std::string imagePath = wallpaperx::testing::writeGradientPng(
        std::string(WALLPAPERX_TEST_IMAGE_DIR) + "/ownership_transfer_gradient.png", 16, 16);

    wallpaperx::Device device(/*enableValidation=*/true);
    grade.watch(device);

    wallpaperx::OffscreenTargetManager targets(device);
    wallpaperx::StaticRenderer renderer(device, targets.renderPass(), imagePath, WALLPAPERX_SHADER_DIR);

    // The first render of each monitor is outside the recording window on purpose: it
    // precedes any export, so it issues no acquire, and including it would only add a
    // barrier the pairing has to special-case.
    for (int monitorId : kMonitors) {
        const wallpaperx::OffscreenTarget& target = targets.create(monitorId, kWidth, kHeight);
        targets.render(renderer, target);
        check(target.exportLayout == wallpaperx::kRenderedLayout,
              "the render leaves the target in the layout the daemon exports from");
    }

    startRecording();
    for (size_t cycle = 0; cycle < kCycles; ++cycle) {
        for (int monitorId : kMonitors) {
            const wallpaperx::ExportedFrame frame = wallpaperx::exportFrame(device, targets.get(monitorId));
            if (frame.fd >= 0) close(frame.fd);
        }
        for (int monitorId : kMonitors) {
            targets.render(renderer, targets.get(monitorId));
        }
    }
    const std::vector<TransferBarrier> recorded = stopRecording();

    for (const TransferBarrier& barrier : recorded) {
        std::cout << "ownership_transfer_probe: recorded " << describe(barrier) << std::endl;
    }

    // GOAL: the interposer actually saw the renderer's barriers -- a probe that recorded
    //       nothing must fail, not report a vacuous pass over an empty list.
    // WHY-THIS-SHAPE: everything below is a loop over `recorded`, so all of it is
    //                 satisfied trivially by an empty vector. The count is what makes the
    //                 interposition itself part of what is graded: if the renderer stops
    //                 reaching the loader's vkCmdPipelineBarrier (a dispatch-table call, a
    //                 statically linked libvulkan) the probe goes red here rather than
    //                 green everywhere.
    // CONTROL: `if (g_recording)` in the interposer changed to `if (false)` -- red with
    //          "recorded 0 queue-family ownership barriers, expected 8". Reverted.
    const bool countOk = recorded.size() == kExpectedTransfers * 2;
    check(countOk, "each export issues one ownership release and each following render one acquire");
    if (!countOk) {
        std::cerr << "  recorded " << recorded.size() << " queue-family ownership barriers, expected "
                  << kExpectedTransfers * 2 << std::endl;
        return false;  // nothing below can be attributed without the expected shape
    }

    const bool isRelease = [](const TransferBarrier& b, uint32_t graphics) {
        return b.srcQueueFamilyIndex == graphics &&
               (b.dstQueueFamilyIndex == VK_QUEUE_FAMILY_EXTERNAL || b.dstQueueFamilyIndex == VK_QUEUE_FAMILY_FOREIGN_EXT);
    }(recorded.front(), device.graphicsQueueFamily());
    check(isRelease, "the first ownership barrier of the run is the export's release, not an acquire");

    // Releases waiting for their acquire, keyed by image. Pairing by image rather than by
    // position is the whole point of running two monitors: see kMonitors above.
    std::vector<TransferBarrier> pending;
    size_t pairsCompared = 0;
    size_t index = 0;
    for (const TransferBarrier& barrier : recorded) {
        const std::string where = "barrier " + std::to_string(index++) + " (" + describe(barrier) + "): ";
        const bool release = barrier.srcQueueFamilyIndex == device.graphicsQueueFamily() &&
                             (barrier.dstQueueFamilyIndex == VK_QUEUE_FAMILY_EXTERNAL ||
                              barrier.dstQueueFamilyIndex == VK_QUEUE_FAMILY_FOREIGN_EXT);
        const bool acquire = barrier.dstQueueFamilyIndex == device.graphicsQueueFamily() &&
                             (barrier.srcQueueFamilyIndex == VK_QUEUE_FAMILY_EXTERNAL ||
                              barrier.srcQueueFamilyIndex == VK_QUEUE_FAMILY_FOREIGN_EXT);

        if (release) {
            // A second release of an image already released, with no acquire between, is
            // an unpaired transfer in its own right.
            const auto duplicate = std::find_if(pending.begin(), pending.end(), [&](const TransferBarrier& held) {
                return held.image == barrier.image;
            });
            check(duplicate == pending.end(), where + "this image was not already released without being acquired back");
            pending.push_back(barrier);
            continue;
        }
        if (!acquire) {
            check(false, where + "a queue-family barrier that is neither a release to nor an acquire from an "
                                 "external consumer");
            continue;
        }

        // GOAL: every acquire pairs with a release of the SAME image -- the renderer never
        //       takes ownership back for an image it did not hand over.
        // WHY-THIS-SHAPE: the lookup is by image handle, so the check cannot be satisfied
        //                 by an acquire that happens to sit next to a release in the
        //                 recording. That matters because the barriers interleave across
        //                 the two monitors.
        // CONTROL: renderer/vulkan/offscreen.cpp `acquire.image = target.image.image;` changed to
        //          `= m_targets.begin()->second.images[0].image.image;`, so one monitor's
        //          acquire names the other's image. Red on NVIDIA: barriers 3 and 7
        //          "acquires an image no release handed over", barrier 4 "this image was
        //          not already released without being acquired back", and the pair count
        //          fell to 2 of 4. Reverted.
        const auto match = std::find_if(pending.begin(), pending.end(), [&](const TransferBarrier& held) {
            return held.image == barrier.image;
        });
        if (match == pending.end()) {
            check(false, where + "acquires an image no release handed over");
            continue;
        }
        const TransferBarrier releaseHalf = *match;
        pending.erase(match);

        check(barrier.srcQueueFamilyIndex == releaseHalf.dstQueueFamilyIndex &&
                  barrier.dstQueueFamilyIndex == releaseHalf.srcQueueFamilyIndex,
              where + "acquires from the family the export released to");

        // GOAL: both halves of the transfer name the same oldLayout and the same
        //       newLayout, as a queue-family ownership transfer requires.
        // WHY-THIS-SHAPE: compared against the recorded release, not against a constant. A
        //                 constant would encode today's render-pass finalLayout in the
        //                 test, so a renderer that legitimately ends in another layout
        //                 would fail here for no defect -- and the check would stop
        //                 tracking what the release actually said, which is the only thing
        //                 the acquire has to match.
        // CONTROL: renderer/vulkan/offscreen.cpp `acquire.oldLayout = target.releasedFromLayout;` changed
        //          to `= target.exportLayout;` -- the defect this probe was written for.
        //          Red on both ICDs: "release 0->EXTERNAL SHADER_READ_ONLY_OPTIMAL->
        //          GENERAL, acquire EXTERNAL->0 GENERAL->GENERAL" (and 0->FOREIGN on
        //          Lavapipe). Reverted.
        const bool layoutsPaired =
            barrier.oldLayout == releaseHalf.oldLayout && barrier.newLayout == releaseHalf.newLayout;
        check(layoutsPaired, where + "names the same layout pair the release did");
        if (!layoutsPaired) {
            std::cerr << "  release " << describe(releaseHalf) << ", acquire " << describe(barrier) << std::endl;
        }

        // GOAL: the comparison above is a real one -- the release names a genuine
        //       transition, so the two halves cannot agree by both saying GENERAL.
        // WHY-THIS-SHAPE: the defect under test produces acquire GENERAL->GENERAL. If a
        //                 future change made the render pass end in GENERAL, the release
        //                 would read GENERAL->GENERAL too, the equality above would hold
        //                 for a defective build, and this probe would go quiet without
        //                 failing. Asserting the release's own pair is distinct is what
        //                 keeps the equality load-bearing.
        // CONTROL: kRenderedLayout in renderer/vulkan/offscreen.h set to VK_IMAGE_LAYOUT_GENERAL -- red
        //          with "the release it pairs with names a layout transition, so matching
        //          it is a real constraint". Reverted.
        check(releaseHalf.oldLayout != releaseHalf.newLayout,
              where + "the release it pairs with names a layout transition, so matching it is a real constraint");
        ++pairsCompared;
    }

    check(pending.empty(), "every ownership release in the run was acquired back before the run ended");

    // COVERAGE-ASSERT: how many pairs were actually compared, not how many barriers were
    // recorded. Every layout comparison above sits behind a `continue`, so an edit that
    // made the pairing fall through would leave a probe that prints eight barriers and
    // compares none -- green, and grading nothing.
    // CONTROL: the acquire branch's `if (match == pending.end())` widened to
    //          `|| true`, so every acquire takes the early `continue` -- red with
    //          "compared 0 release/acquire pairs, expected 4". Reverted.
    const bool comparedAll = pairsCompared == kExpectedTransfers;
    check(comparedAll, "every release/acquire pair the run produced was compared");
    if (!comparedAll) {
        std::cerr << "  compared " << pairsCompared << " release/acquire pairs, expected " << kExpectedTransfers
                  << std::endl;
    }

    return wallpaperx::testing::failureCount() == 0;
}

}  // namespace

int main() {
    // The release is issued for both handle types, so this probe does not force one. It
    // runs on NVIDIA's OPAQUE_FD path and Lavapipe's DMA_BUF path unchanged, and the
    // pairing rule is the same for both.
    const bool ok = wallpaperx::testing::gradedRun("ownership_transfer_probe", [](auto& grade) {
        try {
            return run(grade);
        } catch (const std::exception& e) {
            // Reported, never thrown out of main: a driver that cannot export at all is a
            // host limitation, and blaming the renderer for it is the attribution error
            // X.3 forbids.
            std::cerr << "ownership_transfer_probe: could not complete the export cycle: " << e.what() << std::endl;
            std::cerr << "  no ownership transfer was observed, so nothing was verified" << std::endl;
            g_notVerified = true;
            // Not a bare `true`: checks that already ran before the throw still count, so
            // a run that is inconclusive AND broken reports as broken.
            return wallpaperx::testing::failureCount() == 0;
        }
    });

    if (!ok) {
        wallpaperx::testing::reportFailures("ownership_transfer_probe");
        return 1;
    }
    // After the failure check, so a run that is both inconclusive and broken reports as
    // broken.
    if (g_notVerified) {
        std::cout << "ownership_transfer_probe: NOT VERIFIED -- reported to ctest as a skip, not a pass" << std::endl;
        return kNotVerifiedExitCode;
    }
    std::cout << "ownership_transfer_probe: passed" << std::endl;
    return 0;
}
