// SPDX-License-Identifier: GPL-2.0-or-later

// Grades the descriptor a consumer waits on before it samples a published frame --
// exportFrameAcquireFd() in renderer/vulkan/frame_export.cpp -- and whether the capability flag
// that decides whether there is one is a promise the driver keeps.
//
// WHY THIS PROBE EXISTS, stated as the claims nothing else in the suite can see.
//
//   1. THE DESCRIPTOR IS ALREADY SIGNALLED WHEN IT IS HANDED OVER. That is what makes a binary
//      semaphore safe to send across a process boundary: the producer signals it, waits for that
//      batch, and only then takes the descriptor, so a producer that dies immediately afterwards
//      leaves nothing for the consumer to wait on forever. Delete the signalling submit and every
//      other check in the project stays green -- the daemon still answers GetFrameFd, the plugin
//      still imports the memory, and glWaitSemaphoreEXT is a DEVICE-side wait that blocks no
//      client call, so the wallpaper simply never appears with nothing reporting why.
//
//   2. EACH HAND-OVER CARRIES ITS OWN PAYLOAD. A binary semaphore is consumed by the first wait.
//      One payload shared between two hand-overs leaves the second waiter parked forever, and the
//      KDE plugin re-fetches the same generation on every scene change, so that is the normal
//      path rather than an edge case. Two descriptors that both satisfy a wait is the only
//      observation that distinguishes it.
//
// HOW THE WAITS ARE DONE, and why not through Device::submit(). Both claims are established by
// importing the descriptor back into a semaphore on this same device and submitting an empty
// batch that waits on it. Device::submit() waits on its fence with UINT64_MAX, so under either
// defect this probe would hang until ctest's timeout rather than report -- X.1's failure mode
// exactly. The submit and the fence wait are therefore done here with a bounded timeout, and a
// wait that expires is a FAILED check, never a fall-through.
//
// A wait that expires also leaves the queue holding a batch that can never complete, so the
// process cannot be torn down: destroying the device would block inside the driver. Such a run
// prints its summary and calls _Exit, which is the one place in this file where the harness does
// not unwind. Reporting first is what X.4 asks for; unwinding is not available.
//
// WHAT THIS PROBE CANNOT ESTABLISH. That the acquire PREVENTS a hazard -- that a consumer which
// waits sees a complete frame where one that does not would see a torn one. The producer still
// host-waits on a fence inside its own submit path, so by the time any frame is handed over the
// render has finished and there is no hazard to observe. That becomes reachable only when the
// daemon renders continuously; until then the ordering is graded as a declaration, the way
// ownership_transfer_probe grades the queue-family transfer.

#include "vulkan/device.h"
#include "vulkan/frame_export.h"
#include "vulkan/offscreen.h"
#include "renderers/static_renderer.h"

#include "support/check.h"
#include "support/test_image.h"
#include "support/validation_grade.h"

#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <vector>

namespace {

using wallpaperx::testing::check;
using wallpaperx::testing::fail;

// Not square and not a round number of pixels wide, for frame_export_probe's reason.
constexpr uint32_t kWidth = 68;
constexpr uint32_t kHeight = 36;
constexpr int32_t kMonitor = 0;

// ctest's own convention for "the host could not be put in the state this grades";
// SKIP_RETURN_CODE in renderer/CMakeLists.txt is what makes ctest act on it.
constexpr int kNotVerifiedExitCode = 77;
bool g_notVerified = false;

// Long enough that a loaded machine cannot expire it -- the batches waited on here are empty, so
// a correct run satisfies them in microseconds -- and short enough that a wedged run reports
// inside ctest's own timeout instead of being killed with no output.
constexpr uint64_t kWaitTimeoutNs = 5ull * 1000 * 1000 * 1000;

void notVerified(const char* goals, const std::string& reason) {
    std::printf("frame_acquire_probe: NOT VERIFIED -- %s could not be exercised: %s. This run establishes nothing "
                "about them, and is reported to ctest as a skip rather than as a pass.\n",
                goals, reason.c_str());
    std::fflush(stdout);
    g_notVerified = true;
}

// The device extensions the driver advertises, enumerated here rather than read back from
// DeviceInfo. Printed beside the capability flag so a run says which of the two answers this
// driver gives -- they differ on Lavapipe, which is the whole reason the flag is not the list.
bool advertisesExternalSemaphoreFd(VkPhysicalDevice physical) {
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr) != VK_SUCCESS) return false;
    std::vector<VkExtensionProperties> extensions(count);
    if (vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, extensions.data()) != VK_SUCCESS) return false;
    for (const VkExtensionProperties& extension : extensions) {
        if (std::string(extension.extensionName) == VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME) return true;
    }
    return false;
}

