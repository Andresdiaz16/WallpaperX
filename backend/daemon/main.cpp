#include "dbus_service.h"
#include "http_server.h"
#include "wallpaper_manager.h"

#include <sdbus-c++/sdbus-c++.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <thread>

#include <pthread.h>

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

    // Same reasoning, reachable since the config write: exceeding RLIMIT_FSIZE otherwise
    // kills the daemon mid-SetWallpaper with a core dump and no message. Ignored, so the
    // write fails with EFBIG and ConfigStore reports it.
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
        // Started before anything that touches the filesystem, and that ordering is the
        // point: shutdown signals are blocked at the top of main, so any work done before
        // this thread exists is work during which the daemon cannot be stopped. Reading the
        // config used to happen there, and a config that never answered -- a FIFO, a stalled
        // mount -- left a process that ignored SIGTERM and SIGINT and needed SIGKILL.
        //
        // The flags distinguish the destructor's wake-up SIGTERM from a real one -- without
        // it the daemon announced a signal nobody sent on the throw path -- and record a
        // signal that arrived before the loop began, which would otherwise be a shutdown
        // request delivered to a loop that had nothing to leave. Declared before the guard
        // so they outlive it.
        std::atomic<bool> wakingForShutdown{false};
        std::atomic<bool> shutdownRequested{false};

        struct ShutdownWatch {
            std::thread thread;
            std::atomic<bool>& wakingForShutdown;
            ~ShutdownWatch() {
                if (!thread.joinable()) return;
                // Set BEFORE the signal, or the watcher wakes, reads false, and prints.
                wakingForShutdown.store(true, std::memory_order_release);
                pthread_kill(thread.native_handle(), SIGTERM);
                thread.join();
            }
        } shutdownWatch{std::thread([&connection, &shutdownSignals, &wakingForShutdown,
                                     &shutdownRequested] {
            int received = 0;
            if (sigwait(&shutdownSignals, &received) != 0) return;
            // Skips leaveEventLoop() too: on the throw path the connection is what failed.
            if (wakingForShutdown.load(std::memory_order_acquire)) return;
            std::printf("wallpaperx-daemon: %s received, shutting down\n", strsignal(received));
            std::fflush(stdout);
            shutdownRequested.store(true, std::memory_order_release);
            connection->leaveEventLoop();
        }), wakingForShutdown};

        // The device is created on the first render, not here -- so a machine with a broken
        // GPU stack still comes up and answers GetStatus. That first render is now
        // restoreSaved() below rather than a client's SetWallpaper whenever a config exists.
        //
        // Constructed here, after the watcher: this reads the config file, and a signal
        // arriving during that read must be able to stop the daemon.
        wallpaperx::WallpaperManager manager(config);
        wallpaperx::DbusService service(*connection, manager);

        std::printf("wallpaperx-daemon: serving %s at %s\n", wallpaperx::DbusService::kBusName,
                    wallpaperx::DbusService::kObjectPath);
        std::fflush(stdout);

        // Declared after the service so it is destroyed first: its handlers call
        // manager.snapshot(), so HTTP has to stop before anything it reads is torn down.
        wallpaperx::HttpServer httpServer(manager);

        service.restoreSaved();

        // A signal delivered while the work above ran calls leaveEventLoop() on a loop that
        // has not started. sdbus-c++ was measured to latch that -- 40 of 40 shutdowns during
        // startup exited cleanly with this check removed -- so this guards against a
        // behaviour that is relied on rather than documented. Without the latch, entering
        // here would be a daemon holding the bus name and ignoring a shutdown it was already
        // told about.
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
    return 0;
}
