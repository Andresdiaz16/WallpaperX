// SPDX-License-Identifier: GPL-2.0-or-later

// wpxctl, the command-line tool, run as its own process against the real daemon on the private bus,
// and against a stand-in for Plasma's desktop shell that owns org.kde.plasmashell there and records
// the script it is sent. Each command is graded on its exit status and on what the daemon then
// reports, never on wpxctl's word alone.

#include "support/test_image.h"
#include "tests/client/daemon_client.h"
#include "tests/client/scratch_dir.h"
#include "tests/name_watcher.h"
#include "tests/private_test_bus.h"
#include "tests/spawned_daemon.h"
#include "tests/test_phase.h"
#include "tests/test_report.h"

#include <sdbus-c++/sdbus-c++.h>

#include <nlohmann/json.hpp>
#include <vulkan/vulkan.h>

#include <atomic>
#include <format>
#include <cerrno>
#include <cstdint>
#include <map>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {

namespace fs = std::filesystem;
using nlohmann::json;
using wallpaperx::testing::check;
using wallpaperx::testing::guardedPhase;
using wallpaperx::testing::harnessFailure;
using wallpaperx::testing::NameWatcher;
using wallpaperx::testing::notVerified;
using wallpaperx::testing::phaseSelected;
using wallpaperx::testing::ScratchDir;
using wallpaperx::testing::SpawnedDaemon;
using wallpaperx::testing::StatusReply;

constexpr auto kRunTimeout = std::chrono::seconds(60);

struct Run {
    // -1: it did not exit by itself in time, or could not be started.
    int status = -1;
    std::string out;
    std::string err;
};

std::string drain(int fd) {
    std::string text;
    ::lseek(fd, 0, SEEK_SET);
    char buf[4096];
    for (ssize_t n; (n = ::read(fd, buf, sizeof buf)) > 0;) text.append(buf, static_cast<size_t>(n));
    ::close(fd);
    return text;
}

