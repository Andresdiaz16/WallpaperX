// SPDX-License-Identifier: GPL-2.0-or-later

#include "vulkan/descriptor_heap.h"
#include "vulkan/device.h"
#include "vulkan/memory.h"

#include "support/descriptor_probe_support.h"
#include "support/validation_grade.h"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

// Both live in support/descriptor_probe_support.h, shared with descriptor_inflight_probe:
// what a missing Heap tier means is a policy, not a local detail, and a second copy of it
// is how the two probes stop agreeing.
using wallpaperx::testing::TestResource;

// ctest reads 77 as a skip (SKIP_RETURN_CODE on both entries in renderer/CMakeLists.txt). Four
// heap phases skipped for want of a heap tier must not exit 0 beside the tiers that did run: that
// is a phase nothing exercised reading as a graded one.
//
// CONTROL: heapTierGuard in support/descriptor_probe_support.h forced to report no heap tier, with
// WALLPAPERX_ALLOW_NO_HEAP_TIER=1 -> the four heap phases printed "skipped", then "NOT VERIFIED",
// exit 77 on both ICDs; with the g_notVerified return in main() deleted, the same run exited 0 with
// those phases silently ungraded. Both reverted.
constexpr int kNotVerifiedExitCode = 77;
bool g_notVerified = false;

std::optional<bool> heapTierGuard(wallpaperx::Device& device, const char* label) {
    std::optional<bool> result = wallpaperx::testing::heapTierGuard(device, "descriptor_allocator_probe", label);
    if (result && *result) g_notVerified = true;
    return result;
}

// GOAL: S2.8/S2.10 on the Classic and Buffer tiers (one image slot each): a second live allocate
//       throws, allocate after free succeeds with a valid handle, and a double free throws.
// CONTROL: each reverted, on descriptor_allocator_probe_lavapipe. renderer/vulkan/descriptor_slots.cpp
//       acquire() returning slot 0 instead of throwing when full -> "descriptor_allocator_probe (classic):
//       second allocate() should have thrown but silently succeeded", the same for (buffer), and "(heap
//       exhaustion): allocate() past kMaxDescriptorSlots should have thrown". releaseTo()'s Issued-state
//       test removed -> "(classic): second free() should have thrown", the same for (buffer) and (heap
//       double-free). renderer/vulkan/descriptor_classic.cpp allocate() returning an empty handle, with
//       free() accepting one -> "(classic): allocate() after free() should succeed".
bool testTierRejectsDoubleAllocate(const char* forceTier) {
    setenv("WALLPAPERX_FORCE_TIER", forceTier, 1);

    const bool ok = wallpaperx::testing::gradedRun(
        std::string("descriptor_allocator_probe (double-allocate, ") + forceTier + ")",
        [&](wallpaperx::testing::ValidationGrade& grade) {
    try {
        wallpaperx::Device device(/*enableValidation=*/true);
        grade.watch(device);
        TestResource a(device);
        TestResource b(device);

        wallpaperx::DescriptorHandle handleA = device.descriptorAllocator().allocate(a.view(), a.sampler());

        bool threw = false;
        try {
            device.descriptorAllocator().allocate(b.view(), b.sampler());
        } catch (const std::runtime_error&) {
            threw = true;
        }
        if (!threw) {
            std::cerr << "descriptor_allocator_probe (" << forceTier
                      << "): second allocate() should have thrown but silently succeeded" << std::endl;
            return false;
        }

        device.descriptorAllocator().free(handleA);
        wallpaperx::DescriptorHandle handleB = device.descriptorAllocator().allocate(b.view(), b.sampler());
        if (!handleB.valid()) {
            std::cerr << "descriptor_allocator_probe (" << forceTier
                      << "): allocate() after free() should succeed" << std::endl;
            return false;
        }

        device.descriptorAllocator().free(handleB);

        bool doubleFreeThrew = false;
        try {
            device.descriptorAllocator().free(handleB);  // double-free the same handle
        } catch (const std::runtime_error&) {
            doubleFreeThrew = true;
        }
        if (!doubleFreeThrew) {
            std::cerr << "descriptor_allocator_probe (" << forceTier << "): second free() should have thrown"
                      << std::endl;
            return false;
        }

    } catch (const std::exception& e) {
        std::cerr << "descriptor_allocator_probe (" << forceTier << ") failed: " << e.what() << std::endl;
        return false;
    }

    return true;
    });
    if (!ok) return false;
    std::cout << "descriptor_allocator_probe (" << forceTier << "): passed" << std::endl;
    return true;
}

