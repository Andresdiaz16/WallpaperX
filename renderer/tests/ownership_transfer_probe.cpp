// SPDX-License-Identifier: GPL-2.0-or-later

// Grades the queue-family ownership transfer OffscreenTargetManager::render() performs:
// the acquire that takes the image back from whoever held the previously published frame,
// and the release that hands the freshly drawn one over. Both are recorded into the single
// command buffer a render submits. exportFrame() takes part in neither -- it hands out a
// file descriptor and metadata and touches no target state.
//
// WHY THIS PROBE EXISTS. Vulkan requires the two halves of an ownership transfer to name
// the *same* oldLayout and the *same* newLayout, because the layout transition is
// performed exactly once and the driver may choose either side to perform it on. Halves
// that disagree leave the conversion undone; on a compressed modifier (AMD DCC, Intel
// CCS) what the consumer then reads is the unresolved metadata rather than the frame.
//
// Nothing else in the suite can see that. The defect was restored deliberately -- the
// acquire in `recordAcquire` naming VK_IMAGE_LAYOUT_GENERAL as its oldLayout instead of
// `target.releasedFromLayout`, giving GENERAL->GENERAL against a release of
// SHADER_READ_ONLY_OPTIMAL->GENERAL -- and every other renderer check stayed green on both
// NVIDIA and Lavapipe. The reasons are structural, not accidental:
//
//   * The validation layer cannot see it. The layer's own layout tracking has already
//     recorded GENERAL from the release, so the *defective* acquire is the one that agrees
//     with its map and the correct one is the one that would disagree. It has nothing to
//     compare against.
//   * A pixel round trip cannot see it. Neither available driver has a compressed
//     modifier -- NVIDIA's block-linear layout and Lavapipe's LINEAR both survive an
//     unpaired transfer intact -- so the pixels come back correct either way. This is
//     recorded in modifier_roundtrip_probe's header as the queue-family line it cannot
//     establish, and in GOALS.md as S5.10.
//
// So the only oracle left is the barrier itself: what the submissions actually declared.
// This probe reads them directly, by interposing vkCmdPipelineBarrier and vkQueueSubmit
// (see below), and compares them. That is a weaker claim than "the driver resolved the
// metadata" -- it cannot say the transfer works on hardware nobody here has -- but it is
// the exact property the spec states and the exact one the defect violates, and it holds
// identically on every driver because it never asks a driver anything.
//
// WHAT THE PRODUCER-DRIVEN PUBLISH MEANS FOR THE SHAPE OF THIS FILE. Publishing is part of
// the render, not of the export, so "drawn but never handed over" is no longer a state the
// producer can be in. Three consequences the checks below are built around: a render's
// barriers are an acquire and a release in ONE submit rather than one barrier per
// submission a frame apart; a target that is rendered and never exported still ends up
// released, which is newly expressible; and an export contributes no barrier and no submit
// at all, so a count of them is the way to say "the export stayed out of it".
//
// WHAT THIS PROBE STILL CANNOT ESTABLISH, on either driver available here: that the
// transfer makes a *compressed* frame readable. That needs a driver whose modifier list
// carries compression metadata -- NVIDIA exports an opaque fd with no modifier at all, and
// Lavapipe offers only DRM_FORMAT_MOD_LINEAR. Both survive an unpaired transfer, so the
// declaration is all there is to grade here.

#include "vulkan/device.h"
#include "vulkan/frame_export.h"
#include "vulkan/memory.h"
#include "vulkan/offscreen.h"
#include "renderers/static_renderer.h"

#include "support/check.h"
#include "support/gpu_readback.h"
#include "support/test_image.h"
#include "support/validation_grade.h"

#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
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
// spec's pairing rule constrains are kept, plus which submit carried it.
struct TransferBarrier {
    VkImage image = VK_NULL_HANDLE;
    uint32_t srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    uint32_t dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    VkImageLayout oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout newLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // Which command buffer this went into, and how many submits had been made when it was
    // recorded. Both, because neither alone says "one submit": a command buffer handle is
    // recycled by the pool, so the same handle can be two different submissions, and the
    // submit counter alone is defeated by a nested immediateSubmit whose inner buffer is
    // submitted *before* the outer one -- measured, and it left a two-submit release
    // indistinguishable from a one-submit one.
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    uint64_t submitIndex = 0;
    // Position in the single ordering that covers barriers and image reads alike, so
    // "the acquire was recorded before the copy" is answerable. Command-buffer order is
    // what matters and the submit counter cannot express it: everything in the readback
    // sequence lands in a different one-shot buffer.
    uint64_t seq = 0;
};

// One recorded read of an image into a buffer -- the shape of every readback in the suite.
struct ImageCopy {
    VkImage image = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint64_t seq = 0;
};

// One recorded same-family layout transition: not an ownership transfer, so the pairing
// rule says nothing about it, but it is how a reader moves a reclaimed image into a layout
// it can copy from, and what it names as oldLayout is a claim about production's
// post-condition that can be wrong.
struct LayoutTransition {
    VkImage image = VK_NULL_HANDLE;
    VkImageLayout oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout newLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint64_t seq = 0;
};

// Two barriers the driver received as part of the same submitted command buffer.
bool sameSubmit(const TransferBarrier& a, const TransferBarrier& b) {
    return a.submitIndex == b.submitIndex && a.commandBuffer == b.commandBuffer;
}

