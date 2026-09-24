// SPDX-License-Identifier: GPL-2.0-or-later

// Grades the one rule that keeps a descriptor from being rewritten under a GPU that may
// still be reading it: a freed slot carries the last batch that could have read it, and is
// handed out again only once that batch has completed.
//
// WHY THIS NEEDS A PROBE OF ITS OWN. Until Step 13 the rule held by accident -- the static
// renderer waited for device idle at the end of every render pass, so every pass began on
// an idle device and no descriptor write could ever land under pending work. A renderer
// kept alive across frames removes that accident. Nothing else in the suite submits work
// and then rewrites a descriptor without waiting, so nothing else can see the difference.
//
// THE THREE TIERS DO NOT SHARE A HAZARD, and a check that folds them together grades the
// wrong thing on two of them:
//
//   * Heap and descriptor-buffer write host memory the GPU reads at a byte offset derived
//     from the slot. The hazard is rewriting *that slot*; a different slot is safe. Heap
//     has 64 slots, so the safe answer is a fresh slot and no wait. The buffer tier has one
//     binding at a fixed offset -- there is nowhere else to put a descriptor -- so its only
//     safe answer is to block.
//   * The classic tier calls vkUpdateDescriptorSets on a single set, where the spec's
//     protected unit is the whole set and not the slot: writing a binding no pending draw
//     reads, that has never been written before, is as illegal as overwriting the one it
//     samples (VUID-vkUpdateDescriptorSets-None-03047). Slot discipline alone leaves that
//     defect standing, which is why that tier drains outstanding batches before any write.
//
// One entry per tier, because the three have different skip conditions and folding them
// into one verdict would report a claim that was never established as a pass -- or a claim
// that was established as a skip. See renderer/CMakeLists.txt.
//
// TWO ORACLES, AND WHY NEITHER ALONE IS ENOUGH.
//
//   * The validation layer is graded on every phase and is the oracle for none of them. It
//     does not model a host write into a mapped descriptor heap, so heap and buffer reuse
//     is silent; and on classic its VUID-vkUpdateDescriptorSets-None-03047 check marks a set
//     in use at DRAW time, so the pending batch here -- which binds the set and never draws
//     through it -- does not trip it either. Measured with the drain stubbed out: zero
//     warnings on both NVIDIA and Lavapipe while the write was plainly unsafe.
//   * "Did it have to wait?" must be answered from OUTSIDE the renderer. Asking
//     Device::hasCompleted() is what makes a probe agree with the defect -- a mutation
//     forcing that function to return true poisons the code under test and the oracle in
//     one edit, and the probe comes back green. So this file interposes vkWaitForFences,
//     vkQueueSubmit and vkResetFences and reads the driver directly.
//
// WHY A BATCH IS KEPT DELIBERATELY BUSY. "The slot was not reused" is only meaningful while
// the batch that could read it is still running; a batch that finished on its own makes
// reuse correct, so a check that assumed pending would be asserting on the scheduler. Every
// phase submits hundreds of megabytes of buffer copies and then asks the driver -- not the
// renderer -- what state that batch was in on each side of the descriptor write. A phase
// that cannot establish the state it needs is retried with more work and finally reported
// as NOT VERIFIED (exit 77), never as a pass.
//
// ONE DRIVER CANNOT ANSWER THE HEAP QUESTION, and it is named rather than papered over.
// Measured on Lavapipe (Mesa 25.x, llvmpipe): the batch is pending when
// vkWriteResourceDescriptorsEXT is entered and complete when it returns, and the allocator
// itself never waits -- the call takes exactly as long as the outstanding work (83 ms, 315
// ms and 1495 ms for 1, 6 and 32 GiB of copies), so that driver drains the queue inside the
// descriptor write. The same call on NVIDIA takes 0.025 ms and leaves the batch running.
// There is therefore no in-flight state left on Lavapipe for this phase to observe, and its
// heap entry reports a skip with that reason. NVIDIA -- the GPU this project targets --
// grades it fully.

#include "vulkan/descriptor_allocator.h"
#include "vulkan/device.h"
#include "vulkan/memory.h"

#include "support/check.h"
#include "support/descriptor_probe_support.h"
#include "support/real_entry_point.h"
#include "support/validation_grade.h"

