// SPDX-License-Identifier: GPL-2.0-or-later

// The refusal half of the crash-loop breaker: what a start does when it finds, in the state
// directory, a record saying the last run died applying one of the wallpapers it is about to
// apply.
//
// Driven through WallpaperManager directly -- restoreSaved(), renderPending(),
// exportCurrentFrame() -- rather than over the bus, for the reason http_server_test's
// IsolatedDaemon gives: the state being graded is per-monitor and permanent, so building it on
// a shared daemon would change what every later check reads. No render worker exists here, so
// the Device is created, used and destroyed on this one thread, which is what the Device's own
// thread pinning requires.
//
// Whether a wallpaper was DECODED is watched with inotify rather than inferred from status: a
// build that reported a refusal and then decoded the file anyway -- which is exactly the
// crash loop this feature exists to stop -- produces the same last_error either way. The watch
// is proved live in the same run before its silence is read as evidence.
//
// The state directory is a scratch directory this file owns. WallpaperManager::Config defaults
// it to $XDG_STATE_HOME, so a test that says nothing about it plants records in the
// developer's own state directory and reads whatever is already there.
//
// Validation layers are deliberately not enabled: nothing here grades what the GPU draws, only
// whether a file was opened and what the daemon recorded. The renderer's own probes and
// dbus_service_test carry the validation oracle.

#include "daemon/crash_record.h"
#include "daemon/wallpaper_manager.h"

#include "vulkan/frame_export.h"
#include "vulkan/offscreen.h"

#include "support/test_image.h"
#include "tests/client/scratch_dir.h"
#include "tests/test_report.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <sys/inotify.h>
#include <unistd.h>

using wallpaperx::CrashRecord;
using wallpaperx::crashRecordPath;
using wallpaperx::NoFrameReason;
using wallpaperx::readCrashRecord;
using wallpaperx::WallpaperManager;
using wallpaperx::testing::check;
using wallpaperx::testing::harnessFailure;
using wallpaperx::testing::notVerified;
using wallpaperx::testing::ScratchDir;

namespace {

// The monitors this run builds, each in a state named by its constant so a failure line and
// the setup that produced it can be matched without counting.
constexpr int kCrashedMonitor = 0;      // a stamped record naming exactly what it is set to
constexpr int kMovedOnMonitor = 1;      // a record naming a wallpaper this row no longer holds
constexpr int kKilledMonitor = 2;       // a record with no signal: the process was killed
constexpr int kOtherTypeMonitor = 3;    // a record naming another type for the same path
constexpr int kUnsupportedMonitor = 4;  // a video row -- this build renders no video -- and a
                                        // stale record beside it

// Small: every render here is a software rasteriser on some hosts, and nothing grades pixels.
constexpr uint32_t kWidth = 64;
constexpr uint32_t kHeight = 64;

// ---------------------------------------------------------------------------- //
// Was the file opened?
// ---------------------------------------------------------------------------- //

// Counts opens of one file, through inotify.
//
// The property "this wallpaper was not decoded" has no other observable: status, health and
// GetFrameFd all report the refusal, and would report it identically for a build that refused
// in status and decoded the file anyway. stat(2) times are no good either -- atime is
// relatime on most mounts, so a second read within the day updates nothing.
class OpenWatch {
public:
    explicit OpenWatch(const std::filesystem::path& file) {
        m_fd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (m_fd < 0) return;
        m_wd = ::inotify_add_watch(m_fd, file.c_str(), IN_OPEN);
        if (m_wd < 0) {
            ::close(m_fd);
            m_fd = -1;
        }
    }

    ~OpenWatch() {
        if (m_fd >= 0) ::close(m_fd);
    }

    OpenWatch(const OpenWatch&) = delete;
    OpenWatch& operator=(const OpenWatch&) = delete;

    bool ok() const { return m_fd >= 0; }