bool testHeapTierUniformSlotsDoNotCollideWithImageSlots() {
    // No override needed -- Heap is already top priority and auto-selected
    // whenever supported (see heapTierGuard's comment above).
    unsetenv("WALLPAPERX_FORCE_TIER");

    bool skipped = false;
    const bool graded = wallpaperx::testing::gradedRun(
        "descriptor_allocator_probe (heap uniform)", [&](wallpaperx::testing::ValidationGrade& grade) {
    bool ok = true;
    try {
        wallpaperx::Device device(/*enableValidation=*/true);
        grade.watch(device);
        if (auto result = heapTierGuard(device, "heap uniform")) {
            skipped = true;
            return *result;
        }
        TestResource image1(device);

        wallpaperx::AllocatedBuffer uniformBuffer = device.memory().createBuffer(
            256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            wallpaperx::HostAccess::Write, "descriptor-allocator-probe-uniform");

        wallpaperx::DescriptorHandle imageHandle = device.descriptorAllocator().allocate(image1.view(), image1.sampler());
        wallpaperx::DescriptorHandle uniformHandle =
            device.descriptorAllocator().allocateUniform(uniformBuffer.buffer, 256);

        if (!imageHandle.valid() || !uniformHandle.valid()) {
            std::cerr << "descriptor_allocator_probe (heap uniform): expected both handles valid" << std::endl;
            ok = false;
        }
        // CONTROL: renderer/vulkan/descriptor_heap.cpp, each reverted, on descriptor_allocator_probe_lavapipe:
        //       allocateUniform returning the bare slot -> "(heap uniform): uniform index collides with image
        //       range"; allocate returning slot + kMaxDescriptorSlots -> "(heap uniform): image index out of
        //       expected range". "expected both handles valid" has no control of its own.
        // Image indices live in [0, kMaxDescriptorSlots); uniform indices must never
        // land in that range, or free() cannot tell the two allocations apart.
        if (imageHandle.index >= wallpaperx::kMaxDescriptorSlots) {
            std::cerr << "descriptor_allocator_probe (heap uniform): image index out of expected range"
                      << std::endl;
            ok = false;
        }
        if (uniformHandle.index < wallpaperx::kMaxDescriptorSlots) {
            std::cerr << "descriptor_allocator_probe (heap uniform): uniform index collides with image range"
                      << std::endl;
            ok = false;
        }

        device.descriptorAllocator().free(imageHandle);
        device.descriptorAllocator().free(uniformHandle);

        // Re-allocating each type should reuse its own free-list, not the other's.
        // Reusing image1's SAME view+sampler here is deliberate: free() no longer
        // unregisters them, so a register-once / reuse-across-cycles caller works.
        // This is the regression guard for the old bug where free() dropped the
        // registration and this allocate() would have thrown "never registered".
        // allocateUniform() has no registration step, so reusing uniformBuffer is fine.
        //
        // GOAL: a freed slot is the one handed back -- ON AN IDLE DEVICE. Since the pool
        //       started delaying reuse until the last batch that could read a slot has
        //       completed, "same index" is only the right answer while nothing is in flight;
        //       a version of this case that submitted work first would correctly get a
        //       different index, and reading this as an unconditional rule is how a later
        //       edit would "fix" the pool back into the defect.
        // WHY-THIS-SHAPE: the idleness is asserted rather than left to the reader. Nothing
        //                 above submits, so it holds today for a reason this case does not
        //                 state -- and a future setup step that submitted something would
        //                 quietly turn the index comparison into a race instead of failing.
        //                 The guard reads the renderer's own view of completion, which is
        //                 all this binary has; that is sound for a precondition, since it
        //                 cannot make the index comparison below pass when it otherwise
        //                 would not, and it is deliberately not the oracle for anything.
        // CONTROL: renderer/vulkan/descriptor_slots.cpp -- acquire()'s released-slot branch
        //          removed, so every allocate takes a fresh slot. Observed red here.
        // The in-flight half of the same rule lives in descriptor_inflight_probe, which
        // answers "did it wait?" from interposed driver calls instead.
        if (!device.hasCompleted(device.lastSubmission())) {
            std::cerr << "descriptor_allocator_probe (heap uniform): this case only means anything on an idle device, "
                         "and work was still in flight"
                      << std::endl;
            ok = false;
        }
        wallpaperx::DescriptorHandle imageHandle2 = device.descriptorAllocator().allocate(image1.view(), image1.sampler());
        wallpaperx::DescriptorHandle uniformHandle2 =
            device.descriptorAllocator().allocateUniform(uniformBuffer.buffer, 256);
        if (imageHandle2.index != imageHandle.index || uniformHandle2.index != uniformHandle.index) {
            std::cerr << "descriptor_allocator_probe (heap uniform): re-allocate did not reuse the freed slot on an "
                         "idle device"
                      << std::endl;
            ok = false;
        }

        device.descriptorAllocator().free(imageHandle2);
        device.descriptorAllocator().free(uniformHandle2);
        device.memory().destroyBuffer(uniformBuffer);
    } catch (const std::exception& e) {
        std::cerr << "descriptor_allocator_probe (heap uniform) failed: " << e.what() << std::endl;
        return false;
    }

    // The accumulator's only reader. It is an accumulator rather than four early returns
    // so one run reports every failed collision invariant instead of stopping at the
    // first -- which means deleting this line does not break the build, does not warn
    // (no -Wall here), and silently turns all four assertions into dead stores. That is
    // exactly what happened when the success print was hoisted out of this lambda.
    return ok;
    });
    if (!graded) return false;
    if (skipped) return true;
    std::cout << "descriptor_allocator_probe (heap uniform): passed" << std::endl;
    return true;
}

