// SPDX-License-Identifier: GPL-2.0-or-later

// RenderWorker's stop for a lost device (S14.5), in process.
//
// The worker is where the daemon learns its device is gone: it asks the manager after every job
// and every clock round, tells its owner once, and then does no more GPU work. The daemon system
// test sees the result of that from outside -- an exit status -- and cannot tell a worker that
// stopped from one that kept submitting to a dead device while main() was on its way out. This
// file can: it counts the submissions the daemon's code makes, through an interposed
// vkQueueSubmit that answers VK_ERROR_DEVICE_LOST once armed.

#include "daemon/render_worker.h"
#include "daemon/wallpaper_manager.h"

#include "support/real_entry_point.h"
#include "support/test_image.h"
#include "tests/client/daemon_client.h"
#include "tests/client/scratch_dir.h"
#include "tests/test_report.h"

#include <vulkan/vulkan.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using wallpaperx::RenderWorker;
using wallpaperx::WallpaperManager;
using wallpaperx::testing::check;
using wallpaperx::testing::FrameReadyLog;
using wallpaperx::testing::notVerified;
using TaskState = RenderWorker::TaskState;

namespace {

// Generous: the first render builds the device, which on a software rasteriser under load is
// seconds. Every wait has a failure branch; none of them is ever the success path.
constexpr auto kWait = std::chrono::seconds(30);
constexpr uint32_t kSize = 64;

std::atomic<bool> g_lose{false};
std::atomic<int> g_submits{0};

// A task's answer, and whether it came before postTask() returned on the posting thread.
struct Answer {
    std::promise<TaskState> promise;
    std::future<TaskState> future = promise.get_future();
    std::atomic<bool> answered{false};
    std::thread::id thread;