// Guarded because both entry points are interposed process-wide and a future probe linking
// this file might record from more than one thread. The renderer records on one thread
// today (Device pins itself), so this is never contended.
std::mutex g_recordedMutex;
std::vector<TransferBarrier> g_recorded;
std::vector<ImageCopy> g_copies;
std::vector<LayoutTransition> g_transitions;
bool g_recording = false;
uint64_t g_submitIndex = 0;
uint64_t g_eventSeq = 0;
// Submits made while recording, so "the export submitted nothing" is a measured delta
// rather than an assumption about what exportFrame is made of.
uint64_t g_submitsRecorded = 0;

void startRecording() {
    std::lock_guard lock(g_recordedMutex);
    g_recorded.clear();
    g_copies.clear();
    g_transitions.clear();
    g_submitsRecorded = 0;
    g_eventSeq = 0;
    g_recording = true;
}

std::vector<ImageCopy> recordedCopies() {
    std::lock_guard lock(g_recordedMutex);
    return g_copies;
}

std::vector<LayoutTransition> recordedTransitions() {
    std::lock_guard lock(g_recordedMutex);
    return g_transitions;
}

std::vector<TransferBarrier> peekRecorded() {
    std::lock_guard lock(g_recordedMutex);
    return g_recorded;
}

uint64_t submitsRecorded() {
    std::lock_guard lock(g_recordedMutex);
    return g_submitsRecorded;
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
    return "submit " + std::to_string(barrier.submitIndex) + " " + familyName(barrier.srcQueueFamilyIndex) + "->" +
           familyName(barrier.dstQueueFamilyIndex) + " " + layoutName(barrier.oldLayout) + "->" +
           layoutName(barrier.newLayout);
}

// The two mutable fields OffscreenTarget tracks. Copied as a unit because the contract is
// that one function writes both, so they can never describe different moments -- comparing
// them one at a time would not say that.
//
// There is no third field for "the layout the image is now in". A published frame is in
// GENERAL by construction: that is what render()'s release names as its newLayout and what
// reclaim()'s acquire names, both compile-time constants, so a field recording it could
// only drift from them. The checks below assert GENERAL literally instead.
struct TargetState {
    uint32_t externalOwner = VK_QUEUE_FAMILY_IGNORED;
    VkImageLayout releasedFromLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    bool operator==(const TargetState&) const = default;
};

TargetState stateOf(const wallpaperx::OffscreenTarget& target) {
    return TargetState{target.externalOwner, target.releasedFromLayout};
}

std::string describe(const TargetState& state) {
    return std::string("owner ") + familyName(state.externalOwner) + ", released from " +
           layoutName(state.releasedFromLayout);
}

}  // namespace

// GOAL: the two halves of a published frame's ownership transfer name the same layout
//       pair, so the single transition the spec allows is actually performed.
// WHY-THIS-SHAPE: symbol interposition, not a hook in the renderer. X.6 forbids adding a
//                 test-only seam to production, and the barriers are local variables
//                 inside an immediateSubmit lambda that no public interface exposes.
//                 Interposing the loader entry points the renderer already calls reads
//                 exactly what was submitted, changes no production line, and works on
//                 both ICDs. The obvious alternative -- asserting on OffscreenTarget's
//                 externalOwner/releasedFromLayout members -- cannot distinguish the
//                 defect: those members hold the right values in both the correct and the
//                 defective build, because the bug is in what the acquire names, not in
//                 what the release recorded.
// CONTROL: see the block above the pairing check in analyse().
//
// The executable's definition of these symbols takes precedence over libvulkan's for every
// call site linked into it, including those inside wallpaperx-renderer.a; the real entry
// points are reached through RTLD_NEXT. Forwarding is unconditional, so the renderer's
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
                // Ownership transfers and ordinary layout transitions are kept apart: the
                // pairing rule constrains only the first, and everything analyse() does is
                // about those. The second list exists for the readback phase alone.
                if (barrier.srcQueueFamilyIndex == barrier.dstQueueFamilyIndex) {
                    g_transitions.push_back(
                        LayoutTransition{barrier.image, barrier.oldLayout, barrier.newLayout, g_eventSeq++});
                    continue;
                }
                g_recorded.push_back(TransferBarrier{barrier.image, barrier.srcQueueFamilyIndex,
                                                     barrier.dstQueueFamilyIndex, barrier.oldLayout,
                                                     barrier.newLayout, commandBuffer, g_submitIndex,
                                                     g_eventSeq++});
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

// Interposed for the same reason as the barrier above: a copy out of an image is the one
// thing on this device that reads a rendered frame, and whether the graphics queue owned
// the image when it did so is only answerable by placing the copy in the same ordering as
// the ownership barriers. Nothing about the image or the pixels can answer it -- see the
// reclaim-before-readback check for why.
extern "C" void vkCmdCopyImageToBuffer(VkCommandBuffer commandBuffer, VkImage srcImage, VkImageLayout srcImageLayout,
                                       VkBuffer dstBuffer, uint32_t regionCount, const VkBufferImageCopy* pRegions) {
    {
        std::lock_guard lock(g_recordedMutex);
        if (g_recording) g_copies.push_back(ImageCopy{srcImage, srcImageLayout, g_eventSeq++});
    }

    using Fn = void (*)(VkCommandBuffer, VkImage, VkImageLayout, VkBuffer, uint32_t, const VkBufferImageCopy*);
    static const Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "vkCmdCopyImageToBuffer"));
    if (real == nullptr) {
        std::fprintf(stderr, "ownership_transfer_probe: cannot resolve the real vkCmdCopyImageToBuffer\n");
        std::abort();
    }
    real(commandBuffer, srcImage, srcImageLayout, dstBuffer, regionCount, pRegions);
}

