// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// A served frame's picture read back as bytes, on either export path: a LINEAR dma_buf is mapped,
// anything else -- an opaque fd above all, which is what NVIDIA exports -- is imported by the plugin's
// own GL importer on the exporting GPU and sampled back. The import half exists only in a test built
// with WALLPAPERX_HAVE_FRAME_IMPORT=1 and linked against wallpaperx-frame-import; without it a frame
// that cannot be mapped comes back unreadable, with the reason.

#include "support/exported_frame_map.h"
#include "tests/client/daemon_client.h"
#include "tests/test_report.h"

#if WALLPAPERX_HAVE_FRAME_IMPORT
#include "frame_import.h"
#include "tests/gl_import_harness.h"
#endif

#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace wallpaperx::testing {

// What reading a hand-over needs, whether it came over the bus or straight from the manager. Every
// descriptor here is borrowed: the caller still owns and closes them.
struct FrameLayout {
    int fd = -1;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t memorySize = 0;
    int handleType = 0;  // the wire's numbering: 1 is a dma_buf
    uint64_t modifier = 0;
    uint32_t drmFormat = 0;
    std::vector<uint8_t> deviceUuid;
    // One entry per plane, in both; plane 0 is what a mapped read uses.
    std::vector<uint64_t> planeOffsets;
    std::vector<uint64_t> rowPitches;
    // The hand-over's acquire: what a consumer waits on before sampling.
    std::vector<int> syncFds;
};

inline FrameLayout layoutOf(const Frame& frame) {
    FrameLayout layout{frame.fd.get(), frame.width, frame.height, frame.memorySize, frame.handleType,
                       frame.drmModifier};
    for (const auto& plane : frame.planes) {
        layout.planeOffsets.push_back(plane.get<0>());
        layout.rowPitches.push_back(plane.get<1>());
    }
    layout.drmFormat = frame.drmFormat;
    layout.deviceUuid = frame.deviceUuid;
    for (const sdbus::UnixFd& sync : frame.syncFds) layout.syncFds.push_back(sync.get());
    return layout;
}

// Whether readServedBgra() maps this frame rather than importing it: a LINEAR dma_buf with a plane.
// readServedBgra() sizes the fd itself before mapping.
inline bool servedFrameIsMapped(const FrameLayout& frame) {
    return frame.handleType == 1 && frame.modifier == kDrmModifierLinear && !frame.planeOffsets.empty();
}

// How many of `bgra`'s pixels differ from B,G,R by more than `tolerance` in any channel; the first such
// pixel is copied to `firstBad`. Alpha is not compared.
inline size_t countOffColour(const std::vector<unsigned char>& bgra, unsigned char b, unsigned char g,
                             unsigned char r, int tolerance, unsigned char firstBad[4]) {
    size_t bad = 0;
    for (size_t at = 0; at + 3 < bgra.size(); at += 4) {
        const unsigned char* px = bgra.data() + at;
        if (std::abs(int(px[0]) - int(b)) > tolerance || std::abs(int(px[1]) - int(g)) > tolerance ||
            std::abs(int(px[2]) - int(r)) > tolerance) {
            if (bad == 0) std::memcpy(firstBad, px, 4);
            ++bad;
        }
    }
    return bad;
}

