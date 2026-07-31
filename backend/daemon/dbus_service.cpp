#include "dbus_service.h"

#include <nlohmann/json.hpp>

#include <cstdio>

namespace wallpaperx {

DbusService::DbusService(sdbus::IConnection& connection)
    : m_object(sdbus::createObject(connection, sdbus::ObjectPath{kObjectPath})) {
    registerVTable();
}

void DbusService::registerVTable() {
    m_object
        ->addVTable(
            sdbus::registerMethod("SetWallpaper")
                .implementedAs([this](const std::string& type, const std::string& path, int32_t monitorId) {
                    setWallpaper(type, path, monitorId);
                })
                .withInputParamNames("type", "path", "monitor_id"),
            sdbus::registerMethod("GetFrameFd")
                .implementedAs([this](int32_t monitorId) { return getFrameFd(monitorId); })
                .withInputParamNames("monitor_id")
                .withOutputParamNames("fd", "width", "height", "memory_size", "handle_type", "sync_fds",
                                      "frame_generation", "drm_format", "drm_modifier", "planes"),
            sdbus::registerMethod("GetStatus").implementedAs([this]() { return getStatus(); }).withOutputParamNames("status"),
            sdbus::registerSignal("FrameReady").withParameters<int32_t>("monitor_id"))
        .forInterface(std::string(kInterface));
}

void DbusService::setWallpaper(const std::string& type, const std::string& path, int32_t monitorId) {
    // Task 1 records the request; the actual render (static tier) is wired in task 2.
    // video/shader are accepted but not yet rendered (Step 13 / Phase 3).
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_wallpapers[monitorId] = WallpaperRequest{type, path};
    }
    std::printf("wallpaperx-daemon: SetWallpaper(type=%s, path=%s, monitor=%d)\n", type.c_str(), path.c_str(),
                monitorId);
    std::fflush(stdout);
}

DbusService::FrameFdResult DbusService::getFrameFd(int32_t monitorId) {
    // Task 1 stub: there is no renderer yet, so no frame to export. Task 2 wires this
    // to the renderer's exportFrame(). Throw a DBus error rather than return a bogus
    // fd, so callers get a clean, named failure.
    throw sdbus::Error(sdbus::Error::Name{"io.wallpaperx.Daemon.Error.NoFrame"},
                       "no frame available for monitor " + std::to_string(monitorId) +
                           " (renderer not yet wired -- Step 6 task 2)");
}

std::string DbusService::getStatus() {
    nlohmann::json status;
    status["running"] = true;
    nlohmann::json wallpapers = nlohmann::json::object();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& [monitorId, req] : m_wallpapers) {
            wallpapers[std::to_string(monitorId)] = {{"type", req.type}, {"path", req.path}};
        }
    }
    status["wallpapers"] = wallpapers;
    return status.dump();
}

}