    RenderWorker::Task task() {
        return [this](TaskState state) {
            thread = std::this_thread::get_id();
            answered.store(true);
            promise.set_value(state);
        };
    }
    // The answer, once it has come; nothing when it has not come within the bound.
    std::optional<TaskState> await() {
        if (future.wait_for(kWait) != std::future_status::ready) return std::nullopt;
        return future.get();
    }
};

const char* nameOf(std::optional<TaskState> state) {
    if (!state) return "no answer";
    switch (*state) {
        case TaskState::Run:
            return "Run";
        case TaskState::ShuttingDown:
            return "ShuttingDown";
        case TaskState::Busy:
            return "Busy";
    }
    return "?";
}

WallpaperManager::Config smallConfig() {
    WallpaperManager::Config config;
    config.width = kSize;
    config.height = kSize;
    return config;
}

// Renders one still on a healthy device, so the device exists before it is lost: a device that
// was never created is not lost (S14.4), and the worker must not stop for it.
bool firstFrame(WallpaperManager& manager, RenderWorker& worker, FrameReadyLog& frames, const std::string& image,
                const char* goals) {
    manager.requestWallpaper("static", image, 0);
    worker.post(0);
    // Any generation: this is the first render the worker ever runs, so nothing can satisfy the
    // wait early.
    if (!frames.waitAtLeast(0, 1)) {
        notVerified(goals, "the first still never rendered on this host, so there was no device to lose");
        return false;
    }
    return true;
}

void withCallback(const std::string& blue, const std::string& red) {
    constexpr const char* kGoals = "S14.5 with a device-lost callback";
    g_lose.store(false);

    WallpaperManager manager(smallConfig());
    FrameReadyLog frames;
    std::atomic<int> calls{0};
    std::atomic<int> submitsAtCall{-1};
    std::promise<void> called;
    std::future<void> calledFuture = called.get_future();
    // Ahead of the worker: a task left queued is answered by the worker's destructor, so what it
    // writes to must still exist then.
    Answer healthy;
    Answer after;
    Answer later;
    RenderWorker worker(
        manager, [&frames](int monitorId, uint64_t generation) { frames.record(monitorId, generation); },
        [&] {
            if (calls.fetch_add(1) == 0) {
                submitsAtCall.store(g_submits.load());
                called.set_value();
            }
        });

    if (!firstFrame(manager, worker, frames, blue, kGoals)) return;

    // GOAL: S14.5 -- a healthy device does not stop the worker. False if the check between jobs
    //       reports loss for a device that works, which would restart the daemon after its
    //       first frame.
    // WHY-THIS-SHAPE: read after a task has been answered from the worker, which is after the
    //       check that follows the render has run; read straight after FrameReady it would race
    //       that check and pass for a worker that was about to stop.
    // CONTROL: WallpaperManager::deviceLost() (backend/daemon/wallpaper_manager.cpp) made to
    //       return `m_device != nullptr` -> "FAIL: a task after a frame on a healthy device runs"
    //       and "FAIL: the device-lost callback is not called for a healthy device". Observed
    //       2026-09-24 on NVIDIA and Lavapipe. Reverted.
    worker.postTask(healthy.task());
    const std::optional<TaskState> healthyState = healthy.await();
    check(healthyState == TaskState::Run, "a task after a frame on a healthy device runs");
    check(calls.load() == 0, "the device-lost callback is not called for a healthy device");

    g_lose.store(true);
    manager.requestWallpaper("static", red, 1);
    worker.post(1);
    if (calledFuture.wait_for(kWait) != std::future_status::ready) {
        check(false, "a render that loses the device makes the worker call its device-lost callback");
        return;
    }

    // GOAL: S14.5 -- once the owner has been told, the worker refuses new work the way a stopping
    //       worker does, answering a task ShuttingDown on the posting thread before postTask()
    //       returns, and starts no GPU work. False if a request after the loss is queued and run
    //       on the dead device, or answered only when the process tears the worker down.
    // WHY-THIS-SHAPE: the answer is read immediately after postTask(), because the claim is that
    //       it is answered inline; a wait would also pass for a worker that answered it much
    //       later from its own thread. The second task is posted after a render and a wake, and
    //       the worker is not stopped until that task has been answered: a worker still serving
    //       runs the render first (one FIFO), so its submission is counted before the join,
    //       rather than racing stop() for it.
    // CONTROL: in backend/daemon/render_worker.cpp, stopForLostDevice()'s `m_stop = true` block
    //       deleted, so the worker returns without refusing -> "FAIL: a task posted after the
    //       loss is answered ShuttingDown on the posting thread before postTask returns" and "FAIL:
    //       a task posted after a later render request is refused too" (each after its 30 s
    //       bound, as nothing answers them until stop()). Observed 2026-09-24. Reverted.
    worker.postTask(after.task());
    const bool inline1 = after.answered.load();
    const std::optional<TaskState> afterState = after.await();
    check(inline1 && afterState == TaskState::ShuttingDown && after.thread == std::this_thread::get_id(),
          "a task posted after the loss is answered ShuttingDown on the posting thread before postTask returns");
    if (!(inline1 && afterState == TaskState::ShuttingDown)) {
        std::fprintf(stderr, "  answered %s, %s\n", nameOf(afterState),
                     inline1 ? "inline" : "not before postTask returned");
    }

    manager.requestWallpaper("static", blue, 2);
    worker.post(2);
    worker.wake();
    worker.postTask(later.task());
    const std::optional<TaskState> laterState = later.await();
    check(laterState == TaskState::ShuttingDown, "a task posted after a later render request is refused too");
    if (laterState != TaskState::ShuttingDown) std::fprintf(stderr, "  answered %s\n", nameOf(laterState));

    worker.stop();

    // GOAL: S14.5 -- the callback is called exactly once, and no submission is made after it.
    //       False if a worker that has reported the loss goes on rendering: work started on a
    //       lost device is what a hung driver may hold when the stop deadline kills the process,
    //       and a render killed that way is blamed on its wallpaper.
    // WHY-THIS-SHAPE: submissions are counted at the driver boundary, not inferred from
    //       FrameReady: a render on a lost device fails, so it announces nothing whether or not
    //       it ran. Read after stop() has joined the worker, so nothing can still be in flight.
    // CONTROL: two in backend/daemon/render_worker.cpp, both observed 2026-09-24 on NVIDIA and
    //       Lavapipe, both reverted. (a) run()'s `if (stopForLostDevice()) return;` after runJob()
    //       made to call it and carry on -> "FAIL: the device-lost callback is called exactly
    //       once", and only that: the check after the clock round calls it a second time.
    //       (b) stopForLostDevice() made to call the callback and return false without setting
    //       m_stop, a worker that reports and serves on -> "FAIL: the worker submits nothing after
    //       reporting the loss", with the callback count and both refusal lines red beside it.
    check(calls.load() == 1, "the device-lost callback is called exactly once");
    if (calls.load() != 1) std::fprintf(stderr, "  it was called %d times\n", calls.load());
    check(g_submits.load() == submitsAtCall.load(), "the worker submits nothing after reporting the loss");
    if (g_submits.load() != submitsAtCall.load()) {
        std::fprintf(stderr, "  %d submission(s) after the callback\n", g_submits.load() - submitsAtCall.load());
    }
    std::printf("device_loss_worker_test: with a callback, the loss was reported after %d submission(s) and none "
                "followed\n",
                submitsAtCall.load());
}

// GOAL: S14.5 -- without a callback, a worker whose device is lost serves on, as it did before
//       the callback existed: tasks run, renders are attempted (and fail). False if the loss now
//       stops every worker, which would turn the in-process DEVICE_LOST tests into tests of a
//       stopped worker and change what the daemon's DBus handlers answer without one.
// WHY-THIS-SHAPE: the render after the loss is proven attempted by the submission count, and the
//       task behind it by its Run answer; FrameReady cannot show either, since a render on a
//       lost device announces nothing.
// CONTROL: in backend/daemon/render_worker.cpp, stopForLostDevice() made to stop whenever the
//       device is lost and call the callback only if there is one -> "FAIL: without a callback,
//       a task after the loss still runs" and "FAIL: without a callback, the worker is still
//       serving after two lost renders". Observed 2026-09-24 on NVIDIA and Lavapipe. Reverted.
void withoutCallback(const std::string& blue, const std::string& red) {
    constexpr const char* kGoals = "S14.5 without a device-lost callback";
    g_lose.store(false);

    WallpaperManager manager(smallConfig());
    FrameReadyLog frames;
    Answer first;
    Answer second;
    RenderWorker worker(manager,
                        [&frames](int monitorId, uint64_t generation) { frames.record(monitorId, generation); });
    if (!firstFrame(manager, worker, frames, blue, kGoals)) return;

    g_lose.store(true);
    const int before = g_submits.load();
    manager.requestWallpaper("static", red, 1);
    worker.post(1);
    worker.postTask(first.task());
    const std::optional<TaskState> firstState = first.await();
    check(firstState == TaskState::Run, "without a callback, a task after the loss still runs");
    if (firstState != TaskState::Run) std::fprintf(stderr, "  answered %s\n", nameOf(firstState));
    check(g_submits.load() > before, "without a callback, the render after the loss is still attempted");

    manager.requestWallpaper("static", blue, 2);
    worker.post(2);
    worker.postTask(second.task());
    check(second.await() == TaskState::Run, "without a callback, the worker is still serving after two lost renders");
    worker.stop();
}

}  // namespace