bool testHeapTierRejectsDoubleFree() {
    unsetenv("WALLPAPERX_FORCE_TIER");

    bool skipped = false;
    const bool ok = wallpaperx::testing::gradedRun(
        "descriptor_allocator_probe (heap double-free)", [&](wallpaperx::testing::ValidationGrade& grade) {
    try {
        wallpaperx::Device device(/*enableValidation=*/true);
        grade.watch(device);
        if (auto result = heapTierGuard(device, "heap double-free")) {
            skipped = true;
            return *result;
        }

        TestResource a(device);
        TestResource b(device);
        TestResource c(device);

        wallpaperx::DescriptorHandle handleA = device.descriptorAllocator().allocate(a.view(), a.sampler());
        device.descriptorAllocator().free(handleA);

        bool threw = false;
        try {
            device.descriptorAllocator().free(handleA);  // double-free the same handle
        } catch (const std::runtime_error&) {
            threw = true;
        }
        if (!threw) {
            std::cerr << "descriptor_allocator_probe (heap double-free): second free() should have thrown"
                      << std::endl;
            return false;
        }

        // The double-free must not have corrupted the free-list into handing the
        // same slot to two independent, simultaneously-live allocations.
        wallpaperx::DescriptorHandle handleB = device.descriptorAllocator().allocate(b.view(), b.sampler());
        wallpaperx::DescriptorHandle handleC = device.descriptorAllocator().allocate(c.view(), c.sampler());
        const bool aliased = handleB.index == handleC.index;

        // GOAL: S2.10 -- two simultaneously live allocations never share a slot.
        // WHY-THIS-SHAPE: graded before either handle is freed, and an aliased pair is freed once.
        //       Freeing both first made the line unreachable: under aliasing the second free() is a
        //       double free of the shared slot and throws, so the run went red on "failed: handle
        //       already freed" and never printed this line.
        // CONTROL: renderer/vulkan/descriptor_slots.cpp takeReleased, the `m_released.erase(...)`
        //       deleted so one released slot is handed out twice -> on descriptor_allocator_probe_lavapipe
        //       "descriptor_allocator_probe (heap double-free): two live resources got the same slot".
        //       Reverted.
        if (aliased) {
            std::cerr << "descriptor_allocator_probe (heap double-free): two live resources got the same slot"
                      << std::endl;
            device.descriptorAllocator().free(handleB);
            return false;
        }

        device.descriptorAllocator().free(handleB);
        device.descriptorAllocator().free(handleC);
    } catch (const std::exception& e) {
        std::cerr << "descriptor_allocator_probe (heap double-free) failed: " << e.what() << std::endl;
        return false;
    }

    return true;
    });
    if (!ok) return false;
    if (skipped) return true;
    std::cout << "descriptor_allocator_probe (heap double-free): passed" << std::endl;
    return true;
}

