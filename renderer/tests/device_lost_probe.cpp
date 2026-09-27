// SPDX-License-Identifier: GPL-2.0-or-later

// Device::lost() (S14.3): which queue and fence results latch it, and which must not.
//
// The daemon exits for a restart once lost() is true, so a result that fails to latch it leaves
// a blank daemon up for good, and one that latches it wrongly -- a timeout, an out-of-memory --
// restarts a daemon whose device still works. No driver loses a device on demand, so this
// interposes vkQueueSubmit, vkWaitForFences and vkGetFenceStatus and hands the Device a scripted
// result at exactly one call; every other call passes through to the driver. Each case builds a
// fresh Device, because the latch is permanent by design.

#include "vulkan/device.h"

#include "support/check.h"
#include "support/real_entry_point.h"

#include <vulkan/vulkan.h>

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

using wallpaperx::Device;
using wallpaperx::testing::check;
using wallpaperx::testing::realEntryPoint;

namespace {

constexpr int kNotVerifiedExitCode = 77;

// One script per interposed entry point: the results its next calls return instead of the
// driver's. Empty means pass through. Counts what was actually delivered, so a case can tell a
// result the Device never asked for from one it asked for and ignored.
struct Script {
    std::deque<VkResult> next;
    int delivered = 0;
};

std::mutex g_mutex;
Script g_submit;
Script g_wait;
Script g_status;

std::optional<VkResult> take(Script& script) {
    std::lock_guard lock(g_mutex);
    if (script.next.empty()) return std::nullopt;
    const VkResult r = script.next.front();
    script.next.pop_front();
    ++script.delivered;
    return r;
}

void arm(Script& script, std::initializer_list<VkResult> results) {
    std::lock_guard lock(g_mutex);
    script.next.assign(results);
    script.delivered = 0;
}

int delivered(const Script& script) {
    std::lock_guard lock(g_mutex);
    return script.delivered;
}

void disarmAll() {
    std::lock_guard lock(g_mutex);
    g_submit.next.clear();
    g_wait.next.clear();
    g_status.next.clear();
}

constexpr const char* kProbe = "device_lost_probe";

void emptyBatch(VkCommandBuffer) {}

// Everything a case needs, and the guarantee that no script outlives it: a result left armed
// would be handed to the Device's own teardown, or to the next case's constructor.
struct Case {
    std::unique_ptr<Device> device;
    ~Case() {
        disarmAll();
        device.reset();
    }
};

bool g_noDevice = false;

bool open(Case& c) {
    try {
        c.device = std::make_unique<Device>(false);
        return true;
    } catch (const std::exception& e) {
        std::printf("device_lost_probe: no device on this host (%s)\n", e.what());
        g_noDevice = true;
        return false;
    }
}

// Runs `call`, which is expected to throw what the scripted result makes it throw. A call that
// returns instead is itself a finding: the scripted result was swallowed somewhere.
template <class Call>
void expectThrow(const char* what, Call&& call) {
    try {
        call();
        check(false, std::string(what) + " throws for the scripted result");
    } catch (const std::exception&) {
    }
}

// GOAL: S14.3, the submit side -- a vkQueueSubmit answering VK_ERROR_DEVICE_LOST makes lost()
//       true, and a later submission that succeeds does not make it false again. False if either
//       half fails: the daemon would stay up blank, or forget a loss it has already seen.
// WHY-THIS-SHAPE: the baseline is taken after a real submission that succeeded, so a lost()
//       that is simply always true fails the first check instead of passing all three. The
//       "stays true" half submits for real after the loss, because only a later result passing
//       through the same latch can clear it.
// CONTROL: two, both in renderer/vulkan/device.cpp, both reverted, observed 2026-09-24 on NVIDIA
//       and Lavapipe. (a) observe() removed from the vkQueueSubmit call in Device::submit ->
//       "FAIL: a submit answering DEVICE_LOST makes lost() true" and, as a knock-on, the "stays
//       true" line. (b) observe() made to assign `m_lost = result == VK_ERROR_DEVICE_LOST` ->
//       "FAIL: lost() stays true after a later submission succeeds", and only that; (b) re-observed
//       2026-09-25 via control_run.sh on both entries.
void submitLoss() {
    Case c;
    if (!open(c)) return;
    Device& device = *c.device;
    device.immediateSubmit(emptyBatch);
    check(!device.lost(), "lost() is false on a device whose submissions succeed");

    arm(g_submit, {VK_ERROR_DEVICE_LOST});
    expectThrow("a submit answering DEVICE_LOST", [&] { (void)device.submit(emptyBatch); });
    check(delivered(g_submit) == 1, "the scripted DEVICE_LOST reached Device::submit's vkQueueSubmit");
    check(device.lost(), "a submit answering DEVICE_LOST makes lost() true");

    device.immediateSubmit(emptyBatch);
    check(device.lost(), "lost() stays true after a later submission succeeds");
}

// GOAL: S14.3 -- an out-of-memory result from vkQueueSubmit is a failed submission, not a lost
//       device. False if lost() turns true: the daemon would exit and restart for a shortage.
// CONTROL: observe() in renderer/vulkan/device.cpp made to latch on `result < 0` as well ->
//       "FAIL: a submit answering VK_ERROR_OUT_OF_HOST_MEMORY leaves lost() false", its
//       device-memory twin, and the two out-of-memory lines of the wait and retire cases below --
//       four, and no DEVICE_LOST check. Observed 2026-09-24; re-observed 2026-09-25 via
//       control_run.sh on both entries. Reverted.
void submitOutOfMemory(VkResult result, const char* name) {
    Case c;
    if (!open(c)) return;
    arm(g_submit, {result});
    expectThrow(name, [&] { (void)c.device->submit(emptyBatch); });
    check(delivered(g_submit) == 1, std::string("the scripted ") + name + " reached vkQueueSubmit");
    check(!c.device->lost(), std::string("a submit answering ") + name + " leaves lost() false");
}

// The fence-wait cases need a batch Device::wait still believes is running. Whether a real one
// has finished by the time retireCompleted() asks is a race, and a retired batch is never waited
// on at all. So the first vkGetFenceStatus answers NOT_READY.

// GOAL: S14.3, the wait side -- vkWaitForFences answering VK_ERROR_DEVICE_LOST in Device::wait
//       makes lost() true. False if a device lost while the worker waits on a render is never
//       noticed, which is where a real hang ends on NVIDIA (the driver reports loss after 5 s).
// WHY-THIS-SHAPE: the NOT_READY status keeps the batch pending; a batch that finished first is
//       retired and the scripted wait is never consumed, and lost() would then be false for a
//       reason that says nothing about the wait.
// CONTROL: observe() removed from the vkWaitForFences call in Device::waitFences
//       (renderer/vulkan/device.cpp) -> "FAIL: a fence wait answering DEVICE_LOST makes lost()
//       true". Observed 2026-09-24. Reverted.
// COVERAGE-ASSERT: `delivered == 1`. Controlled from the test side: the NOT_READY arm replaced by
//       a 200 ms sleep before the wait -> "FAIL: the scripted DEVICE_LOST reached
//       Device::waitFences's vkWaitForFences" on both drivers. Without the sleep the batch was
//       still pending on both, so the staging is insurance against a race, not a necessity today.
//       And from production: Device::wait in renderer/vulkan/device.cpp made to skip every
//       pending batch, a wait that never waits -> this line red with the throw and lost() lines,
//       and the other fence-wait cases' "reached" lines. Observed 2026-09-24 via control_run.sh;
//       reverted.
void waitLoss() {
    Case c;
    if (!open(c)) return;
    const Device::SubmitTicket ticket = c.device->submit(emptyBatch);
    arm(g_status, {VK_NOT_READY});
    arm(g_wait, {VK_ERROR_DEVICE_LOST});
    expectThrow("a fence wait answering DEVICE_LOST", [&] { c.device->wait(ticket); });
    check(delivered(g_wait) == 1, "the scripted DEVICE_LOST reached Device::waitFences's vkWaitForFences");
    check(c.device->lost(), "a fence wait answering DEVICE_LOST makes lost() true");
}

// GOAL: S14.3 -- a fence wait that times out is a stall, not a loss, and so is an out-of-memory
//       wait. False if lost() turns true for either: a slow or wedged queue would restart the
//       daemon whose device still works.
// CONTROL: observe() in renderer/vulkan/device.cpp made to latch on `result == VK_TIMEOUT` as
//       well -> "FAIL: a fence wait answering VK_TIMEOUT leaves lost() false", and only that.
//       Observed 2026-09-24; re-observed 2026-09-25 via control_run.sh on both entries. Reverted.
void waitNotLoss(VkResult result, const char* name) {
    Case c;
    if (!open(c)) return;
    const Device::SubmitTicket ticket = c.device->submit(emptyBatch);
    // Two NOT_READYs for a timeout: one for the retire before the wait, one for the per-batch
    // status the timeout branch asks, which is what keeps the batch marked as stalled. That mark
    // makes teardown leak the device, so the timeout cases print "leaked rather than destroyed".
    if (result == VK_TIMEOUT) {
        arm(g_status, {VK_NOT_READY, VK_NOT_READY});
    } else {
        arm(g_status, {VK_NOT_READY});
    }
    arm(g_wait, {result});
    expectThrow(name, [&] { c.device->wait(ticket); });
    check(delivered(g_wait) == 1, std::string("the scripted ") + name + " reached vkWaitForFences");
    check(!c.device->lost(), std::string("a fence wait answering ") + name + " leaves lost() false");
}

// GOAL: S14.3 -- a fence status of VK_ERROR_DEVICE_LOST read while classifying a timed-out wait
//       makes lost() true. False if the one status the timeout branch reads is not observed: a
//       driver that reports the loss there, and only there, would leave the daemon up and blank.
// CONTROL: observe() removed from the vkGetFenceStatus call in Device::waitFences's timeout
//       branch (renderer/vulkan/device.cpp) -> "FAIL: a fence status answering DEVICE_LOST after
//       a timed-out wait makes lost() true". Observed 2026-09-24. Reverted.
void timeoutThenLostStatus() {
    Case c;
    if (!open(c)) return;
    const Device::SubmitTicket ticket = c.device->submit(emptyBatch);
    arm(g_status, {VK_NOT_READY, VK_ERROR_DEVICE_LOST});
    arm(g_wait, {VK_TIMEOUT});
    expectThrow("a timed-out wait", [&] { c.device->wait(ticket); });
    check(delivered(g_status) == 2, "both scripted fence statuses were read, the second in the timeout branch");
    check(c.device->lost(), "a fence status answering DEVICE_LOST after a timed-out wait makes lost() true");
}

// GOAL: S14.3 -- vkGetFenceStatus answering VK_ERROR_DEVICE_LOST while retiring finished work
//       makes lost() true, and an out-of-memory status does not. False if a loss first seen by
//       a poll -- hasCompleted(), the start of every submit -- goes unnoticed.
// CONTROL: observe() removed from the vkGetFenceStatus call in Device::retireCompleted
//       (renderer/vulkan/device.cpp) -> "FAIL: a fence status answering DEVICE_LOST while
//       retiring makes lost() true". Observed 2026-09-24. Reverted.
void retireStatus(VkResult result, const char* name, bool expectLost) {
    Case c;
    if (!open(c)) return;
    const Device::SubmitTicket ticket = c.device->submit(emptyBatch);
    arm(g_status, {result});
    (void)c.device->hasCompleted(ticket);
    check(delivered(g_status) == 1, std::string("the scripted ") + name + " reached Device::retireCompleted");
    check(c.device->lost() == expectLost, std::string("a fence status answering ") + name + " while retiring " +
                                              (expectLost ? "makes lost() true" : "leaves lost() false"));
}

// GOAL: S14.3 -- the bounded wait that teardown and safeToFree() make observes a loss too. False
//       if a loss first reported there is missed; that wait runs whenever a still or a video is
//       replaced, so it can be the first to hear of one.
// CONTROL: observe() removed from the vkWaitForFences call in Device::drainForTeardown
//       (renderer/vulkan/device.cpp) -> "FAIL: the teardown wait answering DEVICE_LOST makes
//       lost() true". Observed 2026-09-24. Reverted.
void drainLoss() {
    Case c;
    if (!open(c)) return;
    (void)c.device->submit(emptyBatch);
    arm(g_status, {VK_NOT_READY});
    arm(g_wait, {VK_ERROR_DEVICE_LOST});
    const bool freeable = c.device->safeToFree("device_lost_probe's empty batch");
    check(delivered(g_wait) == 1, "the scripted DEVICE_LOST reached Device::drainForTeardown's vkWaitForFences");
    check(freeable, "a lost device's work is reported safe to free, as drainForTeardown documents");
    check(c.device->lost(), "the teardown wait answering DEVICE_LOST makes lost() true");
}

// GOAL: the Device's loss handler (its constructor's `onLost`) runs exactly once however many
//       calls answer VK_ERROR_DEVICE_LOST, on the thread that made the first of them and while
//       that call is still in progress, and never for an out-of-memory result. False if a second
//       loss runs it again, or it runs late (after the failing call has returned, where its caller
//       may already be logging to a stalled stream), or a shortage runs it: the daemon's handler
//       stops the process for a restart.
// WHY-THIS-SHAPE: two scripted losses, not one: with a single failing call a handler with no
//       once-only latch looks identical to a correct one. `inCall` is set around each call on this
//       thread, so "during the call" is read by the handler itself, not inferred from order.
// CONTROL: in Device::observe() (renderer/vulkan/device.cpp) via control_run.sh 2026-09-25 on
//       NVIDIA and Lavapipe, all reverted. (a) The once-only latch dropped (`|| m_lost` removed) ->
//       "run the loss handler exactly once" red, alone. (b) The handler call made `if ((false))` ->
//       the exactly-once, in-progress and thread lines red; no mutation of the shipped shape runs
//       the handler late, so those two are only shown able to fail together with the count.
//       (c) Latching on any negative result -> "an out-of-memory submit does not run the loss
//       handler" red, with the four out-of-memory lost() lines above.
void lossHandler() {
    int calls = 0;
    bool inCall = false;
    bool duringCall = false;
    std::thread::id where;
    Case c;
    try {
        c.device = std::make_unique<Device>(false, [&] {
            ++calls;
            duringCall = inCall;
            where = std::this_thread::get_id();
        });
    } catch (const std::exception& e) {
        std::printf("device_lost_probe: no device on this host (%s)\n", e.what());
        g_noDevice = true;
        return;
    }
    const auto failing = [&](const char* what) {
        inCall = true;
        expectThrow(what, [&] { (void)c.device->submit(emptyBatch); });
        inCall = false;
    };

    arm(g_submit, {VK_ERROR_OUT_OF_HOST_MEMORY});
    failing("a submit answering VK_ERROR_OUT_OF_HOST_MEMORY");
    check(calls == 0, "an out-of-memory submit does not run the loss handler");

    arm(g_submit, {VK_ERROR_DEVICE_LOST, VK_ERROR_DEVICE_LOST});
    failing("a submit answering DEVICE_LOST");
    failing("a second submit answering DEVICE_LOST");
    check(delivered(g_submit) == 2, "both scripted DEVICE_LOSTs reached vkQueueSubmit");
    check(calls == 1, "two submits answering DEVICE_LOST run the loss handler exactly once");
    check(calls > 0 && duringCall, "the loss handler runs while the failing call is still in progress");
    check(calls > 0 && where == std::this_thread::get_id(), "the loss handler runs on the thread that saw the loss");
    if (calls != 1) std::fprintf(stderr, "  the handler ran %d times\n", calls);
}

}  // namespace

