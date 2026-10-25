// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "record_file.h"
#include "video/software_scan.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace wallpaperx {

// Whether a video wallpaper's pictures are decoded by the processor, as GetStatus reports it: still being found out,
// not at all, in part, all of them because none could reach the device from the opening on, or not known because its
// file could not be read through.
enum class ProcessorDecoding { Checking, None, Parts, All, Unknown };
const char* toString(ProcessorDecoding decoding);

// A scan's answer, and what the file's path named before and after it was read, when the two agreed: the bytes the
// answer may be saved against.
struct FileScan {
    SoftwareStretches found;
    std::optional<FileIdentity> file;

    // Whether this answer is about the bytes its path names `now`; never when either is unknown.
    bool foundFor(const std::optional<FileIdentity>& now) const { return file && file == now; }
};

// What the check of the wallpaper `forSeq` names has found: its file's scan, and how the first picture drawn for it
// was decoded. Keyed by the wallpaper, so a record for one the monitor no longer holds says nothing.
struct ProcessorUse {
    // Until the scan answers. `previous` is an answer found for the path before -- saved by an earlier run, or held
    // for a wallpaper on it -- which the scan reuses while the path still names its bytes, and the record keeps
    // meanwhile.
    struct Scanning {
        std::optional<FileScan> previous;
    };
    struct ScanFailed {};
    // How the first picture drawn was decoded: on the device; in software, for something the decoder met in the
    // stream; or in software the decoder chose for the whole stream before any picture, which nothing hands back to
    // the device.
    enum class FirstPicture { Device, Software, SoftwareThroughout };

    uint64_t forSeq = 0;
    std::variant<Scanning, ScanFailed, FileScan> scan;
    std::optional<FirstPicture> firstPicture;
};

struct ProcessorDecodingStatus {
    ProcessorDecoding state = ProcessorDecoding::Checking;
    // Why, when Parts: "first_picture", "missing_reference", "parameter_set_change". None with All: its reason is
    // the state.
    std::vector<const char*> reasons;
};

// `inlineParameters` is Device::videoInlineParameters(), nothing before a device exists. None needs every part
// known: the scan, the first picture, and the device where the scan found a parameter-set change.
ProcessorDecodingStatus processorDecoding(const ProcessorUse& use, std::optional<bool> inlineParameters);

// A notice called from several threads that its owner can withdraw: empty stops it, waiting for a call in
// progress, so whoever clears it must not hold a lock the callback takes. The callback must not throw.
class ChangeNotice {
public:
    void set(std::function<void()> changed);
    void operator()() noexcept;

private:
    std::mutex m_mutex;
    std::function<void()> m_changed;
};

// Scans the video wallpapers set, one file at a time, on a thread of its own at the lowest processor and disk
// priority, started by the first check. Never on the GPU thread and never decoding: a scan is one read of the
// file's packets, given up by the next check() or forget() for its monitor, or when this object is destroyed. A
// file several queued checks name is read once while they last: each later one is given the answer while the path
// still names the bytes it was found for.
class ProcessorCheck {
public:
    // Why the scan failed when it did, for the log.
    using Found = std::variant<FileScan, std::string>;
    // On the scan's thread, holding none of this object's locks, for every check that was not replaced or stopped.
    using Done = std::function<void(int monitorId, uint64_t wallpaperSeq, Found found)>;

    explicit ProcessorCheck(Done done);
    ~ProcessorCheck();

    ProcessorCheck(const ProcessorCheck&) = delete;
    ProcessorCheck& operator=(const ProcessorCheck&) = delete;

    // Checks `path`, set on `monitorId` as wallpaper `wallpaperSeq`, replacing whatever check that monitor still
    // had queued or running. `blame` is the crash record's line for it: a file whose demuxing kills the thread is
    // counted against the video, as one that kills a decode is. `saved` is an answer found for this path before,
    // given instead of reading the file when the path still names the bytes it was found for. Without a thread to
    // run on, `done` is called here, failed, so the caller must not hold a lock it takes. Throws std::bad_alloc.
    void check(int monitorId, uint64_t wallpaperSeq, std::string path, std::string blame,
               std::optional<FileScan> saved = std::nullopt);

    // Gives up whatever check `monitorId` has queued or running, for a monitor given a wallpaper that needs none.
    // Does not wait for a running scan to stop: one already ending still reports to `done`.
    void forget(int monitorId) noexcept;

private:
    struct Job {
        int monitorId = 0;
        uint64_t wallpaperSeq = 0;
        std::string path;
        std::string blame;
        std::optional<FileScan> saved;
    };

    void run();
    void forgetLocked(int monitorId) noexcept;

    Done m_done;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<Job> m_queue;
    // The monitor whose scan is running, and that scan's stop.
    std::optional<int> m_running;
    std::atomic<bool> m_stopRunning{false};
    bool m_stop = false;
    std::thread m_thread;
};

}
