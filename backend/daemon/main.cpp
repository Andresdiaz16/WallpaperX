// SPDX-License-Identifier: GPL-2.0-or-later

#include "crash_record.h"
#include "dbus_service.h"
#include "http_server.h"
#include "wallpaper_manager.h"

#include <sdbus-c++/sdbus-c++.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <optional>
#include <string_view>
#include <system_error>
#include <thread>

#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sysexits.h>
#include <unistd.h>

namespace {
// Budgets the whole stop -- signal to process gone -- so work running when the signal lands
// spends it too. Must exceed the config writer's 2 s deadline.
constexpr std::chrono::seconds kTeardownDeadline{3};

// Why the daemon is stopping. The first to arrive stands.
enum class StopReason { None, Signal, DeviceLost };

// Written only under main's LoopGate mutex, which pairs it with whether the event loop was
// entered. At namespace scope and lock-free because signal handlers read it, the SIGALRM one
// possibly after main has returned.
std::atomic<StopReason> g_stopReason{StopReason::None};
static_assert(std::atomic<StopReason>::is_always_lock_free);

// EX_TEMPFAIL: a later start can succeed, which is what the service manager's restart is for.
// Any nonzero status restarts it under Restart=on-failure; this one tells the loss apart.
constexpr int kExitDeviceLost = EX_TEMPFAIL;

// Wakes the signal thread for a stop decided inside the process. A number of its own that is
// never unblocked, so a late wake cannot land in forceExitOnSecondSignal as a user's second ask.
// Anyone can send it, so it means nothing without a reason the daemon recorded before sending.
constexpr int kWakeSignal = SIGUSR1;

// For a stop that did not finish in order. A lost device keeps its status: its teardown is the
// one most likely to block, on a driver that will never answer.
int failureStatus() {
    return g_stopReason.load() == StopReason::DeviceLost ? kExitDeviceLost : 1;
}

// Lets a second SIGINT/SIGTERM end a stop that is stuck. Async-signal-safe.
void forceExitOnSecondSignal(int) {
    static constexpr char kForced[] = "wallpaperx-daemon: asked again, exiting now\n";
    ssize_t ignored = ::write(STDERR_FILENO, kForced, sizeof(kForced) - 1);
    (void)ignored;
    std::_Exit(failureStatus());
}

// The teardown alarm. Any other stop dies of the SIGALRM itself, as a supervisor expects of an
// abandoned teardown.
void onTeardownDeadline(int sig) {
    if (g_stopReason.load() == StopReason::DeviceLost) std::_Exit(kExitDeviceLost);
    std::signal(sig, SIG_DFL);
    ::raise(sig);  // delivered as the handler returns
}

// First caller wins: re-arming per stage would hand each a fresh deadline, and the budget is
// for the whole stop. main() does not block SIGALRM.
void armTeardown(bool& armed) {
    if (armed) return;
    armed = true;
    ::alarm(static_cast<unsigned>(kTeardownDeadline.count()));
}

// For log lines on the path from a signal or a device loss to the stop: a reader that stopped
// draining loses the line rather than holding up the stop. Its own descriptor, because O_NONBLOCK
// on fd 1 or 2 would land on a description every other writer shares, stdio included.
class StopLog {
public:
    explicit StopLog(int fd) {
        struct stat st {};
        if (::fstat(fd, &st) != 0) return;
        if (S_ISSOCK(st.st_mode) || S_ISREG(st.st_mode)) {
            // A socket (journald's stream) is sent to with MSG_DONTWAIT; a file has no reader to
            // fall behind. Shared, not reopened: reopening a file would give it an offset of its own.
            m_fd = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
            return;
        }
        // A pipe or terminal: through /proc this is a new open file description, so the
        // O_NONBLOCK stays on it. If that fails (a pipe with no reader yet, no /proc), lines are dropped.
        char path[32];
        std::snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
        m_fd = ::open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
    }
    ~StopLog() {
        if (m_fd >= 0) ::close(m_fd);
    }
    StopLog(const StopLog&) = delete;
    StopLog& operator=(const StopLog&) = delete;

