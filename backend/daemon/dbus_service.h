#pragma once

#include <sdbus-c++/sdbus-c++.h>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

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

    // Returns (fd, width, height, memory_size, handle_type, sync_fds, frame_generation,
    //          drm_format, drm_modifier, planes).
    //
    // drm_format / drm_modifier / planes are what a DMA_BUF consumer needs to interpret
    // the memory behind the fd: importing via EGL_LINUX_DMA_BUF_EXT or
    // zwp_linux_dmabuf_v1 requires the fourcc, the 64-bit modifier and per-plane
    // offset+pitch. They are unset (0, MOD_INVALID, empty) on the OPAQUE_FD path, which
    // is importable only by another context on the same device. planes is a list rather
    // than a scalar stride because multi-planar modifiers are normal on Intel.
    //
    // sync_fds is a reserved slot, always empty today. A consumer currently has no way
    // to know the GPU finished writing the frame it just imported, which is fine only
    // because static wallpapers render once. Video will need a real sync primitive here,
    // and by then the KDE plugin ships against this signature -- so the slot is carved
    // out now, while the method has no callers, rather than breaking daemon and plugin
    // together later. It is an array (not a bare 'h') because DBus fd types index into
    // the message's fd table and cannot encode "absent"; an empty array can.
    //
    // frameGeneration is a monotonic per-monitor render counter, letting a consumer tell
    // a fresh frame from a stale or repeated one.
    using FrameFdResult =
        std::tuple<sdbus::UnixFd, uint32_t, uint32_t, uint64_t, int32_t, std::vector<sdbus::UnixFd>, uint64_t,
                   uint32_t, uint64_t, std::vector<sdbus::Struct<uint64_t, uint64_t>>>;
    FrameFdResult getFrameFd(int32_t monitorId);

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
