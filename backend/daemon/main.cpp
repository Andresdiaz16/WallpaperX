#include "dbus_service.h"
#include "wallpaper_manager.h"

#include <sdbus-c++/sdbus-c++.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <thread>

#include <pthread.h>

int main() {
    // Blocked in every thread before any is created, so nothing can be killed by the
    // default disposition and the wait below is the only place they are handled. Done
    // first because a thread inherits the mask of the thread that created it, and both
    // the render worker and the sd-bus event loop are started further down.
    //
    // Without this, SIGINT/SIGTERM terminated the process outright. Nothing ever called
    // leaveEventLoop(), so enterEventLoop() had no return path at all and main() had no
    // normal exit: ~DbusService's worker join, ~WallpaperManager's GPU-thread adoption
    // and Memory::shutdown()'s leaked-allocation report were unreachable code in the
    // shipped binary. They ran only in tests, which is the one place a leak backstop is
    // least needed.
    sigset_t shutdownSignals;
    sigemptyset(&shutdownSignals);
    sigaddset(&shutdownSignals, SIGINT);
    sigaddset(&shutdownSignals, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &shutdownSignals, nullptr) != 0) {
        std::fprintf(stderr, "wallpaperx-daemon: fatal: could not block shutdown signals\n");
        return 1;
    }

    try {
        wallpaperx::WallpaperManager::Config config;
        // Opt-in rather than on: the validation layers are a developer tool, they cost
        // real time on every submit, and they are not guaranteed installed on a user's
        // machine. Compared against "1" exactly -- a plain getenv() truthiness check
        // makes WALLPAPERX_VALIDATION=0 mean "on", which is the opposite of what anyone
        // typing it intends.
        const char* validation = std::getenv("WALLPAPERX_VALIDATION");
        config.enableValidation = validation != nullptr && std::strcmp(validation, "1") == 0;

        // The Vulkan device is created on the first SetWallpaper, not here, so the
        // daemon still comes up and answers GetStatus on a machine whose GPU stack is
        // broken -- which is precisely the machine whose owner needs to ask it what is
        // wrong.
        wallpaperx::WallpaperManager manager(config);

        // createSessionBusConnection(name) also requests the bus name; it throws if
        // another instance already owns io.wallpaperx.Daemon.
        auto connection =
            sdbus::createSessionBusConnection(sdbus::ServiceName{wallpaperx::DbusService::kBusName});
        wallpaperx::DbusService service(*connection, manager);

        std::printf("wallpaperx-daemon: serving %s at %s\n", wallpaperx::DbusService::kBusName,
                    wallpaperx::DbusService::kObjectPath);
        std::fflush(stdout);

        // sigwait on a dedicated thread rather than a signal handler. leaveEventLoop()
        // takes locks and is not async-signal-safe, so calling it from a handler is a
        // deadlock waiting for the wrong moment; a thread blocked in sigwait() runs it
        // in ordinary thread context instead.
        std::thread shutdownWatch([&connection, &shutdownSignals] {
            int received = 0;
            if (sigwait(&shutdownSignals, &received) != 0) return;
            std::printf("wallpaperx-daemon: %s received, shutting down\n", strsignal(received));
            std::fflush(stdout);
            connection->leaveEventLoop();
        });

        connection->enterEventLoop();  // returns once leaveEventLoop() is called

        // enterEventLoop() can also return on its own (the connection dropping, say),
        // leaving the watcher parked in sigwait() forever and join() hanging with it.
        // The signals are blocked process-wide, so this is delivered to that thread and
        // consumed by its sigwait rather than acting on the process.
        pthread_kill(shutdownWatch.native_handle(), SIGTERM);
        shutdownWatch.join();

        // Everything below main's try block is destroyed here, in reverse declaration
        // order: the service (which stops the render worker), then the connection, then
        // the manager (which tears down the GPU and reports leaked allocations).
    } catch (const std::exception& e) {
        std::fprintf(stderr, "wallpaperx-daemon: fatal: %s\n", e.what());
        return 1;
    }

    std::printf("wallpaperx-daemon: stopped cleanly\n");
    return 0;
}
