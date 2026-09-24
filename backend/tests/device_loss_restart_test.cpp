// SPDX-License-Identifier: GPL-2.0-or-later

// The shipped daemon after its GPU device is lost (S6.41, S14.1, S14.4), from outside the process.
//
// A lost device is not recoverable inside one process, so the daemon exits 75 and relies on its
// service unit's Restart=on-failure and on its saved config to bring the wallpapers back. This
// file plays the service manager's part: it loses the device with a preloaded library
// (tests/device_lost_shim.cpp), waits for the exit, and starts the daemon again on the same HOME.
// What only this tier can grade is the pair -- that the exit is the one the unit restarts on, and
// that what it leaves on disk lets the next start redraw everything and blame nothing.
//
// Every daemon here runs with a HOME of its own and its XDG directories inside it, and on the
// private bus ctest gives it; main() refuses to run on any other bus.

#include "daemon/crash_record.h"
#include "daemon/dbus_service.h"

#include "support/test_image.h"
#include "tests/client/daemon_client.h"
#include "tests/client/scratch_dir.h"
#include "tests/name_watcher.h"
#include "tests/spawned_daemon.h"
#include "tests/test_report.h"

#include <sdbus-c++/sdbus-c++.h>

#include <nlohmann/json.hpp>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

using wallpaperx::DbusService;
using wallpaperx::testing::check;
using wallpaperx::testing::FrameReadyLog;
using wallpaperx::testing::getStatus;
using wallpaperx::testing::harnessFailure;
using wallpaperx::testing::NameWatcher;
using wallpaperx::testing::notVerified;
using wallpaperx::testing::ScratchDir;
using wallpaperx::testing::SpawnedDaemon;
using wallpaperx::testing::StatusReply;
using wallpaperx::testing::subscribeFrameReady;
using wallpaperx::testing::wallpaperStatus;

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

// EX_TEMPFAIL, restated rather than taken from <sysexits.h>: it is a contract with the service
// unit, and a daemon that changed it must fail here rather than be followed silently.
constexpr int kExitDeviceLost = 75;
constexpr const char* kLostStderr = "the GPU device was lost; exiting with status 75";
constexpr const char* kLostStdout = "stopped after losing the GPU device, exit status 75";
constexpr const char* kShimFired = "device_lost_shim: failing every queue submission from now on";
constexpr const char* kCleanStop = "stopped cleanly";
// daemon_system_test's kSignalShutdownLine.
constexpr const char* kSignalLine = "received, shutting down";

constexpr auto kWait = std::chrono::seconds(30);

// This file drives a daemon and a HOME of its own per phase, so the ids only need to be distinct
// within a phase.
constexpr int32_t kMonitorBlue = 0;
constexpr int32_t kMonitorRed = 1;
constexpr int32_t kMonitorTrigger = 2;
constexpr int32_t kMonitorVideo = 3;

constexpr int kVideoLosses = 3;

// A HOME for one phase, with the daemon's XDG directories inside it, so every file it reads or
// writes is under a directory this test owns and removes.
class Home {
public:
    explicit Home(const char* tag) : m_dir(tag) {}
    bool ok() const { return m_dir.ok(); }
    fs::path root() const { return m_dir.path(); }
    fs::path configHome() const { return root() / ".config"; }
    fs::path stateHome() const { return root() / ".local" / "state"; }
    fs::path stateDir() const { return stateHome() / "wallpaperx"; }
    fs::path configFile() const { return configHome() / "wallpaperx" / "config.json"; }
    fs::path trigger() const { return root() / "lose-device"; }

    // The shim reads WALLPAPERX_TEST_LOSE_DEVICE only when it is preloaded, so the variable is
    // harmless on a start without it.
    std::vector<std::string> env(bool loseDeviceOnTrigger) const {
        std::vector<std::string> out{
            // Off, as the shipped unit runs it: this file grades the process, not the API usage,
            // and daemon_system carries the validation oracle.
            "WALLPAPERX_VALIDATION=0",
            "HOME=" + root().string(),
            "XDG_CONFIG_HOME=" + configHome().string(),
            "XDG_STATE_HOME=" + stateHome().string(),
            "WALLPAPERX_TEST_LOSE_DEVICE=" + trigger().string(),
        };
        if (loseDeviceOnTrigger) {
            const char* inherited = std::getenv("LD_PRELOAD");
            std::string preload = WALLPAPERX_DEVICE_LOST_SHIM;
            if (inherited != nullptr && *inherited != '\0') preload = std::string(inherited) + ":" + preload;
            out.push_back("LD_PRELOAD=" + preload);
        }
        return out;
    }

private:
    ScratchDir m_dir;
};

// Kernel-chosen: nothing here reads the config server, and a fixed port could be the desktop's.
const std::vector<std::string> kDaemonArgs{"--config-port", "0"};

std::vector<std::string> applyingRecords(const fs::path& stateDir) {
    std::vector<std::string> found;
    std::error_code ec;
    for (auto it = fs::directory_iterator(stateDir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.rfind("applying-", 0) == 0) found.push_back(name);
    }
    return found;
}

bool touch(const fs::path& file) {
    std::ofstream out(file);
    return static_cast<bool>(out);
}

size_t occurrences(const std::string& text, const std::string& needle) {
    size_t count = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) ++count;
    return count;
}

