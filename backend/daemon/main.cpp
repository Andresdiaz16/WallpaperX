#include "dbus_service.h"
#include "wallpaper_manager.h"

#include <sdbus-c++/sdbus-c++.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

int main() {
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

        connection->enterEventLoop();  // blocks until the connection is unref'd / interrupted
    } catch (const std::exception& e) {
        std::fprintf(stderr, "wallpaperx-daemon: fatal: %s\n", e.what());
        return 1;
    }
    return 0;
}