    // Opens seen since the last call. Non-blocking: the events for an open that has already
    // happened are in the queue by the time the call that caused it has returned, and a
    // blocking read here would turn "the file was never opened" -- the expected answer -- into
    // a hang.
    int drain() {
        if (m_fd < 0) return -1;
        int opens = 0;
        for (;;) {
            alignas(inotify_event) char buffer[4096];
            const ssize_t n = ::read(m_fd, buffer, sizeof(buffer));
            if (n <= 0) {
                if (n < 0 && errno == EINTR) continue;
                break;  // EAGAIN: the queue is empty
            }
            for (ssize_t offset = 0; offset + static_cast<ssize_t>(sizeof(inotify_event)) <= n;) {
                inotify_event event{};
                std::memcpy(&event, buffer + offset, sizeof(event));
                if ((event.mask & IN_OPEN) != 0) ++opens;
                offset += static_cast<ssize_t>(sizeof(inotify_event)) + event.len;
            }
        }
        return opens;
    }

private:
    int m_fd = -1;
    int m_wd = -1;
};

// ---------------------------------------------------------------------------- //
// Fixtures
// ---------------------------------------------------------------------------- //

void writeText(const std::filesystem::path& file, const std::string& text) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
}

// A record as the last run's handler would have left it: the JSON line, and a signal line only
// when a handler ran. Planted rather than produced by crashing a daemon, because what is being
// graded here is what a start DOES with one; that the handler writes exactly this shape is
// crash_record_test's job, and it grades it against real signals in forked children.
void plantRecord(const std::filesystem::path& stateDir, int monitorId, const std::string& type,
                 const std::string& path, std::optional<int> signal) {
    nlohmann::json line{{"monitor_id", monitorId}, {"type", type}, {"path", path}};
    std::string text = line.dump() + "\n";
    if (signal) text += "signal " + std::to_string(*signal) + "\n";
    writeText(crashRecordPath(stateDir, monitorId), text);
}

std::string configJson(const std::vector<std::tuple<int, std::string, std::string>>& rows) {
    nlohmann::json monitors = nlohmann::json::array();
    for (const auto& [id, type, path] : rows) {
        monitors.push_back({{"monitor_id", id}, {"type", type}, {"path", path}});
    }
    return nlohmann::json{{"version", 1}, {"monitors", std::move(monitors)}}.dump();
}

bool recordOnDisk(const std::filesystem::path& stateDir, int monitorId) {
    std::error_code ec;
    return std::filesystem::exists(crashRecordPath(stateDir, monitorId), ec);
}

// A COPY, not a pointer into the snapshot. Every caller here reads its monitor out of
// `manager.snapshot()` -- a temporary that dies at the end of the full expression -- so a
// pointer-returning version makes a dangling read the natural way to write the call. It was
// written that way first, and ThreadSanitizer caught it as a heap-use-after-free.
std::optional<wallpaperx::MonitorStatus> monitor(const WallpaperManager::Snapshot& snap, int id) {
    const auto it = snap.monitors.find(id);
    if (it == snap.monitors.end()) return std::nullopt;
    return it->second;
}

}  // namespace

