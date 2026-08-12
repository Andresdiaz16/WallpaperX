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

namespace {
// Far above a normal shutdown, measured in hundreds of milliseconds, so this only ever
// fires for teardown that is genuinely stuck rather than merely slow.
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

        // Also requests the bus name; throws if another instance owns it.
        auto connection =
            sdbus::createSessionBusConnection(sdbus::ServiceName{wallpaperx::DbusService::kBusName});

        // sigwait on its own thread, not a handler: leaveEventLoop() takes locks and is
        // not async-signal-safe. A guard rather than a bare std::thread because
        // enterEventLoop() THROWS when the connection drops, and a joinable thread
        // destroyed mid-unwind is std::terminate.
        //
        // Started before anything that touches the filesystem: signals are blocked at the
        // top of main, so work done before this thread exists cannot be stopped.
        //
        // wakingForShutdown separates the destructor's wake-up SIGTERM from a real one;
        // shutdownRequested records a signal that arrived before the loop began. Declared
        // before the guard so they outlive it.
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
            std::printf("wallpaperx-daemon: %s received, shutting down\n", strsignal(received));
            std::fflush(stdout);
            shutdownRequested.store(true, std::memory_order_release);
            connection->leaveEventLoop();

            // This thread is the only one that cannot be stuck -- it waits for signals and
            // nothing else -- so it is where the deadline belongs. Any other thread blocked
            // in an uninterruptible filesystem call would otherwise hold the process open
            // against a stop it already acknowledged.
            std::unique_lock<std::mutex> lock(teardown.mutex);
            if (teardown.done.wait_for(lock, kTeardownDeadline,
                                       [&teardown] { return teardown.finished; })) {
                return;
            }
            std::printf("wallpaperx-daemon: shutdown blocked for %llds, exiting now\n",
                        static_cast<long long>(kTeardownDeadline.count()));
            // Nothing left to unwind that matters: wallpaper choices are written when they
            // are set, and the kernel reclaims the rest.
            std::fflush(nullptr);
            std::_Exit(0);
        }), wakingForShutdown, teardown};

        // Constructed after the watcher: this reads the config file, and a signal arriving
        // during that read must be able to stop the daemon.
        wallpaperx::WallpaperManager manager(config);
        wallpaperx::DbusService service(*connection, manager);

        // Before the banner, which a supervisor may treat as readiness: this is what fills
        // the monitor map. Skipped after a stop -- it starts the first render, and that is
        // GPU work for a process already leaving.
        if (!shutdownRequested.load(std::memory_order_acquire)) service.restoreSaved();

        std::printf("wallpaperx-daemon: serving %s at %s\n", wallpaperx::DbusService::kBusName,
                    wallpaperx::DbusService::kObjectPath);
        std::fflush(stdout);

        // Declared after the service so it is destroyed first: its handlers call
        // manager.snapshot(), so HTTP has to stop before anything it reads is torn down.
        wallpaperx::HttpServer httpServer(manager);

        // A signal delivered while the work above ran calls leaveEventLoop() on a loop that
        // has not started. sdbus-c++ appears to latch that, but it is not documented, so
        // this does not rely on it.
        if (!shutdownRequested.load(std::memory_order_acquire)) {
            connection->enterEventLoop();  // returns once leaveEventLoop() is called
        }

        // Destroyed in reverse declaration order: HTTP, then the service (stopping the
        // render worker), then the manager (GPU teardown + leak report), the shutdown
        // watcher, and the connection it holds last.
    } catch (const std::exception& e) {
        std::fprintf(stderr, "wallpaperx-daemon: fatal: %s\n", e.what());
        return 1;
    }

    std::printf("wallpaperx-daemon: stopped cleanly\n");

    // An abandoned config read left a thread blocked in the kernel; if it wakes during
    // static destruction it runs library code already being torn down. Everything with
    // observable effects is destroyed by here.
    if (wallpaperx::configLoadAbandoned()) {
        std::fflush(nullptr);
        std::_Exit(0);
    }
    return 0;
}
