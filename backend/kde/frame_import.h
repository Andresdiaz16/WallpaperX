// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <utility>
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
        // Set to -1 once import() has consumed it; the caller closes it only if still >= 0. The
        // sentinel is the only thing to key on -- WHEN it is consumed differs by handle type, and
        // each path documents its own rule in frame_import.cpp.
        int fd = -1;
        uint32_t width = 0;
        uint32_t height = 0;
        uint64_t memorySize = 0;
        HandleType handleType = HandleType::OpaqueFd;
        // Required for BOTH handle types: it is the channel order, which the opaque path has no
        // other source for and the dma_buf path hands straight to EGL. Left at 0, the import is
        // refused rather than drawn wrong.
        uint32_t drmFormat = 0;
        // DMA_BUF only, both of them. The modifier's sentinel is DRM_FORMAT_MOD_INVALID, not
        // 0 -- 0 is LINEAR, a perfectly valid layout.
        uint64_t drmModifier = 0;
        std::vector<std::pair<uint64_t, uint64_t>> planes;  // offset, rowPitch
        // The GPU the daemon allocated on. OPAQUE_FD only: that memory is importable on no
        // other device, so a mismatch is refused here rather than left to the driver.
        std::vector<uint8_t> deviceUuid;
        // Which image this is, so a caller holding several can import each once. Not used by
        // import() itself -- it describes the buffer, not how to map it.
        uint64_t bufferId = 0;

        // What the daemon says must be waited on before the frame is sampled -- the sync_fds of
        // GetFrameFd. Each is a binary semaphore payload that is ALREADY signalled; waiting on it
        // is what performs the queue-family acquire and the layout transition the producer's
        // release left outstanding, which OpenGL has no other way to express.
        //
        // Each entry is set to -1 once import() has consumed it, like `fd`. A caller that cannot
        // wait -- supportsSync() says no -- must close them and pass none, rather than pass them
        // and have the import refused: reading without the acquire is undefined but works, and is
        // the fallback.
        std::vector<int> syncFds;
    };

    struct Result {
        unsigned int texture = 0;       // GL texture name, 0 when the import failed
        unsigned int memoryObject = 0;  // GL memory object backing it, 0 on the EGLImage path
        void* eglImage = nullptr;       // EGLImageKHR backing it, null on the memory-object path
        void* eglDisplay = nullptr;     // the display eglImage belongs to, so release need not guess
        std::string error;              // why, when texture is 0
    };

    // The `error` set when the driver refuses the descriptor at the hand-over itself, rather
    // than at one of the checks before or the texture allocation after. Named because a test
    // that grades the hand-over branch has to tell it from every other refusal, and comparing
    // against its own copy of the sentence would silently stop matching if this one changed.
    static constexpr const char* kImportRejectedError = "glImportMemoryFdEXT rejected the exported memory";

    // Whether this context can import the given handle type at all. Checked against the
    // extension strings the driver advertises, NOT against whether the entry points resolve:
    // eglGetProcAddress hands back a non-null pointer for functions the driver does not
    // support, so a resolve-based check reports yes and then fails at the call. Having no EGL
    // display to ask is reported as itself, never as a missing extension: a GLX-backed Qt has
    // none, and its driver advertises the extensions all the same.
    //
    // Safe to ask per frame: the extension answers are cached per GL context, and the only part
    // that can change during a run is the kcmp budget the opaque path spends.
    static bool supports(HandleType type, std::string* whyNot = nullptr);

    // Whether this context can wait on the daemon's sync_fds. Independent of supports(): a driver
    // can import memory and not semaphores, and such a driver must keep drawing wallpapers rather
    // than refuse every frame. Same extension-string rule as supports(), for the same reason.
    static bool supportsSync(std::string* whyNot = nullptr);

    // Every DRM format modifier this EGL implementation can import `drmFormat` as. The daemon
    // cannot work this out for itself: Vulkan's importable bit answers for Vulkan, and this is
    // the only API that answers for the one doing the importing.
    //
    // Empty with a reason set when the layouts cannot be enumerated -- no such query, or no
    // current EGL display to ask, which are different facts and say so. Not a failure: the
    // caller reports nothing and the daemon keeps its unnegotiated default. Needs a current
    // context, so the render thread.
    //
    // Modifiers flagged external-only are excluded: they sample through
    // GL_TEXTURE_EXTERNAL_OES, and import() builds a GL_TEXTURE_2D.
    static std::vector<uint64_t> importableModifiers(uint32_t drmFormat, std::string* whyNot = nullptr);

    // Imports `request` into a new texture. By reference so it can report the fd's fate.
    //
    // Refuses outright if `syncFds` is non-empty and this context cannot wait: being handed a wait
    // it cannot perform means the caller mis-negotiated, and the failure that hides is a texture
    // sampled before its ownership transfer completed, which shows as tearing nothing explains.
    static Result import(Request& request);

private:
    // The dma_buf half of import(), through EGLImage. Separate because the two paths share
    // nothing but their result: one hands the fd to GL, the other describes it to EGL.
    //
    // `displayHandle` is the EGLDisplay supports(DmaBuf) answered for; it is never EGL_NO_DISPLAY,
    // because having none is one of the things that refusal reports.
    static Result importDmaBuf(Request& request, void* displayHandle);

    // The opaque-fd half, through a GL memory object. Its counterpart above; import() validates
    // once and dispatches, so neither repeats the other's checks.
    static Result importOpaqueFd(Request& request);

    // Waits on every descriptor in `request.syncFds` against `texture`, consuming them. The
    // acquire both paths need, so it runs after either has produced a texture.
    static bool acquire(Request& request, unsigned int texture, std::string& error);

public:
    // Frees what import() returned. The GL half must run on the thread whose context created
    // them, with that context current -- the scene graph's render thread, for the QQuickItem.
    // The EGL half destroys against the stored display and needs no current context.
    static void release(const Result& imported);
};

}  // namespace wallpaperx