// What one attempt to consume an exported acquire ended in. TimedOut is kept apart from the other
// failures because it alone leaves the queue unusable.
enum class WaitOutcome { Satisfied, ImportRefused, TimedOut, SubmitFailed };

const char* describe(WaitOutcome outcome) {
    switch (outcome) {
        case WaitOutcome::Satisfied:     return "satisfied";
        case WaitOutcome::ImportRefused: return "the driver refused to import the descriptor";
        case WaitOutcome::TimedOut:      return "the wait never completed";
        case WaitOutcome::SubmitFailed:  return "the wait could not be submitted";
    }
    return "unknown";
}

// Imports `fd` as a binary semaphore on this same device and submits an empty batch that waits on
// it, then waits for that batch's fence with a bounded timeout.
//
// Takes ownership of `fd` on a successful import and must not close it afterwards: an OPAQUE_FD
// import with no TEMPORARY bit transfers the descriptor to the implementation.
WaitOutcome consumeAcquire(wallpaperx::Device& device, int fd) {
    auto pfnImport = reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(
        vkGetDeviceProcAddr(device.device(), "vkImportSemaphoreFdKHR"));
    if (pfnImport == nullptr) {
        ::close(fd);
        return WaitOutcome::ImportRefused;
    }

    VkSemaphore semaphore = VK_NULL_HANDLE;
    VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (vkCreateSemaphore(device.device(), &semaphoreInfo, nullptr, &semaphore) != VK_SUCCESS) {
        ::close(fd);
        return WaitOutcome::ImportRefused;
    }

    VkImportSemaphoreFdInfoKHR importInfo{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
    importInfo.semaphore = semaphore;
    importInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    importInfo.fd = fd;
    if (pfnImport(device.device(), &importInfo) != VK_SUCCESS) {
        // The descriptor is still the caller's after a failed import -- the same contract the
        // frame importer follows.
        ::close(fd);
        vkDestroySemaphore(device.device(), semaphore, nullptr);
        return WaitOutcome::ImportRefused;
    }

    VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.commandPool = device.commandPool();
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkAllocateCommandBuffers(device.device(), &allocInfo, &cmd) != VK_SUCCESS ||
        vkCreateFence(device.device(), &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
        if (fence != VK_NULL_HANDLE) vkDestroyFence(device.device(), fence, nullptr);
        if (cmd != VK_NULL_HANDLE) vkFreeCommandBuffers(device.device(), device.commandPool(), 1, &cmd);
        vkDestroySemaphore(device.device(), semaphore, nullptr);
        return WaitOutcome::SubmitFailed;
    }

    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);
    vkEndCommandBuffer(cmd);

    const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &semaphore;
    submitInfo.pWaitDstStageMask = &stage;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    if (vkQueueSubmit(device.graphicsQueue(), 1, &submitInfo, fence) != VK_SUCCESS) {
        vkDestroyFence(device.device(), fence, nullptr);
        vkFreeCommandBuffers(device.device(), device.commandPool(), 1, &cmd);
        vkDestroySemaphore(device.device(), semaphore, nullptr);
        return WaitOutcome::SubmitFailed;
    }

    if (vkWaitForFences(device.device(), 1, &fence, VK_TRUE, kWaitTimeoutNs) != VK_SUCCESS) {
        // Nothing is destroyed: the batch is still queued behind a wait that will never be
        // satisfied, and reclaiming what it references is undefined. The caller ends the process.
        return WaitOutcome::TimedOut;
    }
    vkDestroyFence(device.device(), fence, nullptr);
    vkFreeCommandBuffers(device.device(), device.commandPool(), 1, &cmd);
    vkDestroySemaphore(device.device(), semaphore, nullptr);
    return WaitOutcome::Satisfied;
}