// wpxctl with `args`, in this process's environment and so on its private bus. Reports nothing, so
// it could run on a thread of its own; `fault` says what went wrong in the harness, if anything.
Run spawnWpxctl(const std::vector<std::string>& args, std::string& fault) {
    Run run;
    const int outFd = ::memfd_create("wpxctl-out", MFD_CLOEXEC);
    const int errFd = ::memfd_create("wpxctl-err", MFD_CLOEXEC);
    if (outFd < 0 || errFd < 0) {
        fault = "memfd_create failed for wpxctl's output";
        if (outFd >= 0) ::close(outFd);
        if (errFd >= 0) ::close(errFd);
        return run;
    }
    std::vector<std::string> storage{WALLPAPERX_WPXCTL_BINARY};
    storage.insert(storage.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (std::string& arg : storage) argv.push_back(arg.data());
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, outFd, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, errFd, STDERR_FILENO);
    pid_t pid = -1;
    const int spawned = ::posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (spawned != 0) {
        fault = std::string("wpxctl could not be started: ") + std::strerror(spawned);
        ::close(outFd);
        ::close(errFd);
        return run;
    }
    const auto deadline = std::chrono::steady_clock::now() + kRunTimeout;
    int status = 0;
    pid_t reaped = 0;
    const auto reapNow = [&] {
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
    };
    for (;;) {
        reaped = ::waitpid(pid, &status, WNOHANG);
        if (reaped < 0 && errno == EINTR) continue;
        if (reaped != 0 || std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (reaped < 0) {
        const int error = errno;
        // ECHILD: nothing of ours is left under that pid, which may already name another process.
        if (error != ECHILD) {
            ::kill(pid, SIGKILL);
            reapNow();
        }
        fault = std::string("waitpid on wpxctl failed: ") + std::strerror(error);
        run.out = drain(outFd);
        run.err = drain(errFd);
        return run;
    }
    if (reaped == 0) {
        ::kill(pid, SIGKILL);
        reapNow();
        run.out = drain(outFd);
        run.err = drain(errFd);
        run.err += "\n(killed: it had not exited within 60 s)";
        return run;
    }
    if (reaped == pid && WIFEXITED(status)) run.status = WEXITSTATUS(status);
    run.out = drain(outFd);
    run.err = drain(errFd);
    return run;
}

Run wpxctl(const std::vector<std::string>& args) {
    std::string fault;
    Run run = spawnWpxctl(args, fault);
    if (!fault.empty()) harnessFailure(fault);
    return run;
}

std::string joined(const std::vector<std::string>& args) {
    std::string text = "wpxctl";
    for (const std::string& arg : args) text += " " + arg;
    return text;
}

// Runs wpxctl and grades its exit status, printing what it said when the status is not `want`.
Run expectExit(const std::vector<std::string>& args, int want) {
    Run run = wpxctl(args);
    const std::string what = joined(args) + " exits " + std::to_string(want) + " (got " + std::to_string(run.status) + ")";
    check(run.status == want, what.c_str());
    if (run.status != want) std::fprintf(stderr, "  stdout: %s\n  stderr: %s\n", run.out.c_str(), run.err.c_str());
    return run;
}

void expectText(const Run& run, bool inOut, const std::string& needle, const std::string& what) {
    const std::string& text = inOut ? run.out : run.err;
    const bool found = text.find(needle) != std::string::npos;
    check(found, what.c_str());
    if (!found) std::fprintf(stderr, "  looked for \"%s\" in: %s\n", needle.c_str(), text.c_str());
}

// ---- the daemon --------------------------------------------------------------------------------------

struct Session {
    NameWatcher& names;
    std::unique_ptr<SpawnedDaemon> daemon;
    std::unique_ptr<sdbus::IProxy> proxy;

    // The daemon, started on first use with two named screens: DP-3 as monitor 0, HDMI-A-1 as 1.
    // Null, with a not-verified line, when it does not start.
    sdbus::IProxy* client() {
        if (proxy) return proxy.get();
        if (daemon) return nullptr;
        daemon = std::make_unique<SpawnedDaemon>(std::vector<std::string>{"WALLPAPERX_VALIDATION=0"},
                                                 SpawnedDaemon::Stdout::Captured, std::string(), 0,
                                                 std::vector<std::string>{"--config-port", "0"});
        if (!names.waitFor(true)) {
            harnessFailure("the daemon did not take its bus name");
            return nullptr;
        }
        // Kept only once both screens are named: a daemon without them would fail every later phase
        // for this one fault, which is the harness's.
        try {
            auto named = sdbus::createProxy(sdbus::ServiceName{wallpaperx::kDaemonBusName},
                                            sdbus::ObjectPath{wallpaperx::kDaemonObjectPath});
            // GOAL: S17.26 -- a name answered SavePending (bound, its save late on a loaded disk) is sent
            //       again by the harness, not reported as a harness fault; any other error still is.
            // CONTROL: backend/daemon/wallpaper_manager.cpp setMonitorOutput(), 2026-11-10: a SavePending
            //       thrown after the bind, before requirePersisted() -> wpxctl.settings green, printing
            //       "naming DP-3: its save was not on disk in time, naming it again" (the retry, which the
            //       daemon then answers as unchanged); one thrown at the top of every call (control_batch)
            //       -> red, "FAIL (harness): the daemon's screens could not be named:
            //       [io.wallpaperx.Daemon.Error.SavePending] control: not on disk yet".
            wallpaperx::testing::setMonitorOutput(*named, 0, "DP-3");
            wallpaperx::testing::setMonitorOutput(*named, 1, "HDMI-A-1");
            proxy = std::move(named);
        } catch (const std::exception& e) {
            harnessFailure(std::string("the daemon's screens could not be named: ") + e.what());
            return nullptr;
        }
        return proxy.get();
    }

    // Whether this host can draw: false only when a still, set straight through the test's own client on
    // a monitor no phase grades, drew nothing and this process's own Vulkan instance lists no device. A
    // refused set, or no picture on a host with a device, is a failure, and the drawing checks still run.
    // Asked once.
    // GOAL: S17.20, S17.22 -- a refused probe set, or no picture on a host with a Vulkan device, fails
    //       the run instead of turning it into exit 77 -- including a daemon whose own GPU start failed.
    // WHY-THIS-SHAPE: the daemon's GetStatus reports only gpu.initialized, false both on a host with no
    //       device and after a GPU start that threw, so the daemon cannot be asked which it was; the
    //       host is asked instead, as cross_process_import_test asks it.
    // CONTROL: 2026-11-10: the probe's type made "nonsense" by hand (harness input; reverted) -> red,
    //       wpxctl.setFile Failed, "FAIL: the daemon takes a still set on monitor 9";
    //       backend/daemon/dbus_service.cpp emitFrameReady() returning at once for monitor 9
    //       (control_batch) -> red, "FAIL: a daemon with a GPU draws ..." (that message since renamed);
    //       backend/daemon/wallpaper_manager.cpp throwing before `new Device(...)` (control_batch,
    //       2026-11-10; NVIDIA) -> red, "FAIL: a host with a Vulkan device draws and announces a still
    //       set on monitor 9", where it had printed NOT VERIFIED before.
    bool draws(const std::string& png) {
        if (drawsHere) return *drawsHere;
        if (client() == nullptr) return false;
        wallpaperx::testing::FrameReadyLog log;
        const auto listener = wallpaperx::testing::subscribeFrameReady(log);
        if (!wallpaperx::testing::trySetWallpaper(*listener, "static", png, kProbeMonitor)) {
            check(false, std::format("the daemon takes a still set on monitor {}", kProbeMonitor).c_str());
            return *(drawsHere = true);
        }
        if (log.waitAtLeast(kProbeMonitor, 1)) return *(drawsHere = true);
        const uint32_t devices = hostVulkanDevices();
        check(devices == 0,
              std::format("a host with a Vulkan device draws and announces a still set on monitor {}", kProbeMonitor).c_str());
        if (devices > 0) std::fprintf(stderr, "  %u device(s); the daemon's gpu: %s\n", devices, status().value("gpu", json::object()).dump().c_str());
        return *(drawsHere = devices > 0);
    }

    // The devices this process's own Vulkan loader lists; 0 when it cannot make an instance at all.
    static uint32_t hostVulkanDevices() {
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        info.pApplicationInfo = &app;
        VkInstance instance = VK_NULL_HANDLE;
        if (vkCreateInstance(&info, nullptr, &instance) != VK_SUCCESS) return 0;
        uint32_t count = 0;
        if (vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS) count = 0;
        vkDestroyInstance(instance, nullptr);
        return count;
    }

    static constexpr int32_t kProbeMonitor = 9;
    std::optional<bool> drawsHere;

    json status() {
        sdbus::IProxy* p = client();
        if (p == nullptr) return json::object();
        const StatusReply reply = wallpaperx::testing::getStatus(*p);
        if (!reply.ok()) harnessFailure("GetStatus failed: " + reply.error());
        return reply.json();
    }

    void stop() {
        proxy.reset();
        if (!daemon) return;
        daemon->shutdown(SIGTERM);
        if (!names.waitFor(false)) harnessFailure("the bus name is still owned after the daemon was stopped");
        daemon.reset();
    }
};

json wallpaperOf(const json& status, const char* monitor) {
    const auto wallpapers = status.find("wallpapers");
    if (wallpapers == status.end() || !wallpapers->contains(monitor)) return json::object();
    return (*wallpapers)[monitor];
}

json propertiesOf(const json& status, const char* monitor) {
    return wallpaperOf(status, monitor).value("properties", json::object());
}

void writeFile(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

// ---- phases ------------------------------------------------------------------------------------------

void usage() {
    Run help = expectExit({"--help"}, 0);
    expectText(help, true, "Commands:", "wpxctl --help lists the commands");
    for (const char* command : {"status", "monitors", "set", "props", "pause", "sound", "fps", "activate"}) {
        Run run = expectExit({command, "--help"}, 0);
        expectText(run, true, std::string("Usage: wpxctl ") + command, std::string("wpxctl ") + command + " --help");
    }
    expectExit({}, 2);
    expectExit({"bogus"}, 2);
    expectExit({"status", "extra"}, 2);
    expectExit({"set", "0"}, 2);
    expectExit({"set", "0", "/nonexistent/file.png"}, 2);
    expectExit({"pause", "0", "sometimes"}, 2);
    expectExit({"sound", "loud"}, 2);
    expectExit({"fps", "45"}, 2);
    // GOAL: S17.10 -- `fps off` with no screen is a usage error (exit 2) before any call: there is no
    //       "off" for the rate every screen shares.
    // WHY-THIS-SHAPE: run with no daemon on the bus, so a wpxctl that sent SetFrameRate(0) instead
    //       exits 3 and the 2 can only come from the local refusal.
    // CONTROL: backend/wpxctl/main.cpp runFps(), `if (off && parsed.positional.size() == 1)` made
    //       `if (false)` (control_batch, 2026-11-07) -> red, "FAIL: wpxctl fps off exits 2 (got 3)".
    Run fpsOff = expectExit({"fps", "off"}, 2);
    expectText(fpsOff, false, "name the screen", "wpxctl fps off says a screen must be named");
    expectExit({"fps", "60", "--keep-higher", "--no-keep-higher"}, 2);
    expectExit({"set", "0", "/nonexistent", "--frobnicate"}, 2);
}

void notRunning() {
    // GOAL: S17.2 -- with no daemon on the bus every command that needs it exits 3 and says the
    //       daemon is not running, rather than 1 (a refusal) or a crash.
    // CONTROL: backend/wpxctl/main.cpp daemonFailed(), the unreachable() branch made kRefused
    //       (control_batch, 2026-11-07) -> red, "FAIL: wpxctl status exits 3 (got 1)".
    for (const char* command : {"status", "monitors"}) {
        Run run = expectExit({command}, 3);
        expectText(run, false, "not running", std::string("wpxctl ") + command + " says the daemon is not running");
    }
    expectExit({"sound", "on"}, 3);
}

void statusAndMonitors(Session& session) {
    if (session.client() == nullptr) return notVerified("wpxctl status and monitors", "the daemon did not start");
    // GOAL: S17.7 -- `monitors` pairs each monitor number with the connector the daemon binds to it.
    // CONTROL: backend/wpxctl/main.cpp runMonitors(), the printed `monitor` made `monitor + 1`
    //       (control_batch, 2026-11-07) -> red, "FAIL: wpxctl monitors lists DP-3 as monitor 0".
    Run monitors = expectExit({"monitors"}, 0);
    expectText(monitors, true, "0\tDP-3", "wpxctl monitors lists DP-3 as monitor 0");
    expectText(monitors, true, "1\tHDMI-A-1", "wpxctl monitors lists HDMI-A-1 as monitor 1");

    Run raw = expectExit({"status", "--json"}, 0);
    const json parsed = json::parse(raw.out, nullptr, false);
    check(parsed.is_object() && parsed.value("outputs", json::object()).value("DP-3", -1) == 0,
          "wpxctl status --json prints the daemon's status document");

    Run text = expectExit({"status"}, 0);
    expectText(text, true, "DP-3 (monitor 0)", "wpxctl status names DP-3 and its monitor");
    expectText(text, true, "Every screen:", "wpxctl status prints the settings for every screen");
}

void setFile(Session& session, const fs::path& root) {
    if (session.client() == nullptr) return notVerified("wpxctl set with a file", "the daemon did not start");
    const std::string png = wallpaperx::testing::writeSolidPng((root / "blue.png").string(), 8, 8, {0, 0, 255, 255});
    const std::string other = wallpaperx::testing::writeSolidPng((root / "never.png").string(), 8, 8, {1, 2, 3, 255});

    // GOAL: S17.1, S17.7 -- `set DP-3 <png> --position fit` sends SetWallpaperWithProperties for
    //       monitor 0 (the connector resolved), type static from the file, the path, and exactly
    //       {position: fit}.
    // WHY-THIS-SHAPE: graded on the daemon's GetStatus, not wpxctl's "Set ..." line, and DP-3 is
    //       bound to 0 while 1 is also named, so an off-by-one monitor lands on a real other screen.
    // CONTROL: backend/wpxctl/main.cpp runSet(), the monitor sent made `monitor + 1` (control_batch,
    //       2026-11-07) -> red, "FAIL: wpxctl set DP-3 <png> sets a still image on monitor 0".
    const bool draws = session.draws(png);
    if (draws) {
        Run run = expectExit({"set", "DP-3", png, "--position", "fit", "--wait"}, 0);
        expectText(run, true, "Drawn on DP-3", "wpxctl set --wait reports the picture drawn");
    } else {
        notVerified("wpxctl set --wait returning once the picture is drawn",
                    "the daemon brought no GPU up on this host, so it draws nothing");
        expectExit({"set", "DP-3", png, "--position", "fit"}, 0);
    }
    json status = session.status();
    json entry = wallpaperOf(status, "0");
    check(entry.value("type", "") == "static" && entry.value("path_literal", "") == png,
          "wpxctl set DP-3 <png> sets a still image on monitor 0");
    check(propertiesOf(status, "0") == json{{"position", "fit"}}, "wpxctl set --position sends position=fit alone");
    if (draws) check(entry.value("has_frame", false), "after wpxctl set --wait the monitor holds a picture");

    expectExit({"set", "1", png}, 0);
    check(wallpaperOf(session.status(), "1").value("path_literal", "") == png, "wpxctl set 1 <png> sets monitor 1");

    // GOAL: S17.4 -- a value outside what the wallpaper allows is refused with exit 2 and the reason,
    //       and nothing is sent.
    // WHY-THIS-SHAPE: the refused sets name a different file, so a call that went out anyway shows as
    //       a changed path; comparing properties alone misses a call the daemon would also accept.
    // CONTROL: backend/wpxctl/main.cpp allowedValue(), `else if (!soundOrSpeed(key))` made
    //       `else if (true)`, so a still's speed is sent (control_batch, 2026-11-10) -> red, "FAIL: a
    //       refused wpxctl set sends nothing".
    Run refused = expectExit({"set", "DP-3", other, "--speed", "2"}, 2);
    expectText(refused, false, "applies to a video only", "a speed for a still image is refused with the reason");
    expectText(refused, false, "Nothing was sent.", "a refused wpxctl set says nothing was sent");
    expectExit({"set", "DP-3", other, "--position", "sideways"}, 2);
    expectExit({"set", "DP-3", other, "--volume", "150"}, 2);
    expectExit({"set", "DP-3", other, "--position", "tile", "--speed", "0.5"}, 2);
    status = session.status();
    check(wallpaperOf(status, "0").value("path_literal", "") == png && propertiesOf(status, "0") == json{{"position", "fit"}},
          "a refused wpxctl set sends nothing");

    // GOAL: S17.3, S17.5 -- a plain file's type comes from what the file holds, and --speed, --mute
    //       and --volume N reach the daemon, the volume as N/100; a render that fails makes --wait
    //       exit 1 with the daemon's last_error.
    // CONTROL: backend/wpxctl/main.cpp, each through control_batch, 2026-11-07: `*volume / 100.0`
    //       made `*volume` -> red, "FAIL: --speed, --volume (as 0 to 1) and --mute reach the daemon";
    //       fileType() returning "static" for video/* -> red, "FAIL: a .mp4 file is set as a video";
    //       awaitPicture()'s last_error branch made `if (false)` -> red, "FAIL: wpxctl set 1
    //       .../not-really.mp4 ... exits 1 (got 4)".
    const fs::path clip = root / "not-really.mp4";
    writeFile(clip, "not a video");
    Run video = expectExit({"set", "1", clip.string(), "--speed", "0.5", "--volume", "40", "--mute", "--wait",
                            "--timeout", "5"},
                           1);
    expectText(video, false, "could not draw", "wpxctl set --wait reports a render that failed, with the daemon's error");
    const json videoEntry = wallpaperOf(session.status(), "1");
    check(videoEntry.value("type", "") == "video", "a .mp4 file is set as a video");
    check(propertiesOf(session.status(), "1") == json{{"speed", 0.5}, {"volume", 0.4}, {"mute", true}},
          "--speed, --volume (as 0 to 1) and --mute reach the daemon");
}

void setLibrary(Session& session, const fs::path& root) {
    if (session.client() == nullptr) return notVerified("wpxctl set with a library folder", "the daemon did not start");
    const fs::path still = root / "library" / "still";
    wallpaperx::testing::writeSolidPng((still / "assets" / "pic.png").string(), 8, 8, {255, 0, 0, 255});
    writeFile(still / "wallpaper.json", R"({"schema_version": "1", "type": "static", "title": "Red",
        "preview": "assets/pic.png", "file": "assets/pic.png",
        "properties": {"tint": {"type": "color", "default": "#ff8800"},
                       "strength": {"type": "float", "min": 0, "max": 1, "default": 0.25}}})");
    // GOAL: S17.3 -- `set` with a library folder sends the file wallpaper.json names as the folder's
    //       absolute path without "." parts, links not resolved (the path the app's library knows),
    //       and exactly its declared defaults, overridden by the flags given.
    // WHY-THIS-SHAPE: the folder is passed through a "." component, so a path sent as given differs
    //       from the expected one; whole property maps are compared.
    // CONTROL: backend/wpxctl/main.cpp runSet(), each through control_batch, 2026-11-10: the defaults
    //       loop deleted -> red, "FAIL: a library folder's declared defaults are sent"; `asset =
    //       library->asset` made `(given / "assets" / "pic.png").string()` -> red, "FAIL: a library
    //       folder is set as the file its wallpaper.json names".
    expectExit({"set", "DP-3", (root / "library" / "." / "still").string(), "--position", "center"}, 0);
    const json status = session.status();
    check(wallpaperOf(status, "0").value("path_literal", "") ==
              fs::absolute(still / "assets" / "pic.png").lexically_normal().string(),
          "a library folder is set as the file its wallpaper.json names");
    check(propertiesOf(status, "0") == json{{"tint", "#ff8800"}, {"strength", 0.25}, {"position", "center"}},
          "a library folder's declared defaults are sent");
    // GOAL: S17.4 -- a library wallpaper's declared speed and volume ranges are enforced locally, the
    //       volume reason given in percent, and nothing is sent.
    // CONTROL: backend/wpxctl/main.cpp, each through control_batch, 2026-11-10: allowedValue()'s
    //       `kept = manifest::acceptValue(...)` made `kept = value` -> red, "FAIL: wpxctl set DP-3
    //       .../moving --speed 2 exits 2 (got 0)" and the --volume 90 line; refusal()'s
    //       `percent(declared->minimum)` made `percent(declared->maximum)` -> red, "FAIL: a volume
    //       outside the declared range is refused in percent". The range rules themselves
    //       (shared/wallpaper_manifest.cpp Property::accept()/choices()) are controlled in wpx_package_test.

    const fs::path moving = root / "library" / "moving";
    writeFile(moving / "clip.mp4", "never decoded");
    writeFile(moving / "wallpaper.json", R"({"schema_version": 1, "type": "video", "title": "Clip",
        "preview": "clip.mp4", "file": "clip.mp4",
        "properties": {"speed": {"type": "float", "min": 0.5, "max": 1, "default": 1},
                       "volume": {"type": "float", "min": 0.2, "max": 0.8, "default": 0.5}}})");
    Run speed = expectExit({"set", "DP-3", moving.string(), "--speed", "2"}, 2);
    expectText(speed, false, "plays at 0.5 or 1", "a speed outside the declared range is refused with the range");
    Run volume = expectExit({"set", "DP-3", moving.string(), "--volume", "90"}, 2);
    expectText(volume, false, "from 20 to 80", "a volume outside the declared range is refused in percent");
    check(propertiesOf(session.status(), "0") == json{{"tint", "#ff8800"}, {"strength", 0.25}, {"position", "center"}},
          "a refused library set leaves monitor 0 as it was");

    writeFile(root / "library" / "broken" / "wallpaper.json",
              R"({"schema_version": "1", "type": "video", "title": "Broken", "preview": "clip.mp4"})");
    Run broken = expectExit({"set", "DP-3", (root / "library" / "broken").string()}, 2);
    expectText(broken, false, "has no \"file\"", "a wallpaper.json without a file is refused with the reason");

    // GOAL: S17.12 -- `set` on a link to a library folder sets it, and sends the path through the link,
    //       not the link's target.
    // WHY-THIS-SHAPE: the link and its target hold the same wallpaper, so only path_literal tells a
    //       resolved path from the one given.
    // CONTROL: backend/wpxctl/main.cpp, each through control_batch, 2026-11-10: openFolder() given
    //       O_NOFOLLOW -> red, "FAIL: wpxctl set DP-3 .../library-link exits 0 (got 2)"; readLibrary()
    //       passing `fs::canonical(folder)` -> red, "FAIL: a library folder reached through a link is
    //       set by the path through the link". The app's refusal of a linked folder is S15.34 in
    //       wpx_package_test (re-run the same day).
    const fs::path link = fs::absolute(root / "library-link").lexically_normal();
    std::error_code linkError;
    fs::create_directory_symlink(still, link, linkError);
    if (linkError) {
        harnessFailure("the link to the library folder could not be made: " + linkError.message());
    } else {
        expectExit({"set", "DP-3", link.string()}, 0);
        check(wallpaperOf(session.status(), "0").value("path_literal", "") == (link / "assets" / "pic.png").string(),
              "a library folder reached through a link is set by the path through the link");
    }

    // GOAL: S17.13 -- `set a/link/../x.png` checks, reads and sends a/x.png, the file its text names,
    //       where the kernel would reach elsewhere/x.png, which does not exist.
    // WHY-THIS-SHAPE: with nothing at the kernel's answer, a check made on the path as given refuses the
    //       set, and a path sent as given is caught by path_literal.
    // CONTROL: backend/wpxctl/main.cpp runSet(), `fs::is_regular_file(where, error)` made
    //       `(given, error)` (control_batch, 2026-11-10) -> red, "FAIL: wpxctl set DP-3
    //       .../dotdot/a/link/../x.png exits 0 (got 2)". fileType() has no control here: the MIME
    //       guess goes by the name's extension, the same for both paths.
    const fs::path a = fs::absolute(root / "dotdot" / "a").lexically_normal();
    const std::string named = wallpaperx::testing::writeSolidPng((a / "x.png").string(), 8, 8, {4, 5, 6, 255});
    fs::create_directories(root / "dotdot" / "elsewhere" / "deep");
    fs::create_directory_symlink(fs::absolute(root / "dotdot" / "elsewhere" / "deep"), a / "link", linkError);
    if (linkError) {
        harnessFailure("the link for a/link/../x.png could not be made: " + linkError.message());
    } else {
        expectExit({"set", "DP-3", (a / "link" / ".." / "x.png").string()}, 0);
        check(wallpaperOf(session.status(), "0").value("path_literal", "") == named,
              "wpxctl set a/link/../x.png sets a/x.png");
    }
}

void props(Session& session, const fs::path& root) {
    if (session.client() == nullptr) return notVerified("wpxctl props", "the daemon did not start");
    const std::string png = wallpaperx::testing::writeSolidPng((root / "props.png").string(), 8, 8, {0, 255, 0, 255});
    const fs::path still = root / "library" / "props";
    wallpaperx::testing::writeSolidPng((still / "pic.png").string(), 8, 8, {0, 255, 0, 255});
    writeFile(still / "wallpaper.json", R"({"schema_version": "1", "type": "static", "title": "Green",
        "preview": "pic.png", "file": "pic.png",
        "properties": {"tint": {"type": "color", "default": "#00ff00"},
                       "strength": {"type": "float", "min": 0, "max": 1, "default": 0.25}}})");
    expectExit({"set", "DP-3", still.string()}, 0);

    // GOAL: S17.1, S17.5 (props) -- `props` sends the current properties with the given ones changed
    //       and `key=` ones removed; for a library wallpaper, values outside its wallpaper.json are
    //       refused and nothing is sent; volume=N reaches the daemon as N/100.
    // WHY-THIS-SHAPE: whole property maps from GetStatus are compared after each call, so a dropped
    //       key and a stray one are both caught; the refusals are followed by a map that must not move.
    // CONTROL: backend/wpxctl/main.cpp runProps(), each through control_batch, 2026-11-10: the loop
    //       copying the current properties deleted -> red, "FAIL: wpxctl props changes the named
    //       properties and keeps the others"; `values.erase(key)` deleted -> red, "FAIL: wpxctl props
    //       key= removes that property"; the props volume `/ 100.0` deleted -> red, "FAIL: wpxctl
    //       props volume=40 reaches the daemon as 0.4"; libraryOfAsset() returning std::nullopt at
    //       once (re-run on the rewritten function, control_batch 2026-11-10) -> red, "FAIL: wpxctl
    //       props DP-3 strength=2 exits 2 (got 0)".
    expectExit({"props", "DP-3", "strength=0.5", "position=center"}, 0);
    check(propertiesOf(session.status(), "0") == json{{"tint", "#00ff00"}, {"strength", 0.5}, {"position", "center"}},
          "wpxctl props changes the named properties and keeps the others");
    expectExit({"props", "DP-3", "tint="}, 0);
    check(propertiesOf(session.status(), "0") == json{{"strength", 0.5}, {"position", "center"}},
          "wpxctl props key= removes that property");

    Run range = expectExit({"props", "DP-3", "strength=2"}, 2);
    expectText(range, false, "goes from 0 to 1", "wpxctl props refuses a value outside the declared range");
    expectExit({"props", "DP-3", "glow=1"}, 2);
    expectExit({"props", "DP-3", "tint=orange"}, 2);
    check(propertiesOf(session.status(), "0") == json{{"strength", 0.5}, {"position", "center"}},
          "a refused wpxctl props sends nothing");

    expectExit({"set", "1", png}, 0);
    expectExit({"props", "1", "glow=1.5", "position=tile"}, 0);
    check(propertiesOf(session.status(), "1") == json{{"glow", 1.5}, {"position", "tile"}},
          "wpxctl props on a plain file passes keys the daemon only carries");

    const fs::path clip = root / "props-clip.mp4";
    writeFile(clip, "not decoded");
    expectExit({"set", "1", clip.string()}, 0);
    expectExit({"props", "1", "volume=40"}, 0);
    const json volume = propertiesOf(session.status(), "1");
    check(volume.contains("volume") && volume["volume"].is_number() && volume["volume"].get<double>() == 0.4,
          "wpxctl props volume=40 reaches the daemon as 0.4");

    // GOAL: S17.14 -- a library video that declares no speed takes 0.5, 1 and 2, on `set` and on
    //       `props`, and refuses 3 on both with the standard speeds named.
    // WHY-THIS-SHAPE: each accepted speed is read back from GetStatus; the refusals are followed by a
    //       speed that must not have moved.
    // CONTROL: backend/wpxctl/main.cpp, each through control_batch, 2026-11-10: allowedValue()'s
    //       `rules = nullptr` line for an undeclared speed deleted -> red, "FAIL: wpxctl set DP-3
    //       .../nospeed --speed 0.5 exits 0 (got 2)" (and 1, 2); plainVideoValue() keeping any speed
    //       -> red, "FAIL: props speed=3 on a library video declaring no speed names the speeds"; that
    //       and runSet()'s own --speed test cut to `!speed` -> red, "FAIL: wpxctl set DP-3 .../nospeed
    //       --speed 3 exits 2 (got 1)" (the daemon refused what went out).
    const fs::path nospeed = root / "library" / "nospeed";
    writeFile(nospeed / "clip.mp4", "never decoded");
    writeFile(nospeed / "wallpaper.json", R"({"schema_version": "1", "type": "video", "title": "No speed",
        "preview": "clip.mp4", "file": "clip.mp4"})");
    const auto speedOn0 = [&session] {
        const json held = propertiesOf(session.status(), "0");
        return held.contains("speed") && held["speed"].is_number() ? held["speed"].get<double>() : -1.0;
    };
    for (const char* speed : {"0.5", "1", "2"}) {
        expectExit({"set", "DP-3", nospeed.string(), "--speed", speed}, 0);
        check(speedOn0() == std::stod(speed), (std::string("a library video declaring no speed is set at speed ") + speed).c_str());
    }
    Run setThree = expectExit({"set", "DP-3", nospeed.string(), "--speed", "3"}, 2);
    expectText(setThree, false, "0.5, 1 or 2", "set --speed 3 on a library video declaring no speed names the speeds");
    expectExit({"props", "DP-3", "speed=1"}, 0);
    check(speedOn0() == 1.0, "wpxctl props speed=1 on a library video declaring no speed is taken");
    Run propsThree = expectExit({"props", "DP-3", "speed=3"}, 2);
    expectText(propsThree, false, "speed must be 0.5, 1 or 2.", "props speed=3 on a library video declaring no speed names the speeds");
    check(speedOn0() == 1.0, "a refused speed on a library video declaring no speed sends nothing");

    // GOAL: S17.15 -- `props` on a library wallpaper whose preview has gone still holds values to its
    //       wallpaper.json; one whose wallpaper.json still names the asset but no longer passes the
    //       rules refuses (exit 2) and sends nothing.
    // WHY-THIS-SHAPE: a plain file's rules pass any key and any number, so a fall back to them shows as
    //       an exit 0 and a moved property map.
    // CONTROL: backend/wpxctl/main.cpp libraryOfAsset(), each through control_batch, 2026-11-10:
    //       `if (!readLibrary(folder)) return std::nullopt;` added before parse(), a whole-folder read
    //       falling back to plain-file rules -> red, "FAIL: wpxctl props DP-3 glow=1 exits 2 (got 0)";
    //       `if (!parsed) refuse(...)` made `return std::nullopt` -> red, "FAIL: wpxctl props DP-3
    //       strength=0.75 exits 2 (got 0)".
    const fs::path damaged = root / "library" / "damaged";
    wallpaperx::testing::writeSolidPng((damaged / "pic.png").string(), 8, 8, {7, 7, 7, 255});
    wallpaperx::testing::writeSolidPng((damaged / "shot.png").string(), 8, 8, {8, 8, 8, 255});
    writeFile(damaged / "wallpaper.json", R"({"schema_version": "1", "type": "static", "title": "Damaged",
        "preview": "shot.png", "file": "pic.png",
        "properties": {"strength": {"type": "float", "min": 0, "max": 1, "default": 0.25}}})");
    expectExit({"set", "DP-3", damaged.string()}, 0);
    fs::remove(damaged / "shot.png");
    expectExit({"props", "DP-3", "glow=1"}, 2);
    expectExit({"props", "DP-3", "strength=2"}, 2);
    check(propertiesOf(session.status(), "0") == json{{"strength", 0.25}},
          "with its preview gone, a library wallpaper's undeclared key and out-of-range value are refused");
    expectExit({"props", "DP-3", "strength=0.5"}, 0);
    writeFile(damaged / "wallpaper.json", R"({"schema_version": "1", "type": "static", "file": "pic.png"})");
    Run unreadable = expectExit({"props", "DP-3", "strength=0.75"}, 2);
    expectText(unreadable, false, "cannot be checked",
               "props under a wallpaper.json naming the asset but breaking the rules says it cannot be checked");
    check(propertiesOf(session.status(), "0") == json{{"strength", 0.5}},
          "props under a wallpaper.json naming the asset but breaking the rules sends nothing");

    // GOAL: S17.23 -- `props` on a plain file below a wallpaper.json that cannot be parsed, cannot be
    //       opened, names another file, or names it by a path that is not plain and relative (absolute,
    //       "../"), exits 0, sends the value, and prints one stderr line naming that wallpaper.json.
    // WHY-THIS-SHAPE: every stray file but the unparseable one is a valid manifest declaring strength 0
    //       to 1, so a wpxctl that took it as the asset's would refuse strength=5 (exit 2) instead of
    //       sending it; the line is counted, not just found. The "../" row is also rejected by
    //       libraryOfAsset()'s path comparison (folder/"../stray/pic.png" is never the asset's text),
    //       so only the absolute row is controlled on assetNameIn().
    // CONTROL: control_batch, 2026-11-10: backend/wpxctl/main.cpp libraryOfAsset(), `if (!text) return
    //       plain(...)` made `fail(kUsage, ...)` -> red, "FAIL: props beside an unreadable wallpaper.json
    //       sends the value"; `if (!named) return plain(...)` made a refusal -> red, "... an unparseable
    //       wallpaper.json sends the value"; the `plain()` fprintf deleted -> red, "FAIL: props beside an
    //       unparseable wallpaper.json says once which file it did not take"; shared/wallpaper_manifest.cpp
    //       assetNameIn()'s `!isPlainRelativePath(...)` term dropped -> red, "FAIL: props beside an
    //       absolute-path wallpaper.json sends the value".
    const fs::path stray = root / "stray";
    const std::string strayPng = wallpaperx::testing::writeSolidPng((stray / "pic.png").string(), 8, 8, {9, 9, 9, 255});
    const fs::path strayJson = stray / "wallpaper.json";
    const auto declaring = [](const std::string& file) {
        return R"({"schema_version": "1", "type": "static", "title": "Stray", "preview": ")" + file + R"(", "file": ")" +
               file + R"(", "properties": {"strength": {"type": "float", "min": 0, "max": 1, "default": 0.25}}})";
    };
    struct Stray {
        const char* label;
        std::string text;
        const char* why;
        bool unreadable = false;
    };
    const std::vector<Stray> strays{
        {"an unparseable", "{not json", "It names no file"},
        {"an unreadable", declaring("pic.png"), "", true},
        {"another file's", declaring("other.png"), "It names other.png"},
        {"an absolute-path", declaring(strayPng), "It names no file"},
        {"a ../-path", declaring("../stray/pic.png"), "It names no file"},
    };
    expectExit({"set", "1", strayPng}, 0);
    int strayRows = 0;
    for (const Stray& row : strays) {
        writeFile(strayJson, row.text);
        if (row.unreadable) {
            fs::permissions(strayJson, fs::perms::none);
            if (::access(strayJson.c_str(), R_OK) == 0) {
                notVerified("S17.23 (unreadable row)", "this process can read a mode-000 file, so it cannot be made unreadable");
                fs::permissions(strayJson, fs::perms::owner_read | fs::perms::owner_write);
                continue;
            }
        }
        Run run = expectExit({"props", "1", "strength=5"}, 0);
        if (row.unreadable) fs::permissions(strayJson, fs::perms::owner_read | fs::perms::owner_write);
        const json held = propertiesOf(session.status(), "1");
        check(held.contains("strength") && held["strength"] == 5,
              (std::string("props beside ") + row.label + " wallpaper.json sends the value").c_str());
        std::size_t lines = 0;
        for (std::size_t at = 0; (at = run.err.find("is not taken as this wallpaper's", at)) != std::string::npos; ++at) ++lines;
        const bool named = run.err.find(strayJson.string() + " is not taken") != std::string::npos &&
                           run.err.find(row.why) != std::string::npos;
        check(lines == 1 && named,
              (std::string("props beside ") + row.label + " wallpaper.json says once which file it did not take").c_str());
        if (lines != 1 || !named) std::fprintf(stderr, "  stderr: %s\n", run.err.c_str());
        expectExit({"props", "1", "strength="}, 0);
        ++strayRows;
    }
    // COVERAGE-ASSERT: a row skipped for a readable mode-000 file prints NOT VERIFIED above; the rest ran.
    check(strayRows >= 4, "at least the four stray rows that need no permission ran");

    // GOAL: S17.24 -- a wallpaper.json that names the asset but declares a sound key malformed (mute
    //       as a number) ends `props` with exit 2, and nothing is sent.
    // CONTROL: control_batch, 2026-11-10: backend/wpxctl/main.cpp libraryOfAsset(), the
    //       settleSoundFor() refusal made to fall through -> red, "FAIL: wpxctl props 1 speed=1 exits
    //       2 (got 0)".
    const fs::path loud = root / "library" / "loud";
    writeFile(loud / "clip.mp4", "never decoded");
    writeFile(loud / "wallpaper.json", R"({"schema_version": "1", "type": "video", "title": "Loud",
        "preview": "clip.mp4", "file": "clip.mp4"})");
    expectExit({"set", "1", loud.string()}, 0);
    const json loudBefore = propertiesOf(session.status(), "1");
    writeFile(loud / "wallpaper.json", R"({"schema_version": "1", "type": "video", "title": "Loud",
        "preview": "clip.mp4", "file": "clip.mp4",
        "properties": {"mute": {"type": "float", "min": 0, "max": 1, "default": 0}}})");
    Run badSound = expectExit({"props", "1", "speed=1"}, 2);
    expectText(badSound, false, "cannot be checked", "props under malformed sound keys says the values cannot be checked");
    check(propertiesOf(session.status(), "1") == loudBefore, "props under malformed sound keys sends nothing");

    // GOAL: S17.25 -- for the same library wallpaper, `set` and `props` offer the same sound keys: a
    //       video's set sends mute and volume and props takes volume; a still's set sends neither and
    //       props refuses volume.
    // WHY-THIS-SHAPE: set's offer is read from what it sent (GetStatus), props's from whether volume=40
    //       is taken; the two readers settle sound separately (readFolder() and libraryOfAsset()), so
    //       one drifting shows as a disagreement on the video row.
    // CONTROL: control_batch, 2026-11-10: backend/wpxctl/main.cpp libraryOfAsset()'s settleSoundFor()
    //       given `[] { return AudioStreams::Absent; }` -> red, "FAIL: wpxctl props 1 volume=40 exits 0
    //       (got 2)" (S17.24's rows above red too: Absent strips the malformed key instead of refusing
    //       it); takenToHaveSound() returning Absent -> red, "FAIL: set on a library video sends mute and
    //       volume".
    writeFile(loud / "wallpaper.json", R"({"schema_version": "1", "type": "video", "title": "Loud",
        "preview": "clip.mp4", "file": "clip.mp4"})");
    expectExit({"set", "1", loud.string()}, 0);
    const json videoSent = propertiesOf(session.status(), "1");
    check(videoSent.contains("mute") && videoSent.contains("volume"), "set on a library video sends mute and volume");
    expectExit({"props", "1", "volume=40"}, 0);
    const fs::path quiet = root / "library" / "quiet";
    wallpaperx::testing::writeSolidPng((quiet / "pic.png").string(), 8, 8, {3, 3, 3, 255});
    writeFile(quiet / "wallpaper.json", R"({"schema_version": "1", "type": "static", "title": "Quiet",
        "preview": "pic.png", "file": "pic.png",
        "properties": {"volume": {"type": "float", "min": 0, "max": 1, "default": 0.5}}})");
    expectExit({"set", "1", quiet.string()}, 0);
    const json stillSent = propertiesOf(session.status(), "1");
    check(!stillSent.contains("mute") && !stillSent.contains("volume"), "set on a library still sends no sound keys");
    expectExit({"props", "1", "volume=40"}, 2);
}