// Waits for `needle` to have appeared `count` times. False when the daemon went away first or the
// deadline passed.
bool waitForOccurrences(const SpawnedDaemon& daemon, const std::string& needle, size_t count) {
    const auto deadline = Clock::now() + kWait;
    while (Clock::now() < deadline) {
        if (occurrences(daemon.output(), needle) >= count) return true;
        if (daemon.exited()) return occurrences(daemon.output(), needle) >= count;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

// A phase whose daemon could not be brought to the state it needs. A preloaded test library that
// could not find the driver function it forwards to kills the daemon at its first submission,
// which is this harness's fault, not the host's: reported as such rather than as a skip.
// CONTROL: by hand 2026-09-24, reverted: the shim's lookup made "vkQueueSubmitX" -> five "FAIL
//       (harness)" lines quoting the shim, where before this each phase was skipped as a host that
//       "never played the video".
void setupFailed(const SpawnedDaemon& daemon, const char* goals, const std::string& reason) {
    const std::string out = daemon.output();
    const size_t at = out.find("cannot resolve the real");
    if (at == std::string::npos) {
        notVerified(goals, reason);
        return;
    }
    const size_t from = out.rfind('\n', at);
    const size_t start = from == std::string::npos ? 0 : from + 1;
    harnessFailure(std::string(goals) + ": a preloaded test library killed the daemon -- " +
                   out.substr(start, out.find('\n', at) - start));
}

// "<signal> received, shutting down" for one signal, as the daemon words it.
std::string receivedLine(int signo) { return std::string(strsignal(signo)) + " " + kSignalLine; }

// Before every spawn. A killed daemon's name is released when dbus-daemon handles its disconnect,
// which can be after waitpid has returned, so a stale owner would otherwise answer the next spawn's
// name wait. Not a check: no daemon holds its name past its own process, so none could fail it;
// "the name is released" in S6.41 is carried by the exit checks.
bool nameFree(NameWatcher& names, const std::string& next) {
    if (names.waitFor(false)) return true;
    harnessFailure("before " + next + ": the bus name is still owned by a daemon an earlier phase stopped");
    return false;
}

// The exit status `what` is about, unless a sanitizer replaced it: then the report is what goes
// red, named as such, rather than the exit path. False when the status was not the expected one.
bool checkStatus(const SpawnedDaemon& daemon, int exitCode, int expected, const std::string& what) {
    if (daemon.sanitizerReported()) {
        check(false, (what + " -- not graded: the daemon's sanitizer reported warnings, and the exit status is "
                             "the sanitizer's")
                         .c_str());
        return false;
    }
    const bool held = daemon.exitedNormally() && exitCode == expected;
    check(held, what.c_str());
    return held;
}

bool setWallpaper(sdbus::IProxy& proxy, const std::string& type, const std::string& path, int32_t monitorId) {
    return wallpaperx::testing::trySetWallpaper(proxy, type, path, monitorId, std::chrono::seconds(10));
}

// Touches the trigger, runs `provoke`, and waits for the shim to fail a submission. False when it
// never did, which is this harness failing to lose the device, not the daemon failing to notice.
bool loseDevice(const SpawnedDaemon& daemon, const fs::path& trigger, const std::function<void()>& provoke,
                const std::string& phase) {
    if (!touch(trigger)) {
        harnessFailure(phase + ": the device-loss trigger file could not be created");
        return false;
    }
    provoke();
    if (!daemon.waitForOutput(kShimFired, kWait)) {
        harnessFailure(phase + ": the preloaded shim never failed a submission, so no device was lost -- " +
                       "check that " WALLPAPERX_DEVICE_LOST_SHIM " loads");
        return false;
    }
    return true;
}

// loseDevice, then the daemon's exit code in `exitCode`.
bool loseDeviceAndWait(SpawnedDaemon& daemon, const fs::path& trigger, const std::function<void()>& provoke,
                       const std::string& phase, int& exitCode) {
    if (!loseDevice(daemon, trigger, provoke, phase)) return false;
    exitCode = daemon.waitForExit();
    return true;
}

// Grades one device-loss exit: the status the unit restarts on, and what the process said.
// True when it was that exit.
bool checkLostExit(const SpawnedDaemon& daemon, int exitCode, const std::string& phase) {
    const std::string out = daemon.output();
    check(!daemon.timedOut(), (phase + ": a daemon whose device was lost exits on its own").c_str());
    // GOAL: S6.41 -- the loss ends in the status the unit's Restart=on-failure restarts on, told
    //       apart from a crash (a signal) and from the fatal-error exit 1.
    // WHY-THIS-SHAPE: exitedNormally() is asserted with the code, because a death by signal
    //       reports -1 and "nonzero" alone would pass a crash as the designed exit.
    // CONTROL: in backend/daemon/main.cpp, via control_run.sh, re-observed 2026-09-24 on NVIDIA,
    //       reverted. main's device-loss status made 1 -> this line and "reports its stop as the
    //       device-loss one" red for the still and all three video rounds, with "every round ended
    //       in a device-loss exit"; nothing else. The device-loss callback not passed to
    //       DbusService -> "exits on its own", this line and both message lines red for the still
    //       and every round, with "every round ended ...": each daemon killed after 30 s. The signal
    //       thread made to count its wake only with an outside signal (`reasonRecorded = received
    //       != kWakeSignal && ...`), so the loss's own wake is ignored -> "exits on its own", this
    //       line and "reports its stop as the device-loss one" red for the still and every round,
    //       with "every round ended ...": 13 in all. Observed 2026-09-24; reverted.
    const bool designed =
        checkStatus(daemon, exitCode, kExitDeviceLost, phase + ": a daemon whose device was lost exits with status 75");
    if (!designed) {
        std::fprintf(stderr, "  %s: exit code %d, signal %d%s\n", phase.c_str(), exitCode, daemon.termSignal(),
                     daemon.timedOut() ? ", killed by the harness after the timeout" : "");
    }
    check(out.find(kLostStderr) != std::string::npos,
          (phase + ": the daemon says the device was lost and that it exits 75 for a restart").c_str());
    check(out.find(kLostStdout) != std::string::npos,
          (phase + ": the daemon reports its stop as the device-loss one").c_str());
    // CONTROL: backend/daemon/main.cpp printing "stopped cleanly" whatever the exit status -> the
    //       check below and the one above red for the still and all three video rounds, and
    //       nothing else. Re-observed 2026-09-24 via control_run.sh; reverted.
    check(out.find(kCleanStop) == std::string::npos,
          (phase + ": a device-loss stop does not report itself as a clean one").c_str());
    // GOAL: the log of a device-loss stop names no signal. The stop is carried out by waking the
    //       signal thread with a signal of the daemon's own (SIGUSR1), and an operator reading
    //       "User defined signal 1 received" would look for whoever sent it.
    // WHY-THIS-SHAPE: an absence check, kept able to fire by the no-device phase below, which
    //       asserts the same needle's presence on a SIGTERM stop: a reworded line turns that red.
    // CONTROL: the signal thread in backend/daemon/main.cpp made to print its "received" line
    //       for every stop (its first `if (outside)`, now around stopOut.print, made `if (true)`)
    //       -> this line red for the still and all three video rounds, nothing else. Re-observed
    //       2026-09-24 via control_run.sh after the switch to StopLog. Reverted.
    check(out.find(kSignalLine) == std::string::npos,
          (phase + ": a device-loss stop does not report a signal it was never sent").c_str());
    // A round whose status the sanitizer replaced still reached its exit, which is what the
    // caller's round count asserts; its report is already red above.
    return designed || daemon.sanitizerReported();
}

// What was left on disk after a loss: no crash record for any monitor, and no death counted for
// any video.
void checkNothingBlamed(const Home& home, const std::string& phase, bool videoPlayed) {
    // GOAL: S14.1 -- a device-loss exit leaves no applying-<monitor>.json, so the restart does not
    //       refuse the wallpaper that happened to be rendering when the GPU went.
    // WHY-THIS-SHAPE: read off the state directory rather than inferred from the restart, which
    //       is also graded below: a restart that redraws everything could still be refusing
    //       nothing only because the record named a monitor with no saved wallpaper.
    // CONTROL: in renderer/vulkan/device.cpp, Device::observe() made to `std::_Exit(75)` on
    //       VK_ERROR_DEVICE_LOST, i.e. exiting at the moment of loss instead of between jobs ->
    //       this line red for the still, with the restart's two monitor-2 lines; and, since
    //       main's callback and final line never run, "says the device was lost" and "reports
    //       its stop as the device-loss one" red for the still and all three rounds. Every
    //       "exits with status 75" line stayed green. Re-observed 2026-09-24 via control_run.sh,
    //       all 11 red lines read off the run; reverted.
    const std::vector<std::string> records = applyingRecords(home.stateDir());
    check(records.empty(), (phase + ": no applying-* record is left in the state directory").c_str());
    for (const std::string& name : records) std::fprintf(stderr, "  %s: left behind: %s\n", phase.c_str(), name.c_str());

    // GOAL: S14.1 -- a device-loss exit counts no death against any video, so repeated losses
    //       on one video do not get it refused as a video that kills the daemon.
    // WHY-THIS-SHAPE: `died` and every count are both read. The count is written by the NEXT
    //       start, from `died`, so reading the counts alone right after the exit would miss a
    //       death the handler had only just recorded.
    // CONTROL: in renderer/vulkan/device.cpp, Device::observe() made to `std::abort()` on
    //       VK_ERROR_DEVICE_LOST, so the loss kills the render thread while it plays the video
    //       -> round 1's "blames no video" red, then round 2's blame and "counts no deaths" lines,
    //       the count written by round 2's start. Being a signal death before main's callback,
    //       it also reddens "exits with status 75" and both message lines for the still and
    //       rounds 1-2, the still's applying-record line and monitor-2 restart lines, round 3's
    //       "still plays", "every round ended in a device-loss exit" and both final "after three
    //       device losses" lines: 19 in all, read off the run. Re-observed 2026-09-24 via
    //       control_run.sh; reverted.
    // COVERAGE-ASSERT: after a video played the record must be there to read; absent, these two
    //       checks would compare nothing. Controlled in backend/daemon/crash_record.cpp:
    //       PlayingRecord::start() made to return right after claiming the record, so none is
    //       ever written -> "the playing record is on disk to be read" red for all three rounds,
    //       alone. Observed 2026-09-24 via control_run.sh; reverted.
    const std::optional<wallpaperx::PlayingHistory> played = wallpaperx::readPlayingRecord(home.stateDir());
    if (videoPlayed) check(played.has_value(), (phase + ": the playing record is on disk to be read").c_str());
    if (played.has_value()) {
        check(!played->died.has_value(), (phase + ": the playing record blames no video for the exit").c_str());
        if (played->died.has_value()) {
            std::fprintf(stderr, "  %s: blamed monitor %d (%s)\n", phase.c_str(), played->died->monitorId,
                         played->died->path.c_str());
        }
        int counted = 0;
        for (const wallpaperx::PlayingVideo& video : played->videos) counted += video.deaths;
        check(counted == 0, (phase + ": the playing record counts no deaths").c_str());
        if (counted != 0) std::fprintf(stderr, "  %s: %d death(s) counted\n", phase.c_str(), counted);
    }
}

// Saved monitors and their paths, read from the config file the daemon wrote.
std::map<int, std::string> savedMonitors(const fs::path& configFile) {
    std::map<int, std::string> out;
    std::ifstream in(configFile);
    const nlohmann::json json = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (!json.is_object() || !json.contains("monitors") || !json["monitors"].is_array()) return out;
    for (const nlohmann::json& entry : json["monitors"]) {
        if (entry.is_object() && entry.contains("monitor_id") && entry["monitor_id"].is_number_integer()) {
            out[entry["monitor_id"].get<int>()] = entry.value("path", std::string());
        }
    }
    return out;
}

// S6.41 and the static half of S14.1: two stills showing, the device lost on a third render, and
// a start on the same HOME that redraws all three with no client asking.
void checkStaticLossAndRestart(NameWatcher& names, const std::string& blue, const std::string& red) {
    constexpr const char* kGoals = "S6.41/S14.1 for stills (the wallpapers come back after a device-loss exit)";
    const std::string phase = "static loss";
    Home home("devloss_static");
    if (!home.ok()) {
        notVerified(kGoals, "a scratch HOME could not be created under the temp directory");
        return;
    }

    if (!nameFree(names, phase)) return;
    {
        FrameReadyLog frames;
        auto proxy = subscribeFrameReady(frames);
        SpawnedDaemon daemon(home.env(true), SpawnedDaemon::Stdout::Captured, {}, 0, kDaemonArgs);
        if (!names.waitFor(true)) {
            setupFailed(daemon, kGoals, "the first daemon never acquired the bus name");
            return;
        }
        if (!setWallpaper(*proxy, "static", blue, kMonitorBlue) || !setWallpaper(*proxy, "static", red, kMonitorRed) ||
            !frames.wait(kMonitorBlue, 1) || !frames.wait(kMonitorRed, 1)) {
            setupFailed(daemon, kGoals, "the daemon did not draw the two stills before any loss, so this host cannot "
                                        "show that a restart brings them back");
            return;
        }
        int exitCode = -1;
        if (!loseDeviceAndWait(daemon, home.trigger(),
                               [&] { (void)setWallpaper(*proxy, "static", blue, kMonitorTrigger); }, phase, exitCode)) {
            return;
        }
        checkLostExit(daemon, exitCode, phase);
    }

    checkNothingBlamed(home, phase, false);

    const std::map<int, std::string> saved = savedMonitors(home.configFile());
    const std::map<int, std::string> expected{{kMonitorBlue, blue}, {kMonitorRed, red}, {kMonitorTrigger, blue}};
    // CONTROL: backend/daemon/main.cpp's device-loss callback made to delete the config file ->
    //       this line and all six restart lines below red, with the video phase's round-2
    //       restart, its round count and both final lines. Re-observed 2026-09-24 via
    //       control_run.sh; reverted.
    check(saved == expected, "static loss: the config file still holds all three monitors after the exit");

    fs::remove(home.trigger());

    // GOAL: S6.41 -- after a device-loss exit, a start on the same HOME redraws every saved
    //       monitor with no client acting; that start is what the unit's restart performs.
    // WHY-THIS-SHAPE: a FrameReady per monitor, not only GetStatus's state -- a monitor restored
    //       as state but refused, or never drawn, would still list its path. Generation 1: a new
    //       process counts from its first render. The subscription is made before the spawn.
    // CONTROL: `service.restoreSaved();` disabled in backend/daemon/main.cpp -> all six
    //       "redraws monitor N" / "reports monitor N" lines red, with the video phase's round-2
    //       restart, its round count and both final lines. And the Device::observe() `_Exit(75)`
    //       control above -> of these six, only monitor 2's two lines red, since the record left
    //       behind makes the restart refuse that wallpaper. Both re-observed 2026-09-24 on NVIDIA
    //       via control_run.sh; reverted.
    if (!nameFree(names, "static loss's restart")) return;
    FrameReadyLog restored;
    auto proxy = subscribeFrameReady(restored);
    SpawnedDaemon restart(home.env(false), SpawnedDaemon::Stdout::Captured, {}, 0, kDaemonArgs);
    if (!names.waitFor(true)) {
        check(false, "static loss: a daemon started on the same HOME after the loss acquires the bus name");
        return;
    }
    for (const auto& [monitorId, path] : expected) {
        const std::string what = "static loss: the restarted daemon redraws monitor " + std::to_string(monitorId) +
                                 " with no client asking";
        check(restored.wait(monitorId, 1), what.c_str());
    }
    const StatusReply status = getStatus(*proxy, std::chrono::seconds(10));
    for (const auto& [monitorId, path] : expected) {
        const nlohmann::json* entry = wallpaperStatus(status, std::to_string(monitorId));
        const bool shown = entry != nullptr && entry->value("has_frame", false) && !entry->contains("last_error");
        check(shown, ("static loss: the restarted daemon reports monitor " + std::to_string(monitorId) +
                      " showing a frame and no error")
                         .c_str());
        if (!shown) std::fprintf(stderr, "  %s\n", entry != nullptr ? entry->dump().c_str() : "(absent)");
    }
    const int restartExit = restart.shutdown(SIGTERM);
    checkStatus(restart, restartExit, 0, "static loss: the restarted daemon exits 0 on SIGTERM");
}

// The video half of S14.1: the device lost while one video plays, three times running on one
// HOME, and a fourth start that still plays it.
void checkRepeatedVideoLoss(NameWatcher& names) {
    constexpr const char* kGoals = "S14.1 for videos (repeated device losses do not get a video refused)";
    const std::string clip = std::string(WALLPAPERX_VIDEO_ASSET_DIR) + "/wallpaper_bands.mp4";
    std::error_code ec;
    if (!fs::exists(clip, ec)) {
        notVerified(kGoals, "the video fixture " + clip + " was not generated (no ffmpeg at build time)");
        return;
    }
    Home home("devloss_video");
    if (!home.ok()) {
        notVerified(kGoals, "a scratch HOME could not be created under the temp directory");
        return;
    }

    // GOAL: S14.1 -- several device losses in a row while one video plays leave it playable: the
    //       daemon's crash-loop breaker refuses a video after two deaths while playing it, so a
    //       loss must not count as one.
    // WHY-THIS-SHAPE: the loss is provoked only after the video has shown several pictures, so
    //       it lands in playback (the clock rounds) rather than in the first apply, which carries
    //       an applying record of its own. Starts after the first restore the video themselves:
    //       a start that refused it draws nothing and fails the wait below.
    // CONTROL: the Device::observe() `std::abort()` control -> the blame and count lines above,
    //       then "video loss 3: the restarted daemon still plays the video" and both final
    //       "after three device losses" lines red: the video refused after two counted deaths.
    //       The full list of 19 red lines is in checkNothingBlamed's annotation. Reverted.
    // COVERAGE-ASSERT: the number of losses that actually reached a device-loss exit is counted
    //       and asserted, so a loop that stopped early cannot read as three. Red under the
    //       exit-status-1, no-callback and abort controls above.
    int losses = 0;
    for (int round = 1; round <= kVideoLosses; ++round) {
        const std::string phase = "video loss " + std::to_string(round);
        if (!nameFree(names, phase)) return;
        FrameReadyLog frames;
        auto proxy = subscribeFrameReady(frames);
        SpawnedDaemon daemon(home.env(true), SpawnedDaemon::Stdout::Captured, {}, 0, kDaemonArgs);
        if (!names.waitFor(true)) {
            check(false, (phase + ": the daemon acquires the bus name").c_str());
            break;
        }
        if (round == 1 && !setWallpaper(*proxy, "video", clip, kMonitorVideo)) {
            setupFailed(daemon, kGoals, "the daemon refused the video fixture before any loss");
            return;
        }
        if (!frames.waitAtLeast(kMonitorVideo, 3)) {
            if (round == 1) {
                setupFailed(daemon, kGoals, "this host never played the video fixture, so no loss can land in playback");
                return;
            }
            check(false, (phase + ": the restarted daemon still plays the video").c_str());
            break;
        }
        // No request provokes it: the playback clock's next submission is the one the shim fails.
        int exitCode = -1;
        if (!loseDeviceAndWait(daemon, home.trigger(), [] {}, phase, exitCode)) return;
        if (checkLostExit(daemon, exitCode, phase)) ++losses;
        checkNothingBlamed(home, phase, true);
        fs::remove(home.trigger());
    }
    check(losses == kVideoLosses, "video loss: every round ended in a device-loss exit while the video played");
    std::printf("device_loss_restart_test: %d of %d video rounds ended in a device-loss exit\n", losses,
                kVideoLosses);

    if (!nameFree(names, "the start after the video losses")) return;
    FrameReadyLog frames;
    auto proxy = subscribeFrameReady(frames);
    SpawnedDaemon daemon(home.env(false), SpawnedDaemon::Stdout::Captured, {}, 0, kDaemonArgs);
    if (!names.waitFor(true)) {
        check(false, "video: a start after the losses acquires the bus name");
        return;
    }
    check(frames.waitAtLeast(kMonitorVideo, 2),
          "video: a start after three device losses still plays the video, with no client asking");
    const StatusReply status = getStatus(*proxy, std::chrono::seconds(10));
    const nlohmann::json* entry = wallpaperStatus(status, std::to_string(kMonitorVideo));
    const bool clean = entry != nullptr && !entry->contains("last_error");
    check(clean, "video: the video carries no refusal after three device losses");
    if (!clean) std::fprintf(stderr, "  %s\n", entry != nullptr ? entry->dump().c_str() : "(absent)");
    const int stopExit = daemon.shutdown(SIGTERM);
    checkStatus(daemon, stopExit, 0, "video: the daemon exits 0 on SIGTERM after the losses");
}

// Waits for GetStatus to report `monitorId` as failed. False when the daemon went away or the
// deadline passed; `lastError` is what it reported.
bool awaitFailure(sdbus::IProxy& proxy, const SpawnedDaemon& daemon, int32_t monitorId, std::string& lastError,
                  bool& drew) {
    const auto deadline = Clock::now() + kWait;
    while (Clock::now() < deadline && !daemon.exited()) {
        const StatusReply status = getStatus(proxy, std::chrono::seconds(5));
        const nlohmann::json* entry = wallpaperStatus(status, std::to_string(monitorId));
        if (entry != nullptr && entry->value("has_frame", false)) {
            drew = true;
            return false;
        }
        if (entry != nullptr && entry->contains("last_error")) {
            lastError = entry->value("last_error", std::string());
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

// S14.4: a device that was never created is not a lost one.
void checkNoDeviceIsNotLoss(NameWatcher& names, const std::string& blue, const std::string& red) {
    constexpr const char* kGoals = "S14.4 (a daemon with no device stays up rather than exiting as device-lost)";
    Home home("devloss_nodevice");
    if (!home.ok()) {
        notVerified(kGoals, "a scratch HOME could not be created under the temp directory");
        return;
    }
    std::vector<std::string> env = home.env(false);
    // Every loader spelling of "use only this driver list", pointed at a file that is not there,
    // so vkCreateInstance finds no driver at all.
    const std::string noDriver = (home.root() / "no-such-icd.json").string();
    env.push_back("VK_ICD_FILENAMES=" + noDriver);
    env.push_back("VK_DRIVER_FILES=" + noDriver);
    env.push_back("VK_ADD_DRIVER_FILES=");

    if (!nameFree(names, "the no-device phase")) return;
    FrameReadyLog frames;
    auto proxy = subscribeFrameReady(frames);
    SpawnedDaemon daemon(env, SpawnedDaemon::Stdout::Captured, {}, 0, kDaemonArgs);
    if (!names.waitFor(true)) {
        check(false, "no device: the daemon acquires the bus name");
        return;
    }

    // GOAL: S14.4 -- a daemon whose device creation fails stays up, reports why on the monitor,
    //       and keeps serving renders; it does not take the device-loss exit, which would have
    //       the unit restart it into the same failure every few seconds.
    // WHY-THIS-SHAPE: a second monitor is set after the first failure and must fail too, since
    //       a monitor's own error proves only that one render ran; the second proves the worker
    //       is still taking work. Polled GetStatus with a deadline and an exit check, so a daemon
    //       that left fails at once instead of waiting the deadline out.
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, deviceLost() made to return
    //       `!m_device || m_device->lost()` -> "FAIL: no device: the daemon is still running after
    //       its device creation failed" and all seven lines the phase then had red: the daemon exited
    //       75 after the first render, before its error could be read. Observed 2026-09-24. Reverted.
    std::string firstError;
    bool drew = false;
    const bool firstFailed =
        setWallpaper(*proxy, "static", blue, kMonitorBlue) && awaitFailure(*proxy, daemon, kMonitorBlue, firstError, drew);
    if (drew) {
        notVerified(kGoals, "this host created a device despite the loader being given no driver");
        daemon.shutdown(SIGTERM);
        return;
    }
    check(firstFailed, "no device: the render on a daemon with no device reports an error on its monitor");
    std::printf("device_loss_restart_test: with no driver the daemon reports -- %s\n", firstError.c_str());
    std::string secondError;
    const bool secondFailed =
        setWallpaper(*proxy, "static", red, kMonitorRed) && awaitFailure(*proxy, daemon, kMonitorRed, secondError, drew);
    check(!daemon.exited(), "no device: the daemon is still running after its device creation failed");
    check(secondFailed, "no device: a later render is still attempted and reported");
    check(names.ownedNow(), "no device: the daemon still holds the bus name");

    const int exitCode = daemon.shutdown(SIGTERM);
    const std::string out = daemon.output();
    checkStatus(daemon, exitCode, 0, "no device: the daemon exits 0 on SIGTERM");
    check(out.find(kLostStderr) == std::string::npos, "no device: the daemon never reports its device as lost");
    check(out.find(kCleanStop) != std::string::npos, "no device: the daemon stops cleanly");
    check(out.find(kSignalLine) != std::string::npos, "no device: the daemon names the signal it stops for");
}

// A HOME's environment with the device-loss library and tests/wedge_shim.cpp both preloaded, the
// wedge armed: the teardown after the loss frees the video's decoder on the main thread, and that
// free never returns, so the stop cannot finish. On a real GPU the same stop blocks on fences a
// lost device never signals.
std::vector<std::string> stuckStopEnv(const Home& home) {
    std::vector<std::string> env = home.env(true);
    for (std::string& entry : env) {
        if (entry.rfind("LD_PRELOAD=", 0) == 0) entry += std::string(":") + WALLPAPERX_WEDGE_SHIM;
    }
    env.push_back("WALLPAPERX_TEST_WEDGE_FREE=1");
    return env;
}

// Waits for GetStatus to report `monitorId` unwatched: nothing collected its pictures, so the clock
// no longer advances it, while its decoder stays open for the idle release's 5 s.
bool awaitUnwatched(sdbus::IProxy& proxy, const SpawnedDaemon& daemon, int32_t monitorId) {
    const auto deadline = Clock::now() + kWait;
    while (Clock::now() < deadline && !daemon.exited()) {
        const StatusReply status = getStatus(proxy, std::chrono::seconds(5));
        const nlohmann::json* entry = wallpaperStatus(status, std::to_string(monitorId));
        if (entry != nullptr && entry->contains("watched") && !entry->value("watched", true)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

// Which outside signals a stuck device-loss stop is sent, as systemd or a logout would send them.
enum class OutsideSignals {
    None,
    // One SIGTERM once the teardown is stuck, after the loss has woken the signal thread.
    OneWhenStuck,
};

// The device lost with a video's decoder open, and the stop that follows stuck in the teardown.
void checkStuckLossStop(NameWatcher& names, const std::string& still, OutsideSignals signals) {
    const bool outsideSignal = signals != OutsideSignals::None;
    const char* goals = outsideSignal
                            ? "the device-loss stop's second-signal clause (one outside SIGTERM does not force it)"
                            : "the device-loss stop's deadline clause (a stuck stop still exits 75)";
    const std::string phase = outsideSignal ? "stuck device-loss stop, outside SIGTERM" : "stuck device-loss stop";
    constexpr const char* kWedged = "wedge_shim: holding a decoder free on the main thread";
    const std::string clip = std::string(WALLPAPERX_VIDEO_ASSET_DIR) + "/wallpaper_bands.mp4";
    std::error_code ec;
    if (!fs::exists(clip, ec)) {
        notVerified(goals, "the video fixture " + clip + " was not generated, so no teardown frees a decoder");
        return;
    }
    Home home(signals == OutsideSignals::None ? "devloss_stuck" : "devloss_stuck_signal");
    if (!home.ok()) {
        notVerified(goals, "a scratch HOME could not be created under the temp directory");
        return;
    }
    if (!nameFree(names, phase)) return;

    FrameReadyLog frames;
    auto proxy = subscribeFrameReady(frames);
    SpawnedDaemon daemon(stuckStopEnv(home), SpawnedDaemon::Stdout::Captured, {}, 0, kDaemonArgs);
    if (!names.waitFor(true)) {
        setupFailed(daemon, goals, "the daemon with both libraries preloaded never acquired the bus name");
        return;
    }
    if (!setWallpaper(*proxy, "video", clip, kMonitorVideo) || !frames.waitAtLeast(kMonitorVideo, 3)) {
        setupFailed(daemon, goals, "this host never played the video, so its teardown holds no decoder to wedge");
        return;
    }
    // The loss has to land on a still's render with the video idle: a video whose own draw fails
    // closes its session on the render thread, and its teardown then has no decoder left to free.
    if (!awaitUnwatched(*proxy, daemon, kMonitorVideo)) {
        setupFailed(daemon, goals, "the video never went idle, so the loss could not be kept off its own draw");
        return;
    }
    if (!loseDevice(daemon, home.trigger(), [&] { (void)setWallpaper(*proxy, "static", still, kMonitorBlue); },
                    phase)) {
        return;
    }
    if (!daemon.waitForOutput(kWedged, kWait)) {
        daemon.waitForExit();
        setupFailed(daemon, goals, "the stop after the loss never reached the wedged decoder free, so it was not stuck");
        return;
    }
    if (outsideSignal) {
        if (daemon.exited()) {
            daemon.waitForExit();
            notVerified(goals, "the daemon was gone before the outside SIGTERM could be sent");
            return;
        }
        ::kill(daemon.pid(), SIGTERM);
    }
    const int exitCode = daemon.waitForExit();
    const std::string out = daemon.output();
    const bool forced = out.find("asked again") != std::string::npos;

    check(!daemon.timedOut(), (phase + ": a device-loss stop whose teardown is stuck still ends").c_str());
    // GOAL: a device-loss stop that cannot finish its teardown still exits 75, so the service
    //       manager tells a lost device from a crash; on a real loss this is the likely stop, as
    //       teardown waits on a driver that will not answer. One outside SIGTERM during it
    //       (systemd stopping the session, a logout) neither forces it nor changes the status.
    //       NOT GRADED: that a forced exit (a second outside SIGTERM) during a device-loss stop
    //       keeps 75. forceExitOnSecondSignal is armed only when an outside signal wakes the signal
    //       thread after the loss is recorded; onDeviceLost records it and sends its own SIGUSR1
    //       back to back, with nothing observable between, and sigwait takes SIGUSR1 ahead of a
    //       pending SIGTERM. No outside timing reaches that state, and a hook would be production
    //       surface for a test.
    // WHY-THIS-SHAPE: either deadline may end the stop -- the signal thread's wait and the unwind's
    //       alarm are both 3 s and start microseconds apart -- so either path is accepted and only
    //       the status is graded. Controls (a) and (b) each disable one path, which is what makes
    //       them deterministic. The wedge line is waited for before the verdict, so a stop that was
    //       never stuck is reported NOT VERIFIED rather than graded as this one.
    // CONTROL: in backend/daemon/main.cpp via control_run.sh, re-observed 2026-09-24 on NVIDIA,
    //       reverted. (a) The signal thread's deadline exit made `_Exit(1)` and armTeardown's alarm
    //       removed, so only that path can end the stop -> "exits with status 75" red in both
    //       phases. (b) The DeviceLost line removed from onTeardownDeadline and the signal thread's
    //       wait made 60 s, so only the alarm can -> the same two lines, nothing else. (c) The
    //       second-signal handler armed whatever woke the signal thread (its second `if (outside)`
    //       made `if (true)`) -> "not taken for a second ask", alone; both 75 lines stay green, as
    //       a forced exit keeps 75. (d) Both paths disabled -> "still ends" and "exits with status
    //       75" red in both phases. (a) and (d) also redden the stalled-stdout phase's "ends on its
    //       own" below, which only the unwind's alarm can end.
    const bool designed = checkStatus(daemon, exitCode, kExitDeviceLost, phase + ": exits with status 75");
    if (!designed) {
        std::fprintf(stderr, "  %s: exit code %d, signal %d\n", phase.c_str(), exitCode, daemon.termSignal());
    }
    if (outsideSignal) check(!forced, (phase + ": the outside SIGTERM is not taken for a second ask").c_str());
    std::printf("device_loss_restart_test: %s ended by %s\n", phase.c_str(),
                forced                                            ? "the second SIGTERM"
                : out.find("shutdown blocked") != std::string::npos ? "the signal thread's deadline"
                                                                    : "the alarm");
}

// GOAL: a SIGUSR1 from outside the process -- a script, a monitoring tool -- is not a stop: the
//       daemon keeps serving, logs one line per signal saying it ignored it, and a later SIGTERM
//       still stops it cleanly with exit 0. SIGUSR1 is how the daemon wakes its own signal thread,
//       and taking an outside one for a stop would exit 0, which the unit does not restart.
// WHY-THIS-SHAPE: the second SIGUSR1 is sent only once the first one's line is out, so each is
//       consumed on its own (two pending standard signals merge into one) and "exactly one line
//       each" is not a question for the scheduler. A render after the signals shows the daemon is
//       still serving, not merely alive.
// CONTROL: in backend/daemon/main.cpp via control_run.sh, observed 2026-09-24, reverted. (a) An
//       outside SIGUSR1 taken for a stop again (`received != kWakeSignal &&` dropped from the
//       watcher's reason test) -> "answered with a line", "still running", "still draws", "the
//       stop is reported as the SIGTERM's" and "exactly one line" red; the exit-0 and "stops
//       cleanly" lines stay green, because the SIGUSR1 stop itself exits 0 -- the defect. (b) The
//       "ignoring" line's stopErr.print duplicated -> "exactly one line per outside SIGUSR1",
//       alone; re-observed after the switch to StopLog.
void checkOutsideWakeSignalIgnored(NameWatcher& names, const std::string& blue) {
    constexpr const char* kGoals = "an outside SIGUSR1 is ignored, not taken for a stop";
    const std::string phase = "outside SIGUSR1";
    const std::string ignoring = std::string("ignoring ") + strsignal(SIGUSR1);
    Home home("devloss_usr1");
    if (!home.ok()) {
        notVerified(kGoals, "a scratch HOME could not be created under the temp directory");
        return;
    }
    if (!nameFree(names, phase)) return;
    FrameReadyLog frames;
    auto proxy = subscribeFrameReady(frames);
    SpawnedDaemon daemon(home.env(false), SpawnedDaemon::Stdout::Captured, {}, 0, kDaemonArgs);
    if (!names.waitFor(true)) {
        notVerified(kGoals, "the daemon never acquired the bus name");
        return;
    }

    bool bothAnswered = true;
    for (size_t sent = 1; sent <= 2 && bothAnswered; ++sent) {
        ::kill(daemon.pid(), SIGUSR1);
        bothAnswered = waitForOccurrences(daemon, ignoring, sent);
    }
    check(bothAnswered, (phase + ": each outside SIGUSR1 is answered with a line saying it was ignored").c_str());
    check(!daemon.exited() && names.ownedNow(), (phase + ": the daemon is still running and holds its name").c_str());
    check(setWallpaper(*proxy, "static", blue, kMonitorBlue) && frames.wait(kMonitorBlue, 1),
          (phase + ": the daemon still draws after two outside SIGUSR1s").c_str());

    const int exitCode = daemon.shutdown(SIGTERM);
    const std::string out = daemon.output();
    checkStatus(daemon, exitCode, 0, phase + ": a later SIGTERM still exits 0");
    check(out.find(kCleanStop) != std::string::npos, (phase + ": a later SIGTERM still stops cleanly").c_str());
    check(out.find(receivedLine(SIGTERM)) != std::string::npos && out.find(receivedLine(SIGUSR1)) == std::string::npos,
          (phase + ": the stop is reported as the SIGTERM's, not the SIGUSR1's").c_str());
    const size_t lines = occurrences(out, ignoring);
    check(lines == 2, (phase + ": exactly one line per outside SIGUSR1").c_str());
    if (lines != 2) std::fprintf(stderr, "  %s: %zu \"%s\" lines for 2 signals\n", phase.c_str(), lines, ignoring.c_str());
}

// The pipe behind the daemon's descriptor `fd`, filled through a non-blocking description of this
// test's own until not one more byte fits: a reader that stopped reading. The description stays
// open so stillFull() can ask again at the end of the phase.
class StalledPipe {
public:
    StalledPipe(pid_t pid, int fd) {
        const std::string path = "/proc/" + std::to_string(pid) + "/fd/" + std::to_string(fd);
        m_fd = ::open(path.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (m_fd < 0) return;
        // Page-sized first, then single bytes: a pipe refuses a 4096-byte write with up to 4095 free.
        for (const size_t size : {size_t{4096}, size_t{1}}) {
            if (!fill(size)) return;
        }
        m_full = true;
    }
    ~StalledPipe() {
        if (m_fd >= 0) ::close(m_fd);
    }
    StalledPipe(const StalledPipe&) = delete;
    StalledPipe& operator=(const StalledPipe&) = delete;

    bool full() const { return m_full; }
    size_t written() const { return m_written; }
    // False if even one byte now fits, i.e. something drained the pipe since it was filled.
    bool stillFull() const {
        const char byte = '.';
        ssize_t n = -1;
        do n = ::write(m_fd, &byte, 1);
        while (n < 0 && errno == EINTR);
        return n < 0 && errno == EAGAIN;
    }

private:
    // Until a write of `size` is refused as full. False on any other error, or at a cap far past
    // any pipe's capacity, which a reader still draining it would reach.
    bool fill(size_t size) {
        constexpr size_t kCap = size_t{4} << 20;  // four times the default pipe-max-size
        const std::string chunk(size, '.');
        while (m_written < kCap) {
            const ssize_t n = ::write(m_fd, chunk.data(), size);
            if (n > 0) {
                m_written += static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            return n < 0 && errno == EAGAIN;
        }
        return false;
    }

    int m_fd = -1;
    bool m_full = false;
    size_t m_written = 0;
};

// Waits, to `patience`, for the daemon's bus name to have no owner.
bool nameReleasedWithin(NameWatcher& names, std::chrono::seconds patience) {
    const auto deadline = Clock::now() + patience;
    for (;;) {
        const uint64_t seen = names.events();
        if (!names.ownedNow()) return true;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        if (left.count() <= 0) return false;
        names.waitForEventsPast(seen, left);
    }
}

// Which output stops draining, and what then asks the daemon to stop.
enum class Stall {
    StdoutThenSigterm,      // the "received, shutting down" line
    StderrThenSigusr1,      // the "ignoring" line, then a SIGTERM
    StderrThenDeviceLoss,   // onDeviceLost's line
};

// GOAL: a reader that stopped draining the daemon's stdout or stderr (a stalled journald stream,
//       a parent that stopped reading) does not keep a stop from happening: after a SIGTERM, an
//       ignored outside SIGUSR1 then a SIGTERM, or a device loss, the daemon still ends on its
//       own and releases its bus name, well inside the harness's 30 s.
// WHY-THIS-SHAPE: the pipe is filled from this side through /proc, so the stall is real and
//       verified full (a one-byte write refused) before the stop is asked for. Neither the exit
//       status nor which deadline ends it is graded: with an output stalled, the main thread's own
//       stdio blocks during the unwind, and the alarm and the signal thread's wait race at 3 s.
//       The name release is bounded at 10 s, so a stuck daemon fails there rather than being
//       found only by the harness's kill. For a device loss the test learns the shim fired from
//       a marker file, as the shim's line is on the stalled stderr.
// CONTROL: in backend/daemon/main.cpp via control_run.sh, observed 2026-09-24 on NVIDIA, reverted;
//       each daemon then had to be killed by the harness at 30 s. The "received" line put back on
//       blocking printf + fflush(stdout) -> the stalled-stdout phase's two lines, alone. The
//       "ignoring" line on blocking fprintf(stderr) -> the SIGUSR1 phase's two lines, alone.
//       onDeviceLost's line on blocking fprintf + fflush, before its pthread_kill -> the
//       device-loss phase's two lines, alone.
// COVERAGE-ASSERT: the pipe is asked for one more byte once the stop is over, before the reap.
//       holdCapture() made a no-op by hand (reverted): both stderr phases report "FAIL (harness):
//       something drained the stalled pipe"; without that re-ask they had passed on a live reader.
void checkStopWithStalledOutput(NameWatcher& names, const std::string& blue, Stall stall) {
    constexpr const char* kGoals = "a stalled stdout or stderr does not keep the daemon from stopping";
    const std::string phase = stall == Stall::StdoutThenSigterm   ? "stalled stdout, SIGTERM"
                              : stall == Stall::StderrThenSigusr1 ? "stalled stderr, outside SIGUSR1 then SIGTERM"
                                                                  : "stalled stderr, device loss";
    const bool lose = stall == Stall::StderrThenDeviceLoss;
    Home home("devloss_stall");
    if (!home.ok()) {
        notVerified(kGoals, "a scratch HOME could not be created under the temp directory");
        return;
    }
    std::vector<std::string> env = home.env(lose);
    // A sanitizer report written to the stalled stderr would block the process that writes it.
    const std::string sanitizerLog = (home.root() / "sanitizer").string();
    for (const char* name : {"TSAN_OPTIONS", "ASAN_OPTIONS", "UBSAN_OPTIONS"}) {
        const char* inherited = std::getenv(name);
        const std::string prefix = inherited != nullptr && *inherited != '\0' ? std::string(inherited) + ":" : "";
        env.push_back(std::string(name) + "=" + prefix + "log_path=" + sanitizerLog);
    }
    if (!nameFree(names, phase)) return;
    FrameReadyLog frames;
    auto proxy = subscribeFrameReady(frames);
    SpawnedDaemon daemon(env, SpawnedDaemon::Stdout::Breakable, {}, 0, kDaemonArgs);
    // The config server's line is main's last stdout write before its event loop; a stall landing
    // earlier would block startup rather than the stop.
    if (!names.waitFor(true) || !daemon.waitForStdout("config server on", kWait)) {
        setupFailed(daemon, kGoals, phase + ": the daemon never came up serving");
        return;
    }
    // A device that is lost has to have been made first: a render before the stall creates it.
    if (lose && (!setWallpaper(*proxy, "static", blue, kMonitorBlue) || !frames.wait(kMonitorBlue, 1))) {
        setupFailed(daemon, kGoals, phase + ": the daemon did not draw before the loss");
        return;
    }
    const int stalledFd = stall == Stall::StdoutThenSigterm ? STDOUT_FILENO : STDERR_FILENO;
    if (stalledFd == STDERR_FILENO && !daemon.holdCapture()) {
        harnessFailure(phase + ": the capture reader did not stop reading, so stderr could not be stalled");
        return;
    }
    const StalledPipe stalled(daemon.pid(), stalledFd);
    if (!stalled.full()) {
        harnessFailure(phase + ": the daemon's output pipe could not be filled from /proc (" + std::strerror(errno) +
                       ")");
        return;
    }

    if (stall == Stall::StdoutThenSigterm) {
        ::kill(daemon.pid(), SIGTERM);
    } else if (stall == Stall::StderrThenSigusr1) {
        // Both may be pending at once: sigwait takes the lower-numbered SIGUSR1 first either way.
        ::kill(daemon.pid(), SIGUSR1);
        ::kill(daemon.pid(), SIGTERM);
    } else {
        const fs::path fired = home.trigger().string() + ".fired";
        if (!touch(home.trigger())) {
            harnessFailure(phase + ": the device-loss trigger file could not be created");
            return;
        }
        (void)setWallpaper(*proxy, "static", blue, kMonitorTrigger);
        const auto deadline = Clock::now() + kWait;
        std::error_code ec;
        while (!fs::exists(fired, ec) && Clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (!fs::exists(fired, ec)) {
            harnessFailure(phase + ": the preloaded shim never failed a submission, so no device was lost");
            return;
        }
    }

    const bool released = nameReleasedWithin(names, std::chrono::seconds(10));
    // Before the reap, which closes the capture pipe's read end: a write with no reader is SIGPIPE.
    const bool held = stalled.stillFull();
    const int exitCode = daemon.waitForExit();
    if (!held) {
        harnessFailure(phase + ": something drained the stalled pipe during the phase, so the daemon's output never "
                               "stalled and its stop says nothing about one");
        return;
    }
    check(released, (phase + ": the daemon's bus name is released within 10 s").c_str());
    check(!daemon.timedOut(), (phase + ": the daemon ends on its own, before the harness's timeout").c_str());
    std::error_code ec;
    bool sanitizerWrote = false;
    for (auto it = fs::directory_iterator(home.root(), ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (it->path().filename().string().rfind("sanitizer", 0) == 0) sanitizerWrote = true;
    }
    std::printf("device_loss_restart_test: %s: %zu bytes filled, exit code %d, signal %d%s\n", phase.c_str(), stalled.written(),
                exitCode, daemon.termSignal(), sanitizerWrote ? ", and the sanitizer wrote a report" : "");
}

}  // namespace

int main() {
    wallpaperx::testing::g_testName = "device_loss_restart_test";

    // S14.9, graded in name_watcher.h: before any phase, since each phase's name wait is
    // satisfied by a foreign owner.
    if (const std::optional<int> refused = wallpaperx::testing::refuseUnlessPrivateBus()) return *refused;

    try {
        ScratchDir assets("devloss_assets");
        if (!assets.ok()) {
            notVerified("every goal in this file", "a scratch directory for the fixtures could not be created");
            return wallpaperx::testing::reportNotVerifiedOrPassed();
        }
        const std::string blue =
            wallpaperx::testing::writeSolidPng((assets.path() / "blue.png").string(), 8, 8, {0, 0, 255, 255});
        const std::string red =
            wallpaperx::testing::writeSolidPng((assets.path() / "red.png").string(), 8, 8, {255, 0, 0, 255});

        NameWatcher names;
        checkStaticLossAndRestart(names, blue, red);
        checkRepeatedVideoLoss(names);
        checkNoDeviceIsNotLoss(names, blue, red);
        checkStuckLossStop(names, blue, OutsideSignals::None);
        checkStuckLossStop(names, blue, OutsideSignals::OneWhenStuck);
        checkOutsideWakeSignalIgnored(names, blue);
        checkStopWithStalledOutput(names, blue, Stall::StdoutThenSigterm);
        checkStopWithStalledOutput(names, blue, Stall::StderrThenSigusr1);
        checkStopWithStalledOutput(names, blue, Stall::StderrThenDeviceLoss);
    } catch (const std::exception& e) {
        check(false, (std::string("unexpected exception: ") + e.what()).c_str());
    }

    if (wallpaperx::testing::reportCheckFailures()) return 1;
    if (wallpaperx::testing::reportHarnessFaults("in this test's harness, not in the daemon")) return 1;
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
