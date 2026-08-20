// SPDX-License-Identifier: GPL-2.0-or-later

#include "dbus_service.h"
#include "http_server.h"
#include "wallpaper_manager.h"

#include <sdbus-c++/sdbus-c++.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <thread>

#include <pthread.h>
#include <unistd.h>

namespace {
// Budgets the whole stop -- signal to process gone, not just teardown -- so work still
// running when the signal lands spends it too. Far above a normal shutdown of a few hundred
// milliseconds, so it fires only for a stop that is stuck rather than slow.
constexpr std::chrono::seconds kTeardownDeadline{5};
}  // namespace

int main() {
    // Blocked before any thread exists, since a thread inherits the mask of its creator
    // and both the render worker and the sd-bus loop start below. The sigwait thread is
    // then the only place these are handled.
    sigset_t shutdownSignals;
    sigemptyset(&shutdownSignals);
    sigaddset(&shutdownSignals, SIGINT);
    sigaddset(&shutdownSignals, SIGTERM);
    sigaddset(&shutdownSignals, SIGHUP);  // terminal close / logout
    if (pthread_sigmask(SIG_BLOCK, &shutdownSignals, nullptr) != 0) {
        std::fprintf(stderr, "wallpaperx-daemon: fatal: could not block shutdown signals\n");
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

        // Connected without claiming the name yet -- see the request below.
        auto connection = sdbus::createSessionBusConnection();

        // sigwait on its own thread, not a handler: leaveEventLoop() takes locks and is not
        // async-signal-safe. A guard rather than a bare std::thread because enterEventLoop()
        // THROWS when the connection drops, and a joinable thread destroyed mid-unwind is
        // std::terminate. Started before anything touching the filesystem: signals are blocked
        // at the top of main, and work done before this thread cannot be stopped.
        std::atomic<bool> wakingForShutdown{false};
        std::atomic<bool> shutdownRequested{false};

        // Reached only if teardown gets past the part that can block forever -- a wallpaper
        // or config file on a stalled mount, which no thread can be interrupted out of.
        struct Teardown {
            std::mutex mutex;
            std::condition_variable done;
            bool finished = false;
        } teardown;

        struct ShutdownWatch {
            std::thread thread;
            std::atomic<bool>& wakingForShutdown;
            Teardown& teardown;
            ~ShutdownWatch() {
                if (!thread.joinable()) return;
                // Releasing the watchdog leaves the rest of teardown -- the bus connection,
                // then static destruction -- with nothing bounding it. SIGALRM's default
                // action ends the process, and it is not among the signals main() blocks,
                // so this covers the tail. A process that exits first never sees it.
                ::alarm(static_cast<unsigned>(kTeardownDeadline.count()));
                {
                    std::lock_guard<std::mutex> lock(teardown.mutex);
                    teardown.finished = true;
                }
                teardown.done.notify_all();
                // Set BEFORE the signal, or the watcher wakes, reads false, and prints.
                wakingForShutdown.store(true, std::memory_order_release);
                pthread_kill(thread.native_handle(), SIGTERM);
                thread.join();
            }
        } shutdownWatch{std::thread([&connection, &shutdownSignals, &wakingForShutdown,
                                     &shutdownRequested, &teardown] {
            int received = 0;
            if (sigwait(&shutdownSignals, &received) != 0) return;
            // Skips leaveEventLoop() too: on the throw path the connection is what failed.
            if (wakingForShutdown.load(std::memory_order_acquire)) return;
            // Guarded because this is a bare thread, where an escaping exception is
            // std::terminate -- a core dump in place of the clean stop this thread exists to
            // give, and main's own catch cannot intercept it.
            try {
                std::printf("wallpaperx-daemon: %s received, shutting down\n",
                            strsignal(received));
                std::fflush(stdout);
                shutdownRequested.store(true, std::memory_order_release);
                // Caught on its own: a loop that will not leave still has the deadline below
                // bounding it, which is better than abandoning the stop immediately.
                try {
                    connection->leaveEventLoop();
                } catch (const std::exception& error) {
                    std::fprintf(stderr, "wallpaperx-daemon: could not stop the bus loop: %s\n",
                                 error.what());
                    std::fflush(stderr);
                } catch (...) {
                    std::fprintf(stderr, "wallpaperx-daemon: could not stop the bus loop\n");
                    std::fflush(stderr);
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
            // should not be told this was a clean stop. Wallpaper choices are written when
            // they are set, so nothing durable is lost.
            std::_Exit(1);
        }), wakingForShutdown, teardown};

        // Constructed after the watcher: this reads the config file, and a signal arriving
        // during that read must be able to stop the daemon.
        wallpaperx::WallpaperManager manager(config);
        wallpaperx::DbusService service(*connection, manager);

        // Before the name is claimed: this is what fills the monitor map. Skipped after a
        // stop -- it starts the first render, and that is GPU work for a process leaving.
        if (!shutdownRequested.load(std::memory_order_acquire)) service.restoreSaved();

        // Claimed last, so the name on the bus means the daemon can answer: object registered,
        // saved wallpapers back. Throws if another instance owns it, which is why nothing above
        // may write to the config directory. Skipped after a stop, or a daemon on its way out
        // would take the well-known name for a moment and drop it again.
        if (!shutdownRequested.load(std::memory_order_acquire)) {
            connection->requestName(sdbus::ServiceName{wallpaperx::DbusService::kBusName});
            // Only now: this moves the user's config aside, and until the name is held this
            // process does not know it is the daemon.
            manager.preserveUnreadableConfig();
            // After the copy aside: the repair rewrites config.json from live state, and the
            // copy preserves the bytes it destroys. Nothing enforces the order -- reversed, the
            // copy is taken of bytes the repair has already replaced.
            manager.writeRepairedConfig();
        }

        std::printf("wallpaperx-daemon: serving %s at %s\n", wallpaperx::DbusService::kBusName,
                    wallpaperx::DbusService::kObjectPath);
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
        wallpaperx::HttpServer httpServer(manager);

        // A signal delivered while the work above ran calls leaveEventLoop() on a loop that has
        // not started. sdbus-c++ appears to latch that, but it is undocumented, so do not rely
        // on it.
        if (!shutdownRequested.load(std::memory_order_acquire)) {
            connection->enterEventLoop();  // returns once leaveEventLoop() is called
        }

        // Destroyed in reverse declaration order: HTTP, then the service (stopping the
        // render worker), then the manager (GPU teardown + leak report), the shutdown
        // watcher, and the connection it holds last.
    } catch (const std::exception& e) {
        std::fprintf(stderr, "wallpaperx-daemon: fatal: %s\n", e.what());
        // The same hazard as the clean path below, and more likely here: a start that
        // failed is a start whose config read may still be outstanding.
        if (wallpaperx::configLoadAbandoned()) {
            std::fflush(nullptr);
            std::_Exit(1);
        }
        return 1;
    }

    std::printf("wallpaperx-daemon: stopped cleanly\n");

    // An abandoned config read left a thread blocked in the kernel, and waking during static
    // destruction would run library code already torn down. Everything with observable effects
    // is destroyed by here.
    if (wallpaperx::configLoadAbandoned()) {
        std::fflush(nullptr);
        std::_Exit(0);
    }
    return 0;
}
