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
#include "tests/test_phase.h"
#include "tests/test_report.h"

#include <sdbus-c++/sdbus-c++.h>

#include <nlohmann/json.hpp>

#include <algorithm>
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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

using wallpaperx::DbusService;
using wallpaperx::testing::blockedIn;
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
constexpr const char* kCleanStop = "stopped cleanly";
// daemon_system_test's kSignalShutdownLine.
constexpr const char* kSignalLine = "received, shutting down";

constexpr auto kWait = std::chrono::seconds(30);
// A daemon held by a log line does not answer at all; one that is not answers in milliseconds.
constexpr auto kPromptCall = std::chrono::seconds(2);

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

// How long after `from` the process `pid` ended, if it did within `limit`. It is not reaped, so
// SpawnedDaemon still collects the status.
std::optional<Clock::duration> endedWithin(pid_t pid, Clock::time_point from, Clock::duration limit) {
    for (;;) {
        siginfo_t info{};
        info.si_pid = 0;
        if (::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT) != 0) return std::nullopt;
        const auto now = Clock::now();
        if (info.si_pid != 0) return now - from;
        if (now - from >= limit) return std::nullopt;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

bool hasEnded(pid_t pid) { return endedWithin(pid, Clock::now(), {}).has_value(); }

// "exit code N" or "signal N" for an ended process that has not been reaped yet.
std::string howEnded(pid_t pid) {
    siginfo_t info{};
    info.si_pid = 0;
    if (::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT) != 0 || info.si_pid == 0) {
        return "an end /proc did not report";
    }
    return info.si_code == CLD_EXITED ? "exit code " + std::to_string(info.si_status)
                                      : "signal " + std::to_string(info.si_status);
}

double seconds(Clock::duration d) { return std::chrono::duration<double>(d).count(); }