#include <dlfcn.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <mutex>
#include <string>
#include <unordered_map>

using wallpaperx::testing::check;
using wallpaperx::testing::realEntryPoint;

namespace {

// ctest's convention for "the host could not be put in the state this grades"; the
// SKIP_RETURN_CODE property in renderer/CMakeLists.txt is what makes ctest act on it.
constexpr int kNotVerifiedExitCode = 77;
bool g_notVerified = false;

// Guards every field below. The renderer submits from one thread (Device pins itself), but
// these entry points are interposed process-wide.
std::mutex g_vkMutex;
int g_fenceWaits = 0;
VkFence g_lastSubmitFence = VK_NULL_HANDLE;
// How many times each fence has been reset. Device recycles a fence only after seeing it
// signalled, so a bump means the batch it named finished -- see batchState().
std::unordered_map<VkFence, uint64_t> g_fenceResets;

int fenceWaits() {
    std::lock_guard lock(g_vkMutex);
    return g_fenceWaits;
}

// Names one submitted batch by the fence the driver was given for it, pinned to the
// generation of that fence so a later recycle cannot be mistaken for this batch.
struct BatchRef {
    VkFence fence = VK_NULL_HANDLE;
    uint64_t resets = 0;
};

BatchRef lastSubmittedBatch() {
    std::lock_guard lock(g_vkMutex);
    return BatchRef{g_lastSubmitFence, g_fenceResets[g_lastSubmitFence]};
}

enum class Batch { Pending, Finished };

// GOAL: answer "was that batch still running?" without asking the code under test.
// WHY-THIS-SHAPE: a bare vkGetFenceStatus on the captured handle is not enough. Device
//                 resets a fence and returns it to a free list as soon as it sees it
//                 signalled, so a completed-and-retired batch reads back NOT_READY -- the
//                 exact inversion of the answer wanted. The reset count pins the handle to
//                 one batch; only while it is unchanged is the raw status meaningful.
Batch batchState(VkDevice device, const BatchRef& ref) {
    if (ref.fence == VK_NULL_HANDLE) return Batch::Finished;
    {
        std::lock_guard lock(g_vkMutex);
        const auto it = g_fenceResets.find(ref.fence);
        const uint64_t now = it == g_fenceResets.end() ? 0 : it->second;
        if (now != ref.resets) return Batch::Finished;
    }
    return vkGetFenceStatus(device, ref.fence) == VK_SUCCESS ? Batch::Finished : Batch::Pending;
}

constexpr const char* kProbe = "descriptor_inflight_probe";

// A buffer tied to a scope. Every phase here has a throwing path, and a buffer freed only on
// the success path leaks an allocation that VMA turns into a fatal teardown assert -- which
// would replace the diagnosis with an abort.
class ScopedBuffer {
public:
    ScopedBuffer(wallpaperx::Device& device, VkDeviceSize size, VkBufferUsageFlags usage, const char* name)
        : m_device(&device), m_buffer(device.memory().createBuffer(size, usage, wallpaperx::HostAccess::Write, name)) {}

    ~ScopedBuffer() { m_device->memory().destroyBuffer(m_buffer); }

    ScopedBuffer(const ScopedBuffer&) = delete;
    ScopedBuffer& operator=(const ScopedBuffer&) = delete;

    VkBuffer handle() const { return m_buffer.buffer; }
    void* mapped() const { return m_buffer.mapped; }

private:
    wallpaperx::Device* m_device;
    wallpaperx::AllocatedBuffer m_buffer;
};

// Declared AFTER everything the GPU may read and therefore destroyed first, so no exit path
// -- including the exception path -- frees a buffer or an image out from under a batch that
// is still running.
class DrainOnExit {
public:
    explicit DrainOnExit(wallpaperx::Device& device) : m_device(&device) {}

    ~DrainOnExit() {
        try {
            m_device->waitForSubmissions();
        } catch (const std::exception& e) {
            // Reported, never rethrown: a destructor that throws while unwinding takes the
            // whole process down and with it the diagnosis this probe exists to print.
            std::fprintf(stderr, "descriptor_inflight_probe: could not drain the queue: %s\n", e.what());
        }
    }