void settings(Session& session) {
    if (session.client() == nullptr) return notVerified("wpxctl pause, sound and fps", "the daemon did not start");
    // GOAL: S17.1 -- `pause` takes the short mode names (backend/daemon/pause_mode.h kPauseModes) and
    //       sends SetMonitorPause with the daemon's name for that mode, to the screen named.
    // CONTROL: backend/wpxctl/main.cpp runPause(), the mode sent made PauseMode::Always (control_batch,
    //       2026-11-10) -> red, "FAIL: wpxctl pause DP-3 focused".
    expectExit({"pause", "DP-3", "focused"}, 0);
    expectExit({"pause", "1", "maximized"}, 0);
    json status = session.status();
    check(status["pause"]["0"].value("mode", "") == "paused_while_focused", "wpxctl pause DP-3 focused");
    check(status["pause"]["1"].value("mode", "") == "paused_while_maximized", "wpxctl pause 1 maximized");
    // GOAL: S17.16 -- a daemon name for a mode is refused (exit 2) with the four short names, and nothing
    //       is sent.
    // CONTROL: each through control_batch, 2026-11-10: backend/daemon/pause_mode.h
    //       pauseModeFromShortName() also matching `row.name` -> red, "FAIL: wpxctl pause DP-3
    //       paused_while_maximized exits 2 (got 0)"; backend/wpxctl/main.cpp runPause()'s refusal
    //       listing `PauseModeNames::name` -> red, "FAIL: a long pause name is refused with the short names".
    Run longName = expectExit({"pause", "DP-3", "paused_while_maximized"}, 2);
    expectText(longName, false, "play, always, focused or maximized", "a long pause name is refused with the short names");
    check(session.status()["pause"]["0"].value("mode", "") == "paused_while_focused", "a refused pause name sends nothing");

    // GOAL: S17.1 -- `sound off <screen>` sends SetMonitorSound for that screen only and `sound off`
    //       sends SetSound.
    // WHY-THIS-SHAPE: the screen's own setting goes first and the shared one is checked still on, so
    //       a per-screen call sent as SetSound is visible; the other order hides it.
    // CONTROL: backend/wpxctl/main.cpp runSound(), each through control_batch, 2026-11-07: the
    //       per-screen branch sending SetSound(on) -> red, "FAIL: wpxctl sound off HDMI-A-1 leaves
    //       sound on for every screen"; the shared branch sending SetMonitorSound(0, on) -> red,
    //       "FAIL: wpxctl sound off switches sound off for every screen".
    expectExit({"sound", "off", "HDMI-A-1"}, 0);
    status = session.status();
    check(status["sound"]["monitors"]["1"]["enabled"] == false, "wpxctl sound off HDMI-A-1 switches monitor 1's off");
    check(status["sound"]["enabled"] == true, "wpxctl sound off HDMI-A-1 leaves sound on for every screen");
    expectExit({"sound", "off"}, 0);
    status = session.status();
    check(status["sound"]["enabled"] == false, "wpxctl sound off switches sound off for every screen");

    // GOAL: S17.1, S17.10 -- `fps <n>` sends SetFrameRate, `fps <n> <screen>` SetMonitorFrameRate,
    //       `off` sends 0 so the screen follows the shared rate, and --keep-higher / --no-keep-higher
    //       send SetKeepHigherFrameRate true / false.
    // CONTROL: backend/wpxctl/main.cpp runFps(), each through control_batch, 2026-11-07: SetFrameRate
    //       sent 60 -> red, "FAIL: wpxctl fps 120 sets the rate for every screen"; `off` sending 30 ->
    //       red, "FAIL: wpxctl fps off DP-3 drops monitor 0's rate"; `keep` made `true` -> red,
    //       "FAIL: --no-keep-higher stops keeping them".
    expectExit({"fps", "120"}, 0);
    expectExit({"fps", "30", "DP-3", "--keep-higher"}, 0);
    status = session.status();
    check(status["frame_rate"]["rate"] == 120, "wpxctl fps 120 sets the rate for every screen");
    check(status["frame_rate"]["monitors"]["0"]["rate"] == 30, "wpxctl fps 30 DP-3 sets monitor 0's rate");
    check(status["frame_rate"]["keep_higher"] == true, "--keep-higher keeps rates above the refresh rate");
    expectExit({"fps", "off", "DP-3", "--no-keep-higher"}, 0);
    status = session.status();
    check(status["frame_rate"]["monitors"]["0"]["follows_global"] == true, "wpxctl fps off DP-3 drops monitor 0's rate");
    check(status["frame_rate"]["keep_higher"] == false, "--no-keep-higher stops keeping them");
}

