// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
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
//
// The video clock runs here too, as a round of WallpaperManager::advanceVideo() after every job
// and whenever the manager's next due time comes. So one job at most runs between two rounds and
// one round between two jobs: a hand-over waits for at most one round of pictures, and a picture
// for at most one job.
class RenderWorker {
public:
    // Called on the worker thread after each committed frame, a video's pictures included, so it
    // must be safe off the DBus event loop -- emitting a signal is, per sdbus-c++'s IObject
    // contract.
    using FrameReadyFn = std::function<void(int monitorId, uint64_t generation)>;

    // Why a task is being invoked: to do its work, or to answer that it never will. Every task
    // is invoked exactly once -- destroying an sdbus::Result sends nothing, leaving the caller
    // blocked for the full 25 s reply timeout.
    enum class TaskState { Run, ShuttingDown, Busy };

    // move_only_function, not std::function: an export task owns the method's
    // sdbus::Result, which is move-only, so a copyable callable cannot hold it.
    using Task = std::move_only_function<void(TaskState)>;

    // post() is self-limiting at one entry per monitor; tasks are not, and each one waiting
    // pins a live bus message.
    static constexpr size_t kMaxQueuedTasks = 64;

    // `manager` must outlive this worker, as must anything the callbacks touch. Call stop()
    // before tearing either down; the destructor does too, but stop() lets the owner pick when.
    RenderWorker(WallpaperManager& manager, FrameReadyFn onFrameReady);
    ~RenderWorker();

    RenderWorker(const RenderWorker&) = delete;
    RenderWorker& operator=(const RenderWorker&) = delete;

    // Queues a render for `monitorId`, or nothing if one is already queued or the id is
    // negative, which is how a task is told from a render. Non-blocking.
    void post(int monitorId);

    // Runs a clock round now, without queuing any work. What a change that only the manager can
    // act on -- a screen saying it is being drawn again -- reaches the worker by: playback is
    // resumed by the round, and the worker's next wake-up is only recomputed after a job or a
    // round, so without this a video stopped while nobody was looking would stay stopped until
    // something else happened to be posted. Non-blocking.
    void wake();

    // Queues arbitrary GPU work -- an export, replying from the worker thread. Never coalesced,
    // since each task carries its own reply. Non-blocking; a task refused because the worker is
    // stopping or already full is answered inline on the calling thread.
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
    void runJob(Job& job);

    // Runs `task` with nothing escaping: it is called on the worker thread, on a handler thread that
    // posted into a full queue, and from stop() during teardown -- on every one of them a throw
    // would be std::terminate. Logged instead.
    static void runTask(Task& task, TaskState state) noexcept;

    // Rounds that have thrown in a row, so an episode is logged once and retried more slowly the
    // longer it lasts. Worker thread only.
    unsigned m_clockFailures = 0;
    // One clock round and its announcements. Returns when the next is due, or nothing while no
    // video plays.
    std::optional<std::chrono::steady_clock::time_point> runClock();

    WallpaperManager& m_manager;
    FrameReadyFn m_onFrameReady;

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<Job> m_queue;
    std::set<int> m_queuedRenders;  // membership test for the coalescing above
    size_t m_queuedTasks = 0;
    // A wake() that has not been taken yet. In the wait predicate rather than a bare notify:
    // wait_until with a predicate loops until the predicate holds, so a notify alone is spurious
    // and the worker goes straight back to sleep until its own deadline.
    bool m_wakeClock = false;
    bool m_stop = false;
    std::once_flag m_joined;

    // Last member: the thread body touches every field above, so all must outlive it.
    std::thread m_thread;
};

}
