#pragma once

#include <sdbus-c++/sdbus-c++.h>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>

namespace wallpaperx {

// Serves the io.wallpaperx.Daemon DBus interface on a caller-provided connection.
// The connection owns the bus-name request and the event loop (blocking in the
// daemon's main(), async in tests) -- this class only registers and implements the
// methods and signals. All shared state is guarded by m_mutex, since the sdbus-c++
// event loop can invoke handlers from its own thread.
class DbusService {
public:
    static constexpr const char* kBusName = "io.wallpaperx.Daemon";
    static constexpr const char* kObjectPath = "/io/wallpaperx/Daemon";
    static constexpr const char* kInterface = "io.wallpaperx.Daemon";

    explicit DbusService(sdbus::IConnection& connection);

private:
    void registerVTable();

    // DBus method handlers (see the DBus Interface section in PLAN.md).
    void setWallpaper(const std::string& type, const std::string& path, int32_t monitorId);
    std::tuple<sdbus::UnixFd, uint32_t, uint32_t, uint64_t, int32_t> getFrameFd(int32_t monitorId);
    std::string getStatus();

    std::unique_ptr<sdbus::IObject> m_object;

    struct WallpaperRequest {
        std::string type;
        std::string path;
    };
    std::mutex m_mutex;
    std::map<int32_t, WallpaperRequest> m_wallpapers;  // monitor_id -> most recent SetWallpaper
};

}
