// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "config_store.h"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace wallpaperx {

// The config writer's thread and everything it touches. Held by shared_ptr and deliberately
// free of any reference back to its owner: a write on a stalled mount cannot be interrupted,
// so the thread must be abandonable, and abandoning one that could reach its owner would be a
// use-after-free waiting for the mount to recover.
struct PersistWorker {
    std::mutex mutex;

    // wake: the worker waiting for something to write. settled: whoever queued it waiting
    // for the write to land, which is what keeps a healthy save synchronous.
    std::condition_variable wake;
    std::condition_variable settled;

    // Set when a wait ran out, cleared when the filesystem answers again. Stops every later
    // save paying the same deadline for an answer this one already has.
    bool stalled = false;

    // One slot, not a queue: the newest snapshot already describes everything the older ones
    // would have written.
    std::optional<PersistedConfig> pending;

    // Set when a snapshot is handed over, cleared when the write returns. A value that stays
    // put is what tells the health report the filesystem has stopped answering.
    std::optional<std::chrono::steady_clock::time_point> writingSince;

    bool haveResult = false;
    SaveResult result;
};

// Owns getting the user's choices onto disk, and answers the one question every report about
// persistence asks: does the file hold what was handed over, and if not, why.
//
// Takes no lock belonging to anything else and calls nothing back, so it may be used with the
// owner's own lock held. Internally: this object's lock, then the worker's, never the reverse.
class ConfigWriter {
public:
    // Derived rather than stored, since a completed write's result lives in the worker, which
    // outlives this object. Carries everything a report needs, taken under one acquisition of
    // this object's lock: an answer stitched from several calls describes no single instant.
    struct Outcome {
        enum class State {
            NeverAsked,
            OnDisk,
            Outstanding,  // queued or in flight: not on disk YET, which is not a failure
            Stalled,      // outstanding past the deadline: the filesystem stopped answering
            Failed,
        };

        State state = State::NeverAsked;
        // Set for every state but NeverAsked and OnDisk. Raw; sanitized where recorded or thrown.
        std::string why;
        // The last COMPLETED write's result. Default while none has completed, so "nothing
        // counted yet" cannot read as "nothing lost".
        SaveResult saved;

        static bool onDisk(State s) noexcept { return s == State::NeverAsked || s == State::OnDisk; }
        bool onDisk() const noexcept { return onDisk(state); }
    };

    explicit ConfigWriter(ConfigStore store);

    ConfigWriter(const ConfigWriter&) = delete;
    ConfigWriter& operator=(const ConfigWriter&) = delete;

    bool enabled() const { return m_store.enabled(); }
    const std::filesystem::path& file() const { return m_store.file(); }

    // The file holds content this run could not read, so a save rebuilt from live state would
    // replace monitors this run never saw. Lifted once a copy of those bytes exists.
    void refuse(std::string why);
    void allow();
    bool refusing() const;

    // Hands the file's contents over without waiting. False when nothing was queued -- disabled, refused, or
    // no thread -- and outcome() then carries which. Taken by value so the copy, if any, is made
    // before anything here changes: a throw leaves this object as it was.
    bool queue(PersistedConfig config);

    // A bounded wait for the last queue() to land: a wait that runs out leaves the write in
    // flight and the rows not known to be on disk. Returns whether they reached the file, true
    // when persistence is disabled since durability was never promised. Builds no reason, so
    // it cannot throw: a caller that has already committed a change relies on that.
    bool awaitQueued() noexcept;
    // The same wait; on false, `whyNot` receives the write's own reason, raw.
    bool awaitQueued(std::string& whyNot);

    // The single answer to whether the file holds what was handed over. Everything a report
    // needs is in it, so nothing else here answers part of the same question.
    Outcome outcome() const;

private:
    // Spawns the thread on first use and leaves the worker null when it could not be created,
    // recording why. Caller holds m_mutex.
    void startLocked();

    void waitForQueued() noexcept;

    Outcome outcomeLocked() const;
    // The state outcomeLocked() reports, with none of its strings. Caller holds m_mutex and,
    // when m_worker is set, the worker's mutex.
    Outcome::State stateLocked() const noexcept;

    const ConfigStore m_store;

    mutable std::mutex m_mutex;

    // Started on the first save, so a daemon that never sets a wallpaper never spawns it.
    // Detached, never joined: see PersistWorker. Non-null means a thread is servicing it.
    std::shared_ptr<PersistWorker> m_worker;

    // Why the last save never reached the writer -- no thread, or a refusal decided before one
    // exists. Empty once a save is handed over, or once allow() ends the refusal it holds. The
    // half of the outcome the worker cannot know.
    std::string m_notWritten;

    bool m_refusing = false;
    std::string m_refusal;
};

}
