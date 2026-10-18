// SPDX-License-Identifier: GPL-2.0-or-later

#include "config_writer.h"

#include "log_escape.h"

#include <chrono>
#include <thread>
#include <utility>

#include <unistd.h>

namespace wallpaperx {
namespace {

// The same bound the config read uses: long enough that a slow disk is not cut off, short
// enough that a stalled mount does not visibly stall a login.
constexpr std::chrono::seconds kWriteDeadline{2};

// How long a snapshot waits for a newer one to replace it. Bounds both the writes a burst of
// changes makes and what a SIGKILL inside the window loses.
constexpr std::chrono::milliseconds kCoalesce{250};

}

ConfigWriter::ConfigWriter(ConfigStore store) : m_store(std::move(store)) {}

ConfigWriter::~ConfigWriter() { waitForQueued(); }

void ConfigWriter::refuse(std::string why) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_refusing = true;
    m_refusal = std::move(why);
}

void ConfigWriter::allow() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_refusing = false;
    // Including the copy a refused save left behind, which outcome() reads first and which
    // would otherwise report a daemon refusing to write after it stopped refusing.
    if (m_notWritten == m_refusal) m_notWritten.clear();
    m_refusal.clear();
}

bool ConfigWriter::refusing() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_refusing;
}

void ConfigWriter::startLocked() {
    if (m_worker) return;

    // Nothing of this object is captured: this thread outlives its owner whenever a write is
    // stuck, and a write on a dead mount cannot be interrupted.
    std::shared_ptr<PersistWorker> worker;
    try {
        worker = std::make_shared<PersistWorker>();
        std::thread([worker, store = m_store, notice = m_notice] {
            for (;;) {
                PersistedConfig contents;
                {
                    std::unique_lock<std::mutex> lock(worker->mutex);
                    // No stop condition: there is nothing safe to wait for at exit, since a
                    // write on a dead mount cannot be interrupted. The thread ends with the
                    // process, and holds nothing the process needs to release.
                    worker->wake.wait(lock, [&worker] { return worker->pending.has_value(); });
                    worker->wake.wait_for(lock, kCoalesce, [&worker] { return worker->flush; });
                    worker->flush = false;
                    contents = std::move(*worker->pending);
                    worker->pending.reset();
                    worker->writingSince = std::chrono::steady_clock::now();
                }

                const SaveResult saved = store.save(contents);

                // Said by the thread that learns it: the startup repair is queued, so any other
                // report of it races the write and can read as a clean start.
                //
                // write(2), not stdio: this thread outlives main() when a write is stuck, and a
                // FILE* torn down by exit is not something it can hold.
                if (!saved.error.empty()) {
                    const std::string line =
                        "wallpaperx-daemon: config not saved: " + sanitizeForLog(saved.error) + "\n";
                    const ssize_t ignored = ::write(STDERR_FILENO, line.data(), line.size());
                    (void)ignored;
                }

                {
                    std::lock_guard<std::mutex> lock(worker->mutex);
                    worker->result = saved;
                    worker->haveResult = true;
                    worker->writingSince.reset();
                    // The filesystem answered, so the next caller is allowed to wait again.
                    worker->stalled = false;
                }
                worker->settled.notify_all();

                // After the worker's lock is released: a bus handler waiting for this write holds
                // the bus's lock, which the callback needs.
                std::lock_guard<std::mutex> lock(notice->mutex);
                if (notice->written) notice->written();
            }
        }).detach();
    } catch (const std::exception& e) {
        // std::exception, not std::system_error: std::thread also allocates the callable's
        // state, so std::bad_alloc reaches here and would otherwise kill the daemon at startup.
        //
        // Not published and not rethrown: a worker pointer with no thread behind it makes every
        // later save wait out the deadline for an answer that cannot come. Recorded instead,
        // and retried by the next save that is queued.
        try {
            m_notWritten = m_store.file().string() + ": not saved, no thread available: " + e.what();
        } catch (...) {  // NOLINT(bugprone-empty-catch) -- nothing left to report it with
        }
        return;
    }

    // Published only now: a non-null worker means a thread is servicing it.
    m_worker = std::move(worker);
}

bool ConfigWriter::queue(PersistedConfig config) {
    if (!m_store.enabled()) return false;

    std::shared_ptr<PersistWorker> worker;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // The save rebuilds the file from live state, so writing now would replace monitors
        // this run never managed to read with the one the user just set.
        if (m_refusing) {
            m_notWritten = m_refusal;
            return false;
        }
        startLocked();
        worker = m_worker;
        // Cleared only once the snapshot is on its way to a writer: from here the worker's own
        // result is the authority, and a stale refusal would outrank it.
        if (worker) m_notWritten.clear();
        // No writer, and startLocked() recorded why: the next save retries.
        if (!worker) return false;

        // Still under m_mutex, so no reader sees a worker that exists with nothing pending and
        // calls that a clean file. This lock then the worker's is the documented order.
        std::lock_guard<std::mutex> workerLock(worker->mutex);
        // Replaced, not appended: an older snapshot waiting behind a stalled write describes a
        // state this one already supersedes, so writing it would only be a slower route to the
        // same file.
        worker->pending = std::move(config);
    }
    worker->wake.notify_one();
    return true;
}

