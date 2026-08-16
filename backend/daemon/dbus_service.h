// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "render_worker.h"

#include <sdbus-c++/sdbus-c++.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace wallpaperx {

class WallpaperManager;

// Serves the io.wallpaperx.Daemon DBus interface on a caller-provided connection.
// The connection owns the bus-name request and the event loop (blocking in the
// daemon's main(), async in tests) -- this class only registers and implements the
// methods and signals.
//
// Stateless by design: every handler forwards to the WallpaperManager, which owns
// both the wallpaper state and the lock guarding it. An earlier version kept its own
// mutex and request map here; that made the recorded requests and the rendered frames
// two separate truths under two separate locks, which is exactly the split that lets
// GetStatus and GetFrameFd disagree.
class DbusService {
public:
    static constexpr const char* kBusName = "io.wallpaperx.Daemon";
    static constexpr const char* kObjectPath = "/io/wallpaperx/Daemon";
    static constexpr const char* kInterface = "io.wallpaperx.Daemon";

    // `manager` must outlive this object.
    DbusService(sdbus::IConnection& connection, WallpaperManager& manager);
    ~DbusService();

    // Call once, before the event loop.
    void restoreSaved();

private:
    void registerVTable();

    // DBus method handlers (see the DBus Interface section in PLAN.md).
    //
    // Returns as soon as the request is recorded and queued; the render happens on the
    // worker thread. A successful reply therefore means "accepted", not "the frame is
    // ready" -- FrameReady announces that. Only client-side mistakes (unknown type,
    // out-of-range monitor, a path that is not a file) still fail synchronously, since
    // those cost nothing to detect and are the ones worth failing fast.
    void setWallpaper(const std::string& type, const std::string& path, int32_t monitorId);

    // Complete on return, unlike SetWallpaper: no render sits behind it, so an accepted
    // reply means the mapping is in place rather than merely queued.
    void setMonitorOutput(int32_t monitorId, const std::string& outputName);

    void emitFrameReady(int monitorId, uint64_t generation);

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
    //
    // Answered asynchronously, from the render worker's thread. Exporting a frame needs
    // the GPU, and the GPU lock is held for a whole render -- so doing it in the handler
    // would park the daemon's single event-loop thread behind whatever is rendering and
    // stall GetStatus and SetWallpaper with it. That is the same stall this design moved
    // SetWallpaper off; the rule that keeps it fixed is that no DBus handler touches the
    // GPU. The caller still waits for its frame, which is unavoidable, but nothing else
    // does.
    using FrameFdCall =
        sdbus::Result<sdbus::UnixFd, uint32_t, uint32_t, uint64_t, int32_t, std::vector<sdbus::UnixFd>, uint64_t,
                      uint32_t, uint64_t, std::vector<sdbus::Struct<uint64_t, uint64_t>>>;
    void completeGetFrameFd(FrameFdCall call, int32_t monitorId);

    std::string getStatus();

    // Destroyed first among the members, but the destructor stops the worker before any
    // of that runs -- see ~DbusService for why the ordering cannot be left to
    // declaration order alone.
    WallpaperManager& m_manager;
    std::unique_ptr<sdbus::IObject> m_object;
    RenderWorker m_worker;
};

}