    DrainOnExit(const DrainOnExit&) = delete;
    DrainOnExit& operator=(const DrainOnExit&) = delete;

private:
    wallpaperx::Device* m_device;
};

// GPU work long enough that a descriptor rewrite issued straight after the submit lands
// while it is still running. Two host-visible buffers copied one into the other: no shader,
// no pipeline, nothing that varies by tier or driver.
class BusyWork {
public:
    BusyWork(wallpaperx::Device& device, VkDeviceSize bytes, uint32_t copies)
        : m_bytes(bytes),
          m_copies(copies),
          m_src(device, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, "inflight-probe-busy-src"),
          m_dst(device, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, "inflight-probe-busy-dst") {
        // Written once from the host before anything is submitted, so the copies below read
        // memory with a defined history rather than whatever the allocation came with.
        if (m_src.mapped()) std::memset(m_src.mapped(), 0x5a, static_cast<size_t>(bytes));
    }

    // The barrier between copies is not decoration: synchronization validation is enabled on
    // this Device (see renderer/vulkan/device.cpp), and back-to-back copies into the same
    // range are a write-after-write hazard it reports -- which would land in the same warning
    // counter the classic phase reads as its oracle.
    void record(VkCommandBuffer cmd) const {
        VkBufferCopy region{};
        region.size = m_bytes;
        for (uint32_t i = 0; i < m_copies; ++i) {
            if (i > 0) {
                VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
                                     &barrier, 0, nullptr, 0, nullptr);
            }
            vkCmdCopyBuffer(cmd, m_src.handle(), m_dst.handle(), 1, &region);
        }
    }

private:
    VkDeviceSize m_bytes;
    uint32_t m_copies;
    ScopedBuffer m_src;
    ScopedBuffer m_dst;
};

// Binds the tier's descriptor state and then keeps the queue busy, so the batch both is a
// plausible reader of the slot just handed out and is still running when the next descriptor
// write is issued.
BatchRef submitBoundBusyBatch(wallpaperx::Device& device, const BusyWork& busy) {
    device.submit([&](VkCommandBuffer cmd) {
        device.descriptorAllocator().bind(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS);
        busy.record(cmd);
    });
    return lastSubmittedBatch();
}

// What the driver's own fence says about the window the descriptor write ran in.
enum class Window {
    Graded,         // the write is answerable: it either blocked, or returned with the batch
                    // still running -- in both cases the decision it made is observable
    DrainedInside,  // pending on the way in, complete on the way out, and the allocator never
                    // waited: something below the renderer completed the batch
    AlreadyDone,    // the batch had finished before the write started, so reuse was legal and
                    // nothing about the rule was exercised
};

Window classifyWindow(Batch before, Batch after, int waits) {
    if (waits > 0) return Window::Graded;
    if (before == Batch::Finished) return Window::AlreadyDone;
    return after == Batch::Pending ? Window::Graded : Window::DrainedInside;
}

enum class Outcome { Pass, Fail, Ambiguous, DriverDrained, Skipped };

// The batch outran the descriptor write, so the tier was never asked the question. Printed
// per attempt so a run that ends NOT VERIFIED says which phase could not be set up.
Outcome tooFast(const char* phase, uint32_t copies) {
    std::printf("descriptor_inflight_probe (%s): the GPU finished %u copies before the descriptor write started"
                " -- nothing was in flight, retrying with more work\n",
                phase, copies);
    return Outcome::Ambiguous;
}

// gradedRun's verdict covers the Vulkan validation layer, which is this probe's only oracle
// on the classic tier. Its own FAIL line names the count; this one names the phase and, more
// importantly, puts it in the process exit code -- and it is skipped when a check in the
// phase already failed, so one defect is not counted twice.
void foldValidationVerdict(const char* phase, bool graded, int failuresBefore) {
    if (graded) return;
    if (wallpaperx::testing::failureCount() != failuresBefore) return;
    check(false, std::string("descriptor_inflight_probe (") + phase +
                     "): the Vulkan validation layer reported a problem (see the FAIL line above)");
}

// A phase that reached a verdict of its own keeps it; one that thought it passed while a
// check failed does not. Folding this in one place keeps the two ways a phase can fail --
// its own checks, and the validation layer -- from disagreeing with its Outcome.
Outcome settle(const char* phase, Outcome outcome, bool graded, int failuresBefore) {
    foldValidationVerdict(phase, graded, failuresBefore);
    if (wallpaperx::testing::failureCount() != failuresBefore) return Outcome::Fail;
    return outcome;
}