void ConfigWriter::waitForQueued() noexcept {
    std::shared_ptr<PersistWorker> worker;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        worker = m_worker;
    }

    // Waited on, briefly, so the reply to a save can say the choice is on disk in the healthy
    // case -- which is the only case it can be said in. Past the deadline the write keeps going
    // on the worker and this thread stops caring, so a stalled filesystem costs one wait rather
    // than the daemon, and the choice is then NOT known to be saved.
    if (worker) {
        std::unique_lock<std::mutex> workerLock(worker->mutex);
        if (worker->pending) {
            worker->flush = true;
            worker->wake.notify_one();
        }
        const auto quiesced = [&worker] { return !worker->pending && !worker->writingSince; };
        // Not waited on again until something completes: a second stalled call would add
        // another deadline's delay to a thread that has already learned the answer.
        if (!worker->stalled && !worker->settled.wait_for(workerLock, kWriteDeadline, quiesced)) {
            worker->stalled = true;
        }
    }
}

bool ConfigWriter::awaitQueued() noexcept {
    waitForQueued();
    std::lock_guard<std::mutex> lock(m_mutex);
    std::unique_lock<std::mutex> workerLock;
    if (m_worker) workerLock = std::unique_lock<std::mutex>(m_worker->mutex);
    return Outcome::onDisk(stateLocked());
}

bool ConfigWriter::awaitQueued(std::string& whyNot) {
    waitForQueued();
    const Outcome result = outcome();
    if (!result.onDisk()) whyNot = result.why;
    return result.onDisk();
}

void ConfigWriter::onWritten(std::function<void()> written) {
    std::lock_guard<std::mutex> lock(m_notice->mutex);
    m_notice->written = std::move(written);
}

ConfigWriter::Outcome ConfigWriter::outcome() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return outcomeLocked();
}

ConfigWriter::Outcome ConfigWriter::outcomeLocked() const {
    using State = Outcome::State;

    std::unique_lock<std::mutex> workerLock;
    if (m_worker) workerLock = std::unique_lock<std::mutex>(m_worker->mutex);
    const State state = stateLocked();

    if (state == State::NeverAsked) return {};
    if (!m_notWritten.empty()) return {state, m_notWritten, {}};

    // Past both checks above, stateLocked() only answers from a worker.
    const SaveResult saved = m_worker->haveResult ? m_worker->result : SaveResult{};
    switch (state) {
        case State::Stalled:
            return {state,
                    m_store.file().string() +
                        ": has not answered a write, the filesystem may be stalled",
                    saved};
        case State::Failed:
            return {state, m_worker->result.error, saved};
        case State::Outstanding:
            return {state,
                    m_store.file().string() +
                        ": a write has not come back, the choice may not be on disk yet",
                    saved};
        case State::NeverAsked:
        case State::OnDisk:
            break;
    }
    return {State::OnDisk, {}, saved};
}

ConfigWriter::Outcome::State ConfigWriter::stateLocked() const noexcept {
    using State = Outcome::State;

    // Durability was never promised, so the file cannot disagree with anything.
    if (!m_store.enabled()) return State::NeverAsked;

    // The half the worker cannot know: a save refused, or a writer that never started. Recorded
    // rather than inferred from a null worker, which also means "nothing asked yet".
    if (!m_notWritten.empty()) return State::Failed;
    if (!m_worker) return State::NeverAsked;

    // Outstanding covers all three ways a write can be unfinished: still queued, in flight, or
    // abandoned by a caller that stopped waiting. Reading only `haveResult` reported a name as
    // durable while the write that would have made it so had never completed.
    //
    // Answers for ANY write, not the caller's own -- the same question only while one thread
    // writes. It holds today because saves are reached from bus handlers alone and sdbus-c++
    // dispatches those on one event-loop thread; the startup repair queues without waiting
    // before that loop exists. A second writer would make a landed write report as not on disk
    // because another had since started. The fix then is to give each write an identity, not to
    // widen this test.
    const bool outstanding = m_worker->pending.has_value() ||
                             m_worker->writingSince.has_value() || m_worker->stalled;
    // A write nobody is waiting for any more is the only symptom a stalled config filesystem
    // leaves, so it is told apart from one that is merely still going.
    const bool pastDeadline =
        m_worker->writingSince.has_value() &&
        std::chrono::steady_clock::now() - *m_worker->writingSince > kWriteDeadline;

    if (m_worker->haveResult && !m_worker->result.error.empty()) {
        return outstanding && pastDeadline ? State::Stalled : State::Failed;
    }
    if (outstanding) return pastDeadline ? State::Stalled : State::Outstanding;
    return State::OnDisk;
}

}