void refused(Session& session, const fs::path& root) {
    if (session.client() == nullptr) return notVerified("wpxctl's refusals", "the daemon did not start");
    // GOAL: S17.2 -- a daemon refusal exits 1 and prints the daemon's error name and message; an
    //       unknown connector exits 2 naming it, before any call.
    // CONTROL: backend/wpxctl/main.cpp, each through control_batch, 2026-11-07: daemonFailed()'s
    //       ` (" + name + ")` dropped -> red, "FAIL: a daemon refusal prints the error's name";
    //       resolveScreen()'s fail(kUsage, ...) made kRefused -> red, "FAIL: wpxctl set NOPE-1
    //       .../unnamed.png exits 2 (got 1)".
    Run run = expectExit({"pause", "40", "play"}, 1);
    expectText(run, false, "org.freedesktop.DBus.Error.InvalidArgs", "a daemon refusal prints the error's name");
    expectText(run, false, "The daemon refused SetMonitorPause: ", "a daemon refusal prints the method it refused");
    const std::string png = wallpaperx::testing::writeSolidPng((root / "unnamed.png").string(), 8, 8, {9, 9, 9, 255});
    Run unknown = expectExit({"set", "NOPE-1", png}, 2);
    expectText(unknown, false, "knows no screen named NOPE-1", "an unknown connector is a usage error naming it");
}