// GOAL: a render's acquire and release travel in ONE submitted command buffer, and an
//       export submits nothing at all.
// WHY-THIS-SHAPE: a submit counter, paired with the command buffer handle the barrier went
//                 into (see TransferBarrier). The counter alone is not enough: with the
//                 release moved into a nested immediateSubmit, the inner buffer is
//                 submitted before the outer one, so both barriers are recorded at the same
//                 count -- measured, and the two-submit build passed until the handle was
//                 added. The handle alone is not enough either, because the pool recycles
//                 it.
// CONTROL: renderer/vulkan/offscreen.cpp -- the release's vkCmdPipelineBarrier wrapped in a
//          nested m_device.immediateSubmit() of its own -> red with "11 command buffer(s)
//          published only (expected 3), 0 acquired and republished (expected 4)". Reverted.
//          The export half is controlled separately, in run().
extern "C" VkResult vkQueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits, VkFence fence) {
    {
        std::lock_guard lock(g_recordedMutex);
        ++g_submitIndex;
        if (g_recording) ++g_submitsRecorded;
    }

    using Fn = VkResult (*)(VkQueue, uint32_t, const VkSubmitInfo*, VkFence);
    static const Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "vkQueueSubmit"));
    if (real == nullptr) {
        std::fprintf(stderr, "ownership_transfer_probe: cannot resolve the real vkQueueSubmit\n");
        std::abort();
    }
    return real(queue, submitCount, pSubmits, fence);
}

