// SPDX-License-Identifier: GPL-2.0-or-later

#include "render_worker.h"

#include "wallpaper_manager.h"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <utility>

namespace wallpaperx {

RenderWorker::RenderWorker(WallpaperManager& manager, FrameReadyFn onFrameReady)
    : m_manager(manager), m_onFrameReady(std::move(onFrameReady)), m_thread([this] { run(); }) {
    m_manager.setClockWake([this] { wake(); });
}

RenderWorker::~RenderWorker() { stop(); }

void RenderWorker::stop() {
    m_manager.setClockWake({});
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_wake.notify_all();
    // Every caller waits for the join, not just the first: returning early because another
    // thread set m_stop would let this caller destroy state the worker is still reading.
    std::call_once(m_joined, [this] { m_thread.join(); });

    // After the join, so no task is answered beside a running render.
    std::deque<Job> dropped;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        dropped.swap(m_queue);
        m_queuedRenders.clear();
        m_queuedTasks = 0;
    }
    for (Job& job : dropped) runTask(job.task, TaskState::ShuttingDown);
}

void RenderWorker::runTask(Task& task, TaskState state) noexcept {
    if (!task) return;
    try {
        task(state);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "wallpaperx-daemon: render worker task error: %s\n", e.what());
    } catch (...) {
        std::fprintf(stderr, "wallpaperx-daemon: render worker task error: unknown exception\n");
    }
}

void RenderWorker::post(int monitorId) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stop || monitorId < 0) return;
        if (!m_queuedRenders.insert(monitorId).second) {
            return;  // already queued; the pending pass will pick up the newer request
        }
        m_queue.push_back(Job{monitorId, {}});
    }
    m_wake.notify_one();
}

void RenderWorker::wake() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stop) return;
        m_wakeClock = true;
    }
    m_wake.notify_one();
}

void RenderWorker::postTask(Task task) {
    TaskState refused = TaskState::Run;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // Stopping: the work would touch the GPU while the Device is being torn down.
        // Full: the queue would otherwise grow with a live bus message pinned in each entry.
        if (m_stop) {
            refused = TaskState::ShuttingDown;
        } else if (m_queuedTasks >= kMaxQueuedTasks) {
            refused = TaskState::Busy;
        } else {
            m_queue.push_back(Job{-1, std::move(task)});
            ++m_queuedTasks;
        }
    }
    if (refused != TaskState::Run) {
        runTask(task, refused);
        return;
    }
    m_wake.notify_one();
}

void RenderWorker::run() {
    // Only this thread changes what plays, and it asks again after every job, so the answer is
    // current whenever it waits. A request that stops a video posts a job, which wakes it.
    std::optional<std::chrono::steady_clock::time_point> clockDue;
    for (;;) {
        Job job;
        bool haveJob = false;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            const auto ready = [this] { return m_stop || m_wakeClock || !m_queue.empty(); };
            if (clockDue) {
                m_wake.wait_until(lock, *clockDue, ready);
            } else {
                m_wake.wait(lock, ready);
            }

            // Stop wins over draining. Anything still queued is a wallpaper nobody will
            // see -- the daemon is going away -- and rendering it would only delay
            // shutdown by a full GPU submit per entry. stop() answers the tasks left behind.
            if (m_stop) return;

            // Taken whatever woke this: the round below runs on every pass, so a wake that is
            // still set afterwards would spin.
            m_wakeClock = false;

            if (!m_queue.empty()) {
                job = std::move(m_queue.front());
                m_queue.pop_front();
                haveJob = true;
                if (job.monitorId >= 0) {
                    m_queuedRenders.erase(job.monitorId);
                } else if (m_queuedTasks > 0) {
                    --m_queuedTasks;
                }
            }
        }

        if (haveJob) runJob(job);
        if (stopForLostDevice()) return;
        clockDue = runClock();
        if (stopForLostDevice()) return;
    }
}

bool RenderWorker::stopForLostDevice() {
    if (!m_manager.stopsForLostDevice()) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stop = true;
    return true;
}

std::optional<std::chrono::steady_clock::time_point> RenderWorker::runClock() {
    // What a round that threw is retried after, doubling to a ceiling: its only throws are a lock
    // or an allocation failing, and a fixed retry spends ten wakes and ten log lines a second on a
    // machine that is already short of whatever ran out. Without any retry, every playing video
    // would stop until the next job.
    constexpr std::chrono::milliseconds kFirstRetry{100};
    constexpr std::chrono::milliseconds kMaxRetry{5000};
    constexpr unsigned kMaxDoublings = 6;
    try {
        const ClockRound round = m_manager.advanceVideo(std::chrono::steady_clock::now());
        if (m_onFrameReady) {
            for (const ClockRound::Shown& picture : round.shown) m_onFrameReady(picture.monitorId, picture.generation);
        }
        // A picture that failed leaves its monitor owing a render, which reopens the file.
        for (const int monitorId : round.retry) post(monitorId);
        if (m_clockFailures != 0) {
            std::fprintf(stderr, "wallpaperx-daemon: the video clock recovered after %u failed round(s)\n",
                         m_clockFailures);
            m_clockFailures = 0;
        }
        return round.nextDue;
    } catch (const std::exception& e) {
        // The first of an episode only: the hundredth identical line says nothing the first did not.
        if (m_clockFailures == 0) std::fprintf(stderr, "wallpaperx-daemon: video clock error: %s\n", e.what());
    } catch (...) {
        if (m_clockFailures == 0) std::fprintf(stderr, "wallpaperx-daemon: video clock error: unknown exception\n");
    }
    const auto delay = std::min(kFirstRetry * (1u << std::min(m_clockFailures, kMaxDoublings)), kMaxRetry);
    ++m_clockFailures;
    return std::chrono::steady_clock::now() + delay;
}

void RenderWorker::runJob(Job& job) {
    // Dequeued before running, so a request arriving mid-render re-queues the monitor and
    // gets its own pass. A request landing between that erase and renderPending() reading
    // it is therefore serviced twice; servicedSeq makes the second a no-op, and that has to
    // live in the manager, which is the only thing that knows what a pass rendered.
    try {
        if (job.monitorId >= 0) {
            const RenderOutcome outcome = m_manager.renderPending(job.monitorId);
            if (outcome.rendered && m_onFrameReady) {
                m_onFrameReady(job.monitorId, outcome.generation);
            }
        } else {
            runTask(job.task, TaskState::Run);
        }
    } catch (const std::exception& e) {
        // renderPending records failures in the monitor's status rather than throwing and
        // an export task answers its own reply, so reaching here is unexpected -- but this
        // is a bare thread, where an escaping exception is std::terminate. Log and serve on.
        std::fprintf(stderr, "wallpaperx-daemon: render worker error (monitor=%d): %s\n", job.monitorId,
                     e.what());
    } catch (...) {
        std::fprintf(stderr, "wallpaperx-daemon: render worker error (monitor=%d): unknown exception\n",
                     job.monitorId);
    }
}

}
