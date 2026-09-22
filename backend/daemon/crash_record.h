// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <sys/types.h>

namespace wallpaperx {

// What the last run was applying to a monitor when it died. Read at startup, before the saved
// wallpapers are applied: a file that kills the daemon while it is being applied would otherwise
// be re-applied by the restart that follows, and kill it again.
struct CrashRecord {
    std::string type;
    std::string path;

    // The signal the applying thread died of. Empty when the process ended with no handler
    // running -- SIGKILL, the teardown alarm, a power cut -- which says only that it did not
    // survive applying this.
    std::optional<int> signal;
};

// <stateDir>/applying-<monitorId>.json. Empty when `stateDir` is.
std::filesystem::path crashRecordPath(const std::filesystem::path& stateDir, int monitorId);

// The record the last run left for this monitor, or nothing when there is none or it cannot be
// read. Unreadable is treated as absent: a refusal on a record that says nothing would refuse a
// wallpaper on no evidence.
std::optional<CrashRecord> readCrashRecord(const std::filesystem::path& stateDir, int monitorId);

void removeCrashRecord(const std::filesystem::path& stateDir, int monitorId);

// The reason a client reads, raw: sanitized where it is recorded.
std::string describeCrash(const CrashRecord& crash);

// On disk from construction to destruction, on the thread that is applying the wallpaper. One at
// a time, enforced: the fatal-signal handler has a single slot, and a record constructed while
// another is live gets no slot and writes no file. A record that could not be written is
// silently absent -- there is nothing to report it to that the next start would read.
class ApplyingRecord {
public:
    ApplyingRecord(const std::filesystem::path& stateDir, int monitorId, const std::string& type,
                   const std::string& path);
    ~ApplyingRecord();

    ApplyingRecord(const ApplyingRecord&) = delete;
    ApplyingRecord& operator=(const ApplyingRecord&) = delete;

private:
    std::string m_file;
    int m_fd = -1;
};

struct VideoOnMonitor {
    int monitorId = 0;
    std::string path;
};

// A video, and how many of the daemon's deaths since it was last set were caught while working on
// it.
struct PlayingVideo {
    int monitorId = 0;
    std::string path;
    int deaths = 0;
};

// What the last run left in its playing record.
struct PlayingHistory {
    // The counts it carried in from the runs before it.
    std::vector<PlayingVideo> videos;

    // The video its render thread was working on when a fatal signal killed it, if any.
    std::optional<VideoOnMonitor> died;
    std::optional<int> signal;
};

// <stateDir>/playing.json. Empty when `stateDir` is.
std::filesystem::path playingRecordPath(const std::filesystem::path& stateDir);

// Nothing when there is none or it cannot be read, for readCrashRecord()'s reason. A monitor whose
// count that run cleared, as PlayingRecord::clear() recorded, carries none.
std::optional<PlayingHistory> readPlayingRecord(const std::filesystem::path& stateDir);

// The reason a client reads for a video refused after `deaths` deaths caught while playing it.
// `signal` names the last of them, and only when that death was this video's.
std::string describePlaybackDeaths(int deaths, std::optional<int> signal);

// The line the fatal-signal handler writes into the playing record to name a video. Formatted in
// advance because the handler can format nothing; empty when the path cannot be recorded.
std::string playingBlameLine(int monitorId, const std::string& path);

// This run's playing record: on disk and held open from start() to end(), so the fatal-signal
// handler can name the video that killed it without anything being written while videos play.
// One live at a time: a second start() in the process gets no slot and writes nothing.
class PlayingRecord {
public:
    explicit PlayingRecord(std::filesystem::path stateDir);
    ~PlayingRecord();

    PlayingRecord(const PlayingRecord&) = delete;
    PlayingRecord& operator=(const PlayingRecord&) = delete;

    // Replaces what the last run left with the counts in `videos`. Call once, and only once this
    // process is the daemon: a second instance must not overwrite the running one's record.
    void start(const std::vector<PlayingVideo>& videos);

    // Zeroes `monitorId`'s count from the next start on: a client has set its wallpaper again, or
    // it has played cleanly long enough. Appended to a file of its own, since the handler may be
    // writing into this record's descriptor, and at most once per counted monitor per run, which
    // bounds that file. Blocks on the write; a count lost to a failed write stands. Never throws.
    void clear(int monitorId) noexcept;

    // Withdraws the record from the handler, so a death in the teardown after an orderly stop is
    // counted for nobody. The counts stay on disk. Idempotent, and safe while a thread is working
    // on a video.
    void end();

private:
    const std::filesystem::path m_file;
    const std::filesystem::path m_clearedFile;
    int m_fd = -1;

    // This run's name, which clear() writes beside each monitor so a file left from another run is
    // never read as this one's.
    std::string m_run;
    std::mutex m_mutex;
    std::set<int> m_counted;  // monitors whose count start() wrote and clear() has not zeroed
};

// While one exists, a fatal signal on the thread that made it writes `line` into the playing
// record: that thread is working on the video `line` names. A death on any other thread names
// nothing. `line` must be a playingBlameLine() and outlive the guard; guards nest on one thread.
class PlayingBlame {
public:
    explicit PlayingBlame(const std::string& line);
    ~PlayingBlame();

    PlayingBlame(const PlayingBlame&) = delete;
    PlayingBlame& operator=(const PlayingBlame&) = delete;

private:
    const std::string* m_previous;
    pid_t m_previousTid;
};

// SIGSEGV, SIGBUS, SIGILL, SIGFPE and SIGABRT: stamps the live record with the signal when the
// dying thread is the one applying, removes it when another thread dies -- the wallpaper being
// applied is not what killed that one -- and names in the playing record the video the dying
// thread's PlayingBlame names, if any. Then hands the signal to whatever handler was there before
// and re-raises it. Call once, before any thread exists.
void installFatalSignalHandlers();

}
