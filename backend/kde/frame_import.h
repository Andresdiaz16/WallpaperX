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
        // Set to -1 once import() has consumed it; the caller closes it only if still >= 0.
        // Not keyed on success -- GL takes it before the texture exists, so a failed import
        // can still have consumed it.
        int fd = -1;
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
        void* eglImage = nullptr;       // EGLImageKHR backing it, null on the memory-object path
        void* eglDisplay = nullptr;     // the display eglImage belongs to, so release need not guess
        std::string error;              // why, when texture is 0
    };

    // Whether this context can import the given handle type at all. Checked against the
    // extension strings the driver advertises, NOT against whether the entry points resolve:
    // eglGetProcAddress hands back a non-null pointer for functions the driver does not
    // support, so a resolve-based check reports yes and then fails at the call.
    static bool supports(HandleType type, std::string* whyNot = nullptr);

    // Imports `request` into a new texture. By reference so it can report the fd's fate.
    static Result import(Request& request);

private:
    // The dma_buf half of import(), through EGLImage. Separate because the two paths share
    // nothing but their result: one hands the fd to GL, the other describes it to EGL.
    static Result importDmaBuf(Request& request);

public:
    // Frees what import() returned. The GL half must run on the thread whose context created
    // them, with that context current -- the scene graph's render thread, for the QQuickItem.
    // The EGL half destroys against the stored display and needs no current context.
    static void release(const Result& imported);
};

}  // namespace wallpaperx