constexpr VkDeviceSize kBusyBytes = 64ull << 20;

// GOAL: on the tier this project ships on, a slot whose last reader is still running is not
//       handed out again -- and taking a different one instead costs no wait.
// WHY-THIS-SHAPE: "the second allocate returned a different index" alone is not a check: on
//                 an idle device the pool correctly returns the SAME index, so which answer
//                 is right depends on whether the batch was still running. That is a race
//                 with the GPU, so it is established rather than assumed -- the driver's
//                 fence is read on both sides of the write, and only a batch still running
//                 when the write RETURNED proves it was running when the slot was chosen
//                 (a fence goes unsignalled to signalled and never back). The no-wait clause
//                 is counted from interposed vkWaitForFences rather than timed, because a
//                 fast machine and a skipped wait produce the same microsecond reading.
// CONTROL: renderer/vulkan/descriptor_slots.cpp -- release() stamping Device::SubmitTicket{}
//          instead of m_device.lastSubmission(); and separately renderer/vulkan/device.cpp --
//          hasCompleted() returning true unconditionally. Both observed red on the reuse
//          clause. The no-wait clause has its own: a m_device.wait() added ahead of
//          acquire()'s fresh-slot branch in renderer/vulkan/descriptor_slots.cpp reddens that
//          clause alone, with the index comparison still green.
Outcome heapKeepsAnInFlightSlot(uint32_t copies) {
    constexpr const char* kPhase = "heap in-flight";
    unsetenv("WALLPAPERX_FORCE_TIER");

    Outcome outcome = Outcome::Fail;
    const int failuresBefore = wallpaperx::testing::failureCount();
    const bool graded = wallpaperx::testing::gradedRun(
        std::string("descriptor_inflight_probe (") + kPhase + ")",
        [&](wallpaperx::testing::ValidationGrade& grade) {
            try {
                wallpaperx::Device device(/*enableValidation=*/true);
                grade.watch(device);
                if (auto skip = wallpaperx::testing::heapTierGuard(device, "descriptor_inflight_probe", kPhase)) {
                    outcome = *skip ? Outcome::Skipped : Outcome::Fail;
                    if (!*skip) {
                        check(false, "descriptor_inflight_probe (heap in-flight): the Heap tier was expected here");
                    }
                    return *skip;
                }

                wallpaperx::testing::TestResource a(device);
                wallpaperx::testing::TestResource b(device);
                BusyWork busy(device, kBusyBytes, copies);
                DrainOnExit drain(device);

                const wallpaperx::DescriptorHandle handleA =
                    device.descriptorAllocator().allocate(a.view(), a.sampler());
                const BatchRef batch = submitBoundBusyBatch(device, busy);
                device.descriptorAllocator().free(handleA);

                const int waitsBefore = fenceWaits();
                const Batch before = batchState(device.device(), batch);
                const wallpaperx::DescriptorHandle handleB =
                    device.descriptorAllocator().allocate(b.view(), b.sampler());
                const Batch after = batchState(device.device(), batch);
                const int waits = fenceWaits() - waitsBefore;

                switch (classifyWindow(before, after, waits)) {
                    case Window::AlreadyDone:
                        outcome = tooFast(kPhase, copies);
                        break;
                    case Window::DrainedInside:
                        outcome = Outcome::DriverDrained;
                        break;
                    case Window::Graded:
                        check(handleB.index != handleA.index,
                              "descriptor_inflight_probe (heap in-flight): the freed slot was handed out again while "
                              "the batch that bound it was still running -- its descriptor bytes were overwritten "
                              "mid-flight");
                        check(waits == 0,
                              "descriptor_inflight_probe (heap in-flight): re-allocating blocked on the GPU although "
                              "the heap had unused slots to hand out instead");
                        outcome = Outcome::Pass;
                        break;
                }

                device.descriptorAllocator().free(handleB);
            } catch (const std::exception& e) {
                // Reported rather than thrown out of the lambda: a setup failure must print
                // its own diagnosis instead of surfacing as an abort with nothing to read.
                check(false, std::string("descriptor_inflight_probe (heap in-flight) could not run: ") + e.what());
                outcome = Outcome::Fail;
            }
            return wallpaperx::testing::failureCount() == failuresBefore;
        });
    return settle(kPhase, outcome, graded, failuresBefore);
}