// Owns org.kde.plasmashell on the private bus, records each script and answers as told.
class ShellStandIn {
public:
    ShellStandIn() {
        m_connection = sdbus::createBusConnection(sdbus::ServiceName{"org.kde.plasmashell"});
        m_object = sdbus::createObject(*m_connection, sdbus::ObjectPath{"/PlasmaShell"});
        m_object
            ->addVTable(sdbus::registerMethod("evaluateScript")
                            .implementedAs([this](const std::string& script) -> std::string {
                                std::lock_guard<std::mutex> lock(m_mutex);
                                m_scripts.push_back(script);
                                if (!m_errorName.empty()) throw sdbus::Error(sdbus::Error::Name{m_errorName}, m_answer);
                                return m_answer;
                            })
                            .withInputParamNames("script")
                            .withOutputParamNames("output"))
            .forInterface("org.kde.PlasmaShell");
        m_connection->enterEventLoopAsync();
    }

    ~ShellStandIn() {
        m_object.reset();
        m_connection->leaveEventLoop();
    }

    void answer(std::string printed) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_errorName.clear();
        m_answer = std::move(printed);
    }

    void refuse(std::string name, std::string message) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_errorName = std::move(name);
        m_answer = std::move(message);
    }

    std::vector<std::string> scripts() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_scripts;
    }