bool testHeapTierExhaustion() {
    unsetenv("WALLPAPERX_FORCE_TIER");

    bool skipped = false;
    const bool ok = wallpaperx::testing::gradedRun(
        "descriptor_allocator_probe (heap exhaustion)", [&](wallpaperx::testing::ValidationGrade& grade) {
    try {
        wallpaperx::Device device(/*enableValidation=*/true);
        grade.watch(device);
        if (auto result = heapTierGuard(device, "heap exhaustion")) {
            skipped = true;
            return *result;
        }

        // resources ends up with one MORE entry than handles: the
        // (kMaxDescriptorSlots + 1)th resource is created to attempt (and fail)
        // one allocation past capacity, so it never gets a handle. Both vectors
        // are driven off their own .size() below -- never zip them together by
        // index.
        std::vector<TestResource> resources;
        std::vector<wallpaperx::DescriptorHandle> handles;
        resources.reserve(wallpaperx::kMaxDescriptorSlots + 1);
        handles.reserve(wallpaperx::kMaxDescriptorSlots);

        for (uint32_t i = 0; i < wallpaperx::kMaxDescriptorSlots; ++i) {
            resources.push_back(TestResource(device));
            handles.push_back(device.descriptorAllocator().allocate(resources.back().view(), resources.back().sampler()));
        }

        // The (kMaxDescriptorSlots + 1)th allocation must fail cleanly, not
        // corrupt state or succeed silently past the documented capacity.
        resources.push_back(TestResource(device));
        bool threw = false;
        try {
            device.descriptorAllocator().allocate(resources.back().view(), resources.back().sampler());
        } catch (const std::runtime_error&) {
            threw = true;
        }

        for (size_t i = 0; i < handles.size(); ++i) {
            device.descriptorAllocator().free(handles[i]);
        }
        // resources clean themselves up (RAII) when the vector goes out of scope.

        if (!threw) {
            std::cerr << "descriptor_allocator_probe (heap exhaustion): allocate() past kMaxDescriptorSlots "
                         "should have thrown"
                      << std::endl;
            return false;
        }
    } catch (const std::exception& e) {
        std::cerr << "descriptor_allocator_probe (heap exhaustion) failed: " << e.what() << std::endl;
        return false;
    }

    return true;
    });
    if (!ok) return false;
    if (skipped) return true;
    std::cout << "descriptor_allocator_probe (heap exhaustion): passed" << std::endl;
    return true;
}

}

int main() {
    bool ok = true;
    ok &= testTierRejectsDoubleAllocate("classic");
    ok &= testTierRejectsDoubleAllocate("buffer");
    ok &= testHeapTierUniformSlotsDoNotCollideWithImageSlots();
    ok &= testHeapTierRejectsDoubleFree();
    ok &= testHeapTierExhaustion();
    // Failures first: a run that is both broken and inconclusive must report broken.
    if (!ok) return 1;
    if (g_notVerified) {
        std::cout << "descriptor_allocator_probe: NOT VERIFIED -- the heap phases were skipped, reported to ctest "
                     "as a skip, not a pass"
                  << std::endl;
        return kNotVerifiedExitCode;
    }
    return 0;
}
