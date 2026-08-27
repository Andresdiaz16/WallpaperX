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

// Serves the io.wallpaperx.Daemon DBus interface on a caller-provided connection. The
// connection owns the bus-name request and the event loop (blocking in main(), async in
// tests); this class only registers and implements the methods and signals.
//
// Stateless by design: every handler forwards to the WallpaperManager, which owns both the
// wallpaper state and the lock guarding it. State kept here would be a second truth under a
// second lock, which is what lets GetStatus and GetFrameFd disagree.
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

    // DBus method handlers. PLAN.md's DBus Interface section is the wire contract.
    //
    // Returns once the request is recorded and queued: a successful reply means "accepted",
    // not "the frame is ready" -- FrameReady announces that. Only client-side mistakes fail
    // synchronously, since they cost nothing to detect.
    void setWallpaper(const std::string& type, const std::string& path, int32_t monitorId);

    // Complete on return, unlike SetWallpaper: an accepted reply means the mapping reached the
    // config file. An error means it did not -- the binding still stands for this run but will
    // not survive a restart, and the client may re-assert it later.
    void setMonitorOutput(int32_t monitorId, const std::string& outputName);

    void emitFrameReady(int monitorId, uint64_t generation);

    // Returns (fd, width, height, memory_size, handle_type, sync_fds, frame_generation,
    //          drm_format, drm_modifier, planes). What each field means, and why sync_fds is a
    //          reserved array rather than a bare 'h', is PLAN.md's DBus Interface section.
    //
    // Answered from the render worker's thread, not in the handler. Exporting needs the GPU and
    // the GPU lock is held for a whole render, so answering here would park the single
    // event-loop thread behind whatever is rendering and take GetStatus with it. The rule that
    // keeps it fixed -- no DBus handler touches the GPU -- is enforced by assertGpuThread().
    using FrameFdCall =
        sdbus::Result<sdbus::UnixFd, uint32_t, uint32_t, uint64_t, int32_t, std::vector<sdbus::UnixFd>, uint64_t,
                      uint32_t, uint64_t, std::vector<sdbus::Struct<uint64_t, uint64_t>>, std::vector<uint8_t>>;
    void completeGetFrameFd(FrameFdCall call, int32_t monitorId);

    std::string getStatus();

    // ~DbusService stops the worker before any member is destroyed: the ordering cannot be
    // left to declaration order alone, and that destructor says why.
    WallpaperManager& m_manager;
    std::unique_ptr<sdbus::IObject> m_object;
    RenderWorker m_worker;
};

}
