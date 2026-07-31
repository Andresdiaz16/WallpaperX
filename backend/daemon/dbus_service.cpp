#include "dbus_service.h"

#include "wallpaper_manager.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <exception>
#include <optional>
#include <utility>

namespace wallpaperx {

namespace {

// Error names the client can branch on. NoFrame is an ordinary state (nothing has
// been set on that monitor yet), not a malfunction -- keeping it distinct from
// ExportFailed is what lets a polling consumer tell "not ready" from "broken".
constexpr const char* kErrorNoFrame = "io.wallpaperx.Daemon.Error.NoFrame";
constexpr const char* kErrorExportFailed = "io.wallpaperx.Daemon.Error.ExportFailed";
constexpr const char* kErrorRenderFailed = "io.wallpaperx.Daemon.Error.RenderFailed";
constexpr const char* kErrorInvalidArgs = "org.freedesktop.DBus.Error.InvalidArgs";

}  // namespace

DbusService::DbusService(sdbus::IConnection& connection, WallpaperManager& manager)
    : m_manager(manager), m_object(sdbus::createObject(connection, sdbus::ObjectPath{kObjectPath})) {
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
    // A "static" request renders synchronously inside this call, so the reply is the
    // client's confirmation that the frame exists and GetFrameFd will succeed. That is
    // affordable because rendering is triggered by a user action, not per frame; Step
    // 13's video renderer is what will need a render thread.
    //
    // The cost is real and known: main.cpp runs one blocking event loop, so a slow
    // render delays every other method, and a render slower than sd-bus's 25 s default
    // would fail the caller with NoReply. Handing the render to a worker thread does not
    // fix that on its own -- the reply still cannot be sent until the frame exists, so
    // the caller's timeout is unchanged -- and it would cost the ordering guarantee
    // clients have today, that a returned SetWallpaper means the next GetFrameFd
    // succeeds. The fix is a SetWallpaper that acknowledges immediately plus a
    // FrameReady signal to announce completion, which is Step 6 task 3. Deferred there
    // deliberately rather than half-done here.
    //
    // Logged on entry, not on success. Most DBus clients drop an error reply on the
    // floor, so the daemon's log is the only place a failed request is visible -- and
    // logging after the call meant the requests that failed were exactly the ones that
    // left no trace, making them indistinguishable from requests that never arrived.
    std::printf("wallpaperx-daemon: SetWallpaper(type=%s, path=%s, monitor=%d)\n", type.c_str(), path.c_str(),
                monitorId);
    std::fflush(stdout);

    try {
        m_manager.setWallpaper(type, path, monitorId);
    } catch (const InvalidRequest& e) {
        std::fprintf(stderr, "wallpaperx-daemon: SetWallpaper rejected (monitor=%d): %s\n", monitorId, e.what());
        throw sdbus::Error(sdbus::Error::Name{kErrorInvalidArgs}, e.what());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "wallpaperx-daemon: SetWallpaper failed (monitor=%d): %s\n", monitorId, e.what());
        throw sdbus::Error(sdbus::Error::Name{kErrorRenderFailed}, e.what());
    }
}

DbusService::FrameFdResult DbusService::getFrameFd(int32_t monitorId) {
    std::optional<FrameHandle> handle;
    try {
        handle = m_manager.exportCurrentFrame(monitorId);
    } catch (const InvalidRequest& e) {
        throw sdbus::Error(sdbus::Error::Name{kErrorInvalidArgs}, e.what());
    } catch (const std::exception& e) {
        throw sdbus::Error(sdbus::Error::Name{kErrorExportFailed}, e.what());
    }

    if (!handle) {
        throw sdbus::Error(sdbus::Error::Name{kErrorNoFrame},
                           "no frame available for monitor " + std::to_string(monitorId) +
                               " (call SetWallpaper first)");
    }

    const ExportedFrame& frame = handle->frame;

    // adopt_fd, not the duplicating constructor: exportFrame() handed us ownership of a
    // freshly created fd and nothing else will close it. Taken on the first line after
    // the export so that any throw below runs ~UnixFd instead of leaking a descriptor --
    // GetFrameFd is called repeatedly by a polling consumer, so a leak here is an
    // eventual EMFILE, not a one-off.
    sdbus::UnixFd fd(frame.fd, sdbus::adopt_fd);

    std::vector<sdbus::Struct<uint64_t, uint64_t>> planes;
    planes.reserve(frame.planes.size());
    for (const ExportedFramePlane& plane : frame.planes) {
        planes.emplace_back(plane.offset, plane.rowPitch);
    }

    return FrameFdResult{std::move(fd),
                         frame.width,
                         frame.height,
                         frame.memorySize,
                         frame.type == ExportedFrame::HandleType::DmaBuf ? 1 : 0,
                         {},  // sync_fds: reserved, always empty today
                         handle->generation,
                         frame.drmFormat,
                         frame.modifier,
                         std::move(planes)};
}

std::string DbusService::getStatus() {
    nlohmann::json status;
    status["running"] = true;

    nlohmann::json wallpapers = nlohmann::json::object();
    for (const auto& [monitorId, monitor] : m_manager.status()) {
        nlohmann::json entry{{"type", monitor.type},
                             {"path", monitor.path},
                             {"generation", monitor.generation},
                             {"has_frame", monitor.hasFrame}};
        if (monitor.hasFrame) {
            entry["width"] = monitor.width;
            entry["height"] = monitor.height;
        }
        wallpapers[std::to_string(monitorId)] = std::move(entry);
    }
    status["wallpapers"] = wallpapers;

    return status.dump();
}

}