namespace {

// Two monitors: one with a single slot, one with a two-slot ring.
//
// The ring is not decoration. Under a ring deeper than one slot the producer draws into a
// different image each frame, so two of this monitor's images are published at once and
// their acquires interleave with the other monitor's -- which is what forces the pairing
// below to match on the image handle. With one image per monitor drawn in a fixed order,
// index arithmetic pairs them correctly by accident and an acquire naming the wrong image
// would still be graded as matching.
constexpr int kSingleSlotMonitor = 0;
constexpr int kRingMonitor = 1;
constexpr uint32_t kRingDepth = 2;
constexpr size_t kCycles = 2;

// One first render per image (a release, no acquire: nothing was published yet)...
constexpr size_t kImages = 1 + kRingDepth;
// ...then kCycles renders of each monitor, every one of them acquiring back and releasing.
constexpr size_t kRepublishRenders = 2 * kCycles;
constexpr size_t kExpectedBarriers = kImages + kRepublishRenders * 2;

bool isRelease(const TransferBarrier& b, uint32_t graphics) {
    return b.srcQueueFamilyIndex == graphics &&
           (b.dstQueueFamilyIndex == VK_QUEUE_FAMILY_EXTERNAL || b.dstQueueFamilyIndex == VK_QUEUE_FAMILY_FOREIGN_EXT);
}

bool isAcquire(const TransferBarrier& b, uint32_t graphics) {
    return b.dstQueueFamilyIndex == graphics &&
           (b.srcQueueFamilyIndex == VK_QUEUE_FAMILY_EXTERNAL || b.srcQueueFamilyIndex == VK_QUEUE_FAMILY_FOREIGN_EXT);
}

// Everything that reads the recorded barrier list. Split out from run() so the flow of the
// run (draw, export, draw again) is readable without the analysis in the middle of it.
void analyse(const std::vector<TransferBarrier>& recorded, uint32_t graphics, const std::vector<VkImage>& drawnImages) {
    // GOAL: the interposer actually saw the renderer's barriers -- a probe that recorded
    //       nothing must fail, not report a vacuous pass over an empty list.
    // WHY-THIS-SHAPE: everything below is a loop over `recorded`, so all of it is
    //                 satisfied trivially by an empty vector. The count is what makes the
    //                 interposition itself part of what is graded: if the renderer stops
    //                 reaching the loader's vkCmdPipelineBarrier (a dispatch-table call, a
    //                 statically linked libvulkan) the probe goes red here rather than
    //                 green everywhere.
    // CONTROL: `if (g_recording)` in the vkCmdPipelineBarrier interposer changed to
    //          `if (false)` -- red with "recorded 0 queue-family ownership barriers,
    //          expected 11". Reverted.
    const bool countOk = recorded.size() == kExpectedBarriers;
    check(countOk, "a render contributes two ownership barriers and an export none");
    if (!countOk) {
        std::cerr << "  recorded " << recorded.size() << " queue-family ownership barriers, expected "
                  << kExpectedBarriers << std::endl;
        return;  // nothing below can be attributed without the expected shape
    }

    check(isRelease(recorded.front(), graphics),
          "the first ownership barrier of the run is a release: the first render of an image publishes it, and "
          "there is nothing yet to acquire back");

    // GOAL: a render acquires the image back and releases it again in the SAME submit, in
    //       that order -- so a frame is never left drawn-but-unpublished, and the acquire
    //       cannot be ordered after the writes it is supposed to precede.
    // WHY-THIS-SHAPE: grouped by (submit counter, command buffer) rather than asserted
    //                 pairwise over the flat list. Adjacency in the list is satisfied by two
    //                 barriers in two different submissions, which is exactly the shape the
    //                 previous contract had and exactly what this one replaced; only the
    //                 stamp can tell them apart.
    // CONTROL: renderer/vulkan/offscreen.cpp -- the `vkCmdPipelineBarrier` recording the
    //          release moved into a nested `m_device.immediateSubmit()` -> red with eleven
    //          command buffers instead of seven, four of them "a command buffer carrying one
    //          ownership barrier carries a release, not an unpaired acquire". Reverted.
    // COVERAGE-ASSERT: the two group counts below. A grouping that collapsed (every
    //          barrier landing in one group, or each in its own) leaves both zero.
    size_t publishOnlyRenders = 0;
    size_t republishRenders = 0;
    size_t group = 0;
    for (size_t start = 0; start < recorded.size();) {
        size_t end = start;
        while (end < recorded.size() && sameSubmit(recorded[end], recorded[start])) ++end;
        const size_t size = end - start;
        const std::string where = "command buffer " + std::to_string(group++) + ": ";
        if (size == 1) {
            const bool releaseOnly = isRelease(recorded[start], graphics);
            check(releaseOnly,
                  where + "a command buffer carrying one ownership barrier carries a release, not an unpaired "
                          "acquire");
            if (!releaseOnly) std::cerr << "  " << describe(recorded[start]) << std::endl;
            ++publishOnlyRenders;
        } else if (size == 2) {
            const bool shaped = isAcquire(recorded[start], graphics) && isRelease(recorded[start + 1], graphics) &&
                                recorded[start].image == recorded[start + 1].image;
            check(shaped,
                  where + "a render's ownership barriers are an acquire then a release of the same image, in one "
                          "submit");
            if (!shaped) {
                std::cerr << "  " << describe(recorded[start]) << " then " << describe(recorded[start + 1])
                          << (recorded[start].image == recorded[start + 1].image ? " (same image)"
                                                                                 : " (different images)")
                          << std::endl;
            }
            ++republishRenders;
        } else {
            check(false, where + "carried " + std::to_string(size) +
                             " ownership barriers; a render issues one (first publish) or two (acquire, release)");
        }
        start = end;
    }

    const bool groupsOk = publishOnlyRenders == kImages && republishRenders == kRepublishRenders;
    check(groupsOk, "every render in the run submitted the ownership barriers its position implies");
    if (!groupsOk) {
        std::cerr << "  " << publishOnlyRenders << " command buffer(s) published only (expected " << kImages << "), "
                  << republishRenders << " acquired and republished (expected " << kRepublishRenders << ")"
                  << std::endl;
    }

    // Releases waiting for their acquire, keyed by image. Pairing by image rather than by
    // position is what the two-slot ring above exists for.
    std::vector<TransferBarrier> pending;
    size_t pairsCompared = 0;
    size_t index = 0;
    for (const TransferBarrier& barrier : recorded) {
        const std::string where = "barrier " + std::to_string(index++) + " (" + describe(barrier) + "): ";
        const bool release = isRelease(barrier, graphics);
        const bool acquire = isAcquire(barrier, graphics);

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
        //                 recording. That matters because two of the ring monitor's images
        //                 are published at once and their acquires interleave.
        // CONTROL: renderer/vulkan/offscreen.cpp -- a file-scope `VkImage g_controlImage`
        //          latched to the first image render() ever draws, and recordAcquire
        //          overwriting `acquire.image` with it after transferOf built the barrier,
        //          so an acquire names an image its own render did not publish -> red with
        //          barrier 7 "acquires an image no release handed over" and barriers 6 and
        //          10 "this image was not already released without being acquired back",
        //          nine checks in all. Reverted.
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
              where + "acquires from the family the previous render released to");

        // GOAL: both halves of the transfer name the same oldLayout and the same
        //       newLayout, as a queue-family ownership transfer requires.
        // WHY-THIS-SHAPE: compared against the recorded release, not against a constant. A
        //                 constant would encode today's render-pass finalLayout in the
        //                 test, so a renderer that legitimately ends in another layout
        //                 would fail here for no defect -- and the check would stop
        //                 tracking what the release actually said, which is the only thing
        //                 the acquire has to match.
        // CONTROL: renderer/vulkan/offscreen.cpp, `recordAcquire`'s
        //          `transferOf(..., target.releasedFromLayout, VK_IMAGE_LAYOUT_GENERAL, ...)`
        //          changed to `transferOf(..., VK_IMAGE_LAYOUT_GENERAL,
        //          VK_IMAGE_LAYOUT_GENERAL, ...)` -- the defect this probe was written for.
        //          Red four times with "release 0->EXTERNAL SHADER_READ_ONLY_OPTIMAL->GENERAL,
        //          acquire EXTERNAL->0 GENERAL->GENERAL", and zero validation warnings on the
        //          same run, which is the blindness the header describes. Reverted.
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
        // CONTROL: kRenderedLayout in renderer/vulkan/offscreen.h set to
        //          VK_IMAGE_LAYOUT_GENERAL -- red four times with "the release it pairs with
        //          names a layout transition, so matching it is a real constraint".
        //          Reverted.
        check(releaseHalf.oldLayout != releaseHalf.newLayout,
              where + "the release it pairs with names a layout transition, so matching it is a real constraint");
        ++pairsCompared;
    }

    // GOAL: when the run ends, every image drawn is published exactly once -- one
    //       outstanding release each, none released twice, none still owned by the
    //       graphics queue.
    // WHY-THIS-SHAPE: `pending` is not expected to be empty, which is the whole point of
    //                 the new contract: the last render of each image publishes it and
    //                 nothing acquires it back. Asserting emptiness (what the previous
    //                 contract asserted) would now require the probe to issue a render it
    //                 does not need, and would grade the probe's own epilogue rather than
    //                 the renderer.
    // CONTROL: renderer/vulkan/offscreen.cpp -- recordAcquire's `acquire.image` overwritten
    //          with another target's image (see the pairing check above for the mutation),
    //          so one image is released twice and another acquired without ever having been
    //          handed over -> red with "4 image(s) left published, expected 3 (and one image
    //          was released twice without being acquired back)". Reverted. NOT controllable
    //          by deleting the release: that changes the barrier count, and the count check
    //          above abandons the analysis first.
    std::vector<VkImage> pendingImages;
    for (const TransferBarrier& held : pending) pendingImages.push_back(held.image);
    std::sort(pendingImages.begin(), pendingImages.end());
    const bool distinct = std::adjacent_find(pendingImages.begin(), pendingImages.end()) == pendingImages.end();
    std::vector<VkImage> expectedImages = drawnImages;
    std::sort(expectedImages.begin(), expectedImages.end());
    const bool publishedAll = distinct && pendingImages == expectedImages;
    check(publishedAll, "every image the run drew is left published, exactly once, to an external consumer");
    if (!publishedAll) {
        std::cerr << "  " << pendingImages.size() << " image(s) left published, expected " << expectedImages.size()
                  << (distinct ? "" : " (and one image was released twice without being acquired back)") << std::endl;
    }

