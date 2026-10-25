// SPDX-License-Identifier: GPL-2.0-or-later

#include "crash_record.h"
#include "dbus_service.h"
#include "ffmpeg_log.h"
#include "http_server.h"
#include "log_pump.h"
#include "resume_record.h"
#include "wallpaper_manager.h"
#include "vulkan/device.h"

#include <sdbus-c++/sdbus-c++.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <optional>
#include <set>
#include <string_view>
#include <system_error>
#include <thread>

#include <pthread.h>
#include <sysexits.h>
#include <unistd.h>

namespace {
// Budgets the whole stop -- signal to process gone -- so work running when the signal lands
// spends it too. Must exceed the config writer's 2 s deadline.
constexpr std::chrono::seconds kTeardownDeadline{3};

// Why the daemon is stopping. The first to arrive stands, except that an outside stop turns a
// planned restart into Signal: the restart is only the daemon's preference. Failed: startup or the
// bus loop threw. RestartCommitted: a planned restart an exit already under way has made final.
enum class StopReason { None, Signal, DeviceLost, PlannedRestart, RestartCommitted, Failed };

// Written under main's LoopGate mutex, which pairs it with whether the event loop was entered --
// except by cancelPlannedRestart() and commitToExit(), whose moves out of a planned restart leave
// that pairing alone. At namespace scope and lock-free because signal handlers read it, the SIGALRM
// one possibly after main has returned.
std::atomic<StopReason> g_stopReason{StopReason::None};
static_assert(std::atomic<StopReason>::is_always_lock_free);

// EX_TEMPFAIL: a later start can succeed, which is what the service manager's restart is for.
// Any nonzero status restarts it under Restart=on-failure; this one tells the loss apart.
constexpr int kExitDeviceLost = EX_TEMPFAIL;

// The restart the daemon plans for itself (WallpaperManager::kRestartAfter). Nonzero so Restart=on-failure
// brings it back, and outside both the sysexits range and systemd's own codes, so the journal names it
// as nothing else.
constexpr int kExitPlannedRestart = 79;

// A stop the user asked for that did not end cleanly: its teardown hit the deadline, a second ask
// cut it short, or it threw. Not 1, which a crash shares, so the unit can refuse to restart it.
constexpr int kExitStopCutShort = 80;

// The status of a stop the daemon chose for itself, kept whatever cuts that stop short; nothing for any other.
std::optional<int> chosenStatus(StopReason reason = g_stopReason.load()) {
    switch (reason) {
        case StopReason::DeviceLost: return kExitDeviceLost;
        case StopReason::PlannedRestart:
        case StopReason::RestartCommitted: return kExitPlannedRestart;
        default: return std::nullopt;
    }
}

// The status of a stop that did not get as far as settling one; nothing for one that failed, which
// is restarted as a crash is.
std::optional<int> cutShortStatus() {
    const StopReason reason = g_stopReason.load();
    if (reason == StopReason::Signal) return kExitStopCutShort;
    return chosenStatus(reason);
}

// The resume record's path, spelled out for cancelPlannedRestart() to remove from a handler. Set
// before the signal thread starts; empty when there is no state directory.
char g_resumeRecord[PATH_MAX] = {};

// Turns a planned restart into an outside stop: it exits as any other does, and is not restarted.
// Its record goes too, so no later start resumes from where this one stood. False when there was
// no planned restart left to call off. Async-signal-safe.
bool cancelPlannedRestart() noexcept {
    StopReason planned = StopReason::PlannedRestart;
    if (!g_stopReason.compare_exchange_strong(planned, StopReason::Signal)) return false;
    if (g_resumeRecord[0] != '\0') ::unlink(g_resumeRecord);
    return true;
}

// Wakes the signal thread for a stop decided inside the process. A number of its own that is
// never unblocked, so a late wake cannot land in forceExitOnSecondSignal as a user's second ask.
// Anyone can send it, so it means nothing without a reason the daemon recorded before sending.
constexpr int kWakeSignal = SIGUSR1;

// The status main settled on once nothing observable is left to tear down; -1 until then.
std::atomic<int> g_finalStatus{-1};
static_assert(std::atomic<int>::is_always_lock_free);

int settle(int status) {
    g_finalStatus.store(status);
    return status;
}

// For a stop cut short. One that got as far as its final lines keeps the status it settled on. A
// chosen stop keeps its status: a lost device's teardown is the one most likely to block, on a
// driver that will never answer, and either must still be restarted. A user's must not be.
// A settled restart the user then called off ends as onStopAfterSignalThread ends it.
int failureStatus() {
    if (const int settled = g_finalStatus.load(); settled >= 0) {
        return settled == kExitPlannedRestart && g_stopReason.load() == StopReason::Signal ? 0 : settled;
    }
    return cutShortStatus().value_or(1);
}

// For every exit that skips the rest of teardown: what is queued goes first, when it can in time;
// `line` after it, bypassing the queue. Async-signal-safe.
void flushBeforeExit(std::string_view line = {}) noexcept {
    wallpaperx::logpump::flush(wallpaperx::logpump::kExitFlush);
    if (!line.empty()) wallpaperx::logpump::writeErrNow(line.data(), line.size());
}

// A thread this process gave up on -- a config read blocked in the kernel, a video decode inside
// the driver -- would run library code already torn down if it woke during static destruction, so
// the process then ends without running static destructors.
bool threadLeftRunning() {
    return wallpaperx::configLoadAbandoned() || wallpaperx::Device::threadLeftRunning();
}

[[noreturn]] void exitNow(int status, std::string_view line = {}) noexcept {
    flushBeforeExit(line);
    std::_Exit(status);
}

// For an exit that cuts teardown short, after its flush -- a stop handled during that can still call
// the restart off -- and right before it reads failureStatus(). No stop is handled on this thread
// from here, and one on another loses the compare-exchange to cancelPlannedRestart() or wins it,
// so a restart is never called off and still taken. A user's stop leaves no resume record, even
// one the render worker was still writing when the stop was cut short. Async-signal-safe.
void commitToExit() noexcept {
    sigset_t stops;
    sigemptyset(&stops);
    for (const int sig : {SIGINT, SIGTERM, SIGHUP}) sigaddset(&stops, sig);
    pthread_sigmask(SIG_BLOCK, &stops, nullptr);
    StopReason planned = StopReason::PlannedRestart;
    g_stopReason.compare_exchange_strong(planned, StopReason::RestartCommitted);
    if (g_stopReason.load() == StopReason::Signal && g_resumeRecord[0] != '\0') ::unlink(g_resumeRecord);
}

[[noreturn]] void exitCutShort(std::string_view line) noexcept {
    flushBeforeExit(line);
    commitToExit();
    std::_Exit(failureStatus());
}

// Lets a second SIGINT/SIGTERM end a stop that is stuck.
void forceExitOnSecondSignal(int) {
    exitCutShort("wallpaperx-daemon: asked again, exiting now\n");
}

void handleShutdownSignals(void (*handler)(int)) noexcept {
    struct sigaction action {};
    action.sa_handler = handler;
    sigemptyset(&action.sa_mask);
    for (const int sig : {SIGINT, SIGTERM, SIGHUP}) sigaddset(&action.sa_mask, sig);
    for (const int sig : {SIGINT, SIGTERM, SIGHUP}) sigaction(sig, &action, nullptr);
}

// The first outside stop during a planned restart, which the signal thread is no longer waiting
// for. It overrides the restart; any after it is a second ask.
void onStopDuringPlannedRestart(int) {
    if (!cancelPlannedRestart()) return;  // an exit already committed to the restart
    wallpaperx::logpump::writeErrNow("wallpaperx-daemon: asked to stop, so the planned restart is called off\n");
    handleShutdownSignals(forceExitOnSecondSignal);
}

// A stop during a planned restart, once the signal thread is joined: what is left of teardown has
// nothing observable to finish, so it ends here rather than let main settle the restart's status.
void onStopAfterSignalThread(int) {
    cancelPlannedRestart();
    if (g_stopReason.load() != StopReason::Signal) return;
    exitNow(0, "wallpaperx-daemon: asked to stop, so the planned restart is called off\n");
}

// The teardown alarm. A failed stop dies of the SIGALRM itself, as a supervisor expects of an
// abandoned teardown.
void onTeardownDeadline(int sig) {
    constexpr std::string_view kLine = "wallpaperx-daemon: teardown deadline passed, exiting now\n";
    flushBeforeExit(g_finalStatus.load() >= 0 ? std::string_view{} : kLine);
    commitToExit();
    if (g_finalStatus.load() >= 0 || cutShortStatus()) std::_Exit(failureStatus());
    std::signal(sig, SIG_DFL);
    ::raise(sig);  // delivered as the handler returns
}

// First caller wins: re-arming per stage would hand each a fresh deadline, and the budget is
// for the whole stop. main() does not block SIGALRM, and ignores it until here, so one sent from
// outside cannot end a daemon that is not stopping.
void armTeardown(bool& armed) {
    if (armed) return;
    armed = true;
    struct sigaction deadline {};
    deadline.sa_handler = onTeardownDeadline;
    sigemptyset(&deadline.sa_mask);
    sigaction(SIGALRM, &deadline, nullptr);
    ::alarm(static_cast<unsigned>(kTeardownDeadline.count()));
}

constexpr char kUsage[] =
    "usage: wallpaperx-daemon [--config-port <port>] [--restart-after <seconds>] [--restart-quiet <seconds>]\n"
    "  --config-port <port>       serve the loopback config server on <port> instead of %d; 0 lets\n"
    "                             the kernel choose and the daemon reports what it bound. For tests:\n"
    "                             the desktop plugin only ever asks %d.\n"
    "  --restart-after <seconds>  restart after this long rather than %lld, once no screen has been\n"
    "                             seen for --restart-quiet (%lld unless given); each 1 to %lld.\n";

struct Options {
    int configPort = wallpaperx::HttpServer::kDefaultPort;
    std::chrono::seconds restartAfter = wallpaperx::WallpaperManager::kRestartAfter;
    std::chrono::seconds restartQuiet = wallpaperx::WallpaperManager::kRestartQuiet;
};

constexpr long long kMaxTimingSeconds = std::chrono::seconds(wallpaperx::WallpaperManager::kMaxTiming).count();

// Strict: an option this daemon does not know is refused, never ignored, so a mistyped one cannot
// start a daemon that silently does something else.
std::optional<Options> parseArguments(int argc, char** argv, bool& help) {
    Options options;
    std::set<std::string_view> given;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            help = true;
            return options;
        }
        long long low = 0;
        long long high = 0;
        const char* unit = "port";
        if (arg == "--config-port") {
            high = 65535;
        } else if (arg == "--restart-after" || arg == "--restart-quiet") {
            low = 1;
            high = kMaxTimingSeconds;
            unit = "number of seconds";
        } else {
            std::fprintf(stderr, "wallpaperx-daemon: unknown argument '%s'\n", argv[i]);
            return std::nullopt;
        }
        if (!given.insert(arg).second || i + 1 >= argc) {
            std::fprintf(stderr, "wallpaperx-daemon: %s takes one %s, given once\n", argv[i], unit);
            return std::nullopt;
        }
        const std::string_view value = argv[++i];
        long long number = -1;
        const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), number);
        if (ec != std::errc() || end != value.data() + value.size() || number < low || number > high) {
            std::fprintf(stderr, "wallpaperx-daemon: %s needs a %s from %lld to %lld, got '%s'\n", argv[i - 1], unit,
                         low, high, argv[i]);
            return std::nullopt;
        }
        if (arg == "--config-port") {
            options.configPort = static_cast<int>(number);
        } else if (arg == "--restart-after") {
            options.restartAfter = std::chrono::seconds(number);
        } else {
            options.restartQuiet = std::chrono::seconds(number);
        }
    }
    return options;
}
// For an exit that skips main's return.
void flushLogs() {
    std::fflush(nullptr);
    wallpaperx::logpump::flush(wallpaperx::logpump::kExitFlush);
}

