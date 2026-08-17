// SPDX-License-Identifier: GPL-2.0-or-later

#include "render_worker.h"

#include "wallpaper_manager.h"

#include <cstdio>
#include <exception>
#include <utility>

namespace wallpaperx {

RenderWorker::RenderWorker(WallpaperManager& manager, FrameReadyFn onFrameReady)
    : m_manager(manager), m_onFrameReady(std::move(onFrameReady)), m_thread([this] { run(); }) {}

RenderWorker::~RenderWorker() { stop(); }

void RenderWorker::stop() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stop) return;  // idempotent: the thread has already been joined
        m_stop = true;
    }
    m_wake.notify_all();
    m_thread.join();
}

void RenderWorker::post(int monitorId) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stop) return;
        if (!m_queuedRenders.insert(monitorId).second) {
            return;  // already queued; the pending pass will pick up the newer request
        }
        m_queue.push_back(Job{monitorId, {}});
    }
    m_wake.notify_one();
}

void RenderWorker::postTask(Task task) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // Dropped rather than run during shutdown: the caller sees a NoReply timeout, which is
        // the right outcome for a daemon going away and better than touching the GPU while the
        // Device is being torn down.
        if (m_stop) return;
        m_queue.push_back(Job{-1, std::move(task)});
    }
    m_wake.notify_one();
}

void RenderWorker::run() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait(lock, [this] { return m_stop || !m_queue.empty(); });

            // Stop wins over draining. Anything still queued is a wallpaper nobody will
            // see -- the daemon is going away -- and rendering it would only delay
            // shutdown by a full GPU submit per entry.
            if (m_stop) return;

            job = std::move(m_queue.front());
            m_queue.pop_front();
            if (job.monitorId >= 0) {
                m_queuedRenders.erase(job.monitorId);
            }
        }

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
            } else if (job.task) {
                job.task();
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

}