    // COVERAGE-ASSERT: how many pairs were actually compared, not how many barriers were
    // recorded. Every layout comparison above sits behind a `continue`, so an edit that
    // made the pairing fall through would leave a probe that prints eleven barriers and
    // compares none -- green, and grading nothing.
    // CONTROL: the acquire branch's `if (match == pending.end())` widened to `|| true`, so
    //          every acquire takes the early `continue` -- red with "compared 0
    //          release/acquire pairs, expected 4". Reverted.
    const bool comparedAll = pairsCompared == kRepublishRenders;
    check(comparedAll, "every release/acquire pair the run produced was compared");
    if (!comparedAll) {
        std::cerr << "  compared " << pairsCompared << " release/acquire pairs, expected " << kRepublishRenders
                  << std::endl;
    }
}

// A monitor of its own, so this phase's barriers are recorded on their own and the counts
// analyse() asserts stay describing the run it was written for.
constexpr int kReadbackMonitor = 2;

// GOAL: reading a rendered target back on this device copies from an image the graphics
//       queue owns, in the layout it is actually in. Every completed render() releases the
//       target to an external consumer, so a readback that does not take ownership back
//       first records vkCmdCopyImageToBuffer against memory this queue does not own --
//       undefined contents on any driver whose modifier carries compression metadata.
//       False if a copy of a published image can be recorded with no acquire in front of it.
// WHY-THIS-SHAPE: graded on the submitted command stream -- the ownership barriers and the
//       copy placed in one ordering by a sequence number -- and not on the pixels. The
//       obvious alternative, comparing the readback against a reference image, cannot fail
//       here: neither driver available (NVIDIA block-linear, Lavapipe LINEAR) corrupts an
//       unowned read, and the validation layer does not model queue-family ownership at
//       all, so the defect this replaced produced correct pixels and zero warnings on both.
//       Ownership is replayed from the recorded barriers rather than read off the target,
//       because the target's field is production's own claim and would agree with itself.
// CONTROL: renderer/vulkan/offscreen.cpp -- the `m_device.immediateSubmit(...)` inside
//       reclaim() deleted, so the acquire is never submitted -> red on both ICDs, twice
//       each: "the readback's copy reads an image the graphics queue owns" ("at the copy,
//       the image was owned by EXTERNAL" on NVIDIA, FOREIGN on Lavapipe) and "reclaim()
//       submits exactly one acquire of the target". On the same runs the pixel check below
//       stayed green and neither driver produced a validation warning, which is the
//       blindness this shape is built around. Reverted.
// COVERAGE-ASSERT: two, because the two interposers can fail independently. The copy count
//       guards the whole phase -- every check is a statement about `copy`. The acquire
//       count guards the ownership replay, which starts from the graphics queue and would
//       report "owned by graphics" for a barrier recorder that stopped recording: measured
//       with `if (g_recording)` in the barrier interposer forced false, where the ownership
//       check stayed green and "reclaim() submits exactly one acquire" went red.
void checkReclaimBeforeReadback(wallpaperx::Device& device, wallpaperx::OffscreenTargetManager& targets,
                                wallpaperx::IRenderer& renderer) {
    const uint32_t graphics = device.graphicsQueueFamily();

    startRecording();
    const wallpaperx::OffscreenTarget& target = targets.create(kReadbackMonitor, kWidth, kHeight);
    targets.render(renderer, target);
    const TargetState published = stateOf(target);

    const std::vector<unsigned char> pixels = wallpaperx::testing::reclaimAndReadback(device, targets, target);

    const std::vector<TransferBarrier> barriers = stopRecording();
    const std::vector<ImageCopy> copies = recordedCopies();

    const bool copiedOnce = copies.size() == 1 && copies[0].image == target.image.image;
    check(copiedOnce, "the readback issued exactly one copy out of the target it rendered");
    if (!copiedOnce) {
        std::cerr << "  recorded " << copies.size() << " image-to-buffer copy/copies, expected 1 of this target"
                  << std::endl;
        return;  // nothing below can be attributed without a copy to attribute it to
    }
    const ImageCopy& copy = copies[0];

    // Replay this image's queue-family ownership over everything recorded before the copy.
    // The target was created inside this recording, so it starts owned by the graphics
    // queue; the render's release hands it away and reclaim()'s acquire must bring it back.
    uint32_t owner = graphics;
    size_t acquiresBeforeCopy = 0;
    const TransferBarrier* acquire = nullptr;
    for (const TransferBarrier& barrier : barriers) {
        if (barrier.image != target.image.image || barrier.seq > copy.seq) continue;
        if (isAcquire(barrier, graphics)) {
            ++acquiresBeforeCopy;
            acquire = &barrier;
        }
        owner = barrier.dstQueueFamilyIndex;
    }

    check(owner == graphics, "the readback's copy reads an image the graphics queue owns");
    if (owner != graphics) {
        std::cerr << "  at the copy, the image was owned by " << familyName(owner) << std::endl;
    }

    const bool acquiredOnce = acquiresBeforeCopy == 1;
    check(acquiredOnce, "reclaim() submits exactly one acquire of the target before the copy");
    if (!acquiredOnce) {
        std::cerr << "  " << acquiresBeforeCopy << " acquire(s) of this image precede the copy, expected 1"
                  << std::endl;
    }

    // The acquire has to reverse the release the render actually issued, exactly as the
    // acquire before a re-render does: same families swapped, same layout pair.
    // CONTROL: renderer/vulkan/offscreen.cpp -- recordAcquire's newLayout changed from
    //       VK_IMAGE_LAYOUT_GENERAL to VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL -> red with
    //       "reclaim()'s acquire reverses the release the render published with", alongside
    //       four hits on the pairing check in analyse() and the reader-transition check
    //       below. Reverted.
    if (acquiredOnce) {
        const bool reverses = acquire->srcQueueFamilyIndex == published.externalOwner &&
                              acquire->dstQueueFamilyIndex == graphics &&
                              acquire->oldLayout == published.releasedFromLayout &&
                              acquire->newLayout == VK_IMAGE_LAYOUT_GENERAL;
        check(reverses, "reclaim()'s acquire reverses the release the render published with");
        if (!reverses) {
            std::cerr << "  published " << describe(published) << ", acquire " << describe(*acquire) << std::endl;
        }
    }

    // GOAL: the layout a reader names when it moves a reclaimed image into a copyable
    //       layout is the layout reclaim() actually left it in, and the copy reads it from
    //       where that transition put it. False if the reader names a stale layout -- the
    //       other half of the defect this phase exists for, where a readback named the
    //       render pass's finalLayout for an image the release had since moved to GENERAL.
    // WHY-THIS-SHAPE: the reader's oldLayout is compared against the acquire the driver was
    //       given, not against the constant GENERAL. Against the constant it would restate
    //       gpu_readback.h's own literal on both sides and hold whatever reclaim() did;
    //       against the recorded acquire these are two independent observations, so a
    //       reclaim() that left the image somewhere else fails here.
    // CONTROL: renderer/vulkan/offscreen.cpp -- recordAcquire's newLayout changed from
    //       VK_IMAGE_LAYOUT_GENERAL to VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL -> red with
    //       "the reader transitions the image from where reclaim() left it". Reverted.
    // COVERAGE-ASSERT: exactly one transition of this image between the acquire and the
    //       copy; at zero the comparison below is unreachable.
    if (acquiredOnce) {
        std::vector<LayoutTransition> between;
        for (const LayoutTransition& transition : recordedTransitions()) {
            if (transition.image != target.image.image) continue;
            if (transition.seq < acquire->seq || transition.seq > copy.seq) continue;
            between.push_back(transition);
        }
        const bool oneTransition = between.size() == 1;
        check(oneTransition, "the reader issues one layout transition between the acquire and the copy");
        if (!oneTransition) {
            std::cerr << "  " << between.size() << " transition(s) of this image between acquire and copy"
                      << std::endl;
        } else {
            const bool fromReclaim =
                between[0].oldLayout == acquire->newLayout && between[0].newLayout == copy.layout;
            check(fromReclaim, "the reader transitions the image from where reclaim() left it");
            if (!fromReclaim) {
                std::cerr << "  reclaim() left it in " << layoutName(acquire->newLayout)
                          << ", the reader transitioned " << layoutName(between[0].oldLayout) << "->"
                          << layoutName(between[0].newLayout) << ", the copy read " << layoutName(copy.layout)
                          << std::endl;
            }
        }
    }

    // GOAL: a reclaimed target is no longer published, so nothing may export it until the
    //       next render hands it over again. False if reclaim() submits the acquire but
    //       leaves the target claiming a consumer still holds the frame -- which would make
    //       the next render issue a second acquire for a release that was already reversed.
    // CONTROL: renderer/vulkan/offscreen.cpp -- `target.notifyReclaimed()` deleted from
    //       reclaim() -> red with "a reclaimed target is no longer published" and with
    //       "reclaim() refuses a target with no published frame", the second because the
    //       target still looked published. Reverted.
    check(!target.published(), "a reclaimed target is no longer published");
    if (target.published()) std::cerr << "  after reclaim: " << describe(stateOf(target)) << std::endl;

    // GOAL: reclaim() refuses a target that has nothing to take back. Only a render
    //       publishes and only one reclaim can follow one render, so a second reclaim is a
    //       caller bug and must be reported rather than submitting an acquire from
    //       VK_QUEUE_FAMILY_IGNORED -- a barrier that transfers nothing and whose oldLayout
    //       would be UNDEFINED, licensing the driver to discard the frame just read back.
    // WHY-THIS-SHAPE: the throw is caught and its presence asserted, rather than letting it
    //       escape. A phase that ended in an uncaught exception would take the checks after
    //       it with it, which is X.4's failure mode, not a result.
    // CONTROL: renderer/vulkan/offscreen.cpp -- reclaim()'s `if (!target.published()) throw`
    //       removed -> red with "reclaim() refuses a target with no published frame", and
    //       the validation grade went red alongside it with one warning, which is the
    //       UNDEFINED oldLayout this describes actually being submitted. Reverted.
    bool refused = false;
    try {
        targets.reclaim(target);
    } catch (const std::exception&) {
        refused = true;
    }
    check(refused, "reclaim() refuses a target with no published frame");

    // The copy read the frame that was drawn, not an untouched image: without this every
    // check above would hold for a readback of a blank target, and "reads the right bytes"
    // would rest on nothing. Not a comparison against a reference -- see WHY-THIS-SHAPE for
    // why no pixel comparison can see the defect this phase grades.
    // CONTROL: renderer/renderers/static_renderer.cpp -- the `vkCmdDraw(cmd, 6, ...)` in
    //       record() changed to a zero-vertex draw, so the target keeps the render pass's
    //       clear colour -> red with "the reclaimed readback contains the rendered
    //       gradient, not a uniform image", and nothing else in the probe moved. Reverted.
    check(wallpaperx::testing::variesInBothAxes(pixels, kWidth, kHeight),
          "the reclaimed readback contains the rendered gradient, not a uniform image");
}

