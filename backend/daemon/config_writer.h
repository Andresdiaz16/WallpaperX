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
    std::optional<std::vector<PersistedWallpaper>> pending;

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
    // outlives this object. One derivation for every caller, so no two can disagree.
    struct Outcome {
        bool settled = true;  // nothing outstanding, so onDisk describes the file as it stands
        bool onDisk = true;   // true before anything is asked of the file: it already agrees
        std::string why;      // when !onDisk. Raw; sanitized where recorded or thrown
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

    // Hands rows over without waiting. False when nothing was queued -- disabled, refused, or
    // no thread -- and outcome() then carries which.
    bool queue(const std::vector<PersistedWallpaper>& rows);

    // queue(), then a bounded wait: a wait that runs out leaves the write in flight and the
    // rows not known to be on disk. Returns whether THESE rows reached the file, true when
    // persistence is disabled since durability was never promised. On false, `whyNot` receives
    // this write's own reason, raw.
    bool write(const std::vector<PersistedWallpaper>& rows, std::string* whyNot = nullptr);

    Outcome outcome() const;

    // Whether anything has ever been handed over. False means the file has not been asked to
    // hold anything, so whatever was recorded at startup still stands and must not be restated
    // -- restating it would escape an already-escaped path a second time.
    bool everQueued() const;

    // A write in flight past the deadline -- a stalled filesystem, which earns its own wording.
    bool stalledPastDeadline() const;

    // What the last completed write reported, for the losses it counted.
    SaveResult lastResult() const;

private:
    // Spawns the thread on first use and leaves the worker null when it could not be created,
    // recording why. Caller holds m_mutex.
    void startLocked();

    Outcome outcomeLocked() const;

    const ConfigStore m_store;

    mutable std::mutex m_mutex;

    // Started on the first save, so a daemon that never sets a wallpaper never spawns it.
    // Detached, never joined: see PersistWorker. Non-null means a thread is servicing it.
    std::shared_ptr<PersistWorker> m_worker;

    // Why the last save never reached the writer -- no thread, or a refusal decided before one
    // exists. Empty once a save is handed over. The half of the outcome the worker cannot know.
    std::string m_notWritten;

    bool m_refusing = false;
    std::string m_refusal;
};

}