private:
    std::unique_ptr<sdbus::IConnection> m_connection;
    std::unique_ptr<sdbus::IObject> m_object;
    std::mutex m_mutex;
    std::vector<std::string> m_scripts;
    std::string m_errorName;
    std::string m_answer;
};

// The activation script as written out by hand, `connector` already a JavaScript string literal: an
// expectation that does not come from shared/plasma_activation.h, which builds what wpxctl sends.
std::string expectedScript(const std::string& connector) {
    return "(function () {\n"
           "    var connector = " + connector + ";\n" + R"js(    function fail(why) { throw new Error("[wallpaperx]" + why + "[/wallpaperx]"); }
    if (!("WallpaperX" in knownWallpaperPlugins()))
        fail("The WallpaperX wallpaper plugin is not installed for Plasma");
    if (typeof screenForConnector !== "function")
        fail("This Plasma cannot find a screen by its connector name");
    var screen = screenForConnector(connector);
    if (typeof screen !== "number" || screen < 0)
        fail("Plasma shows no desktop of its own on " + connector);
    var all = desktops();
    var switched = 0;
    for (var i = 0; i < all.length; ++i) {
        var d = all[i];
        var on = d.screen >= 0 ? d.screen : parseInt(d.readConfig("lastScreen", -1), 10);
        if (on !== screen)
            continue;
        d.wallpaperPlugin = "com.wallpaperx.wallpaper";
        ++switched;
    }
    if (switched === 0)
        fail("Plasma has no desktop on " + connector + " yet");
    print("wallpaperx-switched=" + switched);
})();
)js";
}