int serve(int argc, char** argv) {
    bool help = false;
    const std::optional<Options> options = parseArguments(argc, argv, help);
    if (!options || help) {
        std::fprintf(help ? stdout : stderr, kUsage, wallpaperx::HttpServer::kDefaultPort,
                     wallpaperx::HttpServer::kDefaultPort,
                     static_cast<long long>(std::chrono::seconds(wallpaperx::WallpaperManager::kRestartAfter).count()),
                     static_cast<long long>(wallpaperx::WallpaperManager::kRestartQuiet.count()), kMaxTimingSeconds);
        return help ? 0 : 2;
    }

    // Blocked before any thread exists, since a thread inherits the mask of its creator
    // and both the render worker and the sd-bus loop start below. The sigwait thread is
    // then the only place these are handled.
    sigset_t shutdownSignals;
    sigemptyset(&shutdownSignals);
    sigaddset(&shutdownSignals, SIGINT);
    sigaddset(&shutdownSignals, SIGTERM);
    sigaddset(&shutdownSignals, SIGHUP);  // terminal close / logout
    sigset_t watchSignals = shutdownSignals;
    sigaddset(&watchSignals, kWakeSignal);
    if (pthread_sigmask(SIG_BLOCK, &watchSignals, nullptr) != 0) {
        std::fprintf(stderr, "wallpaperx-daemon: fatal: could not block shutdown signals\n");
        return 1;
    }

    // Until a stop arms the deadline; see armTeardown.
    if (std::signal(SIGALRM, SIG_IGN) == SIG_ERR) {
        std::fprintf(stderr, "wallpaperx-daemon: fatal: could not ignore SIGALRM\n");
        return 1;
    }

    // Before any thread: a wallpaper that kills the render thread is recorded so the restart
    // that follows does not apply it again.
    wallpaperx::installFatalSignalHandlers();

    try {
        wallpaperx::WallpaperManager::Config config;
        // Compared against "1" exactly: a truthiness check would make
        // WALLPAPERX_VALIDATION=0 mean "on".
        const char* validation = std::getenv("WALLPAPERX_VALIDATION");
        config.enableValidation = validation != nullptr && std::strcmp(validation, "1") == 0;

        // Opting in here, not in Config's default: a test that says nothing about config
        // must not read the developer's real wallpapers back in or overwrite them.
        config.configFile = wallpaperx::defaultConfigPath();
        if (config.configFile.empty()) {
            std::fprintf(stderr, "wallpaperx-daemon: no usable XDG_CONFIG_HOME or HOME; "
                                 "wallpapers will not survive a restart\n");
        }
        config.restartAfter = options->restartAfter;
        config.restartQuiet = options->restartQuiet;
        config.stateDir = wallpaperx::defaultStateDir();
        if (config.stateDir.empty()) {
            std::fprintf(stderr, "wallpaperx-daemon: no usable XDG_STATE_HOME or HOME; "
                                 "a wallpaper that crashes the daemon will be applied again on restart\n");
        }
        // One too long to fit is one the record could not have been written at either.
        if (const std::string record = wallpaperx::resumeRecordPath(config.stateDir).string();
            record.size() < sizeof(g_resumeRecord)) {
            std::memcpy(g_resumeRecord, record.c_str(), record.size() + 1);
        }

        auto connection = sdbus::createSessionBusConnection();

        // Reached only if teardown gets past the part that can block forever -- a wallpaper
        // or config file on a stalled mount, which no thread can be interrupted out of.
        struct Teardown {
            std::mutex mutex;
            std::condition_variable done;
            bool finished = false;
        } teardown;

        // Entering and leaving the event loop is the one pair sdbus-c++ supports being used
        // from two threads; every other IConnection method is thread-aware but not thread-safe.
        // This keeps the signal thread out of the connection while main is still registering
        // the object and claiming the name, and loses no signal arriving in that window:
        // whichever side arrives first, the loop is not entered.
        struct LoopGate {
            std::mutex mutex;
            bool entered = false;  // g_stopReason is the other half
        } loopGate;

        // Declared beside the objects whose destructors can block, so the watchdog is armed at
        // the top of the unwind rather than after them. A stop with no reason by then is a throw,
        // and recording it keeps a device loss that teardown then sees from becoming the reason.
        bool teardownArmed = false;
        struct ArmTeardown {
            bool& armed;
            LoopGate& gate;
            ~ArmTeardown() {
                {
                    std::lock_guard<std::mutex> lock(gate.mutex);
                    if (g_stopReason.load() == StopReason::None) g_stopReason.store(StopReason::Failed);
                }
                armTeardown(armed);
            }
        };

        // sigwait on its own thread, not a handler: leaveEventLoop() takes locks and is not
        // async-signal-safe. A guard rather than a bare std::thread because enterEventLoop()
        // THROWS when the connection drops, and a joinable thread destroyed mid-unwind is
        // std::terminate. Started before anything touching the filesystem: signals are blocked
        // at the top of main, and work done before this thread cannot be stopped.
        struct ShutdownWatch {
            std::thread thread;
            Teardown& teardown;
            bool& teardownArmed;
            const sigset_t& stopSignals;
            ~ShutdownWatch() {
                if (!thread.joinable()) return;
                // Covers the tail -- the bus connection, then static destruction. A stop that
                // got this far already armed it; this is the case where nothing else did.
                armTeardown(teardownArmed);
                {
                    std::lock_guard<std::mutex> lock(teardown.mutex);
                    teardown.finished = true;
                }
                teardown.done.notify_all();
                // Wakes a watcher still in sigwait; one already carrying out a stop leaves on
                // `finished`, and this wake stays pending, blocked, for good.
                pthread_kill(thread.native_handle(), kWakeSignal);
                thread.join();
                // Every thread left blocks the stop signals, so a stop from here to the exit would
                // otherwise stay pending and the restart go ahead. One already pending lands now.
                // Only the restart: no other stop's outcome can change by now.
                if (g_stopReason.load() == StopReason::PlannedRestart) {
                    handleShutdownSignals(onStopAfterSignalThread);
                    pthread_sigmask(SIG_UNBLOCK, &stopSignals, nullptr);
                }
            }
        } shutdownWatch{std::thread([&connection, &watchSignals, &shutdownSignals, &teardown, &loopGate] {
            // Guarded because this is a bare thread, where an escaping exception is
            // std::terminate -- a core dump in place of the clean stop this thread exists to
            // give, and main's own catch cannot intercept it.
            try {
                int received = 0;
                bool inLoop = false;
                for (;;) {
                    if (sigwait(&watchSignals, &received) != 0) return;
                    {
                        // Skips leaveEventLoop() too: on the throw path the connection is what failed.
                        std::lock_guard<std::mutex> lock(teardown.mutex);
                        if (teardown.finished) return;
                    }
                    // onDeviceLost and onPlannedRestart record a reason before their wake, and ~ShutdownWatch sets
                    // `finished` (checked above) before its own, so a wake with neither came from
                    // outside. A real wake racing it is not lost: it either merged into this one,
                    // its reason or `finished` already recorded, or is still pending.
                    bool reasonRecorded = false;
                    {
                        std::lock_guard<std::mutex> lock(loopGate.mutex);
                        if (received != kWakeSignal) {
                            if (g_stopReason.load() == StopReason::None) g_stopReason.store(StopReason::Signal);
                            cancelPlannedRestart();
                        }
                        reasonRecorded = g_stopReason.load() != StopReason::None;
                        inLoop = loopGate.entered;
                    }
                    if (reasonRecorded) break;
                    std::fprintf(stderr, "wallpaperx-daemon: ignoring %s, which nothing in the daemon sent\n",
                                 strsignal(received));
                }
                // An outside signal is reported even when a chosen stop got its reason in first; the
                // daemon's own wake needs no line, as the loss or the restart announced itself.
                const bool outside = received != kWakeSignal;
                if (outside) std::printf("wallpaperx-daemon: %s received, shutting down\n", strsignal(received));
                // Caught on its own: a loop that will not leave still has the deadline below
                // bounding it, which is better than abandoning the stop immediately.
                try {
                    if (inLoop) connection->leaveEventLoop();
                } catch (const std::exception& error) {
                    std::fprintf(stderr, "wallpaperx-daemon: could not stop the bus loop: %s\n", error.what());
                } catch (...) {
                    std::fprintf(stderr, "wallpaperx-daemon: could not stop the bus loop\n");
                }

                // Unblocked here, not earlier: until now these have to reach the sigwait above, and
                // only on this thread until it is joined (~ShutdownWatch takes over from there). After
                // an outside one the next really is a second ask. A planned restart still yields to the
                // first; a lost device's stop is bounded by the deadline alone.
                const bool planned = g_stopReason.load() == StopReason::PlannedRestart;
                if (outside || planned) {
                    handleShutdownSignals(outside ? forceExitOnSecondSignal : onStopDuringPlannedRestart);
                    pthread_sigmask(SIG_UNBLOCK, &shutdownSignals, nullptr);
                }

                // The deadline belongs on this thread because it is the only one that cannot be
                // stuck: it waits for signals and nothing else. Any other would risk being
                // blocked in an uninterruptible filesystem call while holding the process open.
                std::unique_lock<std::mutex> lock(teardown.mutex);
                if (teardown.done.wait_for(lock, kTeardownDeadline,
                                           [&teardown] { return teardown.finished; })) {
                    return;
                }
                // Not stdio, whose locks the thread this is escaping from may hold.
                exitCutShort("wallpaperx-daemon: shutdown blocked, exiting now\n");
            } catch (...) {
                // The deadline went with the stack it was on, so nothing bounds this stop now.
                exitCutShort("wallpaperx-daemon: shutdown watch failed, exiting now\n");
            }
        }), teardown, teardownArmed, shutdownSignals};

        // Hands the stop to the signal thread, which can already carry one out from any point in
        // startup and bounds it with the deadline. Called by the Device, on the thread that saw
        // the loss and before the call that saw it returns. A stop already under way keeps its reason.
        config.onDeviceLost = [&loopGate, &shutdownWatch] {
            // Whatever the reason: the job that saw the loss is still running, and a deadline or a
            // driver crash may end it before its record is withdrawn.
            wallpaperx::stopBlamingWallpapers();
            {
                std::lock_guard<std::mutex> lock(loopGate.mutex);
                if (g_stopReason.load() != StopReason::None) return;
                g_stopReason.store(StopReason::DeviceLost);
            }
            pthread_kill(shutdownWatch.thread.native_handle(), kWakeSignal);
            std::fprintf(stderr, "wallpaperx-daemon: the GPU device was lost; exiting with status %d so the "
                                 "service manager restarts the daemon\n", kExitDeviceLost);
        };
        // The same hand-over, for the restart the manager plans (WallpaperManager::planRestart). Called on
        // the render worker's thread once the record of where each video stands is on disk. Again after a
        // throw, so everything past the claim is safe to repeat. Another stop already under way keeps
        // the record unless it is the user's, the one stop the service manager does not restart.
        config.onPlannedRestart = [&loopGate, &shutdownWatch] {
            {
                std::lock_guard<std::mutex> lock(loopGate.mutex);
                const StopReason reason = g_stopReason.load();
                if (reason != StopReason::None && reason != StopReason::PlannedRestart) {
                    return reason != StopReason::Signal;
                }
                g_stopReason.store(StopReason::PlannedRestart);
            }
            // Before the wake, so it comes ahead of what the stop it starts logs.
            std::printf("wallpaperx-daemon: planned restart: no screen is in view, so exiting with status %d for "
                        "the service manager to start the daemon again\n", kExitPlannedRestart);
            std::fflush(stdout);
            // Whatever is applied from here on is not what ends this process, so a stop the deadline
            // cuts short refuses nothing on the next start.
            wallpaperx::stopBlamingWallpapers();
            pthread_kill(shutdownWatch.thread.native_handle(), kWakeSignal);
            return true;
        };
        // Constructed after the watcher: this reads the config file, and a signal arriving
        // during that read must be able to stop the daemon.
        wallpaperx::WallpaperManager manager(config);
        wallpaperx::DbusService service(*connection, manager);
        // Destroyed before ~DbusService joins the render worker and ~WallpaperManager tears the
        // device down, which is what the deadline is for. This is the one that covers a throw
        // out of the config server's constructor below.
        ArmTeardown armBeforeService{teardownArmed, loopGate};

        // Before the name is claimed: this is what fills the monitor map.
        if (g_stopReason.load() == StopReason::None) service.restoreSaved();

        // Declared after the service so it is destroyed first: its handlers call
        // manager.snapshot(), so HTTP has to stop before anything it reads is torn down. Before
        // the name, because the name's arrival is what tells a consumer to read /config. A second
        // instance finds the port taken and says so before it fails to claim the name.
        wallpaperx::HttpServer httpServer(manager, options->configPort);
        // Destroyed first of everything here, so the ordinary stop is bounded from its own top.
        ArmTeardown armBeforeHttp{teardownArmed, loopGate};

        // Claimed last, so the name on the bus means the daemon can answer: object registered,
        // saved wallpapers back, /config served. Throws if another instance owns it, which is why
        // nothing above may write to the config directory. Skipped after a stop, or a daemon on
        // its way out would take the well-known name for a moment and drop it again.
        bool claimed = false;
        if (g_stopReason.load() == StopReason::None) {
            connection->requestName(sdbus::ServiceName{wallpaperx::DbusService::kBusName});
            claimed = true;
            // Only now: this moves the user's config aside, and until the name is held this
            // process does not know it is the daemon.
            manager.preserveUnreadableConfig();
            // After the copy aside, which preserves the bytes this rewrite replaces.
            manager.writeRepairedConfig();
            // Only now, for the same reason: it replaces the record the running daemon keeps.
            manager.recordStartingVideos();
            // Read by restoreSaved() already; a start after this run must not resume from it.
            manager.forgetResumeRecord();
            service.renderRestored();
        }

        if (!claimed) {
            std::printf("wallpaperx-daemon: stopped before claiming %s\n",
                        wallpaperx::DbusService::kBusName);
        } else {
            std::printf("wallpaperx-daemon: serving %s at %s\n", wallpaperx::DbusService::kBusName,
                        wallpaperx::DbusService::kObjectPath);
        }
        std::fflush(stdout);

        // The health endpoint is otherwise the only channel saying so, and a user whose
        // wallpapers stopped being remembered has no reason to look there.
        if (const auto snap = manager.snapshot();
            snap.health.persistence.status == wallpaperx::HealthStatus::Fail) {
            std::fprintf(stderr, "wallpaperx-daemon: wallpapers may not survive a restart: %s\n",
                         snap.health.persistence.detail.c_str());
        }

        // Published under the gate, so a signal arriving before this point stops the loop being
        // entered at all rather than calling into the connection while the object is still
        // being registered. From here on the signal thread does call leaveEventLoop(), which
        // sdbus-c++ supports across threads -- including in the instants before the call below
        // starts, where it appears to latch the leave, undocumented.
        bool enter = false;
        {
            std::lock_guard<std::mutex> lock(loopGate.mutex);
            enter = g_stopReason.load() == StopReason::None;
            loopGate.entered = enter;
        }
        if (enter) {
            connection->enterEventLoop();  // returns once leaveEventLoop() is called
        }
        // Before any teardown the deadline could cut short: this stop is orderly whatever follows.
        // A lost device included -- the loss is the GPU's, not any video's.
        manager.endPlayingRecord();
        manager.flushConfig();

        // Destroyed in reverse declaration order: HTTP, then the service (stopping the
        // render worker), then the manager (GPU teardown + leak report), the shutdown
        // watcher, and the connection it holds last.
    } catch (const std::exception& e) {
        // The try scope is gone, so this is as settled as the clean path below.
        const int status = settle(failureStatus());
        std::fprintf(stderr, "wallpaperx-daemon: fatal: %s\n", e.what());
        // The same hazard as the clean path below, and more likely here: a start that
        // failed is a start whose config read may still be outstanding.
        if (threadLeftRunning()) {
            flushLogs();
            std::_Exit(status);
        }
        return status;
    }

    // Final by now: a stop is recorded before the loop can be left, and the signal thread has been
    // joined. A stop that calls off the restart from here on exits on its own.
    const int exitStatus = settle(chosenStatus().value_or(0));
    if (exitStatus == 0) {
        std::printf("wallpaperx-daemon: stopped cleanly\n");
    } else if (exitStatus == kExitPlannedRestart) {
        std::printf("wallpaperx-daemon: stopped for a planned restart, exit status %d\n", exitStatus);
    } else {
        std::printf("wallpaperx-daemon: stopped after losing the GPU device, exit status %d\n", exitStatus);
    }

    // Everything with observable effects is destroyed by here.
    if (threadLeftRunning()) {
        flushLogs();
        std::_Exit(exitStatus);
    }
    return exitStatus;
}
}  // namespace

