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
// single and blocking, so work inside a handler stalls every other method for its duration,
// and a render on a cold GPU stack can approach sd-bus's 25 s reply timeout.
//
// Renders and tasks share one FIFO, so ordering between a render and an export is the order
// the requests arrived in. Coalescing falls out for free because the queue holds monitor ids
// rather than requests -- the request lives in the WallpaperManager -- so a client dragging
// through a wallpaper list queues one render per monitor, not one per image. The manager makes
// each pass idempotent per request, covering the gap between a dequeue and the request read.
class RenderWorker {
public:
    // Called on the worker thread after a render that produced a frame, so it must be safe off
    // the DBus event loop -- emitting a signal is, per sdbus-c++'s IObject contract.
    using FrameReadyFn = std::function<void(int monitorId, uint64_t generation)>;

    // move_only_function, not std::function: an export task owns the method's
    // sdbus::Result, which is move-only, so a copyable callable cannot hold it.
    using Task = std::move_only_function<void()>;

    // `manager` must outlive this worker, as must anything the callbacks touch. Call stop()
    // before tearing either down; the destructor does too, but stop() lets the owner pick when.
    RenderWorker(WallpaperManager& manager, FrameReadyFn onFrameReady);
    ~RenderWorker();

    RenderWorker(const RenderWorker&) = delete;
    RenderWorker& operator=(const RenderWorker&) = delete;

    // Queues a render for `monitorId`, or nothing if one is already queued. Non-blocking.
    void post(int monitorId);

    // Queues arbitrary GPU work -- an export, whose DBus reply is then sent from the worker
    // thread. Never coalesced, since each task carries its own reply. Non-blocking.
    void postTask(Task task);

    // Stops accepting work and joins the thread. Idempotent, and safe while handlers are still
    // dispatching: post()/postTask() become no-ops rather than touching a half-destroyed object.
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
    std::deque<Job> m_queue;      // FIFO across monitors, so one busy monitor cannot starve another
    std::set<int> m_queuedRenders;  // membership test for the coalescing above
    bool m_stop = false;

    // Last member: the thread body touches every field above, so all must be constructed
    // before it starts and destroyed only after the destructor has joined it.
    std::thread m_thread;
};

}
