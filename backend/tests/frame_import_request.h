// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// A served Frame as the importer's Request, for the tests that import what the daemon handed over.
// Only for tests built with the GL importer: it needs frame_import.h and the bus client's Frame.

#include "frame_import.h"
#include "tests/client/daemon_client.h"
#include "tests/test_report.h"

#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

namespace wallpaperx::testing {

// The Request owns a duplicate of the frame's descriptor, as WallpaperXItem::onFrameReply makes one of
// the descriptor its reply object owns; request.fd is -1 when it could not be made.
// A failed dup() is reported here as the harness's fault: one this file failed to duplicate is
// indistinguishable from one the importer refused to take, so a caller must not import it.
// Sync descriptors are left to the caller.
inline wallpaperx::FrameImport::Request requestFromFrame(const Frame& frame) {
    wallpaperx::FrameImport::Request request;
    request.fd = ::dup(frame.fd.get());
    if (request.fd < 0) {
        harnessFailure(std::string("the frame's descriptor could not be duplicated: ") + std::strerror(errno));
    }
    request.width = frame.width;
    request.height = frame.height;
    request.memorySize = frame.memorySize;
    request.handleType = frame.handleType == 1 ? wallpaperx::FrameImport::HandleType::DmaBuf
                                               : wallpaperx::FrameImport::HandleType::OpaqueFd;
    request.drmFormat = frame.drmFormat;
    request.drmModifier = frame.drmModifier;
    request.deviceUuid = frame.deviceUuid;
    for (const auto& plane : frame.planes) request.planes.emplace_back(plane.get<0>(), plane.get<1>());
    return request;
}

}  // namespace wallpaperx::testing
