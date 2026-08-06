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

    try {
        wallpaperx::WallpaperManager::Config config;
        // Compared against "1" exactly: a truthiness check would make
        // WALLPAPERX_VALIDATION=0 mean "on".
        const char* validation = std::getenv("WALLPAPERX_VALIDATION");
        config.enableValidation = validation != nullptr && std::strcmp(validation, "1") == 0;

        // The device is created lazily on the first SetWallpaper, so the daemon still comes
        // up and answers GetStatus on a machine whose GPU stack is broken.
        wallpaperx::WallpaperManager manager(config);

        // Also requests the bus name; throws if another instance owns it.
        auto connection =
            sdbus::createSessionBusConnection(sdbus::ServiceName{wallpaperx::DbusService::kBusName});
        wallpaperx::DbusService service(*connection, manager);

        std::printf("wallpaperx-daemon: serving %s at %s\n", wallpaperx::DbusService::kBusName,
                    wallpaperx::DbusService::kObjectPath);
        std::fflush(stdout);

        // Declared last so it is destroyed first: its handlers call manager.snapshot(),
        // so HTTP has to stop before anything it reads starts being torn down.
        wallpaperx::HttpServer httpServer(manager);
        manager.recordConfigServer(httpServer.listening(), httpServer.lastError());

        // sigwait on its own thread, not a handler: leaveEventLoop() takes locks and is
        // not async-signal-safe. A guard rather than a bare std::thread because
        // enterEventLoop() THROWS when the connection drops, and a joinable thread
        // destroyed mid-unwind is std::terminate.
        //
        // The flag distinguishes the destructor's wake-up SIGTERM from a real one --
        // without it the daemon announced a signal nobody sent on the throw path. Declared
        // before the guard so it outlives it.
        std::atomic<bool> wakingForShutdown{false};

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
        } shutdownWatch{std::thread([&connection, &shutdownSignals, &wakingForShutdown] {
            int received = 0;
            if (sigwait(&shutdownSignals, &received) != 0) return;
            // Skips leaveEventLoop() too: on the throw path the connection is what failed.
            if (wakingForShutdown.load(std::memory_order_acquire)) return;
            std::printf("wallpaperx-daemon: %s received, shutting down\n", strsignal(received));
            std::fflush(stdout);
            connection->leaveEventLoop();
        }), wakingForShutdown};

        connection->enterEventLoop();  // returns once leaveEventLoop() is called

        // Destroyed in reverse declaration order: HTTP, then the service (stopping the
        // render worker), the connection, then the manager (GPU teardown + leak report).
    } catch (const std::exception& e) {
        std::fprintf(stderr, "wallpaperx-daemon: fatal: %s\n", e.what());
        return 1;
    }

    std::printf("wallpaperx-daemon: stopped cleanly\n");
    return 0;
}