// GOAL: the delay is a delay and not a ban -- once the batch that could read a freed slot has
//       completed, that slot is the one handed back.
// WHY-THIS-SHAPE: without this, the phase above passes against a pool that never reuses
//                 anything, which would silently walk through all 64 slots and then throw.
//                 The two together say the pool *chose*: same sequence of calls, opposite
//                 side of the completion test, opposite answer -- which is why they share one
//                 ctest entry and cannot be split apart into one that passes alone.
//                 waitForSubmissions comes first so the state being graded is established
//                 rather than hoped for, and that it worked is read from the driver's fence,
//                 not from the renderer's own view of it.
// CONTROL: renderer/vulkan/descriptor_slots.cpp -- acquire()'s released-slot branch removed
//          so every allocate takes a fresh slot. Observed red here while the in-flight phase
//          above stayed green.
Outcome heapReusesACompletedSlot(uint32_t copies) {
    constexpr const char* kPhase = "heap reuse after completion";
    unsetenv("WALLPAPERX_FORCE_TIER");

    Outcome outcome = Outcome::Fail;
    const int failuresBefore = wallpaperx::testing::failureCount();
    const bool graded = wallpaperx::testing::gradedRun(
        std::string("descriptor_inflight_probe (") + kPhase + ")",
        [&](wallpaperx::testing::ValidationGrade& grade) {
            try {
                wallpaperx::Device device(/*enableValidation=*/true);
                grade.watch(device);
                if (auto skip = wallpaperx::testing::heapTierGuard(device, "descriptor_inflight_probe", kPhase)) {
                    outcome = *skip ? Outcome::Skipped : Outcome::Fail;
                    if (!*skip) {
                        check(false,
                              "descriptor_inflight_probe (heap reuse after completion): the Heap tier was expected");
                    }
                    return *skip;
                }

                wallpaperx::testing::TestResource a(device);
                wallpaperx::testing::TestResource b(device);
                BusyWork busy(device, kBusyBytes, copies);
                DrainOnExit drain(device);

                const wallpaperx::DescriptorHandle handleA =
                    device.descriptorAllocator().allocate(a.view(), a.sampler());
                const BatchRef batch = submitBoundBusyBatch(device, busy);
                device.descriptorAllocator().free(handleA);
                device.waitForSubmissions();

                const int waitsBefore = fenceWaits();
                const wallpaperx::DescriptorHandle handleB =
                    device.descriptorAllocator().allocate(b.view(), b.sampler());
                const int waits = fenceWaits() - waitsBefore;

                check(batchState(device.device(), batch) == Batch::Finished,
                      "descriptor_inflight_probe (heap reuse after completion): waitForSubmissions returned with the "
                      "batch still running, so the precondition this phase needs was never established");
                check(handleB.index == handleA.index,
                      "descriptor_inflight_probe (heap reuse after completion): a slot whose only reader has finished "
                      "was not reused, so the pool walks forward instead of choosing");
                check(waits == 0,
                      "descriptor_inflight_probe (heap reuse after completion): re-allocating blocked on a GPU that "
                      "was already idle");

                device.descriptorAllocator().free(handleB);
                outcome = Outcome::Pass;
            } catch (const std::exception& e) {
                check(false,
                      std::string("descriptor_inflight_probe (heap reuse after completion) could not run: ") + e.what());
                outcome = Outcome::Fail;
            }
            return wallpaperx::testing::failureCount() == failuresBefore;
        });
    return settle(kPhase, outcome, graded, failuresBefore);
}