extern "C" VkResult vkQueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits, VkFence fence) {
    using Fn = VkResult (*)(VkQueue, uint32_t, const VkSubmitInfo*, VkFence);
    static const Fn real = realEntryPoint<Fn>(kProbe, "vkQueueSubmit");
    if (const std::optional<VkResult> scripted = take(g_submit)) return *scripted;
    return real(queue, submitCount, pSubmits, fence);
}

extern "C" VkResult vkWaitForFences(VkDevice device, uint32_t fenceCount, const VkFence* pFences, VkBool32 waitAll,
                                    uint64_t timeout) {
    using Fn = VkResult (*)(VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t);
    static const Fn real = realEntryPoint<Fn>(kProbe, "vkWaitForFences");
    if (const std::optional<VkResult> scripted = take(g_wait)) return *scripted;
    return real(device, fenceCount, pFences, waitAll, timeout);
}

extern "C" VkResult vkGetFenceStatus(VkDevice device, VkFence fence) {
    using Fn = VkResult (*)(VkDevice, VkFence);
    static const Fn real = realEntryPoint<Fn>(kProbe, "vkGetFenceStatus");
    if (const std::optional<VkResult> scripted = take(g_status)) return *scripted;
    return real(device, fence);
}

int main() {
    try {
        submitLoss();
        if (g_noDevice) {
            std::printf("device_lost_probe: NOT VERIFIED -- this host has no Vulkan device; reported to ctest "
                        "as a skip, not a pass\n");
            return kNotVerifiedExitCode;
        }
        submitOutOfMemory(VK_ERROR_OUT_OF_HOST_MEMORY, "VK_ERROR_OUT_OF_HOST_MEMORY");
        submitOutOfMemory(VK_ERROR_OUT_OF_DEVICE_MEMORY, "VK_ERROR_OUT_OF_DEVICE_MEMORY");
        waitLoss();
        waitNotLoss(VK_TIMEOUT, "VK_TIMEOUT");
        waitNotLoss(VK_ERROR_OUT_OF_HOST_MEMORY, "VK_ERROR_OUT_OF_HOST_MEMORY");
        timeoutThenLostStatus();
        retireStatus(VK_ERROR_DEVICE_LOST, "DEVICE_LOST", true);
        retireStatus(VK_ERROR_OUT_OF_HOST_MEMORY, "VK_ERROR_OUT_OF_HOST_MEMORY", false);
        drainLoss();
        lossHandler();
    } catch (const std::exception& e) {
        wallpaperx::testing::fail(std::string("unexpected exception: ") + e.what());
    }
    if (g_noDevice) {
        wallpaperx::testing::fail("the device disappeared part way through the probe");
    }
    if (wallpaperx::testing::reportFailures("device_lost_probe") > 0) return 1;
    std::printf("device_lost_probe: passed\n");
    return 0;
}