void activate(Session& session) {
    // GOAL: S17.8 -- with no org.kde.plasmashell on the bus, `activate` exits 3 saying plasmashell is
    //       not running.
    // CONTROL: backend/wpxctl/main.cpp runActivate(), the unreachable() branch made kRefused
    //       (control_batch, 2026-11-07) -> red, "FAIL: wpxctl activate DP-3 exits 3 (got 1)".
    Run absent = expectExit({"activate", "DP-3"}, 3);
    expectText(absent, false, "plasmashell) is not running", "wpxctl activate without plasmashell exits 3 saying so");

    ShellStandIn shell;
    shell.answer("wallpaperx-switched=2");
    // GOAL: S17.8, S17.9 -- `activate DP-3` sends evaluateScript with exactly the activation script for
    //       DP-3, and reports the count Plasma printed; a connector outside printable ASCII, or holding
    //       a quote, reaches the script escaped by UTF-16 code unit.
    // WHY-THIS-SHAPE: the stand-in records the bytes on the wire and they are compared whole with a
    //       script written out in this file, not with plasmaActivationScript(): an expectation built
    //       by the code under test changes with it and cannot catch a fault in it.
    // CONTROL: backend/wpxctl/main.cpp runActivate(), the script sent made `plasmaActivationScript(wide)
    //       + "\n"` (control_batch, 2026-11-10) -> red, "FAIL: wpxctl activate DP-3 sends the activation
    //       script for DP-3". No break inside shared/plasma_activation.h has been run against it.
    Run named = expectExit({"activate", "DP-3"}, 0);
    expectText(named, true, "2 desktops on DP-3", "wpxctl activate reports the desktops Plasma switched");
    std::vector<std::string> scripts = shell.scripts();
    check(scripts.size() == 1 && scripts.back() == expectedScript("\"DP-3\""),
          "wpxctl activate DP-3 sends the activation script for DP-3");
    expectExit({"activate", "\xC3\x89\"x"}, 0);
    scripts = shell.scripts();
    // É as its UTF-16 escape, then the quote escaped: split so nothing reads it as a C++ escape.
    check(scripts.size() == 2 && scripts.back() == expectedScript("\"\\" "u00c9\\\"x\""),
          "wpxctl activate escapes a connector's non-ASCII letters and quotes");

    // GOAL: S17.7, S17.8 -- `activate <number>` activates the connector the daemon binds to that
    //       monitor, as it was set (outputs_literal), and a monitor with no connector is a usage error.
    // CONTROL: backend/wpxctl/main.cpp runActivate(), `connector = std::move(*named);` deleted
    //       (control_batch, 2026-11-10) -> red, "FAIL: wpxctl activate 1 activates the connector the
    //       daemon binds to monitor 1".
    if (session.client() == nullptr) {
        notVerified("wpxctl activate by monitor number", "the daemon did not start");
    } else {
        expectExit({"activate", "1"}, 0);
        scripts = shell.scripts();
        check(!scripts.empty() && scripts.back() == expectedScript("\"HDMI-A-1\""),
              "wpxctl activate 1 activates the connector the daemon binds to monitor 1");
        expectExit({"activate", "7"}, 2);
    }

    // GOAL: S17.8 -- each Plasma failure maps to its exit and sentence: the script's own thrown
    //       sentence (exit 1, the sentence alone), a reply not confirming the switch (exit 1), no answer
    //       (exit 3), and a shell that takes no desktop scripts (exit 1).
    // CONTROL: each through control_batch, 2026-11-07: shared/plasma_activation.h
    //       plasmaActivationFailure() returning std::nullopt at once -> red, "FAIL: a script's own
    //       failure is printed"; plasmaActivationSwitched()'s final `return 0;` made `return 1;` ->
    //       red, "FAIL: wpxctl activate DP-3 exits 1 (got 0)"; backend/wpxctl/main.cpp runActivate(),
    //       the noAnswer() branch made kRefused -> red, "FAIL: wpxctl activate DP-3 with no answer
    //       exits 3"; the UnknownMethod branch deleted -> red, "FAIL: a shell without desktop scripts
    //       is named as such".
    shell.refuse("org.freedesktop.DBus.Error.Failed",
                 "Error: [wallpaperx]Plasma shows no desktop of its own on DP-3[/wallpaperx] at line 9");
    Run thrown = expectExit({"activate", "DP-3"}, 1);
    expectText(thrown, false, "wpxctl: Plasma shows no desktop of its own on DP-3.\n", "a script's own failure is printed");
    shell.answer("");
    Run silent = expectExit({"activate", "DP-3"}, 1);
    expectText(silent, false, "did not say it had", "a reply that does not confirm the switch is a failure");
    shell.refuse("org.freedesktop.DBus.Error.NoReply", "no reply");
    Run quiet = wpxctl({"activate", "DP-3"});
    check(quiet.status == 3, "wpxctl activate DP-3 with no answer exits 3");
    expectText(quiet, false, "did not answer", "wpxctl activate with no answer says the shell did not answer");
    shell.refuse("org.freedesktop.DBus.Error.UnknownMethod", "No such method");
    Run foreign = expectExit({"activate", "DP-3"}, 1);
    expectText(foreign, false, "does not take Plasma's desktop scripts", "a shell without desktop scripts is named as such");
}