// Touches the trigger, runs `provoke`, and waits for the shim's <trigger>.fired: the moment a
// submission failed. Nothing when it never fired.
// GOAL: a loss that never happened is attributed to whoever stopped it: a daemon that ended first
//       fails as the daemon (unless a preloaded library's own lookup killed it), one alive but not
//       answering GetStatus fails as the daemon too (held by its output, it submits nothing), and
//       only one alive and answering is this harness failing to lose the device.
// WHY-THIS-SHAPE: the file, not the shim's line, which it drops rather than block on a stream that
//       will not take it; a lost line would read as a loss that never happened. The file left by
//       an earlier round on the same HOME is removed first.
// CONTROL: in backend/daemon/dbus_service.cpp, `if (type == "static" && monitorId == 2) std::_Exit(3);`
//       added to DbusService::setWallpaper() before the request is recorded -- a still on monitor 2,
//       the request that provokes each loss -> "the daemon is still running when the shim fails its
//       next submission" red for the static, stalled-stderr and unfinished-render phases, and no
//       harness fault. Observed 2026-09-29 via control_run.sh on NVIDIA; reverted.
std::optional<Clock::time_point> loseDevice(const SpawnedDaemon& daemon, const Home& home, sdbus::IProxy& proxy,
                                            const std::function<void()>& provoke, const std::string& phase) {
    const fs::path fired = home.trigger().string() + ".fired";
    std::error_code ec;
    fs::remove(fired, ec);  // an earlier round's, on the same HOME
    if (!touch(home.trigger())) {
        harnessFailure(phase + ": the device-loss trigger file could not be created");
        return std::nullopt;
    }
    provoke();
    for (const auto deadline = Clock::now() + kWait; Clock::now() < deadline;) {
        if (fs::exists(fired, ec)) return Clock::now();
        if (hasEnded(daemon.pid())) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (fs::exists(fired, ec)) return Clock::now();
    if (hasEnded(daemon.pid())) {
        const std::string out = daemon.output();
        if (const size_t at = out.find("cannot resolve the real"); at != std::string::npos) {
            const size_t from = out.rfind('\n', at);
            const size_t start = from == std::string::npos ? 0 : from + 1;
            harnessFailure(phase + ": a preloaded test library killed the daemon -- " +
                           out.substr(start, out.find('\n', at) - start));
            return std::nullopt;
        }
        check(false, (phase + ": the daemon is still running when the shim fails its next submission").c_str());
        std::fprintf(stderr, "  %s: it ended first, with %s\n", phase.c_str(), howEnded(daemon.pid()).c_str());
        return std::nullopt;
    }
    if (!getStatus(proxy, kPromptCall).ok()) {
        check(false, (phase + ": the daemon still answers GetStatus, so a loss can be provoked").c_str());
        return std::nullopt;
    }
    harnessFailure(phase + ": the preloaded shim never failed a submission, so no device was lost -- check that " +
                   WALLPAPERX_DEVICE_LOST_SHIM + " loads");
    return std::nullopt;
}

// loseDevice, then the daemon's exit code in `exitCode`.
bool loseDeviceAndWait(SpawnedDaemon& daemon, const Home& home, sdbus::IProxy& proxy,
                       const std::function<void()>& provoke, const std::string& phase, int& exitCode) {
    if (!loseDevice(daemon, home, proxy, provoke, phase)) return false;
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
    // CONTROL: in backend/daemon/main.cpp, via control_run.sh, observed 2026-09-26 on NVIDIA,
    //       reverted. (a) kExitDeviceLost made 1 -> this line and both message lines red for the
    //       still and all three video rounds, with "every round ended in a device-loss exit", both
    //       stuck stops', the unfinished render's and the four stalled-output loss phases' status
    //       lines: 20. (b) `config.onDeviceLost` never set, so nothing hears of the loss -> "exits on
    //       its own", this line and both message lines for the still and every round, "every round
    //       ended ...", and the stalled-output loss phases' "ends within" (20 shown, the tool's
    //       limit). (c) The signal thread made to count its wake only with an outside signal
    //       (`reasonRecorded = received != kWakeSignal && ...`) -> as (b) less the "says the device
    //       was lost" lines: 17.
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
    // CONTROL: backend/daemon/main.cpp printing "stopped cleanly" whatever the exit status (an
    //       unconditional line ahead of the status-dependent one) -> the check below red for the
    //       still and all three video rounds, and nothing else. Observed 2026-09-24 via
    //       control_run.sh; reverted.
    check(out.find(kCleanStop) == std::string::npos,
          (phase + ": a device-loss stop does not report itself as a clean one").c_str());
    // GOAL: the log of a device-loss stop names no signal. The stop is carried out by waking the
    //       signal thread with a signal of the daemon's own (SIGUSR1), and an operator reading
    //       "User defined signal 1 received" would look for whoever sent it.
    // WHY-THIS-SHAPE: an absence check, kept able to fire by the no-device phase below, which
    //       asserts the same needle's presence on a SIGTERM stop: a reworded line turns that red.
    // CONTROL: the signal thread in backend/daemon/main.cpp made to print its "received" line
    //       for every stop (its first `if (outside)`, the one before std::printf, made `if (true)`)
    //       -> this line red for the still and all three video rounds, nothing else. Observed
    //       2026-09-25 via control_run.sh. Reverted.
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
    //       its stop as the device-loss one" red for the still and all three rounds: 11. Every
    //       "exits with status 75" line stayed green, the stalled-output loss phases' included.
    //       The exit is placed ahead of observe()'s loss callback, so main's stopBlamingWallpapers()
    //       never runs. Observed 2026-09-26 via control_run.sh on NVIDIA; reverted.
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
    //       "still plays", "every round ended in a device-loss exit", both final "after three
    //       device losses" lines and the stalled-output loss phases' "exits with status 75" (20
    //       shown, the tool's limit). The abort is placed ahead of observe()'s loss callback.
    //       Observed 2026-09-26 via control_run.sh on NVIDIA; reverted.
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
        if (!loseDeviceAndWait(daemon, home, *proxy,
                               [&] { (void)setWallpaper(*proxy, "static", blue, kMonitorTrigger); }, phase, exitCode)) {
            return;
        }
        checkLostExit(daemon, exitCode, phase);
    }

    checkNothingBlamed(home, phase, false);

    const std::map<int, std::string> saved = savedMonitors(home.configFile());
    const std::map<int, std::string> expected{{kMonitorBlue, blue}, {kMonitorRed, red}, {kMonitorTrigger, blue}};
    // CONTROL: backend/daemon/main.cpp's device-loss handler (`config.onDeviceLost`) made to delete
    //       the config file -> this line and all six restart lines below red, with the video
    //       phase's round-2 restart, its round count, both final lines and the unfinished-render
    //       phase's restart line: 12. Observed 2026-09-26 via control_run.sh on NVIDIA; reverted.
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
    //       behind makes the restart refuse that wallpaper. Both observed 2026-09-24 on NVIDIA
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
    //       The fuller list is in checkNothingBlamed's annotation. Observed 2026-09-26. Reverted.
    // COVERAGE-ASSERT: the number of losses that actually reached a device-loss exit is counted
    //       and asserted, so a loop that stopped early cannot read as three. Red under the
    //       exit-status-1, no-handler, wake-only and abort controls above, all 2026-09-26.
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
        if (!loseDeviceAndWait(daemon, home, *proxy, [] {}, phase, exitCode)) return;
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
    // CONTROL: in backend/daemon/wallpaper_manager.cpp, stopsForLostDevice() made to return
    //       `!m_device || m_device->lost()` (a missing device taken for a lost one) -> "no device: a
    //       later render is still attempted and reported" red, alone: the worker stops after the
    //       first render, and since no Device ever existed to report a loss the daemon stays up.
    //       The three stalled-output video-loss phases report NOT VERIFIED under it (the worker
    //       stopped before any device was made, so no video played). Observed 2026-09-25 via
    //       control_run.sh; reverted.
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
    if (!loseDevice(daemon, home, *proxy, [&] { (void)setWallpaper(*proxy, "static", still, kMonitorBlue); }, phase)) {
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
    // CONTROL: in backend/daemon/main.cpp via control_run.sh, observed 2026-09-26 on NVIDIA,
    //       reverted. (a) The signal thread's deadline exit made `_Exit(1)` and armTeardown's alarm
    //       removed, so only that path can end the stop -> "exits with status 75" red in both
    //       phases, alone. (b) onTeardownDeadline's DeviceLost branch (`exitNow(kExitDeviceLost,
    //       ...)`) removed and the signal thread's wait made 60 s, so only the alarm can end it, by
    //       SIGALRM -> the same two lines, with the unfinished-render phase's status line, which
    //       the alarm also ends. (c) The second-signal handler armed whatever woke the signal
    //       thread (its second `if (outside)` made `if (true)`) -> "not taken for a second ask",
    //       alone; both 75 lines stay green, as a forced exit keeps 75. (d) Both paths disabled ->
    //       "still ends" and "exits with status 75" red in both phases, alone. The stalled-output
    //       phases depend on neither deadline and stayed green under all four.
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
//       "ignoring" line's fprintf duplicated -> "exactly one line per outside SIGUSR1", alone;
//       observed 2026-09-25.
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

// ---------------------------------------------------------------------------------------------
// Stalled output. The daemon's descriptors 1 and 2 are pipes to its own log writer
// (backend/daemon/log_pump.cpp), which drains them into the real destination without ever
// blocking; these phases stall that destination -- this test's end of the pipe, socket or
// terminal, filled and no longer read (SpawnedDaemon::stallCapture) -- and grade what the daemon
// still does.
// ---------------------------------------------------------------------------------------------

// Where stdout and stderr both go.
enum class Destination { Pipe, Socket, Terminal };

const char* describe(Destination d) {
    return d == Destination::Pipe ? "pipe" : d == Destination::Socket ? "stream socket" : "terminal";
}

SpawnedDaemon::Stdout captureMode(Destination d) {
    return d == Destination::Pipe     ? SpawnedDaemon::Stdout::Captured
           : d == Destination::Socket ? SpawnedDaemon::Stdout::CapturedSocket
                                      : SpawnedDaemon::Stdout::CapturedTerminal;
}

// Nothing in a stop waits on a stalled destination longer than the log writer's 250 ms exit
// flush, so a stop must end well under the 3 s teardown deadline: one that took the deadline was
// held by the stall, and was only rescued.
constexpr auto kStalledStop = std::chrono::seconds(2);
constexpr auto kStalledLoss = std::chrono::milliseconds(2500);
constexpr auto kSecondSignal = std::chrono::seconds(1);

constexpr const char* kDroppedNote = "log line(s) dropped while output was blocked";

// A SetWallpaper the daemon logs and rejects: stdout gets "SetWallpaper(type=<type>, ...)" and
// stderr "SetWallpaper rejected (...): unknown wallpaper type <type> (...)", so each call writes
// one line to each stream, both carrying `seq`. `padding` lengthens both lines.
std::string markerType(size_t seq, size_t padding) {
    char head[32];
    std::snprintf(head, sizeof(head), "logmark-%06zu-", seq);
    return head + std::string(padding, 'x');
}

// Whether the marker call was answered, with its rejection, within `timeout`.
bool sendMarker(sdbus::IProxy& proxy, size_t seq, size_t padding, std::chrono::microseconds timeout = kPromptCall) {
    try {
        wallpaperx::testing::setWallpaper(proxy, markerType(seq, padding), "/nonexistent", kMonitorBlue, timeout);
    } catch (const sdbus::Error& e) {
        return e.getName() == "org.freedesktop.DBus.Error.InvalidArgs";
    }
    return false;
}

// The daemon's lines from `from` on, without the stall's filler (empty lines) or a terminal's "\r".
std::vector<std::string> linesFrom(const std::string& out, size_t from) {
    std::vector<std::string> lines;
    size_t at = std::min(from, out.size());
    while (at < out.size()) {
        size_t end = out.find('\n', at);
        if (end == std::string::npos) end = out.size();
        std::string line = out.substr(at, end - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(std::move(line));
        at = end + 1;
    }
    return lines;
}

// What marker call `seq` writes, as "<seq> out" and "<seq> err", when the line is whole: its full
// padding and the tail the daemon ends that line with. Anything else is "partial" or "".
std::string markerLine(const std::string& line, size_t padding) {
    const size_t at = line.find("logmark-");
    if (at == std::string::npos) return "";
    const size_t seq = std::strtoul(line.c_str() + at + 8, nullptr, 10);
    const bool whole = line.find(markerType(seq, padding)) != std::string::npos;
    const auto endsWith = [&line](const std::string& tail) {
        return line.size() >= tail.size() && line.compare(line.size() - tail.size(), tail.size(), tail) == 0;
    };
    if (whole && line.find("SetWallpaper(type=") != std::string::npos && endsWith("monitor=0)")) {
        return std::to_string(seq) + " out";
    }
    if (whole && line.find("SetWallpaper rejected") != std::string::npos && endsWith("video or shader)")) {
        return std::to_string(seq) + " err";
    }
    return "partial";
}

// What `count` marker calls from `first` write, in the order written.
std::vector<std::string> expectedMarkers(size_t first, size_t count) {
    std::vector<std::string> out;
    for (size_t seq = first; seq < first + count; ++seq) {
        out.push_back(std::to_string(seq) + " out");
        out.push_back(std::to_string(seq) + " err");
    }
    return out;
}

// The marker lines, and every dropped-lines note, in `lines`, in order: "<seq> out", "<seq> err",
// "partial", or "note <N>".
std::vector<std::string> markersAndNotes(const std::vector<std::string>& lines, size_t padding) {
    std::vector<std::string> out;
    for (const std::string& line : lines) {
        if (const size_t at = line.find(kDroppedNote); at != std::string::npos) {
            const size_t colon = line.rfind(": ", at);
            out.push_back("note " + std::to_string(std::strtoull(
                                        line.c_str() + (colon == std::string::npos ? 0 : colon + 2), nullptr, 10)));
        } else if (std::string kind = markerLine(line, padding); !kind.empty()) {
            out.push_back(std::move(kind));
        }
    }
    return out;
}

std::string joined(const std::vector<std::string>& items, size_t limit = 12) {
    std::string text;
    for (size_t i = 0; i < items.size() && i < limit; ++i) text += (i ? ", " : "") + items[i];
    if (items.size() > limit) text += ", ... (" + std::to_string(items.size()) + " in all)";
    return text;
}

// `env` for a daemon whose output will stall. A sanitizer report written to the stalled stream
// would never be read, so reports go to a file in the HOME.
std::vector<std::string> stalledEnv(const Home& home, std::vector<std::string> env) {
    const std::string sanitizerLog = (home.root() / "sanitizer").string();
    for (const char* name : {"TSAN_OPTIONS", "ASAN_OPTIONS", "UBSAN_OPTIONS"}) {
        const char* inherited = std::getenv(name);
        const std::string prefix = inherited != nullptr && *inherited != '\0' ? std::string(inherited) + ":" : "";
        env.push_back(std::string(name) + "=" + prefix + "log_path=" + sanitizerLog);
    }
    return env;
}

// Whether stalledEnv's sanitizer log was written: the exit status is then the sanitizer's.
bool sanitizerWroteIn(const Home& home) {
    std::error_code ec;
    for (auto it = fs::directory_iterator(home.root(), ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (it->path().filename().string().rfind("sanitizer", 0) == 0) return true;
    }
    return false;
}

// A stalled phase's exit status, unless a sanitizer took it over (its report went to a file in the
// HOME): then that is what goes red, named as such.
void checkStalledStatus(const SpawnedDaemon& daemon, const Home& home, int exitCode, int expected,
                        const std::string& what) {
    if (sanitizerWroteIn(home)) {
        check(false, (what + " -- not graded: the sanitizer wrote a report, and the exit status is the sanitizer's")
                         .c_str());
        return;
    }
    const bool held = daemon.exitedNormally() && exitCode == expected;
    check(held, what.c_str());
    if (!held) std::fprintf(stderr, "  exit code %d, signal %d\n", exitCode, daemon.termSignal());
}

// Which one output stops draining, and what then ends the daemon.
enum class OneStalled {
    StdoutThenSigterm,
    StderrThenSigusr1,  // an outside SIGUSR1, ignored with a line, then a SIGTERM
    StderrThenDeviceLoss,
};

// GOAL: S14.11/S14.14 -- with ONE of stdout and stderr on a reader that stopped reading, and the
//       other a stream with room left (stderr read by the capture thread; stdout an unread pipe far
//       short of its capacity), a SIGTERM, an ignored outside SIGUSR1 then a SIGTERM, or a device
//       loss ends the daemon under the 3 s teardown deadline, with status 0, 0 and 75; a bus call
//       that logs to both is still answered. S14.14 is the first: the clean stop exits 0, not by
//       SIGALRM. NOT GRADED: in the stderr phases, a daemon writing more than a pipe's capacity to
//       stdout, which nothing here drains.
// WHY-THIS-SHAPE: stdout on a pipe of its own (Stdout::Breakable), so the log writer runs two
//       queues and one of them stalls. The stall is this test's end of that pipe, filled, and
//       asked for one more byte after the end, so a phase whose stream drained is a harness fault,
//       not a verdict. The end is measured with waitid(WNOWAIT) against 2 s (2.5 s for a loss):
//       the teardown alarm would end a held stop at 3 s with the same status, so "it ended" or
//       "exit 0" alone would pass a stop the stall held -- as the controls show. Those bounds
//       sit below the 3 s they must be told apart from; a stop here takes the 250 ms flush and a
//       few tens of ms more.
// CONTROL: via control_run.sh 2026-09-26 on NVIDIA, all reverted. (a) logpump::release() in
//       backend/daemon/log_pump.cpp given an hour instead of kExitFlush, the flush main()'s return
//       waits on -> the three "ends within" lines red; every status line stays green, since the
//       alarm still exits 0 or 75. (b) The same with `g_finalStatus.store(exitStatus);` deleted in
//       backend/daemon/main.cpp -> also "exits 0, not by SIGALRM" and the SIGUSR1 phase's "exits
//       0". (c) The log writer never installed (main()'s `logpump::install()` skipped) -> both
//       stops' "ends within" and all three "still answered" lines, with the loss phase's "still
//       answers GetStatus".
void checkOneStreamStalled(NameWatcher& names, const std::string& blue, OneStalled stall) {
    const bool onStdout = stall == OneStalled::StdoutThenSigterm;
    const bool lose = stall == OneStalled::StderrThenDeviceLoss;
    const char* goals = onStdout ? "S14.14 (a clean stop with stdout stalled exits 0)"
                                 : "S14.11 (a stop with stderr stalled)";
    const std::string phase = onStdout ? "stalled stdout, SIGTERM"
                              : lose   ? "stalled stderr, device loss"
                                       : "stalled stderr, outside SIGUSR1 then SIGTERM";
    Home home("devloss_stall");
    if (!home.ok()) {
        notVerified(goals, "a scratch HOME could not be created under the temp directory");
        return;
    }
    if (!nameFree(names, phase)) return;
    FrameReadyLog frames;
    auto proxy = subscribeFrameReady(frames);
    SpawnedDaemon daemon(stalledEnv(home, home.env(lose)), SpawnedDaemon::Stdout::Breakable, {}, 0, kDaemonArgs);
    if (!names.waitFor(true) || !daemon.waitForStdout("config server on", kWait)) {
        setupFailed(daemon, goals, phase + ": the daemon never came up serving");
        return;
    }
    // A device that is lost has to have been made first: a render before the stall creates it.
    if (lose && (!setWallpaper(*proxy, "static", blue, kMonitorBlue) || !frames.wait(kMonitorBlue, 1))) {
        setupFailed(daemon, goals, phase + ": the daemon did not draw before the loss");
        return;
    }
    std::string why = "the stdout pipe could not be filled from this side";
    if (!(onStdout ? daemon.stallStdout() : daemon.stallCapture(why))) {
        harnessFailure(phase + ": " + why);
        return;
    }
    const bool answered = sendMarker(*proxy, 0, 64);

    std::optional<Clock::time_point> from = Clock::now();
    if (stall == OneStalled::StdoutThenSigterm) {
        ::kill(daemon.pid(), SIGTERM);
    } else if (stall == OneStalled::StderrThenSigusr1) {
        // Both may be pending at once: sigwait takes the lower-numbered SIGUSR1 first either way.
        ::kill(daemon.pid(), SIGUSR1);
        ::kill(daemon.pid(), SIGTERM);
    } else {
        check(answered, (phase + ": a bus call that logs to both streams is still answered within 2 s").c_str());
        from = loseDevice(daemon, home, *proxy, [&] { (void)setWallpaper(*proxy, "static", blue, kMonitorTrigger); },
                          phase);
        if (!from) return;
    }
    const auto bound = lose ? Clock::duration(kStalledLoss) : Clock::duration(kStalledStop);
    const std::optional<Clock::duration> ended = endedWithin(daemon.pid(), *from, bound);
    // Before the reap, which closes this test's read end.
    const bool held = onStdout ? daemon.stdoutStillStalled() : daemon.captureStillStalled();
    // Waited for past the bound, so the status of an end the deadline forced is still graded.
    const int exitCode = daemon.waitForExit();
    if (!held) {
        harnessFailure(phase + ": something drained the stalled stream during the phase, so the daemon's output never "
                               "stalled and its stop says nothing about one");
        return;
    }
    if (!lose) check(answered, (phase + ": a bus call that logs to both streams is still answered within 2 s").c_str());
    check(ended.has_value(), (phase + (lose ? ": the daemon ends within 2.5 s of the failed submission"
                                            : ": the daemon ends within 2 s, under the 3 s teardown deadline"))
                                 .c_str());
    if (!daemon.timedOut()) {
        checkStalledStatus(daemon, home, exitCode, lose ? kExitDeviceLost : 0,
                           phase + (onStdout ? ": a clean stop whose final line cannot be written exits 0, not by SIGALRM"
                                             : lose ? ": exits with status 75" : ": exits 0"));
    }
    std::printf("device_loss_restart_test: %s: %zu bytes filled, %s, exit code %d, signal %d\n", phase.c_str(),
                onStdout ? daemon.stdoutStallBytes() : daemon.stallBytes(),
                ended ? ("ended " + std::to_string(seconds(*ended)) + " s after the provocation").c_str()
                      : "still running at the bound, killed",
                exitCode, daemon.termSignal());
}

// A phase's daemon on a shared destination, up with its bus name and its startup lines read.
// False (reported) if it never came up.
bool cameUp(NameWatcher& names, const SpawnedDaemon& daemon, const char* goals, const std::string& phase) {
    if (names.waitFor(true) && daemon.waitForOutput("config server on", kWait)) return true;
    setupFailed(daemon, goals, phase + ": the daemon never came up serving");
    return false;
}

// One picture of `monitorId` fetched and handed back, as a consumer does. False when the fetch was
// refused (no image free yet) or failed.
bool collectPicture(sdbus::IProxy& proxy, int32_t monitorId) {
    try {
        const wallpaperx::testing::Frame frame = wallpaperx::testing::getFrame(proxy, monitorId, {.timeout = kPromptCall});
        wallpaperx::testing::releaseFrame(proxy, monitorId, frame, kPromptCall);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// Bound to a screen that is drawn and collected from, so the clock keeps submitting and the render
// thread has a "stopped playing" line to write when a loss lands. False if this host never
// played it to a watching screen.
bool playWatchedVideo(sdbus::IProxy& proxy, FrameReadyLog& frames, const std::string& clip, const std::string& phase) {
    bool playing = false;
    try {
        wallpaperx::testing::setMonitorOutput(proxy, kMonitorVideo, "DP-9", std::chrono::seconds(5));
        wallpaperx::testing::setOutputVisible(proxy, "DP-9", true, std::chrono::seconds(5));
        playing = setWallpaper(proxy, "video", clip, kMonitorVideo) && frames.waitAtLeast(kMonitorVideo, 3);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "  %s: %s\n", phase.c_str(), e.what());
    }
    // Collected and handed back, as a consumer does; a refused fetch (no image free yet) is simply
    // asked again.
    for (const auto until = Clock::now() + std::chrono::seconds(1); playing && Clock::now() < until;) {
        (void)collectPicture(proxy, kMonitorVideo);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    const StatusReply watchedNow = getStatus(proxy, std::chrono::seconds(5));
    const nlohmann::json* entry = wallpaperStatus(watchedNow, std::to_string(kMonitorVideo));
    return playing && entry != nullptr && entry->value("watched", false);
}

// GOAL: S14.21 -- with stdout and stderr on one destination nobody reads (a pipe, a stream socket
//       as journald gives a unit, a terminal suspended as by Ctrl-S), the daemon answers
//       promptly: 40 SetWallpaper calls that each log to both streams, GetStatus, and a new
//       still drawn, announced and handed over by GetFrame, each within 2 s.
// GOAL: S14.22 -- a brief stall loses nothing: once the reader resumes, every line written during
//       it arrives, whole, in the order written -- stdout's and stderr's interleaved as they were
//       written, on the shared destination -- and no dropped-lines note appears.
// WHY-THIS-SHAPE: 40 calls of about 3.8 KB of log each is ~150 KB: more than the log writer's own
//       input pipe holds (64 KiB), so a writer that blocked on the destination would block the
//       bus thread part way through, and less than its 256 KiB queue, so a correct one drops
//       nothing. The order is read off the subsequence of lines carrying this test's markers, so
//       a render's own line may sit between them. A marker sent after the reader resumes is
//       waited for, so the comparison is on output complete up to it, not on whatever had arrived.
//       The draw is bounded by its measured time, not by FrameReadyLog::wait, whose 30 s is only a
//       hang guard: a correct draw takes under 10 ms on NVIDIA and Lavapipe, a held one lasts until
//       the reader resumes, after this check.
// CONTROL: in backend/daemon/dbus_service.cpp via control_run.sh 2026-09-27 on NVIDIA, reverted:
//       emitFrameReady made to sleep 3 s for monitor 1 -> "a new still is drawn and announced
//       within 2 s" red on all three destinations, alone.
// CONTROL: in backend/daemon/log_pump.cpp via control_run.sh 2026-09-25 on NVIDIA, each red on the
//       pipe, socket and terminal phases, all reverted. (a) The destination written blocking
//       (O_NONBLOCK dropped from its /proc reopen, MSG_DONTWAIT from its send) -> all four S14.21
//       lines and the order line (observed 2026-09-27). (b) One queue per stream whatever the layout (`used = shared ? 2 : 2`) -> the order
//       line: stdout's lines all came out before stderr's. (c) kQueueLimit 32 KiB -> the same
//       line, alone: lines were dropped. (d) kMaxLine 1024 -> the same line, every marker split.
//       The log writer never installed (backend/daemon/main.cpp) -> all five.
void checkServesWhileStalled(NameWatcher& names, const std::string& blue, const std::string& red, Destination dest) {
    constexpr const char* kGoals = "S14.21/S14.22 (serving through, and losing nothing to, a brief output stall)";
    constexpr size_t kCalls = 40;
    constexpr size_t kPadding = 1800;
    const std::string phase = std::string("stalled ") + describe(dest) + ", serving";
    Home home("devloss_serve");
    if (!home.ok()) {
        notVerified(kGoals, "a scratch HOME could not be created under the temp directory");
        return;
    }
    if (!nameFree(names, phase)) return;
    FrameReadyLog frames;
    auto proxy = subscribeFrameReady(frames);
    SpawnedDaemon daemon(stalledEnv(home, home.env(false)), captureMode(dest), {}, 0, kDaemonArgs);
    if (!cameUp(names, daemon, kGoals, phase)) return;
    if (!setWallpaper(*proxy, "static", blue, kMonitorBlue) || !frames.wait(kMonitorBlue, 1)) {
        setupFailed(daemon, kGoals, phase + ": the daemon did not draw before the stall");
        return;
    }
    std::string why;
    if (!daemon.stallCapture(why)) {
        harnessFailure(phase + ": " + why);
        return;
    }

    size_t answered = 0;
    Clock::duration slowest{};
    for (size_t seq = 0; seq < kCalls; ++seq) {
        const auto start = Clock::now();
        if (!sendMarker(*proxy, seq, kPadding)) break;
        slowest = std::max(slowest, Clock::now() - start);
        ++answered;
    }
    const bool status = answered == kCalls && getStatus(*proxy, kPromptCall).ok();
    // The 30 s wait only guards against a hang; the bound is the elapsed time, so a draw the
    // stall held for seconds is still a FAIL. GetFrame is asked of any announced frame, so its
    // line grades the hand-over alone.
    const auto drawStart = Clock::now();
    const bool announced = status &&
                           wallpaperx::testing::trySetWallpaper(*proxy, "static", red, kMonitorRed, kPromptCall) &&
                           frames.wait(kMonitorRed, 1);
    const Clock::duration drawTook = Clock::now() - drawStart;
    const bool drew = announced && drawTook <= kPromptCall;
    bool served = false;
    if (announced) {
        try {
            served = wallpaperx::testing::getFrame(*proxy, kMonitorRed, {.timeout = kPromptCall}).fd.isValid();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "  %s: GetFrame: %s\n", phase.c_str(), e.what());
        }
    }
    const bool held = daemon.captureStillStalled();
    // What arrives now is ~150 KB of marker lines, read from output() below rather than echoed.
    daemon.echo(false);
    daemon.resumeCapture();
    // Only once the stall is known to have held is anything graded.
    if (!held) {
        daemon.shutdown(SIGTERM);
        harnessFailure(phase + ": something drained the stalled stream during the phase, so it says nothing about "
                               "a stall");
        return;
    }
    check(answered == kCalls, (phase + ": each of 40 SetWallpaper calls that log is answered within 2 s").c_str());
    check(status, (phase + ": GetStatus is answered within 2 s").c_str());
    check(drew, (phase + ": a new still is drawn and announced within 2 s").c_str());
    if (announced && !drew) std::fprintf(stderr, "  %s: announced %.3f s after SetWallpaper\n", phase.c_str(), seconds(drawTook));
    check(served, (phase + ": GetFrame hands the drawn frame over within 2 s").c_str());

    const bool endArrived = sendMarker(*proxy, kCalls, kPadding, std::chrono::seconds(10)) &&
                            waitForOccurrences(daemon, markerType(kCalls, 0), 2);
    const std::vector<std::string> seen = markersAndNotes(linesFrom(daemon.output(), daemon.stalledAt()), kPadding);
    const std::vector<std::string> expected = expectedMarkers(0, kCalls + 1);
    check(endArrived && seen == expected,
          (phase + ": once the reader resumes, every line written during the stall arrives, whole and in the order "
                   "written, stdout's and stderr's interleaved, with nothing dropped")
              .c_str());
    if (seen != expected) {
        std::fprintf(stderr, "  expected: %s\n  seen:     %s\n", joined(expected).c_str(), joined(seen).c_str());
    }
    const int exitCode = daemon.shutdown(SIGTERM);
    checkStatus(daemon, exitCode, 0, phase + ": exits 0 on SIGTERM once the reader is back");
    std::printf("device_loss_restart_test: %s: %zu bytes filled, slowest of %zu logging calls %.3f s, still drawn "
                "and announced in %.3f s, %zu marker lines after resuming\n",
                phase.c_str(), daemon.stallBytes(), answered, seconds(slowest), seconds(drawTook), seen.size());
}

// GOAL: S14.23 -- a long stall drops lines, and says so exactly: once the reader resumes, the
//       lines kept are exactly the first k written, each whole and in order, followed by exactly
//       one note "N log line(s) dropped while output was blocked" with k + N equal to the lines
//       written, and a line written after that arrives after the note.
// WHY-THIS-SHAPE: 150 calls, ~570 KB: past the 256 KiB queue and the destination's own room, so
//       lines must be dropped. Every call is answered before the reader resumes, so what is kept
//       is what was queued before the first drop -- a prefix of what was written -- and the note
//       stands for the rest; a dropped line counted twice, or a kept one counted, breaks k + N.
// CONTROL: in backend/daemon/log_pump.cpp via control_run.sh 2026-09-25 on NVIDIA, reverted, each
//       on all three destinations. (a) noticeIfRoom()'s appendNotice() removed -> all three
//       long-stall lines. (b) The note's count printed one too high -> the "add up" line, alone.
//       Also red under the blocking-destination, one-queue-per-stream, split-line and unbounded
//       controls.
// COVERAGE-ASSERT: k >= 1 and N >= 1 are part of the check, so it is never arithmetic over a stall
//       that dropped nothing or kept nothing.
void checkLongStall(NameWatcher& names, Destination dest) {
    constexpr const char* kGoals = "S14.23 (a long output stall drops lines and says how many)";
    constexpr size_t kCalls = 150;
    constexpr size_t kPadding = 1800;
    const std::string phase = std::string("stalled ") + describe(dest) + ", long stall";
    Home home("devloss_long");
    if (!home.ok()) {
        notVerified(kGoals, "a scratch HOME could not be created under the temp directory");
        return;
    }
    if (!nameFree(names, phase)) return;
    FrameReadyLog frames;
    auto proxy = subscribeFrameReady(frames);
    SpawnedDaemon daemon(stalledEnv(home, home.env(false)), captureMode(dest), {}, 0, kDaemonArgs);
    if (!cameUp(names, daemon, kGoals, phase)) return;
    daemon.echo(false);
    std::string why;
    if (!daemon.stallCapture(why)) {
        harnessFailure(phase + ": " + why);
        return;
    }
    size_t answered = 0;
    while (answered < kCalls && sendMarker(*proxy, answered, kPadding)) ++answered;
    const bool held = daemon.captureStillStalled();
    daemon.resumeCapture();
    if (!held) {
        daemon.shutdown(SIGTERM);
        harnessFailure(phase + ": something drained the stalled stream during the phase");
        return;
    }
    check(answered == kCalls, (phase + ": each of 150 logging calls is answered within 2 s").c_str());
    const bool noted = daemon.waitForOutput(kDroppedNote, kWait);
    const bool endArrived = noted && sendMarker(*proxy, kCalls, kPadding, std::chrono::seconds(10)) &&
                            waitForOccurrences(daemon, markerType(kCalls, 0), 2);
    const std::vector<std::string> seen = markersAndNotes(linesFrom(daemon.output(), daemon.stalledAt()), kPadding);
    const std::vector<std::string> written = expectedMarkers(0, kCalls);

    size_t kept = 0;
    while (kept < seen.size() && kept < written.size() && seen[kept] == written[kept]) ++kept;
    size_t notes = 0;
    for (const std::string& item : seen) notes += item.rfind("note ", 0) == 0 ? 1 : 0;
    const bool noteNext = kept < seen.size() && seen[kept].rfind("note ", 0) == 0;
    const unsigned long long dropped = noteNext ? std::strtoull(seen[kept].c_str() + 5, nullptr, 10) : 0;
    const std::vector<std::string> after(seen.begin() + static_cast<std::ptrdiff_t>(std::min(kept + 1, seen.size())),
                                         seen.end());

    check(noted && notes == 1 && noteNext,
          (phase + ": exactly one dropped-lines note, straight after the lines kept").c_str());
    check(kept >= 1 && dropped >= 1 && kept + dropped == written.size(),
          (phase + ": the lines kept are the first ones written, whole and in order, and with the note's count "
                   "they add up to every line written")
              .c_str());
    check(endArrived && after == expectedMarkers(kCalls, 1),
          (phase + ": a line written after the stall arrives after the note, and nothing else does").c_str());
    std::printf("device_loss_restart_test: %s: %zu lines written, %zu kept, the note counts %llu dropped, %zu "
                "note(s)\n",
                phase.c_str(), written.size(), kept, dropped, notes);
    if (!(kept + dropped == written.size() && notes == 1)) {
        std::fprintf(stderr, "  seen: %s\n", joined(seen, 400).c_str());
    }
    const int exitCode = daemon.shutdown(SIGTERM);
    checkStatus(daemon, exitCode, 0, phase + ": exits 0 on SIGTERM once the reader is back");
}

// Resident memory of `pid` in bytes, from /proc; nothing when it cannot be read.
std::optional<size_t> residentBytes(pid_t pid) {
    std::ifstream in("/proc/" + std::to_string(pid) + "/status");
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("VmRSS:", 0) == 0) return std::strtoull(line.c_str() + 6, nullptr, 10) * 1024;
    }
    return std::nullopt;
}

// GOAL: S14.24 -- what the log writer holds for a stalled destination is bounded: while stalled,
//       writing ~24 MB of log grows the daemon's resident memory by less than a quarter of that.
//       False if the queue grows with what is written, which on a journald that stalled for good
//       ends in the OOM killer.
// WHY-THIS-SHAPE: memory is sampled once the queue is already full and dropping (after the first
//       200 calls), then after the rest, so what a bounded queue reserved up front is in both
//       samples and cancels. The bound is relative to the bytes written in between -- far above
//       allocator noise, far below an unbounded queue's growth; an absolute figure would grade the
//       allocator.
// CONTROL: in backend/daemon/log_pump.cpp via control_run.sh 2026-09-25 on NVIDIA, reverted.
//       kQueueLimit made 1 GiB -> the resident-memory line red (with the long-stall lines: nothing
//       was dropped). The destination written blocking -> the coverage line red: calls stopped
//       being answered.
// COVERAGE-ASSERT: the bytes written between the samples are counted from the calls answered and
//       must reach 16 MB, so a loop cut short cannot pass on a small window. An unreadable VmRSS
//       is NOT VERIFIED, never a pass: residentBytes() made to find nothing, by hand 2026-09-25,
//       reverted -> this phase NOT VERIFIED, no FAIL.
void checkQueueBounded(NameWatcher& names) {
    constexpr const char* kGoals = "S14.24 (the log queue for a stalled destination is bounded)";
    constexpr size_t kPadding = 3000;
    constexpr size_t kWarmup = 200;
    constexpr size_t kCalls = 4200;
    const std::string phase = "stalled pipe, memory";
    Home home("devloss_mem");
    if (!home.ok()) {
        notVerified(kGoals, "a scratch HOME could not be created under the temp directory");
        return;
    }
    if (!nameFree(names, phase)) return;
    FrameReadyLog frames;
    auto proxy = subscribeFrameReady(frames);
    SpawnedDaemon daemon(stalledEnv(home, home.env(false)), SpawnedDaemon::Stdout::Captured, {}, 0, kDaemonArgs);
    if (!cameUp(names, daemon, kGoals, phase)) return;
    daemon.echo(false);
    std::string why;
    if (!daemon.stallCapture(why)) {
        harnessFailure(phase + ": " + why);
        return;
    }
    size_t answered = 0;
    while (answered < kWarmup && sendMarker(*proxy, answered, kPadding)) ++answered;
    const std::optional<size_t> before = residentBytes(daemon.pid());
    size_t bytes = 0;
    while (answered < kCalls && sendMarker(*proxy, answered, kPadding)) {
        bytes += 2 * (markerType(answered, kPadding).size() + 80);  // both lines, each at least this long
        ++answered;
    }
    const std::optional<size_t> after = residentBytes(daemon.pid());
    const bool held = daemon.captureStillStalled();
    const int exitCode = daemon.shutdown(SIGTERM);
    if (!held) {
        harnessFailure(phase + ": something drained the stalled stream during the phase");
        return;
    }
    if (!before || !after) {
        notVerified(kGoals, phase + ": /proc does not show this test the daemon's resident memory");
        return;
    }
    const long long grew = static_cast<long long>(*after) - static_cast<long long>(*before);
    check(answered == kCalls && bytes >= (size_t{16} << 20),
          (phase + ": every logging call is answered, over at least 16 MB of log").c_str());
    check(grew < static_cast<long long>(bytes / 4),
          (phase + ": resident memory grows by less than a quarter of what was logged while stalled").c_str());
    std::printf("device_loss_restart_test: %s: %zu calls, %.1f MB logged while stalled, resident memory %.1f -> %.1f "
                "MB\n",
                phase.c_str(), answered, bytes / 1048576.0, *before / 1048576.0, *after / 1048576.0);
    checkStatus(daemon, exitCode, 0, phase + ": exits 0 on SIGTERM");
}

// What ends a daemon whose shared destination has stalled.
enum class StalledEnd {
    Sigterm,
    SecondSigterm,  // a SIGTERM whose teardown is wedged (tests/wedge_shim.cpp), then a second
    DeviceLoss,     // while a watched video's render thread has lines to write
};

// GOAL: S14.12 -- with stdout and stderr on one destination nobody reads (pipe, stream socket,
//       suspended terminal), SIGTERM ends the daemon with status 0 within 2 s, under the 3 s
//       teardown deadline; and a second SIGTERM on a stop wedged in its teardown ends it within
//       1 s, with status 1. False if either end waits on the destination.
// GOAL: S14.13 -- in the same layout, with a watched video playing so the render thread has a
//       line to write, a device loss ends the daemon with status 75 within 2.5 s of the shim's
//       failed submission (<trigger>.fired). False if the loss's stop waits on any log line.
// WHY-THIS-SHAPE: ends are measured with waitid(WNOWAIT) against bounds under the 3 s deadline:
//       the deadline would end a held stop too, with the same status, so only the time tells a
//       stop the stall held from one it did not. A logging call is made while stalled, so the
//       log writer has a line queued it cannot write when the stop begins. The wedged stop is
//       confirmed before the second signal: alive 1 s in, its main thread in pause() inside the
//       wedge -- otherwise NOT VERIFIED, since a stop that was not stuck has nothing to force.
// CONTROL: via control_run.sh on NVIDIA, each on all three destinations, all reverted.
//       Observed 2026-09-26: (a) logpump::release() in backend/daemon/log_pump.cpp given an
//       hour -> the SIGTERM and loss "ends within" lines; statuses stay 0 and 75, the alarm's.
//       (b) As (a) with `g_finalStatus.store(exitStatus);` deleted in backend/daemon/main.cpp ->
//       also the SIGTERM "exits 0" lines. (c) flushBeforeExit()'s flush in backend/daemon/main.cpp,
//       which every forced exit runs, given an hour -> the three second-SIGTERM "within 1 s" lines,
//       alone. Observed 2026-09-25: (d) the log writer never installed -> the SIGTERM lines and
//       "the video is still drawn", and the second-SIGTERM phases NOT VERIFIED: the main thread
//       was in write(), not the wedge; 2026-09-26 its red list ran past the tool's 20 lines and
//       showed these phases' SIGTERM lines among them.
// COVERAGE-ASSERT: the destination is asked for one more byte once the daemon has ended; one
//       that took it had drained, and the phase is a harness fault, not a verdict. By hand
//       2026-09-25, reverted: SpawnedDaemon::stallCapture() made to resume the stream straight
//       after filling it -> every stalled phase in this file (serving, long stall, memory, the
//       three ends, both stderr phases) reported "FAIL (harness): something drained", and no FAIL.
//       blockedIn() made to answer nothing -> the three second-SIGTERM phases NOT VERIFIED.
void checkStalledEnd(NameWatcher& names, const std::string& blue, Destination dest, StalledEnd end) {
    const bool lose = end == StalledEnd::DeviceLoss;
    const bool wedged = end == StalledEnd::SecondSigterm;
    const char* goals = lose ? "S14.13 (a device loss with stdout and stderr stalled)"
                             : "S14.12 (a stop with stdout and stderr stalled)";
    const std::string phase = std::string("stalled ") + describe(dest) + ", " +
                              (lose ? "device loss while a video plays" : wedged ? "second SIGTERM" : "SIGTERM");
    const std::string clip = std::string(WALLPAPERX_VIDEO_ASSET_DIR) + "/wallpaper_bands.mp4";
    std::error_code ec;
    if ((lose || wedged) && !fs::exists(clip, ec)) {
        notVerified(goals, "the video fixture " + clip + " was not generated (no ffmpeg at build time)");
        return;
    }
    Home home("devloss_end");
    if (!home.ok()) {
        notVerified(goals, "a scratch HOME could not be created under the temp directory");
        return;
    }
    if (!nameFree(names, phase)) return;
    FrameReadyLog frames;
    auto proxy = subscribeFrameReady(frames);
    SpawnedDaemon daemon(stalledEnv(home, wedged ? stuckStopEnv(home) : home.env(lose)), captureMode(dest), {}, 0,
                         kDaemonArgs);
    if (!cameUp(names, daemon, goals, phase)) return;
    if (lose && !playWatchedVideo(*proxy, frames, clip, phase)) {
        setupFailed(daemon, goals, phase + ": this host never played the video to a watching screen");
        return;
    }
    if (wedged && (!setWallpaper(*proxy, "video", clip, kMonitorVideo) || !frames.waitAtLeast(kMonitorVideo, 3) ||
                   !awaitUnwatched(*proxy, daemon, kMonitorVideo))) {
        setupFailed(daemon, goals, phase + ": this host never played the video and let it go idle, so the teardown "
                                           "holds no decoder to wedge");
        return;
    }
    if (end == StalledEnd::Sigterm && (!setWallpaper(*proxy, "static", blue, kMonitorBlue) || !frames.wait(kMonitorBlue, 1))) {
        setupFailed(daemon, goals, phase + ": the daemon did not draw before the stall");
        return;
    }
    std::string why;
    if (!daemon.stallCapture(why)) {
        harnessFailure(phase + ": " + why);
        return;
    }
    (void)sendMarker(*proxy, 0, 64);

    std::optional<Clock::duration> ended;
    Clock::duration bound = kStalledStop;
    if (lose) {
        // The render thread logs as it plays; a video that stops being drawn is a daemon held by its
        // output, which is its failure, not a loss this harness failed to provoke. Collected on
        // every pass and once more just before the trigger: a picture nobody collects for 2 s marks
        // the screen unwatched and stops the clock, and then no submission would meet the trigger.
        // By hand 2026-09-25, reverted: collectPicture() made to fail -> NOT VERIFIED, no FAIL.
        const size_t shown = frames.countFor(kMonitorVideo);
        bool advancing = false;
        for (const auto until = Clock::now() + std::chrono::seconds(5); !advancing && Clock::now() < until;) {
            (void)collectPicture(*proxy, kMonitorVideo);
            advancing = frames.countFor(kMonitorVideo) >= shown + 2;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(advancing, (phase + ": the video is still drawn with its output stalled").c_str());
        if (!advancing) {
            daemon.shutdown(SIGKILL);
            return;
        }
        bool collected = false;
        for (const auto until = Clock::now() + std::chrono::seconds(2); !collected && Clock::now() < until;) {
            collected = collectPicture(*proxy, kMonitorVideo);
            if (!collected) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (!collected) {
            daemon.shutdown(SIGKILL);
            setupFailed(daemon, goals, phase + ": no picture could be collected just before the loss, so the video "
                                               "may have been unwatched when the trigger was touched");
            return;
        }
        // No request provokes it: the playback clock's next submission is the one the shim fails.
        const std::optional<Clock::time_point> fired = loseDevice(daemon, home, *proxy, [] {}, phase);
        if (!fired) return;
        bound = kStalledLoss;
        ended = endedWithin(daemon.pid(), *fired, bound);
    } else if (!wedged) {
        const auto sent = Clock::now();
        ::kill(daemon.pid(), SIGTERM);
        ended = endedWithin(daemon.pid(), sent, bound);
    } else {
        ::kill(daemon.pid(), SIGTERM);
        if (endedWithin(daemon.pid(), Clock::now(), std::chrono::seconds(1))) {
            daemon.waitForExit();
            notVerified(goals, phase + ": the stop ended within 1 s, so it was never stuck for a second SIGTERM");
            return;
        }
        const std::optional<long> mainIn = blockedIn(daemon.pid(), daemon.pid());
#ifdef SYS_pause
        const bool inPause = mainIn && *mainIn == SYS_pause;
#else
        const bool inPause = false;  // no pause system call here to recognise the wedge by
#endif
        if (!inPause) {
            daemon.shutdown(SIGKILL);
            notVerified(goals, phase + ": the stop was not seen stuck in the wedged decoder free (main thread in " +
                                   (mainIn ? "system call " + std::to_string(*mainIn) : "a state /proc did not show") +
                                   ")");
            return;
        }
        const auto second = Clock::now();
        ::kill(daemon.pid(), SIGTERM);
        bound = kSecondSignal;
        ended = endedWithin(daemon.pid(), second, bound);
    }
    const bool held = daemon.captureStillStalled();
    // Waited for past the bound, so the status of an end the deadline forced is still graded.
    const int exitCode = daemon.waitForExit();
    if (!held) {
        harnessFailure(phase + ": something drained the stalled stream during the phase, so the daemon's output never "
                               "stalled and its stop says nothing about one");
        return;
    }
    const std::string within = lose ? ": the daemon ends within 2.5 s of the shim's failed submission"
                               : wedged ? ": a second SIGTERM ends a wedged stop within 1 s"
                                        : ": SIGTERM ends the daemon within 2 s, under the 3 s teardown deadline";
    check(ended.has_value(), (phase + within).c_str());
    if (!daemon.timedOut()) checkStalledStatus(daemon, home, exitCode, lose ? kExitDeviceLost : wedged ? 1 : 0,
                                  phase + (lose ? ": exits with status 75" : wedged ? ": exits 1" : ": exits 0"));
    std::printf("device_loss_restart_test: %s: %zu bytes filled, %s, exit code %d, signal %d\n", phase.c_str(),
                daemon.stallBytes(),
                ended ? ("ended " + std::to_string(seconds(*ended)) + " s after the last provocation").c_str()
                      : "still running at the bound, killed",
                exitCode, daemon.termSignal());
}

// GOAL: S14.25 -- a daemon started with stdout and stderr already full (a pipe, a stream socket, a
//       suspended terminal: a journald that stalled before the service started) serves: it takes
//       the bus name, answers GetStatus and a SetWallpaper within 2 s, draws, and a SIGTERM ends
//       it with status 0 within 2 s. False if its first line holds it before it serves. Output
//       written before main() (the Vulkan loader's VK_LOADER_DEBUG line) is out of scope: nothing
//       in the daemon runs early enough to take it.
// WHY-THIS-SHAPE: the destination is filled and the reader stopped before the fork, so every line
//       the daemon ever writes meets a full destination. Nothing here can wait for a line, so the
//       name and the bus calls are what show it came up; a harness that could not fill the stream
//       first reports that, never a verdict (by hand 2026-09-25, reverted: the pre-fill reported
//       as failed -> all three phases "FAIL (harness)", no FAIL).
// CONTROL: via control_run.sh on NVIDIA, each on all three destinations, reverted, observed
//       2026-09-26. The log writer never installed (backend/daemon/main.cpp's `logpump::install()`
//       skipped) -> GetStatus, SetWallpaper and SIGTERM lines red: the daemon took its name and
//       then stopped on its first line. logpump::release() in backend/daemon/log_pump.cpp given
//       an hour -> the SIGTERM line; with `g_finalStatus.store` also deleted, "exits 0" too.
void checkStartedStalled(NameWatcher& names, const std::string& blue, Destination dest) {
    constexpr const char* kGoals = "S14.25 (a daemon started with its output already full serves and stops)";
    const std::string phase = std::string("full ") + describe(dest) + " from the start";
    Home home("devloss_prefull");
    if (!home.ok()) {
        notVerified(kGoals, "a scratch HOME could not be created under the temp directory");
        return;
    }
    if (!nameFree(names, phase)) return;
    FrameReadyLog frames;
    auto proxy = subscribeFrameReady(frames);
    SpawnedDaemon daemon(stalledEnv(home, home.env(false)), captureMode(dest), {}, 0, kDaemonArgs, true);
    if (!daemon.stalledFromStart()) {
        harnessFailure(phase + ": the capture stream could not be filled before the daemon started");
        return;
    }
    const bool named = names.waitFor(true);
    if (!named) {
        if (const char* fault = daemon.startupFault()) {
            harnessFailure(phase + ": the child died before it became the daemon: " + fault);
            return;
        }
    }
    const bool status = named && getStatus(*proxy, kPromptCall).ok();
    const bool drew = status &&
                      wallpaperx::testing::trySetWallpaper(*proxy, "static", blue, kMonitorBlue, kPromptCall) &&
                      frames.wait(kMonitorBlue, 1);
    const auto sent = Clock::now();
    ::kill(daemon.pid(), SIGTERM);
    const std::optional<Clock::duration> ended = endedWithin(daemon.pid(), sent, kStalledStop);
    const bool held = daemon.captureStillStalled();
    const int exitCode = daemon.waitForExit();
    if (!held) {
        harnessFailure(phase + ": something drained the stream the daemon started on");
        return;
    }
    check(named, (phase + ": the daemon takes the bus name").c_str());
    check(status, (phase + ": GetStatus is answered within 2 s").c_str());
    check(drew, (phase + ": a SetWallpaper is answered within 2 s and the still is drawn").c_str());
    check(ended.has_value(), (phase + ": SIGTERM ends the daemon within 2 s").c_str());
    if (!daemon.timedOut()) checkStalledStatus(daemon, home, exitCode, 0, phase + ": exits 0");
    std::printf("device_loss_restart_test: %s: %zu bytes filled, %s, exit code %d, signal %d\n", phase.c_str(),
                daemon.stallBytes(), ended ? ("ended " + std::to_string(seconds(*ended)) + " s after SIGTERM").c_str()
                                           : "still running at the bound, killed",
                exitCode, daemon.termSignal());
}

// GOAL: S14.26 -- a daemon started with stdout, stderr or both closed (`>&-`) serves: it takes
//       the bus name, answers GetStatus, draws, and exits 0 on SIGTERM; the stream left open still
//       gets its lines (stdout its "SetWallpaper(" line, stderr its "rejected" line). False if a
//       closed stream stops it starting, or a later open() takes descriptor 1 or 2 and the log goes
//       into it.
// WHY-THIS-SHAPE: the open stream's line is waited for rather than searched after the exit, so a
//       daemon whose lines went elsewhere fails on that line and not on an exit-status check.
// CONTROL: in install() in backend/daemon/log_pump.cpp via control_run.sh 2026-09-25 on NVIDIA,
//       reverted. (a) A closed descriptor refused (`return false` in place of the /dev/null
//       stand-in) -> all fifteen lines of the three phases red. (b) No /dev/null stand-in at all
//       -> the stderr-closed phase's "stdout still gets its lines" and "says it stopped cleanly"
//       red; the stdout-closed and both-closed phases stayed green, so for them only (a) is shown.
void checkClosedAtLaunch(NameWatcher& names, const std::string& blue, SpawnedDaemon::Stdout mode) {
    constexpr const char* kGoals = "S14.26 (a daemon started with stdout or stderr closed)";
    const bool outOpen = mode == SpawnedDaemon::Stdout::StderrClosed;
    const bool errOpen = mode == SpawnedDaemon::Stdout::StdoutClosed;
    const std::string phase = outOpen ? "stderr closed at launch" : errOpen ? "stdout closed at launch"
                                                                            : "stdout and stderr closed at launch";
    Home home("devloss_closed");
    if (!home.ok()) {
        notVerified(kGoals, "a scratch HOME could not be created under the temp directory");
        return;
    }
    if (!nameFree(names, phase)) return;
    FrameReadyLog frames;
    auto proxy = subscribeFrameReady(frames);
    SpawnedDaemon daemon(home.env(false), mode, {}, 0, kDaemonArgs);
    const bool named = names.waitFor(true);
    if (!named) {
        if (const char* fault = daemon.startupFault()) {
            harnessFailure(phase + ": the child died before it became the daemon: " + fault);
            return;
        }
    }
    check(named, (phase + ": the daemon takes the bus name").c_str());
    check(named && getStatus(*proxy, kPromptCall).ok(), (phase + ": GetStatus is answered").c_str());
    check(named && setWallpaper(*proxy, "static", blue, kMonitorBlue) && frames.wait(kMonitorBlue, 1),
          (phase + ": the daemon draws").c_str());
    if (outOpen) {
        check(daemon.waitForOutput("SetWallpaper(type=", kWait), (phase + ": stdout still gets its lines").c_str());
    } else if (errOpen) {
        (void)sendMarker(*proxy, 0, 8);
        check(daemon.waitForOutput("SetWallpaper rejected", kWait), (phase + ": stderr still gets its lines").c_str());
    }
    const int exitCode = daemon.shutdown(SIGTERM);
    checkStatus(daemon, exitCode, 0, phase + ": exits 0 on SIGTERM");
    if (outOpen) check(daemon.output().find(kCleanStop) != std::string::npos, (phase + ": says it stopped cleanly").c_str());
}

// GOAL: S14.15 -- a SIGALRM from outside, sent to a daemon that is serving and not stopping, does
//       not end it: it keeps drawing and answering, and a later SIGTERM still exits 0 with its
//       clean-stop line. The daemon's own 3 s teardown alarm is SIGALRM, so a handler installed
//       at startup takes any outside one for an expired deadline.
// WHY-THIS-SHAPE: four signals, the last three back to back, then a render and a GetStatus: an
//       outside SIGALRM is delivered on the kill() to a thread that has it unblocked, so by the
//       time a render round trip is done a handler would have run. Alive is read from the bus
//       name and the capture pipe, not from kill(0), which a zombie also answers.
// CONTROL: in backend/daemon/main.cpp via control_run.sh, observed 2026-09-26 on NVIDIA,
//       reverted: the startup `std::signal(SIGALRM, SIG_IGN)` made `std::signal(SIGALRM,
//       onTeardownDeadline)` -> "still draws", "still running", "exits 0" and "stops cleanly" red,
//       alone.
void checkOutsideAlarmIgnored(NameWatcher& names, const std::string& blue, const std::string& red) {
    constexpr const char* kGoals = "S14.15 (an outside SIGALRM does not end a serving daemon)";
    const std::string phase = "outside SIGALRM";
    Home home("devloss_alrm");
    if (!home.ok()) {
        notVerified(kGoals, "a scratch HOME could not be created under the temp directory");
        return;
    }
    if (!nameFree(names, phase)) return;
    FrameReadyLog frames;
    auto proxy = subscribeFrameReady(frames);
    SpawnedDaemon daemon(home.env(false), SpawnedDaemon::Stdout::Captured, {}, 0, kDaemonArgs);
    if (!names.waitFor(true) || !setWallpaper(*proxy, "static", blue, kMonitorBlue) || !frames.wait(kMonitorBlue, 1)) {
        setupFailed(daemon, kGoals, "the daemon did not draw before any SIGALRM was sent");
        return;
    }
    ::kill(daemon.pid(), SIGALRM);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    for (int sent = 0; sent < 3; ++sent) ::kill(daemon.pid(), SIGALRM);
    check(setWallpaper(*proxy, "static", red, kMonitorRed) && frames.wait(kMonitorRed, 1),
          (phase + ": the daemon still draws after four outside SIGALRMs").c_str());
    check(getStatus(*proxy, std::chrono::seconds(5)).ok() && !daemon.exited() && names.ownedNow(),
          (phase + ": the daemon is still running, answering and holding its name").c_str());
    const int exitCode = daemon.shutdown(SIGTERM);
    checkStatus(daemon, exitCode, 0, phase + ": a later SIGTERM still exits 0");
    check(daemon.output().find(kCleanStop) != std::string::npos, (phase + ": a later SIGTERM still stops cleanly").c_str());
    if (daemon.termSignal() != 0) std::fprintf(stderr, "  %s: died of signal %d\n", phase.c_str(), daemon.termSignal());
}

// tests/wedge_shim.cpp preloaded after whatever the environment preloads, with `variable` set.
std::vector<std::string> withWedgeShim(std::vector<std::string> env, const char* variable, const char* value = "1") {
    const char* inherited = std::getenv("LD_PRELOAD");
    const std::string before = inherited != nullptr && *inherited != '\0' ? std::string(inherited) + ":" : "";
    env.push_back("LD_PRELOAD=" + before + WALLPAPERX_WEDGE_SHIM);
    env.push_back(std::string(variable) + "=" + value);
    return env;
}

// GOAL: S14.32 -- a device loss seen inside a render that then never finishes -- its job still
//       inside the applying record when the stop's deadline ends the process with 75 -- leaves no
//       applying record, and the next start on the same HOME draws that wallpaper. False if the
//       record is withdrawn only when the job returns: the restart then refuses a wallpaper the GPU
//       lost, as one that killed the daemon.
// WHY-THIS-SHAPE: the shim makes the job's own record close never return once a submission has
//       failed, which is the one way to keep a render inside its record past the deadline from
//       outside. A deadline's own line ("shutdown blocked" or "teardown deadline passed") is
//       required first: without it the job returned, the record went the ordinary way, and the
//       phase is NOT VERIFIED rather than green.
// CONTROL: in backend/daemon/main.cpp, `wallpaperx::stopBlamingWallpapers();` removed from
//       config.onDeviceLost -> "no applying record is left" and "the restart draws the wallpaper
//       whose render saw the loss" red, alone. Observed 2026-09-25 via control_run.sh on NVIDIA;
//       reverted. By hand, reverted: the hang variable set to 0 -> NOT VERIFIED "the stop ended
//       without a deadline", no FAIL.
void checkLossInsideUnfinishedRender(NameWatcher& names, const std::string& blue) {
    constexpr const char* kGoals = "S14.32 (a loss inside a render that never finishes blames no wallpaper)";
    const std::string phase = "loss inside an unfinished render";
    Home home("devloss_unfinished");
    if (!home.ok()) {
        notVerified(kGoals, "a scratch HOME could not be created under the temp directory");
        return;
    }
    if (!nameFree(names, phase)) return;
    {
        FrameReadyLog frames;
        auto proxy = subscribeFrameReady(frames);
        std::vector<std::string> env = home.env(true);
        env.push_back("WALLPAPERX_TEST_HANG_RECORD_CLOSE=1");
        SpawnedDaemon daemon(env, SpawnedDaemon::Stdout::Captured, {}, 0, kDaemonArgs);
        if (!names.waitFor(true) || !setWallpaper(*proxy, "static", blue, kMonitorBlue) || !frames.wait(kMonitorBlue, 1)) {
            setupFailed(daemon, kGoals, phase + ": the daemon did not draw before the loss");
            return;
        }
        int exitCode = -1;
        if (!loseDeviceAndWait(daemon, home, *proxy,
                               [&] { (void)setWallpaper(*proxy, "static", blue, kMonitorTrigger); }, phase, exitCode)) {
            return;
        }
        const std::string out = daemon.output();
        if (out.find("shutdown blocked") == std::string::npos && out.find("teardown deadline passed") == std::string::npos) {
            notVerified(kGoals, phase + ": the stop ended without a deadline, so the render that saw the loss had left "
                                        "its record the ordinary way");
            return;
        }
        checkStatus(daemon, exitCode, kExitDeviceLost, phase + ": a stop ended by its deadline after a loss exits 75");
    }
    const std::vector<std::string> records = applyingRecords(home.stateDir());
    check(records.empty(), (phase + ": no applying record is left for the render that never finished").c_str());
    for (const std::string& name : records) std::fprintf(stderr, "  %s: left behind: %s\n", phase.c_str(), name.c_str());

    fs::remove(home.trigger());
    if (!nameFree(names, phase + "'s restart")) return;
    FrameReadyLog restored;
    auto proxy = subscribeFrameReady(restored);
    SpawnedDaemon restart(home.env(false), SpawnedDaemon::Stdout::Captured, {}, 0, kDaemonArgs);
    if (!names.waitFor(true)) {
        check(false, (phase + ": a daemon started on the same HOME acquires the bus name").c_str());
        return;
    }
    const bool drawn = restored.wait(kMonitorTrigger, 1);
    const nlohmann::json* entry = nullptr;
    const StatusReply status = getStatus(*proxy, std::chrono::seconds(10));
    entry = wallpaperStatus(status, std::to_string(kMonitorTrigger));
    const bool clean = entry != nullptr && !entry->contains("last_error");
    check(drawn && clean, (phase + ": the restart draws the wallpaper whose render saw the loss, refusing nothing").c_str());
    if (!clean) std::fprintf(stderr, "  %s\n", entry != nullptr ? entry->dump().c_str() : "(absent)");
    const int stopExit = restart.shutdown(SIGTERM);
    checkStatus(restart, stopExit, 0, phase + ": the restarted daemon exits 0 on SIGTERM");
}

// GOAL: S14.36 -- output written after main() returns (a library destructor, the Vulkan loader, a
//       sanitizer's end-of-run report) reaches the destination even when it resumes only after the
//       final 250 ms flush has given up. False if descriptors 1 and 2 still lead into the log
//       writer's pipe then: the process exits with the line still in it.
// WHY-THIS-SHAPE: the writer is tests/wedge_shim.cpp's destructor, whose load-time line is required
//       first, so a shim that never loaded is a harness fault rather than a missing line. The reader
//       resumes 2 s after the SIGTERM: past the flush, which a stop spends within a few hundred ms,
//       and before the 3 s teardown alarm, which would end a process still writing. A resume later
//       than 2.7 s is NOT VERIFIED, not a verdict.
// CONTROL: in backend/daemon/main.cpp, main()'s `logpump::release()` replaced by flushLogs(), which
//       flushes but leaves the pipes on 1 and 2 -> this phase's "reaches the destination" red,
//       alone. Observed 2026-09-25 via control_run.sh on NVIDIA; reverted. By hand, reverted: the
//       shim's variable misspelt -> "FAIL (harness): ... did not say it loaded", no FAIL.
void checkLineAfterMainReturns(NameWatcher& names) {
    constexpr const char* kGoals = "S14.36 (a line written after main() returns is not lost to a stalled output)";
    const std::string phase = "stalled pipe, a line after main returns";
    Home home("devloss_atexit");
    if (!home.ok()) {
        notVerified(kGoals, "a scratch HOME could not be created under the temp directory");
        return;
    }
    if (!nameFree(names, phase)) return;
    SpawnedDaemon daemon(stalledEnv(home, withWedgeShim(home.env(false), "WALLPAPERX_TEST_LINE_AT_EXIT")),
                         SpawnedDaemon::Stdout::Captured, {}, 0, kDaemonArgs);
    if (!cameUp(names, daemon, kGoals, phase)) return;
    if (daemon.output().find("wedge_shim: loaded") == std::string::npos) {
        daemon.shutdown(SIGTERM);
        harnessFailure(phase + ": " WALLPAPERX_WEDGE_SHIM " did not say it loaded with its exit line armed, so "
                               "nothing writes after main() returns");
        return;
    }
    std::string why;
    if (!daemon.stallCapture(why)) {
        daemon.shutdown(SIGTERM);
        harnessFailure(phase + ": " + why);
        return;
    }
    const auto sent = Clock::now();
    ::kill(daemon.pid(), SIGTERM);
    std::this_thread::sleep_until(sent + std::chrono::seconds(2));
    const bool held = daemon.captureStillStalled();
    const auto resumedAt = Clock::now();
    daemon.resumeCapture();
    const int exitCode = daemon.waitForExit();
    if (!held) {
        harnessFailure(phase + ": something drained the stalled stream before the resume");
        return;
    }
    if (resumedAt - sent > std::chrono::milliseconds(2700)) {
        notVerified(kGoals, phase + ": this test resumed the stream " + std::to_string(seconds(resumedAt - sent)) +
                                " s after SIGTERM, when the teardown alarm may already have ended the process");
        return;
    }
    const std::vector<std::string> after = linesFrom(daemon.output(), daemon.stalledAt());
    const bool arrived =
        std::find(after.begin(), after.end(), "wedge_shim: written after main returned") != after.end();
    check(arrived, (phase + ": the line written after main() returned reaches the destination once it resumes").c_str());
    if (!arrived) std::fprintf(stderr, "  %zu line(s) arrived after the resume\n", after.size());
    checkStalledStatus(daemon, home, exitCode, 0, phase + ": exits 0");
}

// Threads of `pid` blocked in system call `number`.
std::vector<pid_t> threadsIn(pid_t pid, long number) {
    std::vector<pid_t> found;
    std::error_code ec;
    for (auto it = fs::directory_iterator("/proc/" + std::to_string(pid) + "/task", ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const pid_t tid = static_cast<pid_t>(std::strtol(it->path().filename().c_str(), nullptr, 10));
        if (const std::optional<long> in = blockedIn(pid, tid); in && *in == number) found.push_back(tid);
    }
    return found;
}

// GOAL: S14.31 -- when the signal thread fails as it starts a stop (its exception exit, "shutdown
//       watch failed"), the lines queued before it still reach a destination that resumes inside
//       the 250 ms flush, and its own line follows them. False if that exit skips the flush: the
//       process ends with the lines still queued.
// WHY-THIS-SHAPE: nothing a client can ask makes the signal thread throw, so tests/wedge_shim.cpp
//       fails the first mutex lock after its sigwait(), which std::mutex throws. The reader resumes
//       when /proc shows that thread in poll(), i.e. inside the flush, so the resume provably follows
//       the start of the exit; resuming on a timer instead could let the writer deliver everything
//       before the exit began, and pass without any flush. A resume more than 200 ms after SIGTERM,
//       or /proc looked at less often than every 50 ms, is NOT VERIFIED.
// CONTROL: in backend/daemon/main.cpp, the signal thread's `catch (...)` made to write its line
//       with writeErrNow and `_Exit` without exitNow()'s flush -> "the lines queued before the
//       failure, then its own line, reach the destination" red, alone. Observed 2026-09-25 via
//       control_run.sh on NVIDIA; reverted. By hand, reverted: the shim's variable misspelt -> NOT
//       VERIFIED "the stop was an ordinary one", no FAIL.
void checkShutdownWatchFailureFlushes(NameWatcher& names) {
    constexpr const char* kGoals = "S14.31 (the signal thread's failure exit delivers what is queued)";
    const std::string phase = "stalled pipe, the signal thread fails";
    Home home("devloss_watchfail");
    if (!home.ok()) {
        notVerified(kGoals, "a scratch HOME could not be created under the temp directory");
        return;
    }
    if (!nameFree(names, phase)) return;
    FrameReadyLog frames;
    auto proxy = subscribeFrameReady(frames);
    SpawnedDaemon daemon(stalledEnv(home, withWedgeShim(home.env(false), "WALLPAPERX_TEST_FAIL_SHUTDOWN_WATCH")),
                         SpawnedDaemon::Stdout::Captured, {}, 0, kDaemonArgs);
    if (!cameUp(names, daemon, kGoals, phase)) return;
    const std::vector<pid_t> watchers = threadsIn(daemon.pid(), SYS_rt_sigtimedwait);
    if (watchers.size() != 1) {
        daemon.shutdown(SIGTERM);
        notVerified(kGoals, phase + ": /proc showed " + std::to_string(watchers.size()) +
                                " threads waiting for signals, not the one signal thread");
        return;
    }
    std::string why;
    if (!daemon.stallCapture(why)) {
        daemon.shutdown(SIGTERM);
        harnessFailure(phase + ": " + why);
        return;
    }
    const bool answered = sendMarker(*proxy, 0, 64);
    const auto sent = Clock::now();
    ::kill(daemon.pid(), SIGTERM);
    bool flushing = false;
    Clock::duration widestGap{};
    for (auto last = Clock::now(); !flushing && !hasEnded(daemon.pid()) && Clock::now() - sent < kWait;) {
        const std::optional<long> in = blockedIn(daemon.pid(), watchers.front());
        flushing = in && (*in == SYS_poll || *in == SYS_ppoll);
        const auto now = Clock::now();
        widestGap = std::max(widestGap, now - last);
        last = now;
        if (!flushing) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool held = daemon.captureStillStalled();
    const auto resumedAt = Clock::now();
    daemon.resumeCapture();
    const int exitCode = daemon.waitForExit();
    if (!held) {
        harnessFailure(phase + ": something drained the stalled stream before the resume");
        return;
    }
    if (!answered) {
        notVerified(kGoals, phase + ": the call that queues a line was not answered, so nothing was queued");
        return;
    }
    if (daemon.exitedNormally() && exitCode == 0) {
        notVerified(kGoals, phase + ": the stop was an ordinary one, so the staged failure in the signal thread never "
                                    "happened");
        return;
    }
    if (flushing ? resumedAt - sent > std::chrono::milliseconds(200) : widestGap > std::chrono::milliseconds(50)) {
        notVerified(kGoals, phase + ": this test did not look at the daemon closely enough to resume inside its flush");
        return;
    }
    const std::vector<std::string> after = linesFrom(daemon.output(), daemon.stalledAt());
    const auto failedAt = std::find_if(after.begin(), after.end(), [](const std::string& line) {
        return line.find("shutdown watch failed, exiting now") != std::string::npos;
    });
    const std::vector<std::string> queued = markersAndNotes(std::vector<std::string>(after.begin(), failedAt), 64);
    const bool delivered = failedAt != after.end() && queued == expectedMarkers(0, 1);
    check(delivered, (phase + ": the lines queued before the failure, then its own line, reach the destination").c_str());
    if (!delivered) std::fprintf(stderr, "  seen: %s\n", joined(after, 20).c_str());
    checkStalledStatus(daemon, home, exitCode, 1, phase + ": exits 1");
}

// GOAL: S14.43 -- a start that failed (the bus name already held) and whose static destruction then
//       runs past the 3 s teardown deadline exits 1 when the deadline cuts it short, not by the
//       deadline's SIGALRM. False if the failed start never records its status where the alarm's
//       handler reads it: the process then dies of SIGALRM, which a service manager reads as a crash.
// WHY-THIS-SHAPE: the failed start is the second of two daemons, as starting it twice makes one; the
//       slow destruction is the wedge shim's destructor sleeping 8 s after main() returns, past the
//       alarm the unwind armed. The shim's line, written as that destructor starts, is required:
//       without it nothing ran past main() and the phase is NOT VERIFIED, not green.
// CONTROL: in backend/daemon/main.cpp via control_run.sh 2026-09-27 on NVIDIA, reverted: serve()'s
//       catch block taking `failureStatus()` without settle() -> "exits 1, not by the teardown
//       alarm's SIGALRM" red, alone.
// COVERAGE-ASSERT: ended within 5 s of the shim's line, where its sleep is 8 s, so the exit graded
//       is the deadline's and not the destructor finishing. Via control_run.sh, reverted:
//       armTeardown()'s `::alarm` removed -> "cuts the 8 s destruction short" red, alone, the exit-1
//       line green. By hand, reverted: the sleep set to 0 -> NOT VERIFIED "never said it was
//       sleeping", no FAIL.
void checkFailedStartPastDeadline(NameWatcher& names) {
    constexpr const char* kGoals = "S14.43 (a failed start cut short by the teardown deadline exits 1)";
    const std::string phase = "failed start, slow static destruction";
    constexpr const char* kSleeping = "wedge_shim: sleeping in a library destructor";
    Home holderHome("devloss_holder");
    Home failedHome("devloss_failed");
    if (!holderHome.ok() || !failedHome.ok()) {
        notVerified(kGoals, "a scratch HOME could not be created under the temp directory");
        return;
    }
    if (!nameFree(names, phase)) return;
    SpawnedDaemon holder(holderHome.env(false), SpawnedDaemon::Stdout::Captured, {}, 0, kDaemonArgs);
    if (!cameUp(names, holder, kGoals, phase)) return;
    SpawnedDaemon failed(withWedgeShim(failedHome.env(false), "WALLPAPERX_TEST_SLOW_EXIT_MS", "8000"),
                         SpawnedDaemon::Stdout::Captured, {}, 0, kDaemonArgs);
    const bool sleeping = failed.waitForOutput(kSleeping, kWait);
    const auto sleepingAt = Clock::now();
    const std::optional<Clock::duration> ended =
        sleeping ? endedWithin(failed.pid(), sleepingAt, std::chrono::seconds(15)) : std::nullopt;
    const int exitCode = failed.waitForExit();
    holder.shutdown(SIGTERM);
    if (const char* fault = failed.startupFault(); fault != nullptr) {
        harnessFailure(phase + ": " + fault);
        return;
    }
    if (!sleeping) {
        notVerified(kGoals, phase + ": the shim's destructor never said it was sleeping (exit code " +
                                std::to_string(exitCode) + ", signal " + std::to_string(failed.termSignal()) +
                                "), so nothing ran past main() for the deadline to cut short");
        return;
    }
    check(ended && *ended < std::chrono::seconds(5),
          (phase + ": the teardown deadline cuts the 8 s destruction short").c_str());
    checkStatus(failed, exitCode, 1, phase + ": exits 1, not by the teardown alarm's SIGALRM");
    if (failed.termSignal() != 0) std::fprintf(stderr, "  %s: died of signal %d\n", phase.c_str(), failed.termSignal());
    std::printf("device_loss_restart_test: %s: ended %s after the destructor began, exit code %d, signal %d\n",
                phase.c_str(), ended ? (std::to_string(seconds(*ended)) + " s").c_str() : "not within 15 s", exitCode,
                failed.termSignal());
}

// ---------------------------------------------------------------------------------------------
// A crash's last lines. The fatal-signal handler (backend/daemon/crash_record.cpp) writes its
// records, then gives the log writer up to kExitFlush (250 ms) to deliver what is queued -- a flush
// only: descriptors 1 and 2 stay on the writer's pipes -- then hands the signal to the previous
// handler and re-raises. The crash is real: a restored video, and tests/wedge_shim.cpp faulting on
// the decoder's thread after writing one line to stderr and creating WALLPAPERX_TEST_CRASH_MARK.
// ---------------------------------------------------------------------------------------------

constexpr const char* kFaultLine = "wedge_shim: faulting on the decoder's thread";
// What it tells apart: a flush bounded at 250 ms from one that waits on the destination (never
// ends) or is given seconds. The second, above the 250 ms, is for the re-raise, tearing down a
// process full of driver threads, and this test's own polling on a loaded shared runner.
constexpr auto kCrashEnd = std::chrono::milliseconds(1250);
// A reader resumed later than this after the fault may have missed the flush through no fault of
// the daemon's.
constexpr auto kResumeInsideFlush = std::chrono::milliseconds(150);

// A daemon that restores a video on kMonitorVideo and faults on its decoder's thread at the second
// picture, with stdout and stderr on `dest`, full before it starts.
struct CrashingDaemon {
    Home home{"devloss_crash"};
    fs::path mark;
    std::optional<SpawnedDaemon> daemon;
    std::string clip = std::string(WALLPAPERX_VIDEO_ASSET_DIR) + "/wallpaper_bands.mp4";

    // False (reported) when it could not be started stalled.
    bool start(NameWatcher& names, Destination dest, const char* goals, const std::string& phase) {
        std::error_code ec;
        if (!fs::exists(clip, ec)) {
            notVerified(goals, phase + ": the video fixture " + clip + " was not generated (no ffmpeg at build time)");
            return false;
        }
        if (!home.ok()) {
            notVerified(goals, phase + ": a scratch HOME could not be created under the temp directory");
            return false;
        }
        fs::create_directories(home.configFile().parent_path(), ec);
        {
            nlohmann::json monitors = nlohmann::json::array();
            monitors.push_back({{"monitor_id", kMonitorVideo}, {"type", "video"}, {"path", clip}});
            std::ofstream out(home.configFile(), std::ios::trunc);
            out << nlohmann::json{{"version", 1}, {"monitors", std::move(monitors)}}.dump();
            if (!out) {
                harnessFailure(phase + ": the saved video could not be written");
                return false;
            }
        }
        if (!nameFree(names, phase)) return false;
        mark = home.root() / "crashed";
        std::vector<std::string> env = withWedgeShim(home.env(false), "WALLPAPERX_TEST_CRASH_IN_PLAYBACK");
        env.push_back("WALLPAPERX_TEST_CRASH_MARK=" + mark.string());
        daemon.emplace(stalledEnv(home, env), captureMode(dest), std::string{}, 0, kDaemonArgs, true);
        if (!daemon->stalledFromStart()) {
            harnessFailure(phase + ": the capture stream could not be filled before the daemon started");
            return false;
        }
        return true;
    }

    // The moment the shim's mark appeared, i.e. its line was written and the fault is next. Nothing
    // (reported, the daemon reaped) when the fault never came.
    std::optional<Clock::time_point> awaitFault(const char* goals, const std::string& phase) {
        std::error_code ec;
        for (const auto deadline = Clock::now() + kWait; Clock::now() < deadline;) {
            if (fs::exists(mark, ec)) return Clock::now();
            if (hasEnded(daemon->pid())) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (fs::exists(mark, ec)) return Clock::now();
        const bool ended = hasEnded(daemon->pid());
        const int exitCode = ended ? daemon->waitForExit() : daemon->shutdown(SIGKILL);
        daemon->resumeCapture();
        notVerified(goals, phase + (ended ? ": the daemon ended before the staged fault (exit code " +
                                                std::to_string(exitCode) + ", signal " +
                                                std::to_string(daemon->termSignal()) + ")"
                                          : ": the staged fault never fired: this host never decoded the restored "
                                            "video"));
        return std::nullopt;
    }

    // The playing record's signal line, which the handler writes before it flushes.
    bool handlerRecorded() const {
        const std::optional<wallpaperx::PlayingHistory> history = wallpaperx::readPlayingRecord(home.stateDir());
        return history && history->signal.has_value();
    }

    // Whether a sanitizer's handler took the fault: it runs first and reports, and ASan's never
    // returns, so the daemon's flush is not what the phase would be grading.
    bool sanitizerTookIt(const char* goals, const std::string& phase) const {
        if (!sanitizerWroteIn(home)) return false;
        notVerified(goals, phase + ": a sanitizer reported the fault, so its handler, not the daemon's flush, decided "
                                   "how the process ended");
        return true;
    }

    // GOAL: X.27 -- the staged SIGSEGV is not handed to the host's crash collector: the reaped
    //       daemon's wait status carries no core bit. Graded on every crash this file stages.
    // WHY-THIS-SHAPE: as daemon_system_test's: on a piped core_pattern only the dumpable flag the
    //       shim clears keeps the crash from the collector, the inherited core limit does not; on a
    //       file pattern there is no collector program, so it counts as not reached, said on stdout.
    //       Called after the sanitizer gate and the "ended" check, before the phase's own timing
    //       can give up.
    // CONTROL: via control_batch 2026-10-04, reverted: the suppressCoreDump() call deleted from
    //       backend/tests/wedge_shim.cpp's fault -> "FAIL: crash, pipe stalled throughout: X.27: ..."
    //       and "FAIL: crash, pipe resumed during the flush: X.27: ...", one coredumpctl entry each.
    void checkNotCollected(const std::string& phase) const {
        std::string why;
        const std::optional<bool> collected = wallpaperx::testing::stagedCrashReachedCollector(*daemon, why);
        if (!collected) {
            notVerified("X.27 (a deliberate daemon crash does not reach the core collector)", phase + ": " + why);
            return;
        }
        check(!*collected, (phase + ": X.27: the staged SIGSEGV is not handed to the core collector (no core bit in "
                                    "the wait status)")
                               .c_str());
    }
};

// GOAL: S14.28 -- a line the daemon wrote just before a fatal signal, still queued in the log writer
//       because the destination was not accepting, reaches the destination if it starts accepting
//       during the crash handler's flush -- on a pipe, a stream socket and a terminal. False if the
//       handler dies without waiting, or the flush waits without delivering.
// WHY-THIS-SHAPE: the destination is full from before the daemon starts until the handler has
//       written the playing record's signal line, which it does before its flush -- so the shim's
//       last line provably had not been delivered when the thread faulted. Reading the output of a
//       crash on a flowing destination instead would pass without any flush: the writer delivers
//       a line within microseconds, well before the fault that follows it. Resumed from the record,
//       not the mark: resumed at the mark, the line could still get out before the fault.
// CONTROL: via control_run.sh 2026-09-27 on NVIDIA and Lavapipe, reverted, each red on all three
//       destinations and on nothing else. (a) The `logpump::flush(logpump::kExitFlush)` call
//       deleted from onFatalSignal in backend/daemon/crash_record.cpp -> "the line written just
//       before the fault ... reaches the destination". (b) logpump::flush in
//       backend/daemon/log_pump.cpp made to return without waiting -> the same lines. Observed
//       2026-09-25 by hand, reverted: with
//       the stream resumed before the fault and the stall gate disabled, (a) went unnoticed -- no
//       FAIL.
// COVERAGE-ASSERT: the stream is asked for one more byte just before it is resumed; if it took it,
//       it had drained and the phase is a harness fault. A resume later than 150 ms after the fault
//       is NOT VERIFIED rather than a verdict, and so is a sanitizer's report. By hand, reverted:
//       the stream resumed right after the start -> "FAIL (harness): ... something drained", no
//       FAIL; a 200 ms sleep before the resume -> NOT VERIFIED "resumed the stream 200 ms after".
void checkCrashDeliversQueuedLine(NameWatcher& names, Destination dest) {
    constexpr const char* kGoals = "S14.28 (a crash's queued last line reaches a destination that resumes)";
    const std::string phase = std::string("crash, ") + describe(dest) + " resumed during the flush";
    CrashingDaemon crash;
    if (!crash.start(names, dest, kGoals, phase)) return;
    SpawnedDaemon& daemon = *crash.daemon;
    const std::optional<Clock::time_point> faulted = crash.awaitFault(kGoals, phase);
    if (!faulted) return;
    bool recorded = false;
    while (!(recorded = crash.handlerRecorded()) && Clock::now() - *faulted < kCrashEnd &&
           !hasEnded(daemon.pid())) {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    const bool held = daemon.captureStillStalled();
    const auto resumedAt = Clock::now();
    daemon.resumeCapture();
    const std::optional<Clock::duration> ended = endedWithin(daemon.pid(), *faulted, std::chrono::seconds(5));
    if (!ended) daemon.shutdown(SIGKILL);
    daemon.waitForExit();
    if (!held) {
        harnessFailure(phase + ": something drained the stream before the crash, so the line was never queued");
        return;
    }
    if (crash.sanitizerTookIt(kGoals, phase)) return;
    // GOAL: S14.28 -- once the destination accepts again the handler's flush has nothing to wait
    //       on, so the crashing daemon ends within 5 s of the fault; one still alive was SIGKILLed
    //       above, and its crash path hung.
    // WHY-THIS-SHAPE: checked before X.27 and before the "record not seen" branch, so a hang is a
    //       FAIL, which outranks the NOT VERIFIED those report for a SIGKILLed daemon, instead of
    //       turning the entry into a skip. The flush is bounded at 250 ms, so 5 s is about 20x.
    // CONTROL: backend/daemon/crash_record.cpp, a 6 s sleep after onFatalSignal's flush
    //       (control_batch, 2026-10-04) -> "FAIL: crash, pipe resumed during the flush: the daemon
    //       ends within 5 s of the fault once the stream is resumed" on each destination.
    check(ended.has_value(), (phase + ": the daemon ends within 5 s of the fault once the stream is resumed").c_str());
    if (!ended) std::fprintf(stderr, "  %s: killed at the bound; it then reported signal %d\n", phase.c_str(), daemon.termSignal());
    crash.checkNotCollected(phase);
    if (!recorded) {
        notVerified(kGoals, phase + ": the handler's record was not seen within 1250 ms of the fault (the daemon ended "
                                "with signal " + std::to_string(daemon.termSignal()) +
                                "), so the stream could not be resumed at a moment known to be inside the handler");
        return;
    }
    const auto late = std::chrono::duration_cast<std::chrono::milliseconds>(resumedAt - *faulted);
    if (late > kResumeInsideFlush) {
        notVerified(kGoals, phase + ": this test resumed the stream " + std::to_string(late.count()) +
                                " ms after the fault, too late to be sure it was inside the 250 ms flush");
        return;
    }
    const std::vector<std::string> lines = linesFrom(daemon.output(), 0);
    const bool delivered = std::find(lines.begin(), lines.end(), kFaultLine) != lines.end();
    check(delivered, (phase + ": the line written just before the fault, queued at the crash, reaches the "
                              "destination")
                         .c_str());
    if (!delivered) {
        std::fprintf(stderr, "  %zu line(s) arrived; the last: %s\n", lines.size(),
                     lines.empty() ? "(none)" : lines.back().c_str());
    }
    std::printf("device_loss_restart_test: %s: %zu bytes filled, resumed %lld ms after the fault, %zu line(s) "
                "delivered\n",
                phase.c_str(), daemon.stallBytes(), static_cast<long long>(late.count()), lines.size());
}

// GOAL: S14.29 -- with the destination never accepting, the crash handler's flush does not hold the
//       crash: the daemon dies of the fault's own signal (SIGSEGV) within 1250 ms of it, and the
//       playing record names the video and the signal. False if the flush waits on the destination
//       or runs for seconds, or the handler stops before its re-raise.
// WHY-THIS-SHAPE: timed from the shim's mark, created just before the fault, not from the record's
//       signal line: a handler held before it writes the record would then leave nothing to time
//       from, and a phase that cannot start its clock could not fail. The end is read with
//       waitid(WNOWAIT) against the bound, and a daemon still alive at it is killed, so a flush that
//       never returns is a FAIL in bounded time, not a 30 s reap. kCrashEnd says why 1250 ms.
// CONTROL: via control_run.sh on NVIDIA and Lavapipe, reverted, each on all three destinations.
//       (a) logpump::flush in backend/daemon/log_pump.cpp made to ignore its limit -> "ends within
//       1250 ms" and "dies of SIGSEGV" (2026-09-27). (b) The handler's flush in onFatalSignal
//       (backend/daemon/crash_record.cpp) given 2 s -> "ends within 1250 ms" alone (2026-09-27).
//       (c) onFatalSignal's `::raise(sig)` made `::_exit(1)` -> "dies of SIGSEGV" alone
//       (2026-09-26). (d) (a) with the handler's flush moved to the top of onFatalSignal -> all
//       three, with "the playing record names the video and SIGSEGV" (2026-09-27).
// COVERAGE-ASSERT: the stream is asked for one more byte once the daemon has ended; one that took
//       it had drained, so the flush had somewhere to write, and the phase is a harness fault. By
//       hand, reverted: the stream resumed right after the start -> "FAIL (harness)", no FAIL; the
//       shim's fault not enabled -> every phase NOT VERIFIED "the staged fault never fired".
void checkCrashFlushIsBounded(NameWatcher& names, Destination dest) {
    constexpr const char* kGoals = "S14.29 (a crash with its output stalled still ends, of its signal, recorded)";
    const std::string phase = std::string("crash, ") + describe(dest) + " stalled throughout";
    CrashingDaemon crash;
    if (!crash.start(names, dest, kGoals, phase)) return;
    SpawnedDaemon& daemon = *crash.daemon;
    const std::optional<Clock::time_point> faulted = crash.awaitFault(kGoals, phase);
    if (!faulted) return;
    const std::optional<Clock::duration> ended = endedWithin(daemon.pid(), *faulted, kCrashEnd);
    // Given longer before it is killed, so a slow end is still graded on how it ended.
    const bool endedAtAll = ended || endedWithin(daemon.pid(), *faulted, std::chrono::seconds(5));
    const bool held = daemon.captureStillStalled();
    if (!endedAtAll) daemon.shutdown(SIGKILL);
    daemon.waitForExit();
    if (!held) {
        harnessFailure(phase + ": something drained the stalled stream during the crash");
        return;
    }
    if (crash.sanitizerTookIt(kGoals, phase)) return;
    crash.checkNotCollected(phase);
    check(ended.has_value(), (phase + ": the daemon ends within 1250 ms of the fault").c_str());
    const bool ofItsSignal = endedAtAll && !daemon.exitedNormally() && daemon.termSignal() == SIGSEGV;
    check(ofItsSignal, (phase + ": and dies of SIGSEGV, the fault's own signal").c_str());
    if (!ofItsSignal) std::fprintf(stderr, "  %s: signal %d\n", phase.c_str(), daemon.termSignal());
    const std::optional<wallpaperx::PlayingHistory> history = wallpaperx::readPlayingRecord(crash.home.stateDir());
    const bool named = history && history->died && history->died->monitorId == kMonitorVideo &&
                       history->died->path == crash.clip && history->signal == SIGSEGV;
    check(named, (phase + ": the playing record names the video and SIGSEGV").c_str());
    std::printf("device_loss_restart_test: %s: %zu bytes filled, %s\n", phase.c_str(), daemon.stallBytes(),
                ended ? ("ended " + std::to_string(seconds(*ended)) + " s after the fault").c_str()
                      : "still running at the bound, killed");
}

void runLossAndStallPhases(NameWatcher& names, const std::string& blue, const std::string& red) {
    using wallpaperx::testing::phaseSelected;
    if (phaseSelected("staticLossAndRestart")) checkStaticLossAndRestart(names, blue, red);
    if (phaseSelected("repeatedVideoLoss")) checkRepeatedVideoLoss(names);
    if (phaseSelected("noDeviceIsNotLoss")) checkNoDeviceIsNotLoss(names, blue, red);
    if (phaseSelected("stuckLossStop/noSignal")) checkStuckLossStop(names, blue, OutsideSignals::None);
    if (phaseSelected("stuckLossStop/oneSigtermWhenStuck")) checkStuckLossStop(names, blue, OutsideSignals::OneWhenStuck);
    if (phaseSelected("outsideWakeSignalIgnored")) checkOutsideWakeSignalIgnored(names, blue);
    if (phaseSelected("oneStreamStalled/stdoutThenSigterm")) {
        checkOneStreamStalled(names, blue, OneStalled::StdoutThenSigterm);
    }
    if (phaseSelected("oneStreamStalled/stderrThenSigusr1")) {
        checkOneStreamStalled(names, blue, OneStalled::StderrThenSigusr1);
    }
    if (phaseSelected("oneStreamStalled/stderrThenDeviceLoss")) {
        checkOneStreamStalled(names, blue, OneStalled::StderrThenDeviceLoss);
    }
    if (phaseSelected("outsideAlarmIgnored")) checkOutsideAlarmIgnored(names, blue, red);
    if (phaseSelected("lossInsideUnfinishedRender")) checkLossInsideUnfinishedRender(names, blue);
    if (phaseSelected("lineAfterMainReturns")) checkLineAfterMainReturns(names);
    if (phaseSelected("shutdownWatchFailureFlushes")) checkShutdownWatchFailureFlushes(names);
    if (phaseSelected("failedStartPastDeadline")) checkFailedStartPastDeadline(names);
    for (const Destination dest : {Destination::Pipe, Destination::Socket, Destination::Terminal}) {
        const std::string on = std::string("/") + describe(dest);
        // A destination this host cannot give (no pseudo-terminals) costs its own phases, not the rest.
        // By hand 2026-09-25, reverted: posix_openpt made to fail -> one "FAIL (harness): terminal
        // phases: ..." and every later phase still ran, no FAIL.
        try {
            if (phaseSelected("servesWhileStalled" + on)) checkServesWhileStalled(names, blue, red, dest);
            if (phaseSelected("longStall" + on)) checkLongStall(names, dest);
            if (phaseSelected("startedStalled" + on)) checkStartedStalled(names, blue, dest);
            for (const StalledEnd end : {StalledEnd::Sigterm, StalledEnd::SecondSigterm, StalledEnd::DeviceLoss}) {
                const char* how = end == StalledEnd::Sigterm         ? "/sigterm"
                                  : end == StalledEnd::SecondSigterm ? "/secondSigterm"
                                                                     : "/deviceLoss";
                if (phaseSelected("stalledEnd" + on + how)) checkStalledEnd(names, blue, dest, end);
            }
        } catch (const std::exception& e) {
            harnessFailure(std::string(describe(dest)) + " phases: " + e.what());
        }
    }
    if (phaseSelected("queueBounded")) checkQueueBounded(names);
    for (const SpawnedDaemon::Stdout mode : {SpawnedDaemon::Stdout::StdoutClosed, SpawnedDaemon::Stdout::StderrClosed,
                                             SpawnedDaemon::Stdout::BothClosed}) {
        const char* which = mode == SpawnedDaemon::Stdout::StdoutClosed   ? "closedAtLaunch/stdout"
                            : mode == SpawnedDaemon::Stdout::StderrClosed ? "closedAtLaunch/stderr"
                                                                          : "closedAtLaunch/both";
        if (phaseSelected(which)) checkClosedAtLaunch(names, blue, mode);
    }
}

}  // namespace

// With "crash-flush", only the crash phases (their own ctest entry); without, everything else.
int main(int argc, char** argv) {
    wallpaperx::testing::g_testName = "device_loss_restart_test";
    const bool crashFlush = argc > 1 && std::strcmp(argv[1], "crash-flush") == 0;

    // S14.9, graded in name_watcher.h: before any phase, since each phase's name wait is
    // satisfied by a foreign owner.
    if (const std::optional<int> refused = wallpaperx::testing::refuseUnlessPrivateBus()) return *refused;

    try {
        NameWatcher names;
        if (crashFlush) {
            for (const Destination dest : {Destination::Pipe, Destination::Socket, Destination::Terminal}) {
                const std::string on = std::string("/") + describe(dest);
                try {
                    if (wallpaperx::testing::phaseSelected("crashDeliversQueuedLine" + on)) {
                        checkCrashDeliversQueuedLine(names, dest);
                    }
                    if (wallpaperx::testing::phaseSelected("crashFlushIsBounded" + on)) {
                        checkCrashFlushIsBounded(names, dest);
                    }
                } catch (const std::exception& e) {
                    harnessFailure(std::string("crash on a ") + describe(dest) + ": " + e.what());
                }
            }
        } else {
            ScratchDir assets("devloss_assets");
            if (!assets.ok()) {
                notVerified("every goal in this file", "a scratch directory for the fixtures could not be created");
                return wallpaperx::testing::reportNotVerifiedOrPassed();
            }
            const std::string blue =
                wallpaperx::testing::writeSolidPng((assets.path() / "blue.png").string(), 8, 8, {0, 0, 255, 255});
            const std::string red =
                wallpaperx::testing::writeSolidPng((assets.path() / "red.png").string(), 8, 8, {255, 0, 0, 255});
            runLossAndStallPhases(names, blue, red);
        }
    } catch (const std::exception& e) {
        // The calls to the daemon report their own failures, so what escapes to here is setup this
        // harness could not do: SpawnedDaemon's pipe, terminal or fork, or a bus connection.
        harnessFailure(std::string("unexpected exception: ") + e.what());
    }

    if (wallpaperx::testing::reportCheckFailures()) return 1;
    if (wallpaperx::testing::reportHarnessFaults("in this test's harness, not in the daemon")) return 1;
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