int main() {
    wallpaperx::testing::g_testName = "crash_quarantine_test";

    // One epilogue for every exit, including the early ones. A `return 1` at a setup failure
    // is red, but it skips the summary line that says which channel went red -- and a reader
    // who sees only "Failed" goes looking in the daemon for a fault in this file.
    const auto finish = [] {
        if (wallpaperx::testing::reportCheckFailures()) return 1;
        if (wallpaperx::testing::reportHarnessFaults("in this test file, not in the daemon")) return 1;
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    };

    ScratchDir scratch("quarantine");
    if (!scratch.ok()) {
        notVerified("S13.13-S13.19", "no scratch directory could be created for the config and state files");
        return finish();
    }

    const std::filesystem::path root = scratch.path();
    const std::filesystem::path stateDir = root / "state" / "wallpaperx";
    const std::filesystem::path configFile = root / "config" / "wallpaperx" / "config.json";

    std::string crashedImage, movedOnImage, killedImage, otherTypeImage, videoRow, notAnImage;
    try {
        crashedImage = wallpaperx::testing::writeGradientPng((root / "crashed.png").string(), 16, 16);
        movedOnImage = wallpaperx::testing::writeGradientPng((root / "moved-on.png").string(), 16, 16);
        killedImage = wallpaperx::testing::writeGradientPng((root / "killed.png").string(), 16, 16);
        otherTypeImage = wallpaperx::testing::writeGradientPng((root / "other-type.png").string(), 16, 16);
        videoRow = (root / "clip.mp4").string();
        writeText(videoRow, "not really a video, and this build renders none");
        notAnImage = (root / "text.png").string();
        writeText(notAnImage, "this is not a PNG");
    } catch (const std::exception& e) {
        harnessFailure(std::string("the fixture images could not be written: ") + e.what());
        return finish();
    }

    writeText(configFile, configJson({
                              {kCrashedMonitor, "static", crashedImage},
                              {kMovedOnMonitor, "static", movedOnImage},
                              {kKilledMonitor, "static", killedImage},
                              {kOtherTypeMonitor, "static", otherTypeImage},
                              {kUnsupportedMonitor, "video", videoRow},
                          }));

    plantRecord(stateDir, kCrashedMonitor, "static", crashedImage, SIGSEGV);
    plantRecord(stateDir, kMovedOnMonitor, "static", (root / "a-wallpaper-since-replaced.png").string(),
                SIGSEGV);
    plantRecord(stateDir, kKilledMonitor, "static", killedImage, std::nullopt);
    plantRecord(stateDir, kOtherTypeMonitor, "video", otherTypeImage, SIGSEGV);
    plantRecord(stateDir, kUnsupportedMonitor, "video", (root / "another-clip.mp4").string(), SIGSEGV);

    WallpaperManager::Config config;
    config.configFile = configFile;
    config.stateDir = stateDir;
    config.width = kWidth;
    config.height = kHeight;

    try {
        WallpaperManager manager(config);
        const std::vector<int> restored = manager.restoreSaved();
        check(restored.size() == 5, "every saved monitor is posted for a pass, refused or not");

        // ------------------------------------------------------------------ //
        // What the restore decided, before anything renders
        // ------------------------------------------------------------------ //

        // GOAL: S13.13 -- a start that finds a record matching a saved row refuses that
        //       wallpaper: last_error says the last run did not survive applying it and how to
        //       retry, no frame, and NO RENDER OUTSTANDING. False if the daemon reports the
        //       refusal and still owes a render, which is a daemon that will draw it.
        // WHY-THIS-SHAPE: renderOutstanding is checked, not just last_error. A build that set
        //       the message and left the request unserviced would satisfy every text
        //       assertion, then decode the file on the first pass -- the crash loop, with a
        //       message on top of it.
        // CONTROL: in backend/daemon/wallpaper_manager.cpp, restoreSaved()'s
        //       `state.quarantined = crash && ...` replaced with `= false` -> 16 FAIL lines,
        //       from "a monitor whose record matches its wallpaper is refused with a reason"
        //       through "the refused wallpaper is never opened, let alone decoded". Reverted.
        {
            const WallpaperManager::Snapshot snap = manager.snapshot();
            const std::optional<wallpaperx::MonitorStatus> crashed = monitor(snap, kCrashedMonitor);
            check(crashed.has_value(), "the crashed monitor was restored at all");
            if (crashed.has_value()) {
                check(!crashed->lastError.empty(),
                      "a monitor whose record matches its wallpaper is refused with a reason");
                check(crashed->lastError.find("SIGSEGV") != std::string::npos,
                      "the refusal names the signal the last run died of");
                check(crashed->lastError.find("set it again") != std::string::npos,
                      "the refusal tells the user how to retry");
                check(!crashed->hasFrame, "a refused monitor has no frame");
                check(!crashed->renderOutstanding, "a refused monitor owes no render");
                check(!crashed->lastErrorIsDaemonFault,
                      "a refusal is not counted as the daemon's own fault, so /health still reports the "
                      "daemon itself as ok");
                std::printf("crash_quarantine_test: monitor %d reports -- %s\n", kCrashedMonitor,
                            crashed->lastError.c_str());
            }

            // GOAL: S13.17 -- a record with no signal (the process was killed: the
            //       out-of-memory killer, a teardown alarm, a power cut) refuses too, and says
            //       it was killed rather than naming a signal it never saw.
            // CONTROL: in backend/daemon/crash_record.cpp, describeCrash()'s unstamped branch
            //       made to return the stamped wording -> "FAIL: an unstamped record's refusal
            //       says the daemon was killed, not that it crashed with a named signal".
            //       Reverted.
            const std::optional<wallpaperx::MonitorStatus> killed = monitor(snap, kKilledMonitor);
            check(killed.has_value() && !killed->lastError.empty(),
                  "a record with no signal refuses its wallpaper too");
            if (killed.has_value()) {
                check(killed->lastError.find("killed") != std::string::npos &&
                          killed->lastError.find("SIG") == std::string::npos,
                      "an unstamped record's refusal says the daemon was killed, not that it crashed with a "
                      "named signal");
                check(!killed->renderOutstanding, "a monitor refused on an unstamped record owes no render");
            }

            // GOAL: S13.14 -- a record that does not match its row refuses nothing. Two ways
            //       not to match, and both are graded: a different path (the user set another
            //       wallpaper after the crash) and a different type for the same path.
            // WHY-THIS-SHAPE: the type half is not redundant. Matching on path alone would
            //       pass every path check here and still refuse a file whose type changed --
            //       which is the same file going through a different decoder.
            // CONTROL: in backend/daemon/wallpaper_manager.cpp, the match narrowed to
            //       `crash.has_value()` -> "FAIL: a monitor whose record names another
            //       wallpaper is applied as normal" and "...another type...". Reverted.
            const std::optional<wallpaperx::MonitorStatus> movedOn = monitor(snap, kMovedOnMonitor);
            check(movedOn.has_value() && movedOn->lastError.empty() && movedOn->renderOutstanding,
                  "a monitor whose record names another wallpaper is applied as normal");
            const std::optional<wallpaperx::MonitorStatus> otherType = monitor(snap, kOtherTypeMonitor);
            check(otherType.has_value() && otherType->lastError.empty() && otherType->renderOutstanding,
                  "a monitor whose record names another type for the same file is applied as normal");
        }

        // ------------------------------------------------------------------ //
        // The passes: what is decoded and what is not
        // ------------------------------------------------------------------ //

        OpenWatch crashedWatch(crashedImage);
        OpenWatch movedOnWatch(movedOnImage);
        if (!crashedWatch.ok() || !movedOnWatch.ok()) {
            notVerified("S13.13 and S13.15 (the not-decoded half)",
                        "inotify could not watch the fixture images on this host, so whether a file was "
                        "opened cannot be observed here");
        }

        for (int id : restored) manager.renderPending(id);

        const WallpaperManager::Snapshot afterPasses = manager.snapshot();
        const std::optional<wallpaperx::MonitorStatus> control = monitor(afterPasses, kMovedOnMonitor);
        const bool gpuDrew = control.has_value() && control->hasFrame;
        if (!gpuDrew) {
            notVerified("the goals that need a completed render",
                        std::string("no monitor in this run rendered a frame; the unrefused monitor reported: ") +
                            (control.has_value() && !control->lastError.empty() ? control->lastError
                                                                              : "nothing at all"));
        }

        // GOAL: S13.13 -- the refused wallpaper is not DECODED. False if the file is opened at
        //       all, which is what a build that quarantines in status and renders anyway does.
        // WHY-THIS-SHAPE: an inotify watch, not a status field, for the reason above the
        //       OpenWatch class. The count for the refused file is only read as evidence after
        //       the instrument has been shown to move: first against an unrefused monitor's
        //       real render, then against an open this harness performs itself. Without that,
        //       "zero opens" is equally consistent with a watch that never armed.
        // COVERAGE-ASSERT: the harness opens the refused file itself and requires the watch to
        //       report it. A watch that silently stopped working makes THAT check fail rather
        //       than making the check above pass. Controlled by pointing that deliberate open
        //       at the other fixture instead -> "FAIL: the watch that reports the refused
        //       wallpaper unopened does report an open when there is one -- the harness opened
        //       the file and the watch counted 0". Restored.
        // CONTROL: two lines of backend/daemon/wallpaper_manager.cpp, because either one alone
        //       still stops the decode and neither is dead: restoreSaved()'s
        //       `state.servicedSeq = state.requestSeq` (which makes the first pass a no-op for
        //       a request nothing owes) and renderPending()'s `if (it->second.quarantined)`
        //       early return (which is what stops a RE-POSTED refusal, and goes red on its own
        //       in the import-modifier checks below). With both removed: "FAIL: the refused
        //       wallpaper is never opened, let alone decoded -- the refused file was opened 1
        //       time(s); the rendered one 1", 11 FAIL lines in all. Both reverted.
        if (crashedWatch.ok() && movedOnWatch.ok()) {
            const int refusedOpens = crashedWatch.drain();
            const int controlOpens = movedOnWatch.drain();
            if (gpuDrew && controlOpens <= 0) {
                harnessFailure("the inotify watch saw no open of a wallpaper that did render, so it cannot "
                               "tell a refused wallpaper from a decoded one and the check below establishes "
                               "nothing");
            }
            // Graded on every host, not only where a render completed: the refused monitor's
            // pass returns before the device is touched, so a machine with no working driver
            // still exercises it -- and the instrument is proved live a few lines down by an
            // open this harness performs, which needs no GPU either.
            check(refusedOpens == 0, "the refused wallpaper is never opened, let alone decoded");
            if (refusedOpens != 0) {
                std::fprintf(stderr, "  the refused file was opened %d time(s); the rendered one %d\n",
                             refusedOpens, controlOpens);
            }

            // The instrument, proved on the very file the check above reads. Runs whether or
            // not a GPU is present, so a run that could not render still says whether its
            // silence meant anything.
            { const std::ifstream probe(crashedImage, std::ios::binary); }
            const int probeOpens = crashedWatch.drain();
            check(probeOpens >= 1,
                  "the watch that reports the refused wallpaper unopened does report an open when there is "
                  "one");
            if (probeOpens < 1) {
                std::fprintf(stderr, "  the harness opened the file and the watch counted %d\n", probeOpens);
            }
        }

        {
            const std::optional<wallpaperx::MonitorStatus> crashed = monitor(afterPasses, kCrashedMonitor);
            check(crashed.has_value() && !crashed->hasFrame && !crashed->lastError.empty(),
                  "a refused monitor still has no frame after its pass, and still says why");
            check(crashed.has_value() && !crashed->renderOutstanding,
                  "a refused monitor's pass leaves nothing outstanding");

            // GOAL: the record survives the refusal, so a second restart refuses again. False
            //       if the first start consumed the record, which would make the second start
            //       re-apply the wallpaper and crash -- the loop, one restart later.
            // CONTROL: in backend/daemon/wallpaper_manager.cpp, `removeCrashRecord(m_config.stateDir,
            //       entry.monitorId)` added to restoreSaved()'s quarantine branch -> "FAIL: the
            //       record stays on disk, so a second restart refuses too" and the unstamped one
            //       beside it. Reverted. (The same call added to renderPending()'s quarantined
            //       branch instead reddens only the two re-post checks further down, because the
            //       first pass returns earlier -- at the already-serviced test.)
            check(recordOnDisk(stateDir, kCrashedMonitor),
                  "the record stays on disk, so a second restart refuses too");
            check(recordOnDisk(stateDir, kKilledMonitor),
                  "an unstamped record stays on disk too, for the same reason");
        }

        // GOAL: S13.14's second half -- a record that refused nothing does not outlive the
        //       render that ran in spite of it. False if a stale record sits in the state
        //       directory until the wallpaper it names is set again, ready to refuse a file
        //       nobody crashed on.
        // CONTROL: the `::unlink(m_file.c_str())` in ~ApplyingRecord in
        //       backend/daemon/crash_record.cpp deleted -> "FAIL: a stale record is replaced
        //       and then removed by the render that ran anyway", with the two completed-render
        //       checks further down. Reverted.
        if (gpuDrew) {
            check(!recordOnDisk(stateDir, kMovedOnMonitor),
                  "a stale record is replaced and then removed by the render that ran anyway");
        }

        // GOAL: a stale record on a row this build cannot render is removed by its pass, not
        //       left for a future build that CAN render that type to refuse a wallpaper the
        //       row no longer holds.
        // CONTROL: in backend/daemon/wallpaper_manager.cpp, the removeCrashRecord() call in
        //       renderPending()'s unsupported-type branch deleted -> "FAIL: a stale record on
        //       an unrenderable row is removed by its pass". Reverted.
        check(!recordOnDisk(stateDir, kUnsupportedMonitor),
              "a stale record on an unrenderable row is removed by its pass");

        // GOAL: S13.13 -- GetFrameFd's answer for a refused monitor is RenderFailed carrying
        //       the reason, not "never requested" and not silence. False if a frontend has to
        //       show an empty screen with nothing to display.
        // CONTROL: in backend/daemon/wallpaper_manager.cpp, the `state.lastError = ...`
        //       assignment in restoreSaved()'s quarantine branch deleted -> the export answers
        //       NotRendered, "FAIL: a refused monitor's frame request answers RenderFailed"
        //       plus the reason check. Reverted.
        {
            const wallpaperx::FrameExport exported = manager.exportCurrentFrame(kCrashedMonitor);
            check(!exported.handle.has_value(), "a refused monitor hands out no frame");
            check(exported.reason == NoFrameReason::RenderFailed,
                  "a refused monitor's frame request answers RenderFailed");
            check(exported.lastError.find("SIGSEGV") != std::string::npos,
                  "the frame request's answer carries the reason the wallpaper was refused");
        }

        // ------------------------------------------------------------------ //
        // S13.15: a re-post that is not an explicit request
        // ------------------------------------------------------------------ //

        // GOAL: S13.15 -- a re-post for any reason other than an explicit request does not
        //       decode a refused wallpaper, and the record survives it. The import-modifier
        //       report is the one every desktop session sends at startup, and it bumps the
        //       monitor's request sequence, so the refusal has to survive a re-post that looks
        //       exactly like new work.
        // WHY-THIS-SHAPE: the returned monitor list is asserted to CONTAIN the refused monitor
        //       before the pass is run. Without that, a build that simply stopped re-posting
        //       would pass every check below while the check proved nothing about the refusal
        //       -- the pass it grades would never have happened.
        // CONTROL: in backend/daemon/wallpaper_manager.cpp, renderPending()'s quarantined
        //       branch deleted -> "FAIL: an import-modifier report does not decode a refused
        //       wallpaper" and "FAIL: the record survives an import-modifier report". Reverted.
        {
            // Taken from the daemon's own answer rather than written as a fourcc here: a
            // report for any other format is refused outright, so a hardcoded constant would
            // turn a change of exported format into this phase throwing instead of running.
            const uint32_t exported =
                wallpaperx::drmFourccForVkFormat(wallpaperx::OffscreenTargetManager::kFormat);
            const wallpaperx::ImportModifierUpdate update =
                manager.setImportModifiers(":1.5", exported, {0 /* DRM_FORMAT_MOD_LINEAR */});
            const bool reposted = std::find(update.toRender.begin(), update.toRender.end(), kCrashedMonitor) !=
                                  update.toRender.end();
            check(reposted,
                  "an import-modifier report re-posts the refused monitor, so the check below is about a "
                  "pass that really ran");
            if (reposted) {
                for (int id : update.toRender) manager.renderPending(id);
                if (crashedWatch.ok()) {
                    check(crashedWatch.drain() == 0,
                          "an import-modifier report does not decode a refused wallpaper");
                }
                check(recordOnDisk(stateDir, kCrashedMonitor),
                      "the record survives an import-modifier report");
                const std::optional<wallpaperx::MonitorStatus> crashed = monitor(manager.snapshot(), kCrashedMonitor);
                check(crashed.has_value() && !crashed->hasFrame && !crashed->lastError.empty() &&
                          !crashed->renderOutstanding,
                      "a refused monitor is still refused, and owes nothing, after an import-modifier report");
            }
        }

        // The same for the withdrawal a consumer's departure from the bus causes, which goes
        // through the same re-post path from the other direction.
        {
            const wallpaperx::ImportModifierUpdate update = manager.forgetImportModifiers(":1.5");
            const bool reposted = std::find(update.toRender.begin(), update.toRender.end(), kCrashedMonitor) !=
                                  update.toRender.end();
            check(reposted, "a consumer leaving the bus re-posts the refused monitor too");
            if (reposted) {
                for (int id : update.toRender) manager.renderPending(id);
                if (crashedWatch.ok()) {
                    check(crashedWatch.drain() == 0,
                          "a consumer leaving the bus does not decode a refused wallpaper");
                }
                check(recordOnDisk(stateDir, kCrashedMonitor),
                      "the record survives a consumer leaving the bus");
            }
        }

        // GOAL: S13.15's third path -- a released image re-posts only a monitor that owes a
        //       render, and a refused one is marked serviced, so it is never posted at all.
        //       Stated as "asks for no work" rather than "does not decode": there is no pass
        //       to observe, which is the point.
        // WHY-THIS-SHAPE: no hold can exist on a refused monitor -- it has no frame to hand
        //       over -- so the release path cannot be driven with a real claim. What is graded
        //       is that neither release entry point returns the refused monitor as work.
        {
            std::vector<wallpaperx::FrameAvailable> announce;
            const std::vector<int> afterRelease = manager.releaseBuffer(":1.5", kCrashedMonitor, 0, &announce);
            check(afterRelease.empty(), "releasing an image asks for no work on a refused monitor");
            const std::vector<int> afterDeparture = manager.forgetHeldBuffers(":1.5", &announce);
            check(std::find(afterDeparture.begin(), afterDeparture.end(), kCrashedMonitor) ==
                      afterDeparture.end(),
                  "the sweep for a departed consumer asks for no work on a refused monitor");
            check(std::none_of(announce.begin(), announce.end(),
                               [](const wallpaperx::FrameAvailable& entry) {
                                   return entry.monitorId == kCrashedMonitor;
                               }),
                  "nothing announces a frame for a refused monitor, which has none");
        }

        // ------------------------------------------------------------------ //
        // S13.16: the user's way out
        // ------------------------------------------------------------------ //

        // GOAL: S13.16 -- an explicit SetWallpaper of the same file is applied: the refusal is
        //       cleared, the render runs, the file IS decoded this time, and the completed
        //       render leaves no record. False if the user cannot retry from the app, or if a
        //       retry that worked leaves a record the next start honours.
        // WHY-THIS-SHAPE: the same path as the refusal, not a different picture. A retry with
        //       another file would be cleared by the path-match rule alone and would say
        //       nothing about whether an explicit request overrides a record that matches.
        //       And the render is graded, not just the cleared message: with
        //       `state.quarantined = false;` deleted from requestWallpaper() in
        //       backend/daemon/wallpaper_manager.cpp, the two status checks below still PASS --
        //       requestWallpaper clears last_error and bumps the sequence on its own -- while
        //       the retry silently renders nothing. The status half alone would be vacuous.
        // CONTROL: that deletion -> "FAIL: the wallpaper the last run died applying renders
        //       when the user asks for it again" (with "the retry reported:" and an empty
        //       reason), "FAIL: the retry actually opens the file the refusal protected",
        //       "FAIL: a completed render leaves no record for the next start to refuse", and
        //       the two clean-failure checks after it: 5 in all. Reverted.
        manager.requestWallpaper("static", crashedImage, kCrashedMonitor);
        {
            const std::optional<wallpaperx::MonitorStatus> crashed = monitor(manager.snapshot(), kCrashedMonitor);
            check(crashed.has_value() && crashed->lastError.empty(), "an explicit request clears the refusal");
            check(crashed.has_value() && crashed->renderOutstanding,
                  "an explicit request leaves a render owed, where the refusal owed none");
        }
        manager.renderPending(kCrashedMonitor);
        if (gpuDrew) {
            const std::optional<wallpaperx::MonitorStatus> crashed = monitor(manager.snapshot(), kCrashedMonitor);
            check(crashed.has_value() && crashed->hasFrame,
                  "the wallpaper the last run died applying renders when the user asks for it again");
            if (crashed.has_value() && !crashed->hasFrame) {
                std::fprintf(stderr, "  the retry reported: %s\n", crashed->lastError.c_str());
            }
            if (crashedWatch.ok()) {
                check(crashedWatch.drain() >= 1, "the retry actually opens the file the refusal protected");
            }
            check(!recordOnDisk(stateDir, kCrashedMonitor),
                  "a completed render leaves no record for the next start to refuse");
        }

        // GOAL: a render that fails cleanly -- an unreadable image, not a crash -- also leaves
        //       no record. False if a file that merely fails to decode is refused on the next
        //       start as though it had killed the daemon.
        // CONTROL: the ~ApplyingRecord unlink deleted in backend/daemon/crash_record.cpp ->
        //       "FAIL: a render that failed cleanly leaves no record either". Reverted.
        if (gpuDrew) {
            manager.requestWallpaper("static", notAnImage, kCrashedMonitor);
            manager.renderPending(kCrashedMonitor);
            const std::optional<wallpaperx::MonitorStatus> crashed = monitor(manager.snapshot(), kCrashedMonitor);
            check(crashed.has_value() && !crashed->hasFrame && !crashed->lastError.empty(),
                  "a file that is not an image fails cleanly");
            check(!recordOnDisk(stateDir, kCrashedMonitor),
                  "a render that failed cleanly leaves no record either");
        }
    } catch (const std::exception& e) {
        harnessFailure(std::string("the run ended in an unhandled exception: ") + e.what());
    }

    return finish();
}