    // One write, so a pipe gets the line whole or not at all.
    [[gnu::format(printf, 2, 3)]] void print(const char* format, ...) const {
        if (m_fd < 0) return;
        char line[512];
        va_list args;
        va_start(args, format);
        const int length = std::vsnprintf(line, sizeof(line), format, args);
        va_end(args);
        if (length <= 0) return;
        const size_t size = std::min(static_cast<size_t>(length), sizeof(line) - 1);
        line[size - 1] = '\n';  // a truncated line still ends one
        if (::send(m_fd, line, size, MSG_DONTWAIT | MSG_NOSIGNAL) < 0 && errno == ENOTSOCK) {
            ssize_t ignored = ::write(m_fd, line, size);
            (void)ignored;
        }
    }

private:
    int m_fd = -1;  // -1: nothing to write to without blocking
};

constexpr char kUsage[] =
    "usage: wallpaperx-daemon [--config-port <port>]\n"
    "  --config-port <port>  serve the loopback config server on <port> instead of %d; 0 lets\n"
    "                        the kernel choose and the daemon reports what it bound. For tests:\n"
    "                        the desktop plugin only ever asks %d.\n";

struct Options {
    int configPort = wallpaperx::HttpServer::kDefaultPort;
};

// Strict: an option this daemon does not know is refused, never ignored, so a mistyped one cannot
// start a daemon that silently does something else.
std::optional<Options> parseArguments(int argc, char** argv, bool& help) {
    Options options;
    bool portGiven = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            help = true;
            return options;
        }
        if (arg != "--config-port") {
            std::fprintf(stderr, "wallpaperx-daemon: unknown argument '%s'\n", argv[i]);
            return std::nullopt;
        }
        if (portGiven || i + 1 >= argc) {
            std::fprintf(stderr, "wallpaperx-daemon: --config-port takes one port, given once\n");
            return std::nullopt;
        }
        const std::string_view value = argv[++i];
        int port = -1;
        const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), port);
        if (ec != std::errc() || end != value.data() + value.size() || port < 0 || port > 65535) {
            std::fprintf(stderr, "wallpaperx-daemon: --config-port needs a port from 0 to 65535, got '%s'\n",
                         argv[i]);
            return std::nullopt;
        }
        options.configPort = port;
        portGiven = true;
    }
    return options;
}
}  // namespace