int main(int argc, char** argv) {
    // Before the log writer exists, the writer being one of the threads this protects.
    const auto fatal = [](std::string_view line) {
        ssize_t ignored = ::write(STDERR_FILENO, line.data(), line.size());
        (void)ignored;
        return 1;
    };
    // SIG_IGN, not blocked: a blocked SIGPIPE still returns EPIPE but stays pending, and the
    // sigwait thread would eventually consume it as a shutdown.
    if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR) return fatal("wallpaperx-daemon: fatal: could not ignore SIGPIPE\n");
    // Same reasoning: exceeding RLIMIT_FSIZE would otherwise kill the daemon mid-write. Ignored, so
    // the write fails with EFBIG and ConfigStore reports it.
    if (std::signal(SIGXFSZ, SIG_IGN) == SIG_ERR) return fatal("wallpaperx-daemon: fatal: could not ignore SIGXFSZ\n");

    // First after those, so every line from here on -- this file's, the libraries' -- goes through it.
    if (!wallpaperx::logpump::install()) return fatal("wallpaperx-daemon: fatal: could not start the log writer\n");
    wallpaperx::ffmpeg_log::install();
    const int status = serve(argc, argv);
    // What runs after main -- static destructors, the Vulkan loader, a sanitizer's end-of-run
    // report -- then writes to the destinations directly.
    std::fflush(nullptr);
    wallpaperx::logpump::release();
    return status;
}