// GOAL: a tier with exactly one place to put a descriptor blocks rather than overwriting the
//       bytes a submitted batch will read.
// WHY-THIS-SHAPE: the slot index cannot be the observable here -- the descriptor-buffer tier
//                 has one binding at one fixed offset, so the index is 0 whether the write
//                 was safe or not. The only difference a defect makes is that the call comes
//                 back without having waited, which is why the wait is counted from an
//                 interposed vkWaitForFences. "It took a long time" is rejected as the
//                 oracle: a slow allocate and a blocking one are the same number.
// CONTROL: renderer/vulkan/descriptor_slots.cpp -- release() stamping Device::SubmitTicket{}
//          instead of m_device.lastSubmission(). Observed red: no wait, batch still running.
Outcome bufferTierBlocksOnItsOnlySlot(uint32_t copies) {
    constexpr const char* kPhase = "buffer single-slot";
    setenv("WALLPAPERX_FORCE_TIER", "buffer", 1);

    Outcome outcome = Outcome::Fail;
    const int failuresBefore = wallpaperx::testing::failureCount();
    const bool graded = wallpaperx::testing::gradedRun(
        std::string("descriptor_inflight_probe (") + kPhase + ")",
        [&](wallpaperx::testing::ValidationGrade& grade) {
            try {
                wallpaperx::Device device(/*enableValidation=*/true);
                grade.watch(device);
                if (device.descriptorAllocator().tier() != wallpaperx::DescriptorTier::Buffer) {
                    // A driver without VK_EXT_descriptor_buffer, not a regression: the
                    // override falls back rather than failing (see tier_selection.cpp).
                    std::cout << "descriptor_inflight_probe (" << kPhase
                              << "): skipped, this driver has no VK_EXT_descriptor_buffer" << std::endl;
                    outcome = Outcome::Skipped;
                    return true;
                }

                wallpaperx::testing::TestResource a(device);
                wallpaperx::testing::TestResource b(device);
                BusyWork busy(device, kBusyBytes, copies);
                DrainOnExit drain(device);

                const wallpaperx::DescriptorHandle handleA =
                    device.descriptorAllocator().allocate(a.view(), a.sampler());
                const BatchRef batch = submitBoundBusyBatch(device, busy);
                device.descriptorAllocator().free(handleA);

                const int waitsBefore = fenceWaits();
                const Batch before = batchState(device.device(), batch);
                const wallpaperx::DescriptorHandle handleB =
                    device.descriptorAllocator().allocate(b.view(), b.sampler());
                const Batch after = batchState(device.device(), batch);
                const int waits = fenceWaits() - waitsBefore;

                switch (classifyWindow(before, after, waits)) {
                    case Window::AlreadyDone:
                        outcome = tooFast(kPhase, copies);
                        break;
                    case Window::DrainedInside:
                        outcome = Outcome::DriverDrained;
                        break;
                    case Window::Graded:
                        if (waits > 0) {
                            check(after == Batch::Finished,
                                  "descriptor_inflight_probe (buffer single-slot): the re-allocate waited, but the "
                                  "batch it should have waited for was still running afterwards");
                        } else {
                            check(false,
                                  "descriptor_inflight_probe (buffer single-slot): the descriptor at the tier's only "
                                  "offset was rewritten without waiting, while the batch that bound it was still "
                                  "running");
                        }
                        outcome = Outcome::Pass;
                        break;
                }

                device.descriptorAllocator().free(handleB);
            } catch (const std::exception& e) {
                check(false, std::string("descriptor_inflight_probe (buffer single-slot) could not run: ") + e.what());
                outcome = Outcome::Fail;
            }
            return wallpaperx::testing::failureCount() == failuresBefore;
        });
    return settle(kPhase, outcome, graded, failuresBefore);
}

