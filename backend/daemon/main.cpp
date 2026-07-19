#include "dbus_service.h"

#include <sdbus-c++/sdbus-c++.h>

#include <cstdio>
#include <exception>

int main() {
    try {
        // createSessionBusConnection(name) also requests the bus name; it throws if
        // another instance already owns io.wallpaperx.Daemon.
        auto connection =
            sdbus::createSessionBusConnection(sdbus::ServiceName{wallpaperx::DbusService::kBusName});
        wallpaperx::DbusService service(*connection);

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