// The hand-over's picture as tightly packed B,G,R,A rows, top row first; empty with `reason` set when
// this host cannot read it. A layout or size that is the producer's fault is also a FAIL here, beside
// the reason, so a caller that turns an empty read into NOT VERIFIED still goes red for it. An imported
// frame waits on the hand-over's acquire as the plugin does.
inline std::vector<unsigned char> readServedBgra(const FrameLayout& frame, std::string& reason) {
    std::vector<unsigned char> pixels;
    const size_t rowBytes = static_cast<size_t>(frame.width) * 4;
    if (frame.planeOffsets.size() != frame.rowPitches.size()) {
        harnessFailure("a FrameLayout was built with " + std::to_string(frame.planeOffsets.size()) +
                       " plane offset(s) and " + std::to_string(frame.rowPitches.size()) + " row pitch(es)");
        reason = "(harness) the frame layout's plane lists differ in length";
        return pixels;
    }
    if (servedFrameIsMapped(frame)) {
        // GOAL: S15.423 -- a frame claiming more memory than its descriptor holds is reported as a
        //       failure of what was served, never mapped (SIGBUS) and never passed off as unreadable.
        // WHY-THIS-SHAPE: the one size gate; callers keep none. A FAIL, not a reason alone, because
        //       every caller reports an empty read as NOT VERIFIED, which would make a producer's wrong
        //       size a skip. daemon_system reads in a child process, which turns this FAIL into its
        //       "mismatched" exit code so the parent goes red too.
        // CONTROL: backend/daemon/dbus_service.cpp GetFrame replying `frame.memorySize + (1 << 20)`, via
        //       control_run.sh, 2026-10-09, Lavapipe -> "FAIL: a served frame's descriptor holds at least the
        //       memory size it claims" on dbus_service_lavapipe (--phase '^coalescing$') and on
        //       daemon_system_lavapipe (--phase '^firstFrameUnderStatusLoad$'), whose parent then prints "FAIL:
        //       a process that never allocated the image reads the expected pixels through the published
        //       layout". Held, restored.
        const off_t realSize = ::lseek(frame.fd, 0, SEEK_END);
        (void)::lseek(frame.fd, 0, SEEK_SET);
        if (realSize < 0) {
            reason = std::string("the exported fd could not be sized (") + std::strerror(errno) +
                     "), so it was not mapped";
            return pixels;
        }
        if (static_cast<uint64_t>(realSize) < frame.memorySize) {
            check(false, "a served frame's descriptor holds at least the memory size it claims");
            reason = "the exported fd holds " + std::to_string(realSize) + " bytes but the frame claims " +
                     std::to_string(frame.memorySize) + "; not mapped";
            return pixels;
        }
        const MappedFrame mapped(frame.fd, frame.memorySize);
        if (!mapped.valid()) {
            reason = "the exported fd could not be mapped: " + mapped.error();
            return pixels;
        }
        pixels.reserve(rowBytes * frame.height);
        for (uint32_t row = 0; row < frame.height; ++row) {
            const unsigned char* bytes = mapped.row(frame.planeOffsets[0], frame.rowPitches[0], row, rowBytes);
            if (bytes == nullptr) {
                // GOAL: S15.424 -- a published plane offset or row pitch that puts a row outside the
                //       exported memory is the producer's failure, not a frame this host cannot read.
                // WHY-THIS-SHAPE: a FAIL beside the reason, as the size gate above; a reason alone is
                //       NOT VERIFIED at every caller, so a wrong layout would pass as a skip.
                // CONTROL: backend/daemon/dbus_service.cpp GetFrame publishing `plane.rowPitch * 4`, via
                //       control_run.sh, 2026-10-09, Lavapipe -> "FAIL: a served frame's published layout
                //       keeps every row inside the memory it exported" on dbus_service_lavapipe (--phase
                //       '^coalescing$') and on daemon_system_lavapipe (--phase '^firstFrameUnderStatusLoad$'),
                //       whose parent then prints "FAIL: a process that never allocated the image reads the
                //       expected pixels through the published layout". Held, restored.
                check(false, "a served frame's published layout keeps every row inside the memory it exported");
                reason = "the frame's own published layout puts row " + std::to_string(row) +
                         " outside the memory it exported";
                pixels.clear();
                return pixels;
            }
            pixels.insert(pixels.end(), bytes, bytes + rowBytes);
        }
        return pixels;
    }
#if WALLPAPERX_HAVE_FRAME_IMPORT
    std::array<uint8_t, 16> uuid{};
    if (frame.deviceUuid.size() == uuid.size()) std::copy(frame.deviceUuid.begin(), frame.deviceUuid.end(), uuid.begin());
    HeadlessGl gl;
    std::string why;
    if (!gl.createMatching(uuid, why)) {
        reason = "no GL context on the exporting GPU to import the frame into: " + why;
        return pixels;
    }
    FrameImport::Request request;
    request.width = frame.width;
    request.height = frame.height;
    request.memorySize = frame.memorySize;
    request.handleType = frame.handleType == 1 ? FrameImport::HandleType::DmaBuf : FrameImport::HandleType::OpaqueFd;
    request.drmFormat = frame.drmFormat;
    request.drmModifier = frame.modifier;
    request.deviceUuid = frame.deviceUuid;
    for (size_t i = 0; i < frame.planeOffsets.size(); ++i) request.planes.emplace_back(frame.planeOffsets[i], frame.rowPitches[i]);
    request.fd = ::dup(frame.fd);
    if (request.fd < 0) {
        harnessFailure(std::string("the frame's descriptor could not be duplicated: ") + std::strerror(errno));
        reason = "(harness) the frame's descriptor could not be duplicated";
        return pixels;
    }
    // Without the extension the plugin reads without the acquire too; that is its documented fallback.
    if (FrameImport::supportsSync()) {
        for (const int sync : frame.syncFds) {
            const int fd = ::dup(sync);
            if (fd >= 0) request.syncFds.push_back(fd);
        }
    }
    const FrameImport::Result imported = FrameImport::import(request);
    // import() sets to -1 what it consumed.
    if (request.fd >= 0) ::close(request.fd);
    for (const int fd : request.syncFds) {
        if (fd >= 0) ::close(fd);
    }
    if (imported.texture == 0) {
        reason = "the frame could not be imported into GL on the exporting GPU: " + imported.error;
        return pixels;
    }
    const std::vector<unsigned char> rgba = readSampled(imported.texture, frame.width, frame.height, why);
    FrameImport::release(imported);
    if (rgba.size() != rowBytes * frame.height) {
        reason = "the imported frame could not be sampled back: " + why;
        return pixels;
    }
    // Sampled through the importer's swizzle, so these are the rendered colours in R,G,B,A order.
    pixels.resize(rgba.size());
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
        pixels[i + 0] = rgba[i + 2];
        pixels[i + 1] = rgba[i + 1];
        pixels[i + 2] = rgba[i + 0];
        pixels[i + 3] = rgba[i + 3];
    }
    return pixels;
#else
    char modifier[32];
    std::snprintf(modifier, sizeof(modifier), "%llx", static_cast<unsigned long long>(frame.modifier));
    reason = "this build has no GL importer, and the export is not a LINEAR dma_buf (handle type " +
             std::to_string(frame.handleType) + ", modifier 0x" + modifier + ") that could be mapped";
    return pixels;
#endif
}

}  // namespace wallpaperx::testing
