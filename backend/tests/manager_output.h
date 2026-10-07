// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "daemon/wallpaper_manager.h"

#include <string>
#include <vector>

namespace wallpaperx::testing {

// WallpaperManager::setMonitorOutput, returning the monitors it says are owed a redraw, for the
// manager-level tests that name a screen only as setup.
inline std::vector<int> nameScreen(WallpaperManager& manager, int monitorId, const std::string& name) {
    std::vector<int> redraw;
    manager.setMonitorOutput(monitorId, name, redraw);
    return redraw;
}

// The hand-over number an export recorded, which a release of its image names; 0 when it recorded
// none (an unnamed caller), which the daemon refuses as a release.
inline uint64_t handoverOf(const FrameHandle& handle) { return handle.handover ? handle.handover->number : 0; }

// An export followed by what a sent reply records: DbusService::completeFrame calls
// handoverDelivered() once its reply is out, and only that marks the monitor collected. `routine`,
// when given, receives handoverDelivered()'s answer, false when nothing was handed over. A check
// about the window between the two calls makes them itself.
inline FrameExport exportDelivered(WallpaperManager& manager, int monitorId, const std::string& client = {},
                                   bool onTimeline = false, const std::vector<uint64_t>& knownBuffers = {},
                                   bool* routine = nullptr) {
    std::vector<int> toRender;
    FrameExport exported = manager.exportCurrentFrame(monitorId, client, onTimeline, knownBuffers, toRender);
    const bool delivered = exported.handle && manager.handoverDelivered(monitorId, client);
    if (routine != nullptr) *routine = delivered;
    return exported;
}

// A copy of `client`'s acquire timeline as DbusService::completeGetAcquireTimeline hands one out: the
// export, then timelineCopyDelivered() once the reply is out, which is what counts against the copy
// limit. A check about a copy that never reached its caller calls acquireTimelineFd() alone.
inline int deliveredTimelineFd(WallpaperManager& manager, const std::string& client) {
    const int fd = manager.acquireTimelineFd(client);
    manager.timelineCopyDelivered(client);
    return fd;
}

// Gives back what `handle` handed `client`, naming its own hand-over.
inline std::vector<int> releaseHandle(WallpaperManager& manager, const std::string& client, int monitorId,
                                      const FrameHandle& handle, std::vector<FrameAvailable>* announce = nullptr) {
    return manager.releaseBuffer(client, monitorId, handle.frame.bufferId, handoverOf(handle), announce);
}

}  // namespace wallpaperx::testing