// Owns the daemon's name on the private bus in its place: GetStatus names DP-3 as monitor 0, and
// SetWallpaperWithProperties announces the given pictures on that monitor, then replies with
// `accepted` -- or with nothing, as a daemon older than the frame number does.
class DaemonStandIn {
public:
    DaemonStandIn(std::optional<uint64_t> accepted, std::vector<uint64_t> announced)
        : m_accepted(accepted), m_announced(std::move(announced)) {
        m_connection = sdbus::createBusConnection(sdbus::ServiceName{wallpaperx::kDaemonBusName});
        m_object = sdbus::createObject(*m_connection, sdbus::ObjectPath{wallpaperx::kDaemonObjectPath});
        auto status = sdbus::registerMethod("GetStatus")
                          .implementedAs([] {
                              return std::string(R"({"outputs": {"DP-3": 0}, "outputs_literal": {"DP-3": 0}, "wallpapers": {}})");
                          })
                          .withOutputParamNames("status");
        auto frame = sdbus::registerSignal("FrameReady").withParameters<int32_t, uint64_t>("monitor_id", "frame_generation");
        using Wire = std::map<std::string, sdbus::Variant>;
        if (m_accepted) {
            auto set = sdbus::registerMethod("SetWallpaperWithProperties")
                           .implementedAs([this](const std::string&, const std::string&, int32_t monitor, const Wire&) {
                               announce(monitor);
                               return *m_accepted;
                           });
            m_object->addVTable(std::move(status), std::move(frame), std::move(set)).forInterface(wallpaperx::kDaemonInterface);
        } else {
            auto set = sdbus::registerMethod("SetWallpaperWithProperties")
                           .implementedAs([this](const std::string&, const std::string&, int32_t monitor, const Wire&) {
                               announce(monitor);
                           });
            m_object->addVTable(std::move(status), std::move(frame), std::move(set)).forInterface(wallpaperx::kDaemonInterface);
        }
        m_connection->enterEventLoopAsync();
    }

    ~DaemonStandIn() {
        m_object.reset();
        m_connection->leaveEventLoop();
    }

    int calls() const { return m_calls.load(); }

private:
    void announce(int32_t monitor) {
        ++m_calls;
        for (const uint64_t picture : m_announced) {
            m_object->emitSignal("FrameReady").onInterface(wallpaperx::kDaemonInterface).withArguments(monitor, picture);
        }
    }

    std::optional<uint64_t> m_accepted;
    std::vector<uint64_t> m_announced;
    std::atomic<int> m_calls{0};
    std::unique_ptr<sdbus::IConnection> m_connection;
    std::unique_ptr<sdbus::IObject> m_object;
};

void waitStandIn(NameWatcher& names, const fs::path& root) {
    const std::string png = wallpaperx::testing::writeSolidPng((root / "standin.png").string(), 8, 8, {3, 3, 3, 255});
    // wpxctl's `set DP-3 <png> --wait --timeout 1` against a stand-in; nothing, with a harness fault, when
    // the stand-in was not reached or its announcements did not go out.
    const auto against = [&](std::optional<uint64_t> accepted, std::vector<uint64_t> announced,
                             const std::string& what) -> std::optional<Run> {
        std::unique_ptr<DaemonStandIn> standIn;
        wallpaperx::testing::FrameReadyLog log;
        std::unique_ptr<sdbus::IProxy> listener;
        try {
            standIn = std::make_unique<DaemonStandIn>(accepted, announced);
            listener = wallpaperx::testing::subscribeFrameReady(log);
        } catch (const std::exception& e) {
            harnessFailure(what + ": the stand-in daemon could not take the daemon's name: " + e.what());
            return std::nullopt;
        }
        Run run = wpxctl({"set", "DP-3", png, "--wait", "--timeout", "1"});
        const bool heard = announced.empty() || log.wait(0, announced.back());
        const int calls = standIn->calls();
        listener.reset();
        standIn.reset();
        if (!names.waitFor(false)) harnessFailure("the stand-in daemon's name is still owned after it was stopped");
        if (calls != 1 || !heard) {
            harnessFailure(what + ": the stand-in got " + std::to_string(calls) + " SetWallpaperWithProperties call(s)" +
                           (heard ? "" : " and its announcements were not heard on the bus"));
            std::fprintf(stderr, "  stdout: %s\n  stderr: %s\n", run.out.c_str(), run.err.c_str());
            return std::nullopt;
        }
        return run;
    };

    // GOAL: S17.17 -- `set --wait` waits for a picture numbered above exactly the reply's frame number:
    //       pictures at and below it, announced before the reply, never satisfy it, and one above it does.
    // WHY-THIS-SHAPE: a stand-in, because the real daemon offers no way to re-announce an older picture
    //       on demand (S17.11); its announcements are checked heard on the bus, so the exit 4 cannot come
    //       from a stand-in that announced nothing.
    // CONTROL: backend/wpxctl/main.cpp FrameWatch::wait(), each through control_batch, 2026-11-10:
    //       `> accepted` made `>=` -> red, "FAIL: pictures at or below the reply's frame number do not
    //       end set --wait (exit 0, want 4)"; made `> accepted + 1` -> red, "FAIL: a picture above the
    //       reply's frame number ends set --wait (exit 4, want 0)".
    if (const auto held = against(7, {6, 7}, "pictures 6 and 7, reply 7")) {
        check(held->status == 4, ("pictures at or below the reply's frame number do not end set --wait (exit " +
                                  std::to_string(held->status) + ", want 4)").c_str());
        if (held->status != 4) std::fprintf(stderr, "  stdout: %s\n  stderr: %s\n", held->out.c_str(), held->err.c_str());
        expectText(*held, false, "No picture was drawn on DP-3", "set --wait not satisfied says no picture was drawn");
    }
    if (const auto drawn = against(7, {7, 8}, "pictures 7 and 8, reply 7")) {
        check(drawn->status == 0, ("a picture above the reply's frame number ends set --wait (exit " +
                                   std::to_string(drawn->status) + ", want 0)").c_str());
        expectText(*drawn, true, "Drawn on DP-3 (monitor 0): picture 8.", "set --wait reports the picture above the reply's number");
    }

    // GOAL: S17.18 -- against a daemon whose reply carries no frame number, `set --wait` exits 4 with
    //       the sentence that the wallpaper was applied but its drawing cannot be reported.
    // CONTROL: backend/wpxctl/main.cpp runSet(), the no-number branch's `fail(kTimedOut, ...)` made
    //       `kRefused` (control_batch, 2026-11-10) -> red, "FAIL: set --wait against a reply with no
    //       frame number exits 4 (got 1)".
    if (const auto older = against(std::nullopt, {}, "a reply with no frame number")) {
        check(older->status == 4, ("set --wait against a reply with no frame number exits 4 (got " +
                                   std::to_string(older->status) + ")").c_str());
        expectText(*older, true, "Set " + png, "set --wait against an older daemon says the wallpaper was set");
        expectText(*older, false, "The wallpaper was applied, but this daemon cannot report when it is drawn",
                   "set --wait against an older daemon says why it cannot wait");
    }
}

}  // namespace

int main() {
    wallpaperx::testing::g_testName = "wpxctl_test";
    // From the environment isolated_run.sh sets, not the parent process: the whole-binary entry runs
    // this under phase_entry.sh rather than straight under dbus-run-session.
    if (const std::optional<int> refusedRun = wallpaperx::testing::refuseOffPrivateTestBus()) return *refusedRun;
    try {
        ScratchDir root("wpxctl");
        if (!root.ok()) {
            notVerified("every goal in this file", "a scratch directory could not be created");
            return wallpaperx::testing::reportNotVerifiedOrPassed();
        }
        NameWatcher names;
        Session session{names, nullptr, nullptr};
        if (phaseSelected("usage")) guardedPhase("usage", usage);
        // Before anything starts the daemon.
        if (phaseSelected("notRunning")) guardedPhase("notRunning", notRunning);
        if (phaseSelected("statusAndMonitors")) guardedPhase("statusAndMonitors", [&] { statusAndMonitors(session); });
        if (phaseSelected("setFile")) guardedPhase("setFile", [&] { setFile(session, root.path()); });
        if (phaseSelected("setLibrary")) guardedPhase("setLibrary", [&] { setLibrary(session, root.path()); });
        if (phaseSelected("props")) guardedPhase("props", [&] { props(session, root.path()); });
        if (phaseSelected("settings")) guardedPhase("settings", [&] { settings(session); });
        if (phaseSelected("refused")) guardedPhase("refused", [&] { refused(session, root.path()); });
        if (phaseSelected("activate")) guardedPhase("activate", [&] { activate(session); });
        session.stop();
        // After the daemon has gone: the stand-in takes its name.
        if (phaseSelected("waitStandIn")) guardedPhase("waitStandIn", [&] { waitStandIn(names, root.path()); });
    } catch (const std::exception& e) {
        check(false, (std::string("unexpected exception: ") + e.what()).c_str());
    }
    if (wallpaperx::testing::reportCheckFailures()) return 1;
    if (wallpaperx::testing::reportHarnessFaults("in this test's harness, not in wpxctl")) return 1;
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
