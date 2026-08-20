// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wallpaperx {

// Turns a frame the daemon exported into a GL texture on the calling thread's current context.
//
// Deliberately free of Qt: the import is the part that can fail in driver-specific ways, and
// keeping it separate lets a probe exercise it against a real daemon with no scene graph, no
// window and no Plasma. WallpaperXItem calls the same function from updatePaintNode().
//
// Every entry point here requires a current GL context and must be called on the thread that
// owns it. None of them throws -- a failure is a false return and a reason, because the caller
// is a paint callback inside the compositor and an exception there is not survivable.
class FrameImport {
public:
    // What the daemon says the memory is. Mirrors the handle_type field of GetFrameFd rather
    // than re-deriving it: the daemon chooses per vendor and only it knows which it picked.
    enum class HandleType { OpaqueFd = 0, DmaBuf = 1 };

    struct Request {
        int fd = -1;  // consumed by a successful import; still the caller's to close on failure
        uint32_t width = 0;
        uint32_t height = 0;
        uint64_t memorySize = 0;
        HandleType handleType = HandleType::OpaqueFd;
        // DMA_BUF only. The modifier's sentinel is DRM_FORMAT_MOD_INVALID, not 0 -- 0 is LINEAR,
        // a perfectly valid layout.
        uint32_t drmFormat = 0;
        uint64_t drmModifier = 0;
        std::vector<std::pair<uint64_t, uint64_t>> planes;  // offset, rowPitch
    };

    struct Result {
        unsigned int texture = 0;       // GL texture name, 0 when the import failed
        unsigned int memoryObject = 0;  // GL memory object backing it, 0 on the EGLImage path
        std::string error;              // why, when texture is 0
    };

    // Whether this context can import the given handle type at all. Checked against the
    // extension strings the driver advertises, NOT against whether the entry points resolve:
    // eglGetProcAddress hands back a non-null pointer for functions the driver does not
    // support, so a resolve-based check reports yes and then fails at the call.
    static bool supports(HandleType type, std::string* whyNot = nullptr);

    // Imports `request` into a new texture. On success the fd has been consumed and must not be
    // closed by the caller; on failure it is untouched and remains the caller's to close.
    static Result import(const Request& request);

    // Frees what import() returned. Must run on the thread whose context created them, with
    // that context current -- the scene graph's render thread, for the QQuickItem.
    static void release(const Result& imported);
};

}  // namespace wallpaperx