// GOAL: on the classic tier the protected unit is the whole descriptor set, not the slot --
//       so a write to a binding no pending batch reads, and that has never been written
//       before, still waits for that batch.
// WHY-THIS-SHAPE: the uniform binding is deliberately the one written, and it is written for
//                 the FIRST time in this Device's life. That is what separates the two rules:
//                 its slot pool has nothing released and a free slot ready, so slot
//                 discipline alone would hand it over with no wait at all, and only a rule
//                 about the whole set can produce a wait here. The image handle is
//                 deliberately NOT freed, so the image pool cannot contribute one either.
//                 The observed drain is the oracle, NOT the validation layer: measured with
//                 the drain stubbed out, the layer reported zero warnings on both NVIDIA and
//                 Lavapipe, because it marks a set in use at draw time and the pending batch
//                 here only binds it. Grading 03047 instead would have been vacuous. The
//                 layer verdict is still folded in (gradedRun) as a second oracle for
//                 anything it does object to.
// COVERAGE-ASSERT: the phase is graded only when the driver's own fence puts the bound batch
//                  in a state that makes the write's decision observable; an attempt that
//                  cannot establish that is retried and finally reported as NOT VERIFIED --
//                  so it cannot quietly degrade into a check against an idle device, which
//                  every tier passes.
// CONTROL: renderer/vulkan/descriptor_classic.cpp -- waitUntilSetIdle() returning
//          immediately. Observed red.
Outcome classicTierProtectsTheWholeSet(uint32_t copies) {
    constexpr const char* kPhase = "classic whole-set";
    setenv("WALLPAPERX_FORCE_TIER", "classic", 1);

    Outcome outcome = Outcome::Fail;
    const int failuresBefore = wallpaperx::testing::failureCount();
    const bool graded = wallpaperx::testing::gradedRun(
        std::string("descriptor_inflight_probe (") + kPhase + ")",
        [&](wallpaperx::testing::ValidationGrade& grade) {
            try {
                wallpaperx::Device device(/*enableValidation=*/true);
                grade.watch(device);
                if (device.descriptorAllocator().tier() != wallpaperx::DescriptorTier::Classic) {
                    check(false, "descriptor_inflight_probe (classic whole-set): WALLPAPERX_FORCE_TIER=classic did not "
                                 "select the classic tier");
                    outcome = Outcome::Fail;
                    return false;
                }

                wallpaperx::testing::TestResource a(device);
                BusyWork busy(device, kBusyBytes, copies);
                ScopedBuffer uniform(device, 256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, "inflight-probe-uniform");
                DrainOnExit drain(device);

                const wallpaperx::DescriptorHandle imageHandle =
                    device.descriptorAllocator().allocate(a.view(), a.sampler());
                const BatchRef batch = submitBoundBusyBatch(device, busy);

                const int waitsBefore = fenceWaits();
                const Batch before = batchState(device.device(), batch);
                const wallpaperx::DescriptorHandle uniformHandle =
                    device.descriptorAllocator().allocateUniform(uniform.handle(), 256);
                const Batch after = batchState(device.device(), batch);
                const int waits = fenceWaits() - waitsBefore;

                switch (classifyWindow(before, after, waits)) {
                    case Window::AlreadyDone:
                        outcome = tooFast(kPhase, copies);
                        break;
                    case Window::DrainedInside:
                        outcome = Outcome::DriverDrained;
                        break;
                    case Window::Graded:
                        if (waits > 0) {
                            check(after == Batch::Finished,
                                  "descriptor_inflight_probe (classic whole-set): the write waited, but left the batch "
                                  "that bound the set still running");
                        } else {
                            check(false,
                                  "descriptor_inflight_probe (classic whole-set): a binding was written into a "
                                  "descriptor set a still-running batch has bound, with no drain -- the set, not the "
                                  "slot, is the unit the spec protects "
                                  "(VUID-vkUpdateDescriptorSets-None-03047)");
                        }
                        outcome = Outcome::Pass;
                        break;
                }

                device.descriptorAllocator().free(uniformHandle);
                device.descriptorAllocator().free(imageHandle);
            } catch (const std::exception& e) {
                check(false, std::string("descriptor_inflight_probe (classic whole-set) could not run: ") + e.what());
                outcome = Outcome::Fail;
            }
            return wallpaperx::testing::failureCount() == failuresBefore;
        });
    return settle(kPhase, outcome, graded, failuresBefore);
}

// How much copying each successive attempt submits. Escalating rather than fixed because
// "the batch was still running" is the precondition every phase needs, and no single figure
// holds across a discrete GPU with resizable BAR and a software rasteriser. It is also what
// separates the two ways a phase can fail to set up: work the GPU outran shrinks as a
// fraction of the window, while a driver that drains inside the descriptor write does not.
constexpr uint32_t kBusyCopies[] = {4, 24, 128};

Outcome runPhase(Outcome (*phase)(uint32_t)) {
    Outcome last = Outcome::Ambiguous;
    for (const uint32_t copies : kBusyCopies) {
        last = phase(copies);
        if (last != Outcome::Ambiguous && last != Outcome::DriverDrained) return last;
    }
    return last;
}