// The daemon's own submissions; ffmpeg's go through a table it fetched itself, and no video
// plays here.
// CONTROL: by hand 2026-09-24, reverted: the lookup made "vkQueueSubmitX" -> "device_loss_worker_test:
//       cannot resolve the real vkQueueSubmitX" and an abort, where a failed lookup used to be
//       answered as VK_ERROR_INITIALIZATION_FAILED and graded as a host that cannot render.
extern "C" VkResult vkQueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* infos, VkFence fence) {
    static const auto real =
        wallpaperx::testing::realEntryPoint<PFN_vkQueueSubmit>("device_loss_worker_test", "vkQueueSubmit");
    g_submits.fetch_add(1);
    if (g_lose.load()) return VK_ERROR_DEVICE_LOST;
    return real(queue, count, infos, fence);
}

int main() {
    wallpaperx::testing::g_testName = "device_loss_worker_test";
    const auto finish = [] {
        if (wallpaperx::testing::reportCheckFailures()) return 1;
        if (wallpaperx::testing::reportHarnessFaults("in this test file, not in the daemon")) return 1;
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    };

    wallpaperx::testing::ScratchDir scratch("deviceloss");
    if (!scratch.ok()) {
        notVerified("S14.5", "no scratch directory could be created for the fixture images");
        return finish();
    }
    std::string blue, red;
    try {
        blue = wallpaperx::testing::writeSolidPng((scratch.path() / "blue.png").string(), 16, 16, {0, 0, 255, 255});
        red = wallpaperx::testing::writeSolidPng((scratch.path() / "red.png").string(), 16, 16, {255, 0, 0, 255});
    } catch (const std::exception& e) {
        wallpaperx::testing::harnessFailure(std::string("the fixture images could not be written: ") + e.what());
        return finish();
    }

    try {
        withCallback(blue, red);
        withoutCallback(blue, red);
    } catch (const std::exception& e) {
        check(false, (std::string("unexpected exception: ") + e.what()).c_str());
    }
    return finish();
}
