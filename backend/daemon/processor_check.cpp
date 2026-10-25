// SPDX-License-Identifier: GPL-2.0-or-later

#include "processor_check.h"

#include "crash_record.h"

#include <linux/ioprio.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <exception>
#include <map>
#include <new>
#include <system_error>
#include <utility>

namespace wallpaperx {

namespace {

// For this thread alone: Linux keeps both per thread. Neither failing stops the scan.
void lowerPriority() {
    static_cast<void>(syscall(SYS_ioprio_set, IOPRIO_WHO_PROCESS, 0, IOPRIO_PRIO_VALUE(IOPRIO_CLASS_IDLE, 0)));
    static_cast<void>(setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), 19));
}

}  // namespace

const char* toString(ProcessorDecoding decoding) {
    switch (decoding) {
        case ProcessorDecoding::Checking: return "checking";
        case ProcessorDecoding::None: return "none";
        case ProcessorDecoding::Parts: return "parts";
        case ProcessorDecoding::All: return "all";
        case ProcessorDecoding::Unknown: return "unknown";
    }
    return "unknown";
}

ProcessorDecodingStatus processorDecoding(const ProcessorUse& use, std::optional<bool> inlineParameters) {
    using FirstPicture = ProcessorUse::FirstPicture;
    ProcessorDecodingStatus status;
    // Whatever the file holds: no picture of it reaches the device.
    if (use.firstPicture == FirstPicture::SoftwareThroughout) {
        status.state = ProcessorDecoding::All;
        return status;
    }
    const auto* scanned = std::get_if<FileScan>(&use.scan);
    const SoftwareStretches* found = scanned != nullptr ? &scanned->found : nullptr;
    const std::optional<bool> parameterSets =
        found != nullptr ? found->parameterSetsToSoftware(inlineParameters) : std::nullopt;
    if (use.firstPicture == FirstPicture::Software) status.reasons.push_back("first_picture");
    if (found != nullptr && found->missingReference) status.reasons.push_back("missing_reference");
    if (parameterSets == true) status.reasons.push_back("parameter_set_change");
    if (!status.reasons.empty()) {
        status.state = ProcessorDecoding::Parts;
    } else if (std::holds_alternative<ProcessorUse::ScanFailed>(use.scan)) {
        status.state = ProcessorDecoding::Unknown;
    } else if (use.firstPicture && parameterSets == false) {
        status.state = ProcessorDecoding::None;
    }
    return status;
}

void ChangeNotice::set(std::function<void()> changed) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_changed = std::move(changed);
}

void ChangeNotice::operator()() noexcept {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_changed) m_changed();
}

ProcessorCheck::ProcessorCheck(Done done) : m_done(std::move(done)) {}

ProcessorCheck::~ProcessorCheck() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
        m_queue.clear();
        m_stopRunning.store(true, std::memory_order_relaxed);
    }
    m_wake.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

void ProcessorCheck::check(int monitorId, uint64_t wallpaperSeq, std::string path, std::string blame,
                           std::optional<FileScan> saved) {
    std::optional<Job> unscanned;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stop) return;
        forgetLocked(monitorId);
        m_queue.push_back(Job{monitorId, wallpaperSeq, std::move(path), std::move(blame), std::move(saved)});
        if (!m_thread.joinable()) {
            try {
                m_thread = std::thread([this] { run(); });
            } catch (const std::system_error&) {
                unscanned = std::move(m_queue.back());
                m_queue.pop_back();
            }
        }
    }
    if (unscanned) {
        m_done(unscanned->monitorId, unscanned->wallpaperSeq, std::string("no thread could be started to scan it"));
        return;
    }
    m_wake.notify_one();
}

void ProcessorCheck::forget(int monitorId) noexcept {
    std::lock_guard<std::mutex> lock(m_mutex);
    forgetLocked(monitorId);
}

void ProcessorCheck::forgetLocked(int monitorId) noexcept {
    std::erase_if(m_queue, [monitorId](const Job& job) { return job.monitorId == monitorId; });
    if (m_running == monitorId) m_stopRunning.store(true, std::memory_order_relaxed);
}

void ProcessorCheck::run() {
    lowerPriority();
    // Answers read since the queue was last empty, by path: the same video on several screens is read once. Emptied
    // with the queue, so it holds no more paths than were queued together.
    std::map<std::string, FileScan> read;
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_running.reset();
            if (m_queue.empty()) read.clear();
            m_wake.wait(lock, [this] { return m_stop || !m_queue.empty(); });
            if (m_stop) return;
            job = std::move(m_queue.front());
            m_queue.pop_front();
            m_running = job.monitorId;
            m_stopRunning.store(false, std::memory_order_relaxed);
        }
        std::optional<Found> found;
        try {
            const std::optional<FileIdentity> before = fileIdentityOf(job.path);
            const auto known = read.find(job.path);
            if (job.saved && job.saved->file && before == job.saved->file) {
                found = *job.saved;
            } else if (known != read.end() && before == known->second.file) {
                found = known->second;
            } else {
                const PlayingBlame scanning(job.blame);
                if (std::optional<SoftwareStretches> stretches = scanSoftwareStretches(job.path, m_stopRunning)) {
                    // Saved against nothing when the path named other bytes by the end of the read.
                    FileScan scan{*stretches, before && fileIdentityOf(job.path) == before ? before : std::nullopt};
                    found = scan;
                    try {
                        if (scan.file) read.insert_or_assign(job.path, std::move(scan));
                    } catch (const std::bad_alloc&) {
                    }
                }
            }
        } catch (const std::exception& e) {
            // A reason that cannot be copied still reports the failure.
            try {
                found = Found(std::string(e.what()));
            } catch (...) {
                found = Found(std::string());
            }
        } catch (...) {
            found = Found(std::string());
        }
        if (!found) continue;
        try {
            m_done(job.monitorId, job.wallpaperSeq, std::move(*found));
        } catch (...) {
        }
    }
}

}