// Prints the summary and ends the process without unwinding. See the file header.
[[noreturn]] void abandonWedged(const char* what) {
    std::fprintf(stderr,
                 "  %s, so this process holds a queue batch that can never complete and cannot be torn down; "
                 "exiting without unwinding\n",
                 what);
    wallpaperx::testing::reportFailures("frame_acquire_probe");
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(1);
}

bool run(wallpaperx::testing::ValidationGrade& grade) {
    const std::string imagePath = wallpaperx::testing::writeGradientPng(
        std::string(WALLPAPERX_TEST_IMAGE_DIR) + "/frame_acquire_gradient.png", 16, 16);

    wallpaperx::Device device(/*enableValidation=*/true);
    grade.watch(device);

    wallpaperx::OffscreenTargetManager targets(device);
    wallpaperx::StaticRenderer renderer(device, targets.renderPass(), imagePath, WALLPAPERX_SHADER_DIR);
    const wallpaperx::OffscreenTarget& target = targets.create(kMonitor, kWidth, kHeight);
    targets.render(renderer, target);

    const bool advertised = advertisesExternalSemaphoreFd(device.physicalDevice());
    const bool exportable = device.info().hasExternalSemaphoreFd;
    std::printf("frame_acquire_probe: %s -- VK_KHR_external_semaphore_fd %s, opaque-fd export reported %s\n",
                device.info().gpuName.c_str(), advertised ? "advertised" : "absent",
                exportable ? "available" : "unavailable");
    std::fflush(stdout);

    if (!exportable) {
        // GOAL (S5.25): a device that cannot export an acquire hands back no descriptor rather
        //       than failing the export. False if a consumer on such a driver gets no frame at
        //       all -- a black desktop where the previous, unsynchronised behaviour drew a
        //       wallpaper.
        // WHY-THIS-SHAPE: the throw is caught and its absence asserted, not allowed to escape. An
        //       escaping exception is caught in main() and reported as a host limitation, which
        //       would turn this defect into a ctest skip instead of a failure.
        // CONTROL: the `if (!device.info().hasExternalSemaphoreFd) return -1;` early return
        //       deleted from exportFrameAcquireFd (renderer/vulkan/frame_export.cpp) -> "FAIL: a
        //       device that cannot export an acquire semaphore returns no descriptor instead of
        //       failing", printing "threw ... vkGetSemaphoreFdKHR entry point missing", on the
        //       lavapipe entry. Observed 2026-08-31. Reverted.
        int none = -2;
        bool threw = false;
        try {
            none = wallpaperx::exportFrameAcquireFd(device, target);
        } catch (const std::exception& e) {
            threw = true;
            std::fprintf(stderr, "  exportFrameAcquireFd threw on a device that cannot export: %s\n", e.what());
        }
        check(!threw && none == -1,
              "a device that cannot export an acquire semaphore returns no descriptor instead of failing");
        if (none >= 0) ::close(none);
        notVerified("S5.22, S5.23", "this driver cannot export an opaque-fd binary semaphore, so no acquire "
                                    "descriptor exists to be waited on");
        return wallpaperx::testing::failureCount() == 0;
    }

    // GOAL (S5.24): a device that REPORTS it can export an acquire actually can. The capability
    //       flag must mean "an exportable opaque-fd binary semaphore can be created", not "the
    //       extension is in the list" -- Lavapipe advertises the extension and supports only
    //       SYNC_FD, which GL_EXT_semaphore_fd cannot import anyway.
    // WHY-THIS-SHAPE: the export attempt itself is the oracle, and its failure is caught here and
    //       graded rather than left to escape. Comparing the flag against
    //       vkGetPhysicalDeviceExternalSemaphoreProperties -- the obvious form -- restates
    //       Device::selectPhysicalDevice's own computation on both sides and would hold for any
    //       implementation of it, the substitution included.
    //       Both descriptors are taken before either is consumed, deliberately. Export, wait,
    //       then export again would let a producer handing out one shared payload pass: the
    //       second export happens after the first wait consumed the signal, and a driver is free
    //       to have re-signalled in between.
    // CONTROL: `m_info.hasExternalSemaphoreFd` in Device::selectPhysicalDevice
    //       (renderer/vulkan/device.cpp) set from `hasExtension(extensions,
    //       VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME)` -- the shape this replaced -> "FAIL: a
    //       device reporting it can export an acquire semaphore produces one for every hand-over,
    //       without failing", printing "threw: ... vkCreateSemaphore failed with
    //       VK_ERROR_INVALID_EXTERNAL_HANDLE", on the lavapipe entry, with the validation grade
    //       red beside it. Observed 2026-08-31. Reverted.
    int first = -1;
    int second = -1;
    std::string exportError;
    try {
        first = wallpaperx::exportFrameAcquireFd(device, target);
        second = wallpaperx::exportFrameAcquireFd(device, target);
    } catch (const std::exception& e) {
        exportError = e.what();
    }
    const bool produced = exportError.empty() && first >= 0 && second >= 0;
    check(produced, "a device reporting it can export an acquire semaphore produces one for every hand-over, "
                    "without failing");
    if (!produced) {
        std::fprintf(stderr, "  exportFrameAcquireFd returned %d and %d%s%s\n", first, second,
                     exportError.empty() ? "" : " and threw: ", exportError.c_str());
        if (first >= 0) ::close(first);
        if (second >= 0) ::close(second);
        return false;
    }

    // GOAL (S5.22): the descriptor refers to a signal that has ALREADY happened. A consumer that
    //       waits on it makes progress with no further work from the producer, which is what
    //       makes a binary semaphore safe to send across a process boundary at all.
    // WHY-THIS-SHAPE: the wait is submitted here with an explicit fence timeout instead of
    //       through Device::submit(), which waits UINT64_MAX -- under the defect this grades that
    //       is a hang until ctest's timeout rather than a failing check (X.1). An expired wait is
    //       counted as a failure and the process then ends without unwinding, because the queue
    //       is holding a batch that can never complete.
    // CONTROL: the `device.submit(...)` line that signals the semaphore deleted from
    //       exportFrameAcquireFd (renderer/vulkan/frame_export.cpp), so the descriptor is
    //       exported from a semaphore nothing ever signalled -> "FAIL: the acquire descriptor is
    //       already signalled when it is handed over" after the 5 s timeout, followed by the
    //       exit-without-unwinding line. Observed 2026-08-31. Reverted.
    const WaitOutcome firstOutcome = consumeAcquire(device, first);
    if (firstOutcome == WaitOutcome::TimedOut) {
        fail("the acquire descriptor is already signalled when it is handed over");
        abandonWedged("the first hand-over's acquire was never signalled");
    }
    check(firstOutcome == WaitOutcome::Satisfied, "the acquire descriptor is already signalled when it is handed over");
    if (firstOutcome != WaitOutcome::Satisfied) {
        std::fprintf(stderr, "  waiting on the first hand-over's acquire: %s\n", describe(firstOutcome));
    }

    // GOAL (S5.23): each hand-over carries its own payload. A binary semaphore is consumed by the
    //       first wait, so two hand-overs sharing one payload park the second consumer forever --
    //       and the KDE plugin re-fetches the same generation on every scene change, so that is
    //       the ordinary path.
    // WHY-THIS-SHAPE: the second descriptor is waited on AFTER the first has been consumed, which
    //       is the only ordering that tells the two apart: with a fresh payload each wait is
    //       satisfied, with a shared one the first consumes the single signal and this one never
    //       completes. Comparing the two descriptor numbers instead cannot fail -- two exports of
    //       one semaphore return two different file descriptors referring to the same payload.
    // CONTROL: the semaphore in exportFrameAcquireFd (renderer/vulkan/frame_export.cpp) made a
    //       function-local static, created and signalled on the first call only and exported on
    //       every call -- the shape a caching "optimisation" would take -> "FAIL: a second
    //       hand-over of the same frame carries its own payload, not one already consumed", with
    //       the already-signalled check above it still GREEN, which is the two of them
    //       discriminating. Observed 2026-08-31. Reverted.
    const WaitOutcome secondOutcome = consumeAcquire(device, second);
    if (secondOutcome == WaitOutcome::TimedOut) {
        fail("a second hand-over of the same frame carries its own payload, not one already consumed");
        abandonWedged("the second hand-over's acquire was already consumed by the first");
    }
    check(secondOutcome == WaitOutcome::Satisfied,
          "a second hand-over of the same frame carries its own payload, not one already consumed");
    if (secondOutcome != WaitOutcome::Satisfied) {
        std::fprintf(stderr, "  waiting on the second hand-over's acquire: %s\n", describe(secondOutcome));
    }

    // GOAL (S5.25, second half): asking for an acquire on a target with no published frame is a
    //       caller bug and is reported as one, rather than exporting a semaphore that orders
    //       nothing against a frame that was never handed over.
    // WHY-THIS-SHAPE: the throw is caught and its presence asserted rather than allowed to
    //       escape; an escaping exception would take the summary with it, which is X.4's failure
    //       mode and not a result.
    // CONTROL: the `if (!target.published()) throw` removed from exportFrameAcquireFd
    //       (renderer/vulkan/frame_export.cpp) -> "FAIL: exporting an acquire for a target with
    //       no published frame is refused", alone. Observed 2026-08-31. Reverted.
    targets.reclaim(target);
    bool refused = false;
    int strayFd = -1;
    try {
        strayFd = wallpaperx::exportFrameAcquireFd(device, target);
    } catch (const std::exception&) {
        refused = true;
    }
    if (strayFd >= 0) ::close(strayFd);
    check(refused, "exporting an acquire for a target with no published frame is refused");

    return wallpaperx::testing::failureCount() == 0;
}

}  // namespace

int main() {
    const bool ok = wallpaperx::testing::gradedRun("frame_acquire_probe", [](auto& grade) {
        try {
            return run(grade);
        } catch (const std::exception& e) {
            // Reported, never thrown out of main: a driver that cannot render or export a frame
            // at all is a host limitation, and blaming the exporter for it is the attribution
            // error X.3 forbids. Every failure this probe is FOR is caught at its own call site
            // above, so nothing that reaches here is a verdict on the acquire path.
            std::fprintf(stderr, "frame_acquire_probe: could not complete the render-and-export cycle: %s\n", e.what());
            notVerified("S5.22, S5.23, S5.24, S5.25",
                        "the render-and-export cycle this probe rests on did not complete on this host");
            return wallpaperx::testing::failureCount() == 0;
        }
    });

    if (!ok) {
        wallpaperx::testing::reportFailures("frame_acquire_probe");
        return 1;
    }
    // After the failure check, so a run that is both inconclusive and broken reports as broken.
    if (g_notVerified) {
        std::printf("frame_acquire_probe: reported to ctest as a skip, not a pass\n");
        return kNotVerifiedExitCode;
    }
    std::printf("frame_acquire_probe: passed, zero validation warnings\n");
    return 0;
}