// A phase that did not reach a verdict marks the whole run NOT VERIFIED, which becomes a
// ctest skip. Deliberately not a pass, including for the driver-capability skips: an entry
// that could not ask its question must not read the same as one that asked and got the right
// answer.
void report(const char* name, Outcome outcome) {
    switch (outcome) {
        case Outcome::Pass:
            std::printf("descriptor_inflight_probe (%s): passed\n", name);
            return;
        case Outcome::Skipped:
            g_notVerified = true;  // the phase printed its own reason
            return;
        case Outcome::Ambiguous:
            std::printf("descriptor_inflight_probe (%s): NOT VERIFIED -- no attempt kept a batch running long enough "
                        "for the descriptor rewrite to be in flight\n",
                        name);
            g_notVerified = true;
            return;
        case Outcome::DriverDrained:
            std::printf("descriptor_inflight_probe (%s): NOT VERIFIED -- at every workload tried, the batch was "
                        "running when the descriptor write began and complete when it returned, with no fence wait "
                        "from the allocator. This driver completes outstanding work inside the descriptor write "
                        "itself, which leaves no in-flight state for this phase to observe (measured on Lavapipe).\n",
                        name);
            g_notVerified = true;
            return;
        case Outcome::Fail:
            return;  // check() already named it
    }
}

int usage() {
    std::fprintf(stderr, "usage: descriptor_inflight_probe heap|buffer|classic\n");
    return 2;
}

}  // namespace

// Counts blocking fence waits from outside the renderer. The point of interposing rather
// than reading Device::hasCompleted() is that a mutation aimed at the fix would poison that
// function -- and with it any oracle routed through it, which is how a probe for this defect
// comes back green.
extern "C" VkResult vkWaitForFences(VkDevice device, uint32_t fenceCount, const VkFence* pFences, VkBool32 waitAll,
                                    uint64_t timeout) {
    {
        std::lock_guard lock(g_vkMutex);
        ++g_fenceWaits;
    }
    using Fn = VkResult (*)(VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t);
    static const Fn real = realEntryPoint<Fn>(kProbe, "vkWaitForFences");
    return real(device, fenceCount, pFences, waitAll, timeout);
}

extern "C" VkResult vkQueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits, VkFence fence) {
    {
        std::lock_guard lock(g_vkMutex);
        g_lastSubmitFence = fence;
    }
    using Fn = VkResult (*)(VkQueue, uint32_t, const VkSubmitInfo*, VkFence);
    static const Fn real = realEntryPoint<Fn>(kProbe, "vkQueueSubmit");
    return real(queue, submitCount, pSubmits, fence);
}

extern "C" VkResult vkResetFences(VkDevice device, uint32_t fenceCount, const VkFence* pFences) {
    {
        std::lock_guard lock(g_vkMutex);
        for (uint32_t i = 0; i < fenceCount; ++i) ++g_fenceResets[pFences[i]];
    }
    using Fn = VkResult (*)(VkDevice, uint32_t, const VkFence*);
    static const Fn real = realEntryPoint<Fn>(kProbe, "vkResetFences");
    return real(device, fenceCount, pFences);
}

int main(int argc, char** argv) {
    if (argc != 2) return usage();
    const std::string mode = argv[1];

    if (mode == "heap") {
        // One entry, both directions of the same rule: see heapReusesACompletedSlot.
        report("heap in-flight", runPhase(heapKeepsAnInFlightSlot));
        report("heap reuse after completion", runPhase(heapReusesACompletedSlot));
    } else if (mode == "buffer") {
        report("buffer single-slot", runPhase(bufferTierBlocksOnItsOnlySlot));
    } else if (mode == "classic") {
        report("classic whole-set", runPhase(classicTierProtectsTheWholeSet));
    } else {
        return usage();
    }

    if (wallpaperx::testing::reportFailures("descriptor_inflight_probe") > 0) return 1;
    // After the failure check, so a run that is both inconclusive and broken reports broken.
    if (g_notVerified) {
        std::printf("descriptor_inflight_probe (%s): NOT VERIFIED -- reported to ctest as a skip, not a pass\n",
                    mode.c_str());
        return kNotVerifiedExitCode;
    }
    std::printf("descriptor_inflight_probe (%s): passed\n", mode.c_str());
    return 0;
}
