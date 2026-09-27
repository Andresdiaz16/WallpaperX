// SPDX-License-Identifier: GPL-2.0-or-later

// RenderWorker's stop for a lost device (S14.5), in process.
//
// The Device tells the owner of a loss (WallpaperManager::Config::onDeviceLost) the moment a call
// answers VK_ERROR_DEVICE_LOST; the worker asks the manager after every job and every clock round
// and, once the device is lost in a daemon that stops for it, does no more GPU work. The daemon
// system test sees the result of that from outside -- an exit status -- and cannot tell a worker
// that stopped from one that kept submitting to a dead device while main() was on its way out.
// This file can: it counts the submissions the daemon's code makes, through an interposed
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
    // The answer, once it has come; nothing when it has not come within the bound. May be asked
    // again.
    std::optional<TaskState> await() {
        if (!got && future.wait_for(kWait) == std::future_status::ready) got = future.get();
        return got;
    }
    std::optional<TaskState> got;
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

    std::atomic<int> calls{0};
    std::atomic<int> submitsAtCall{-1};
    std::promise<void> called;
    std::future<void> calledFuture = called.get_future();
    WallpaperManager::Config config = smallConfig();
    config.onDeviceLost = [&] {
        if (calls.fetch_add(1) == 0) {
            submitsAtCall.store(g_submits.load());
            called.set_value();
        }
    };
    WallpaperManager manager(std::move(config));
    FrameReadyLog frames;
    // Ahead of the worker: a task left queued is answered by the worker's destructor, so what it
    // writes to must still exist then.
    Answer healthy;
    std::vector<std::unique_ptr<Answer>> probes;
    Answer later;
    RenderWorker worker(manager,
                        [&frames](int monitorId, uint64_t generation) { frames.record(monitorId, generation); });

    if (!firstFrame(manager, worker, frames, blue, kGoals)) return;

    // GOAL: S14.5 -- a healthy device does not stop the worker. False if the check between jobs
    //       reports loss for a device that works, which would restart the daemon after its
    //       first frame.
    // WHY-THIS-SHAPE: read after a task has been answered from the worker, which is after the
    //       check that follows the render has run; read straight after FrameReady it would race
    //       that check and pass for a worker that was about to stop.
    // CONTROL: via control_run.sh 2026-09-25 on NVIDIA and Lavapipe, both reverted.
    //       (a) WallpaperManager::stopsForLostDevice() (backend/daemon/wallpaper_manager.cpp) made
    //       to return `m_device != nullptr` -> "a task after a frame on a healthy device runs"
    //       red, with "a render that loses the device calls the manager's device-lost handler",
    //       since the stopped worker never runs that render. (b) Device::observe()
    //       (renderer/vulkan/device.cpp) made to call its handler for every result -> "the
    //       device-lost callback is not called for a healthy device" red, with the two lines of
    //       the exactly-once check below.
    worker.postTask(healthy.task());
    const std::optional<TaskState> healthyState = healthy.await();
    check(healthyState == TaskState::Run, "a task after a frame on a healthy device runs");
    check(calls.load() == 0, "the device-lost callback is not called for a healthy device");

    g_lose.store(true);
    manager.requestWallpaper("static", red, 1);
    worker.post(1);
    if (calledFuture.wait_for(kWait) != std::future_status::ready) {
        check(false, "a render that loses the device calls the manager's device-lost handler");
        return;
    }

    // GOAL: S14.5 -- once the device is lost, the worker comes to refuse new work the way a
    //       stopping worker does, answering a task ShuttingDown on the posting thread before
    //       postTask() returns, and never runs a task posted after the loss. False if a request
    //       after the loss is queued and run on the dead device, or answered only when the
    //       process tears the worker down.
    // WHY-THIS-SHAPE: the handler runs inside the failing submission, before the worker has
    //       finished that job and set its stop, so a single task posted on hearing of the loss
    //       races the worker's own check. Tasks are posted until one is answered inline; the
    //       ones posted in the gap sit queued behind a worker that has returned, and must come
    //       back ShuttingDown from stop(), never Run. "Inline" is the posting thread's id.
    // CONTROL: in backend/daemon/render_worker.cpp via control_run.sh 2026-09-25 on NVIDIA and
    //       Lavapipe, both reverted. (a) stopForLostDevice()'s `m_stop = true;` deleted, so the
    //       worker returns without refusing -> this line, "a task posted after a later render
    //       request is refused too" and "no task posted after the loss is run" red. (b) its
    //       stopsForLostDevice() test made `true ||` so it never stops -> the same three and "the
    //       worker submits nothing after the loss is reported".
    // COVERAGE-ASSERT: the probes posted are counted and printed; the refusal is read off the
    //       last one, so a loop that posted none cannot pass.
    bool refusedInline = false;
    for (const auto until = std::chrono::steady_clock::now() + kWait; std::chrono::steady_clock::now() < until;) {
        Answer& probe = *probes.emplace_back(std::make_unique<Answer>());
        worker.postTask(probe.task());
        if (probe.answered.load() && probe.thread == std::this_thread::get_id()) {
            refusedInline = probe.await() == TaskState::ShuttingDown;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(!probes.empty() && refusedInline,
          "after the loss, a task is answered ShuttingDown on the posting thread before postTask returns");

    manager.requestWallpaper("static", blue, 2);
    worker.post(2);
    worker.wake();
    worker.postTask(later.task());
    const std::optional<TaskState> laterState = later.await();
    check(laterState == TaskState::ShuttingDown, "a task posted after a later render request is refused too");
    if (laterState != TaskState::ShuttingDown) std::fprintf(stderr, "  answered %s\n", nameOf(laterState));

    worker.stop();

    size_t ranAfterLoss = 0;
    for (const std::unique_ptr<Answer>& probe : probes) {
        if (probe->await() != TaskState::ShuttingDown) ++ranAfterLoss;
    }
    check(ranAfterLoss == 0, "no task posted after the loss is run; the ones queued in the gap are refused by stop()");
    std::printf("device_loss_worker_test: %zu task(s) posted after the loss before one was refused inline, %zu run\n",
                probes.size(), ranAfterLoss);

    // GOAL: S14.5 -- the handler is called exactly once, and no submission is made after it.
    //       False if a worker whose device is lost goes on rendering: work started on a lost
    //       device is what a hung driver may hold when the stop deadline kills the process, and a
    //       render killed that way is blamed on its wallpaper.
    // WHY-THIS-SHAPE: submissions are counted at the driver boundary, not inferred from
    //       FrameReady: a render on a lost device fails, so it announces nothing whether or not
    //       it ran. Read after stop() has joined the worker, so nothing can still be in flight.
    // CONTROL: the handler count by the Device::observe() every-result control above (both lines
    //       red); the submission count by the `true ||` worker control above. Removing observe()'s
    //       once-only latch (renderer/vulkan/device.cpp) left this green, 2026-09-25: the stopped
    //       worker makes no second failing call, so the latch is graded in renderer's
    //       device_lost_probe instead.
    check(calls.load() == 1, "the device-lost handler is called exactly once");
    if (calls.load() != 1) std::fprintf(stderr, "  it was called %d times\n", calls.load());
    check(g_submits.load() == submitsAtCall.load(), "the worker submits nothing after the loss is reported");
    if (g_submits.load() != submitsAtCall.load()) {
        std::fprintf(stderr, "  %d submission(s) after the handler\n", g_submits.load() - submitsAtCall.load());
    }
    std::printf("device_loss_worker_test: with a handler, the loss was reported at submission %d and none "
                "followed\n",
                submitsAtCall.load());
}

// GOAL: S14.5 -- without a Config::onDeviceLost handler, a worker whose device is lost serves on:
//       tasks run, renders are attempted (and fail). False if the loss now stops every worker,
//       which would turn the in-process DEVICE_LOST tests into tests of a stopped worker and
//       change what the daemon's DBus handlers answer without one.
// WHY-THIS-SHAPE: the render after the loss is proven attempted by the submission count, and the
//       task behind it by its Run answer; FrameReady cannot show either, since a render on a
//       lost device announces nothing.
// CONTROL: `if (!m_config.onDeviceLost) return false;` deleted from
//       WallpaperManager::stopsForLostDevice() (backend/daemon/wallpaper_manager.cpp), via
//       control_run.sh 2026-09-25 on NVIDIA and Lavapipe -> "without a callback, a task after the
//       loss still runs" and "... still serving after two lost renders" red. Reverted.
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