bool run(wallpaperx::testing::ValidationGrade& grade) {
    const std::string imagePath = wallpaperx::testing::writeGradientPng(
        std::string(WALLPAPERX_TEST_IMAGE_DIR) + "/ownership_transfer_gradient.png", 16, 16);

    wallpaperx::Device device(/*enableValidation=*/true);
    grade.watch(device);

    wallpaperx::OffscreenTargetManager targets(device);
    wallpaperx::StaticRenderer renderer(device, targets.renderPass(), imagePath, WALLPAPERX_SHADER_DIR);

    // Recording covers the first render of every image too, unlike the previous contract's
    // version of this probe: under producer-driven publishing that render issues a release
    // of its own, and leaving it out would hide the one case where a release legitimately
    // has no acquire in front of it.
    startRecording();

    std::vector<const wallpaperx::OffscreenTarget*> drawn;
    drawn.push_back(&targets.create(kSingleSlotMonitor, kWidth, kHeight));
    targets.render(renderer, *drawn.back());

    targets.create(kRingMonitor, kWidth, kHeight, kRingDepth);
    for (uint32_t slot = 0; slot < kRingDepth; ++slot) {
        const wallpaperx::OffscreenTarget& target =
            slot == 0 ? targets.get(kRingMonitor) : targets.advance(kRingMonitor);
        targets.render(renderer, target);
        drawn.push_back(&target);
    }

    // GOAL: a target that has been rendered and never exported is nonetheless published --
    //       its recorded ownership and layout describe the frame that was drawn, not a
    //       handover waiting for a consumer to ask. False if a drawn-but-unfetched target
    //       is left owned by the graphics queue.
    // WHY-THIS-SHAPE: asserted here, before the first exportFrame call in the process, so
    //                 "never exported" is a fact about the run rather than a claim. The
    //                 simpler alternative -- checking the same fields at the end -- cannot
    //                 distinguish a render that published from an export that did, which
    //                 is precisely the two arrangements this contract replaced.
    // CONTROL: renderer/vulkan/offscreen.cpp -- the `target.notifyPublished(consumer,
    //          renderedLayout)` at the end of render() deleted -> red on all three counts
    //          for all three images, printing "owner IGNORED, released from UNDEFINED".
    //          Reverted.
    // COVERAGE-ASSERT: the loop is over `drawn`, and `drawn.size()` is asserted, so a
    //          future edit that stopped collecting targets fails instead of checking none.
    check(drawn.size() == kImages, "the probe drew every slot it intended to");
    for (size_t i = 0; i < drawn.size(); ++i) {
        const std::string where = "image " + std::to_string(i) + " (rendered, never exported): ";
        const TargetState state = stateOf(*drawn[i]);
        check(state.externalOwner != VK_QUEUE_FAMILY_IGNORED,
              where + "is released to an external consumer by the render that drew it, not by a later export");
        check(state.releasedFromLayout == wallpaperx::kRenderedLayout,
              where + "records the layout the renderer actually left it in as what it released from");
        check(drawn[i]->published(), where + "reports itself published");
        if (state.externalOwner == VK_QUEUE_FAMILY_IGNORED || !drawn[i]->published() ||
            state.releasedFromLayout != wallpaperx::kRenderedLayout) {
            std::cerr << "  " << describe(state) << std::endl;
        }
    }

    // GOAL: the target's tracked fields describe the barrier the driver was actually given,
    //       not an intention. False if the bookkeeping and the submitted barrier can
    //       disagree.
    // WHY-THIS-SHAPE: the fields and the barrier come from two independent observations --
    //                 the production struct, and the interposed driver call -- so agreement
    //                 between them is evidence. Reading the barrier alone (or the fields
    //                 alone) would establish half of it: notifyPublished() could stamp a
    //                 layout no barrier named, or a barrier could name one nothing recorded.
    // CONTROL: renderer/vulkan/offscreen.cpp -- `target.notifyPublished(consumer,
    //          renderedLayout)` changed to `notifyPublished(consumer,
    //          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)` -> red for each image with "recorded
    //          ... released from TRANSFER_SRC_OPTIMAL, submitted ...
    //          SHADER_READ_ONLY_OPTIMAL->GENERAL". Reverted.
    const std::vector<TransferBarrier> firstPublishes = peekRecorded();
    const bool firstPublishesOk = firstPublishes.size() == kImages;
    check(firstPublishesOk, "the first render of each image issued exactly one ownership barrier");
    if (firstPublishesOk) {
        for (size_t i = 0; i < drawn.size(); ++i) {
            const TransferBarrier& barrier = firstPublishes[i];
            const TargetState state = stateOf(*drawn[i]);
            // newLayout against the constant, not against a tracked field: GENERAL is
            // render()'s release post-condition and reclaim()'s acquire names the same, so
            // it is the one half of this that has no bookkeeping to agree with.
            const bool agrees = barrier.image == drawn[i]->image.image &&
                                barrier.dstQueueFamilyIndex == state.externalOwner &&
                                barrier.oldLayout == state.releasedFromLayout &&
                                barrier.newLayout == VK_IMAGE_LAYOUT_GENERAL;
            check(agrees, "image " + std::to_string(i) +
                              ": the release the driver was given is the one the target recorded");
            if (!agrees) {
                std::cerr << "  recorded " << describe(state) << ", submitted " << describe(barrier) << std::endl;
            }
        }
    } else {
        std::cerr << "  " << firstPublishes.size() << " barrier(s) from " << kImages << " first render(s)"
                  << std::endl;
    }

    // GOAL: an export issues no ownership barrier, submits nothing, and changes no target
    //       state. False if fetching a frame is what publishes it -- the arrangement this
    //       contract replaced, under which a frame nobody fetched was never handed over.
    // WHY-THIS-SHAPE: measured as a delta across the export calls rather than as an
    //                 absolute count, because the run has already submitted several renders
    //                 by this point and an absolute figure would have to encode how many.
    //                 The submit delta is checked as well as the barrier delta: a rebuilt
    //                 release that used vkCmdPipelineBarrier2 would slip past a barrier
    //                 count alone, and any handover at all needs a submit.
    // CONTROL: renderer/vulkan/frame_export.cpp -- `target.releasedFromLayout =
    //          VK_IMAGE_LAYOUT_UNDEFINED;` added before exportFrame's `return frame;` -> red
    //          with "an export leaves the target's recorded ownership and layout untouched",
    //          and with two later pairing failures naming "UNDEFINED->GENERAL", which is the
    //          corruption that write causes. A second mutation, an ownership-release barrier
    //          submitted from exportFrame, reddens the other two ("an export issues no
    //          queue-family ownership barrier", "an export submits no GPU work"); reduced to
    //          `device.immediateSubmit([](VkCommandBuffer) {});` it reddens the submit check
    //          alone, so neither rides on the other. All three reverted.
    const size_t barriersBeforeExport = firstPublishes.size();
    const uint64_t submitsBeforeExport = submitsRecorded();
    const TargetState singleBefore = stateOf(targets.get(kSingleSlotMonitor));
    const TargetState ringBefore = stateOf(targets.get(kRingMonitor));

    wallpaperx::ExportedFrame::HandleType handleType = wallpaperx::ExportedFrame::HandleType::OpaqueFd;
    for (const int monitorId : {kSingleSlotMonitor, kRingMonitor}) {
        const wallpaperx::ExportedFrame frame = wallpaperx::exportFrame(device, targets.get(monitorId));
        handleType = frame.type;
        if (frame.fd >= 0) close(frame.fd);
    }

    check(peekRecorded().size() == barriersBeforeExport, "an export issues no queue-family ownership barrier");
    check(submitsRecorded() == submitsBeforeExport, "an export submits no GPU work");
    check(stateOf(targets.get(kSingleSlotMonitor)) == singleBefore &&
              stateOf(targets.get(kRingMonitor)) == ringBefore,
          "an export leaves the target's recorded ownership and layout untouched");

    // GOAL: the family a release names is the one the handle type it is exported as must be
    //       imported by -- FOREIGN for a dma_buf a compositor imports, EXTERNAL for an
    //       opaque fd another Vulkan context imports.
    // WHY-THIS-SHAPE: the expectation is derived from the ExportedFrame the export actually
    //                 produced, not from the device's own extension flags. Reading
    //                 device.info() here would restate production's own input and pass for
    //                 any consistent pair of wrong answers; the handle type is what a
    //                 consumer is really told.
    // CONTROL: renderer/vulkan/memory_export.cpp -- externalConsumerQueueFamily()'s
    //          `foreign` condition negated -> red on Lavapipe with "released to EXTERNAL,
    //          expected FOREIGN for a dma_buf export" and on NVIDIA with the mirror image.
    //          Reverted.
    const bool foreign =
        handleType == wallpaperx::ExportedFrame::HandleType::DmaBuf && device.info().hasQueueFamilyForeign;
    const uint32_t expectedFamily = foreign ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_EXTERNAL;
    const uint32_t observedFamily = targets.get(kSingleSlotMonitor).externalOwner;
    check(observedFamily == expectedFamily,
          "the release names the queue family the exported handle type is imported by");
    if (observedFamily != expectedFamily) {
        std::cerr << "  released to " << familyName(observedFamily) << ", expected " << familyName(expectedFamily)
                  << " for a "
                  << (handleType == wallpaperx::ExportedFrame::HandleType::DmaBuf ? "dma_buf" : "opaque fd")
                  << " export" << std::endl;
    }

    // Every render from here on has a publish outstanding, so each must acquire back before
    // it writes and release again after.
    for (size_t cycle = 0; cycle < kCycles; ++cycle) {
        for (const int monitorId : {kSingleSlotMonitor, kRingMonitor}) {
            targets.render(renderer, targets.advance(monitorId));
        }
    }

    const std::vector<TransferBarrier> recorded = stopRecording();
    for (const TransferBarrier& barrier : recorded) {
        std::cout << "ownership_transfer_probe: recorded " << describe(barrier) << std::endl;
    }

    std::vector<VkImage> drawnImages;
    for (const wallpaperx::OffscreenTarget* target : drawn) drawnImages.push_back(target->image.image);
    analyse(recorded, device.graphicsQueueFamily(), drawnImages);

    // Last, and on a monitor of its own: it starts a second recording, and it leaves its
    // target unpublished, which is a state nothing above expects to see.
    checkReclaimBeforeReadback(device, targets, renderer);

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
