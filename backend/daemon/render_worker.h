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

// The daemon's one GPU thread.
//
// It exists so the DBus event loop never touches the GPU. The daemon runs a single
// blocking event loop, so anything done inside a handler stalls every other method for
// its whole duration -- and a render on a cold GPU stack can approach sd-bus's 25 s
// reply timeout. Both kinds of GPU work therefore run here:
//
//   post()     queues a render for a monitor. Coalescing falls out for free because the
//              queue holds *monitor ids*, not requests: the request itself lives in the
//              WallpaperManager, so a client dragging through a wallpaper list queues
//              one render per monitor rather than one per image it passed over. The
//              manager makes the pass idempotent per request, which covers the gap
//              between a monitor being dequeued and its request being read.
//   postTask() queues arbitrary GPU work -- an export, whose DBus reply is then sent
//              from this thread. Never coalesced, since each carries its own reply.
//
// Both share one FIFO, so ordering between a render and an export is the order the
// requests arrived in.
class RenderWorker {
public:
    // Called on the worker thread after a render that actually produced a frame. Must
    // be safe to call from a thread other than the DBus event loop -- emitting a signal
    // is, per sdbus-c++'s IObject contract.
    using FrameReadyFn = std::function<void(int monitorId, uint64_t generation)>;

    // move_only_function, not std::function: an export task owns the method's
    // sdbus::Result, which is move-only, so a copyable callable cannot hold it.
    using Task = std::move_only_function<void()>;

    // `manager` must outlive this worker, as must anything the callbacks touch. Call
    // stop() before tearing either down; the destructor also does, but only stop() lets
    // the owner control *when* relative to its other members.
    RenderWorker(WallpaperManager& manager, FrameReadyFn onFrameReady);
    ~RenderWorker();

    RenderWorker(const RenderWorker&) = delete;
    RenderWorker& operator=(const RenderWorker&) = delete;

    // Queues a render pass for `monitorId`, or does nothing if one is already queued.
    // Non-blocking; safe to call from the DBus event loop.
    void post(int monitorId);

    // Queues arbitrary work on the GPU thread. Non-blocking.
    void postTask(Task task);

    // Stops accepting work and joins the thread. Idempotent, and safe to call while
    // handlers are still being dispatched: post()/postTask() become no-ops rather than
    // touching a half-destroyed object.
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

    // Last member: the thread body touches every field above, so they must all be
    // constructed before it starts and destroyed after the destructor has joined it.
    std::thread m_thread;
};

}
