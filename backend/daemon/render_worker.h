// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <set>
#include <thread>

namespace wallpaperx {

class WallpaperManager;

// The daemon's one GPU thread, so the DBus event loop never touches the GPU: that loop is
// single and blocking, and a render on a cold GPU stack can approach sd-bus's 25 s timeout.
//
// One FIFO for renders and tasks, holding monitor ids rather than requests: ordering is
// arrival order, and a client dragging through a wallpaper list coalesces to one render per
// monitor. The manager makes each pass idempotent, covering dequeue-to-read.
class RenderWorker {
public:
    // Called on the worker thread after a render that produced a frame, so it must be safe off
    // the DBus event loop -- emitting a signal is, per sdbus-c++'s IObject contract.
    using FrameReadyFn = std::function<void(int monitorId, uint64_t generation)>;

    // move_only_function, not std::function: an export task owns the method's
    // sdbus::Result, which is move-only, so a copyable callable cannot hold it.
    //
    // Always invoked exactly once, with `cancelled` true when the worker is stopping and the
    // work will not run. Destroying an sdbus::Result sends nothing, so a task that is only
    // dropped leaves its caller blocked for the full 25 s reply timeout.
    using Task = std::move_only_function<void(bool cancelled)>;

    // `manager` must outlive this worker, as must anything the callbacks touch. Call stop()
    // before tearing either down; the destructor does too, but stop() lets the owner pick when.
    RenderWorker(WallpaperManager& manager, FrameReadyFn onFrameReady);
    ~RenderWorker();

    RenderWorker(const RenderWorker&) = delete;
    RenderWorker& operator=(const RenderWorker&) = delete;

    // Queues a render for `monitorId`, or nothing if one is already queued. Non-blocking.
    void post(int monitorId);

    // Queues arbitrary GPU work -- an export, replying from the worker thread. Never coalesced,
    // since each task carries its own reply. Non-blocking; a task posted after stop() is
    // cancelled inline on the calling thread.
    void postTask(Task task);

    // Stops accepting work, joins, then cancels every task still queued. Idempotent, and safe
    // mid-dispatch: post() becomes a no-op rather than touching a half-destroyed object.
    void stop();

private:
    struct Job {
        int monitorId = -1;  // >= 0 for a render; a task otherwise
        Task task;
    };

    void run();

    WallpaperManager& m_manager;
    FrameReadyFn m_onFrameReady;

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<Job> m_queue;
    std::set<int> m_queuedRenders;  // membership test for the coalescing above
    bool m_stop = false;

    // Last member: the thread body touches every field above, so all must outlive it.
    std::thread m_thread;
};

}