int main(int argc, char** argv) {
    bool help = false;
    const std::optional<Options> options = parseArguments(argc, argv, help);
    if (!options || help) {
        std::fprintf(help ? stdout : stderr, kUsage, wallpaperx::HttpServer::kDefaultPort,
                     wallpaperx::HttpServer::kDefaultPort);
        return help ? 0 : 2;
    }
    const StopLog stopOut(STDOUT_FILENO);
    const StopLog stopErr(STDERR_FILENO);

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

    struct sigaction deadline {};
    deadline.sa_handler = onTeardownDeadline;
    sigemptyset(&deadline.sa_mask);
    if (sigaction(SIGALRM, &deadline, nullptr) != 0) {
        std::fprintf(stderr, "wallpaperx-daemon: fatal: could not install the teardown alarm\n");
        return 1;
    }

    // SIG_IGN, not blocked: a blocked SIGPIPE still returns EPIPE but stays pending, and
    // the sigwait thread below would eventually consume it as a shutdown.
    if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        std::fprintf(stderr, "wallpaperx-daemon: fatal: could not ignore SIGPIPE\n");
        return 1;
    }

    // Same reasoning: exceeding RLIMIT_FSIZE would otherwise kill the daemon mid-write.
    // Ignored, so the write fails with EFBIG and ConfigStore reports it.
    if (std::signal(SIGXFSZ, SIG_IGN) == SIG_ERR) {
        std::fprintf(stderr, "wallpaperx-daemon: fatal: could not ignore SIGXFSZ\n");
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
        config.stateDir = wallpaperx::defaultStateDir();
        if (config.stateDir.empty()) {
            std::fprintf(stderr, "wallpaperx-daemon: no usable XDG_STATE_HOME or HOME; "
                                 "a wallpaper that crashes the daemon will be applied again on restart\n");
        }

        auto connection = sdbus::createSessionBusConnection();

        // Reached only if teardown gets past the part that can block forever -- a wallpaper
        // or config file on a stalled mount, which no thread can be interrupted out of.
        struct Teardown {
            std::mutex mutex;
            std::condition_variable done;
            bool finished = false;
        } teardown;

        // Declared beside the objects whose destructors can block, so the watchdog is armed at
        // the top of the unwind rather than after them.
        bool teardownArmed = false;
        struct ArmTeardown {
            bool& armed;
            ~ArmTeardown() { armTeardown(armed); }
        };

        // Entering and leaving the event loop is the one pair sdbus-c++ supports being used
        // from two threads; every other IConnection method is thread-aware but not thread-safe.
        // This keeps the signal thread out of the connection while main is still registering
        // the object and claiming the name, and loses no signal arriving in that window:
        // whichever side arrives first, the loop is not entered.
        struct LoopGate {
            std::mutex mutex;
            bool entered = false;  // g_stopReason is the other half
        } loopGate;

        // sigwait on its own thread, not a handler: leaveEventLoop() takes locks and is not
        // async-signal-safe. A guard rather than a bare std::thread because enterEventLoop()
        // THROWS when the connection drops, and a joinable thread destroyed mid-unwind is
        // std::terminate. Started before anything touching the filesystem: signals are blocked
        // at the top of main, and work done before this thread cannot be stopped.
        struct ShutdownWatch {
            std::thread thread;
            Teardown& teardown;
            bool& teardownArmed;
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
            }
        } shutdownWatch{std::thread([&connection, &watchSignals, &shutdownSignals, &teardown, &loopGate,
                                     &stopOut, &stopErr] {
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
                    // onDeviceLost records a reason before its wake, and ~ShutdownWatch sets
                    // `finished` (checked above) before its own, so a wake with neither came from
                    // outside. A real wake racing it is not lost: it either merged into this one,
                    // its reason or `finished` already recorded, or is still pending.
                    bool reasonRecorded = false;
                    {
                        std::lock_guard<std::mutex> lock(loopGate.mutex);
                        if (received != kWakeSignal && g_stopReason.load() == StopReason::None) {
                            g_stopReason.store(StopReason::Signal);
                        }
                        reasonRecorded = g_stopReason.load() != StopReason::None;
                        inLoop = loopGate.entered;
                    }
                    if (reasonRecorded) break;
                    stopErr.print("wallpaperx-daemon: ignoring %s, which nothing in the daemon sent\n",
                                  strsignal(received));
                }
                // An outside signal is reported even when a lost device got its reason in first;
                // the device's own wake needs no line, as the loss announced itself.
                const bool outside = received != kWakeSignal;
                if (outside) {
                    stopOut.print("wallpaperx-daemon: %s received, shutting down\n", strsignal(received));
                }
                // Caught on its own: a loop that will not leave still has the deadline below
                // bounding it, which is better than abandoning the stop immediately.
                try {
                    if (inLoop) connection->leaveEventLoop();
                } catch (const std::exception& error) {
                    stopErr.print("wallpaperx-daemon: could not stop the bus loop: %s\n", error.what());
                } catch (...) {
                    stopErr.print("wallpaperx-daemon: could not stop the bus loop\n");
                }

                // Unblocked here, not earlier: until now these have to reach the sigwait above.
                // Only when one of them woke this thread, so the next really is a second ask. A
                // stop the daemon began itself is bounded by the deadline alone.
                if (outside) {
                    struct sigaction forced {};
                    forced.sa_handler = forceExitOnSecondSignal;
                    sigemptyset(&forced.sa_mask);
                    sigaction(SIGINT, &forced, nullptr);
                    sigaction(SIGTERM, &forced, nullptr);
                    sigaction(SIGHUP, &forced, nullptr);
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
                // write(2) on the descriptor, not printf + fflush(nullptr): flushing every
                // stream takes every stream's lock, and the thread this is escaping from may
                // hold one. A last resort that can block on what it is escaping is not one.
                static constexpr char kBlocked[] =
                    "wallpaperx-daemon: shutdown blocked, exiting now\n";
                ssize_t ignored = ::write(STDERR_FILENO, kBlocked, sizeof(kBlocked) - 1);
                (void)ignored;
            } catch (...) {
                // The deadline went with the stack it was on, so nothing bounds this stop now.
                static constexpr char kLost[] =
                    "wallpaperx-daemon: shutdown watch failed, exiting now\n";
                ssize_t ignored = ::write(STDERR_FILENO, kLost, sizeof(kLost) - 1);
                (void)ignored;
            }
            // Nonzero: teardown was abandoned, and a supervisor reading only the status
            // should not be told this was a clean stop.
            std::_Exit(failureStatus());
        }), teardown, teardownArmed};

        // Constructed after the watcher: this reads the config file, and a signal arriving
        // during that read must be able to stop the daemon.
        wallpaperx::WallpaperManager manager(config);
        // On the render worker's thread. Hands the stop to the signal thread, which can already
        // carry one out from any point in startup and bounds it with the deadline.
        const auto onDeviceLost = [&loopGate, &shutdownWatch, &stopErr] {
            {
                std::lock_guard<std::mutex> lock(loopGate.mutex);
                if (g_stopReason.load() != StopReason::None) return;
                g_stopReason.store(StopReason::DeviceLost);
            }
            // Woken before the line is written, so no write can stand between a loss and its stop.
            pthread_kill(shutdownWatch.thread.native_handle(), kWakeSignal);
            stopErr.print("wallpaperx-daemon: the GPU device was lost; exiting with status %d so the "
                          "service manager restarts the daemon\n", kExitDeviceLost);
        };
        wallpaperx::DbusService service(*connection, manager, onDeviceLost);
        // Destroyed before ~DbusService joins the render worker and ~WallpaperManager tears the
        // device down, which is what the deadline is for. This is the one that covers a throw
        // out of requestName() below, where the server further down does not exist yet.
        ArmTeardown armBeforeService{teardownArmed};

        // Before the name is claimed: this is what fills the monitor map.
        if (g_stopReason.load() == StopReason::None) service.restoreSaved();

        // Claimed last, so the name on the bus means the daemon can answer: object registered,
        // saved wallpapers back. Throws if another instance owns it, which is why nothing above
        // may write to the config directory. Skipped after a stop, or a daemon on its way out
        // would take the well-known name for a moment and drop it again.
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

        // Declared after the service so it is destroyed first: its handlers call
        // manager.snapshot(), so HTTP has to stop before anything it reads is torn down.
        wallpaperx::HttpServer httpServer(manager, options->configPort);
        // Destroyed first of everything here, so the ordinary stop is bounded from its own top.
        ArmTeardown armBeforeHttp{teardownArmed};

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

        // Destroyed in reverse declaration order: HTTP, then the service (stopping the
        // render worker), then the manager (GPU teardown + leak report), the shutdown
        // watcher, and the connection it holds last.
    } catch (const std::exception& e) {
        std::fprintf(stderr, "wallpaperx-daemon: fatal: %s\n", e.what());
        // The same hazard as the clean path below, and more likely here: a start that
        // failed is a start whose config read may still be outstanding.
        if (wallpaperx::configLoadAbandoned()) {
            std::fflush(nullptr);
            std::_Exit(failureStatus());
        }
        return failureStatus();
    }

    // Final by now: a stop is recorded before the loop can be left, and the first one stands.
    const int exitStatus = g_stopReason.load() == StopReason::DeviceLost ? kExitDeviceLost : 0;
    if (exitStatus == 0) {
        std::printf("wallpaperx-daemon: stopped cleanly\n");
    } else {
        std::printf("wallpaperx-daemon: stopped after losing the GPU device, exit status %d\n", exitStatus);
    }

    // An abandoned config read left a thread blocked in the kernel, and waking during static
    // destruction would run library code already torn down. Everything with observable effects
    // is destroyed by here.
    if (wallpaperx::configLoadAbandoned()) {
        std::fflush(nullptr);
        std::_Exit(exitStatus);
    }
    return exitStatus;
}
