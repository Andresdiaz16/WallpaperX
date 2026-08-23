// SPDX-License-Identifier: GPL-2.0-or-later

#include "frame_import.h"

// The fourccs the daemon exports, shared with it rather than copied: the two lists had drifted.
#include "vulkan/drm_formats.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
// Declares the core GL 3.0+ entry points glext.h otherwise hides. Needed for glGetStringi, which
// libGL exports and which must NOT be resolved through eglGetProcAddress -- see hasGlExtension.
// Extension entry points are still resolved at runtime; this only makes core ones callable.
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>

#include <fcntl.h>
#include <linux/kcmp.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

namespace wallpaperx {

namespace {

// Resolved once per process, on first use. eglGetProcAddress is not free and this runs from a
// paint callback.
struct MemoryObjectApi {
    using CreateFn = void (*)(GLsizei, GLuint*);
    using DeleteFn = void (*)(GLsizei, const GLuint*);
    using ImportFdFn = void (*)(GLuint, GLuint64, GLenum, GLint);
    using ParameterivFn = void (*)(GLuint, GLenum, const GLint*);
    using TexStorageFn = void (*)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLuint, GLuint64);

    CreateFn create = nullptr;
    DeleteFn destroy = nullptr;
    ImportFdFn importFd = nullptr;
    ParameterivFn parameteriv = nullptr;
    TexStorageFn texStorage = nullptr;

    bool complete() const {
        // parameteriv included: without it the dedicated flag cannot be set, and an import that
        // skips the flag succeeds and samples blank.
        return create != nullptr && destroy != nullptr && importFd != nullptr && parameteriv != nullptr &&
               texStorage != nullptr;
    }
};

const MemoryObjectApi& memoryObjectApi() {
    static const MemoryObjectApi api = [] {
        MemoryObjectApi resolved;
        resolved.create = reinterpret_cast<MemoryObjectApi::CreateFn>(eglGetProcAddress("glCreateMemoryObjectsEXT"));
        resolved.destroy = reinterpret_cast<MemoryObjectApi::DeleteFn>(eglGetProcAddress("glDeleteMemoryObjectsEXT"));
        resolved.importFd = reinterpret_cast<MemoryObjectApi::ImportFdFn>(eglGetProcAddress("glImportMemoryFdEXT"));
        resolved.parameteriv =
            reinterpret_cast<MemoryObjectApi::ParameterivFn>(eglGetProcAddress("glMemoryObjectParameterivEXT"));
        resolved.texStorage =
            reinterpret_cast<MemoryObjectApi::TexStorageFn>(eglGetProcAddress("glTexStorageMem2DEXT"));
        return resolved;
    }();
    return api;
}

// Walked with glGetStringi rather than glGetString(GL_EXTENSIONS): the single-string form is
// removed in a core profile and returns null there, which reads as "no extensions at all".
//
// Called directly rather than resolved through eglGetProcAddress: glGetStringi is CORE GL 3.0,
// and EGL only promises to resolve core client-API functions under EGL 1.5 or
// EGL_KHR_get_all_proc_addresses. Where it does not, the lookup returns null and every extension
// below reads as absent -- reporting "this driver has no GL_EXT_memory_object" for a driver that
// does, which is both false and unactionable.
bool hasGlExtension(const char* wanted) {
    GLint count = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &count);
    for (GLint i = 0; i < count; ++i) {
        const GLubyte* name = glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i));
        if (name != nullptr && std::strcmp(reinterpret_cast<const char*>(name), wanted) == 0) {
            return true;
        }
    }
    return false;
}

// Whether `candidate` is still the same OPEN FILE DESCRIPTION as `reference` -- how a failed
// import decides what the driver did with the descriptor it was handed.
//
// kcmp compares the kernel's open file description. fstat would compare the FILE, and those
// differ exactly where it matters: a re-open of the same file matches on st_dev/st_ino while
// being a different description, so fstat can answer "still yours" about a descriptor that now
// belongs to something else. Measured. fstat remains only as the fallback for kernels built
// without CONFIG_KCMP, where it still beats guessing.
bool sameOpenFile(int candidate, int reference) {
    const long compared = ::syscall(SYS_kcmp, ::getpid(), ::getpid(), KCMP_FILE, candidate, reference);
    if (compared >= 0) return compared == 0;
    if (errno == EBADF) return false;  // the driver closed it

    struct ::stat mine{};
    struct ::stat theirs{};
    if (::fstat(candidate, &mine) != 0 || ::fstat(reference, &theirs) != 0) return false;
    return mine.st_dev == theirs.st_dev && mine.st_ino == theirs.st_ino;
}

void setWhyNot(std::string* whyNot, const char* reason) {
    if (whyNot != nullptr) *whyNot = reason;
}

// The EGLImage side, for dma_buf. A dma_buf cannot go through GL memory objects: that needs
// GL_EXT_memory_object_dma_buf, which neither the NVIDIA driver nor Mesa advertises here.
struct EglImageApi {
    using CreateFn = EGLImageKHR (*)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLAttrib*);
    using DestroyFn = EGLBoolean (*)(EGLDisplay, EGLImageKHR);
    using TargetTextureFn = void (*)(GLenum, GLeglImageOES);

    CreateFn create = nullptr;
    DestroyFn destroy = nullptr;
    TargetTextureFn targetTexture = nullptr;

    bool complete() const { return create != nullptr && destroy != nullptr && targetTexture != nullptr; }
};

const EglImageApi& eglImageApi() {
    static const EglImageApi api = [] {
        EglImageApi resolved;
        // eglCreateImage is core EGL 1.5, and it takes EGLAttrib rather than the KHR form's
        // EGLint. That is not what carries the 64-bit modifier, though: the dma_buf extension
        // splits that across LO/HI attributes precisely so 32-bit values can carry it, which is
        // what the attribute list below does.
        resolved.create = reinterpret_cast<EglImageApi::CreateFn>(eglGetProcAddress("eglCreateImage"));
        resolved.destroy = reinterpret_cast<EglImageApi::DestroyFn>(eglGetProcAddress("eglDestroyImage"));
        resolved.targetTexture =
            reinterpret_cast<EglImageApi::TargetTextureFn>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
        return resolved;
    }();
    return api;
}

bool hasEglExtension(const char* wanted) {
    EGLDisplay display = eglGetCurrentDisplay();
    if (display == EGL_NO_DISPLAY) return false;
    const char* extensions = eglQueryString(display, EGL_EXTENSIONS);
    if (extensions == nullptr) return false;
    // Matched as a whole token: a substring search reports EGL_EXT_image_dma_buf_import present
    // when only EGL_EXT_image_dma_buf_import_modifiers is.
    const size_t length = std::strlen(wanted);
    for (const char* at = std::strstr(extensions, wanted); at != nullptr; at = std::strstr(at + 1, wanted)) {
        const bool startOk = at == extensions || at[-1] == ' ';
        const bool endOk = at[length] == '\0' || at[length] == ' ';
        if (startOk && endOk) return true;
    }
    return false;
}

}  // namespace

bool FrameImport::supports(HandleType type, std::string* whyNot) {
    if (type == HandleType::DmaBuf) {
        if (!hasEglExtension("EGL_EXT_image_dma_buf_import")) {
            setWhyNot(whyNot, "this EGL does not advertise EGL_EXT_image_dma_buf_import");
            return false;
        }
        // The modifier is not optional here: the daemon refuses to export a dma_buf without an
        // explicit one, so an EGL that cannot be told the modifier cannot describe the image.
        if (!hasEglExtension("EGL_EXT_image_dma_buf_import_modifiers")) {
            setWhyNot(whyNot, "this EGL does not advertise EGL_EXT_image_dma_buf_import_modifiers");
            return false;
        }
        if (!hasGlExtension("GL_OES_EGL_image")) {
            setWhyNot(whyNot, "this GL context cannot bind an EGLImage to a texture (no GL_OES_EGL_image)");
            return false;
        }
        if (!eglImageApi().complete()) {
            setWhyNot(whyNot, "the EGLImage entry points could not be resolved");
            return false;
        }
        return true;
    }

    if (!hasGlExtension("GL_EXT_memory_object")) {
        setWhyNot(whyNot, "this GL context does not advertise GL_EXT_memory_object");
        return false;
    }
    if (!hasGlExtension("GL_EXT_memory_object_fd")) {
        setWhyNot(whyNot, "this GL context does not advertise GL_EXT_memory_object_fd");
        return false;
    }
    if (!memoryObjectApi().complete()) {
        setWhyNot(whyNot, "the GL_EXT_memory_object_fd entry points could not be resolved");
        return false;
    }
    return true;
}

// The dma_buf path. The fd, the modifier and the per-plane offsets and pitches describe the
// image to EGL, which hands back an EGLImage that a texture is then pointed at.
//
// Unlike the memory-object path, EGL does NOT take the fd: eglCreateImage dups what it needs.
// So on success this closes the fd itself, which is what makes both paths honour the same
// contract -- a successful import consumes it. On failure it is left for the caller, as there.
FrameImport::Result FrameImport::importDmaBuf(Request& request) {
    Result out;

    if (request.planes.empty()) {
        out.error = "a dma_buf frame carries no plane layout, so EGL cannot be told where the pixels are";
        return out;
    }
    // One plane is what this handles. A compressed modifier on Intel or AMD can carry a second
    // metadata plane, and silently importing only the first would draw the image without its
    // compression metadata -- garbage, not a degraded picture.
    if (request.planes.size() > 1) {
        out.error = "this frame has more than one memory plane, which the importer does not describe yet";
        return out;
    }

    const EglImageApi& api = eglImageApi();
    EGLDisplay display = eglGetCurrentDisplay();
    if (display == EGL_NO_DISPLAY) {
        out.error = "no current EGL display to import against";
        return out;
    }

    const EGLAttrib attributes[] = {
        EGL_WIDTH, static_cast<EGLAttrib>(request.width),
        EGL_HEIGHT, static_cast<EGLAttrib>(request.height),
        EGL_LINUX_DRM_FOURCC_EXT, static_cast<EGLAttrib>(request.drmFormat),
        EGL_DMA_BUF_PLANE0_FD_EXT, static_cast<EGLAttrib>(request.fd),
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, static_cast<EGLAttrib>(request.planes[0].first),
        EGL_DMA_BUF_PLANE0_PITCH_EXT, static_cast<EGLAttrib>(request.planes[0].second),
        // Split across two attributes because the modifier is 64-bit. Passed always, not only
        // when it is non-zero: zero is DRM_FORMAT_MOD_LINEAR, a real layout, and omitting it
        // would let the driver infer a different one.
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, static_cast<EGLAttrib>(request.drmModifier & 0xffffffffu),
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, static_cast<EGLAttrib>(request.drmModifier >> 32),
        EGL_NONE};

    EGLImageKHR image = api.create(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attributes);
    if (image == EGL_NO_IMAGE_KHR) {
        out.error = "eglCreateImage rejected the exported dma_buf";
        return out;
    }

    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    api.targetTexture(GL_TEXTURE_2D, static_cast<GLeglImageOES>(image));
    const bool bound = glGetError() == GL_NO_ERROR;
    glBindTexture(GL_TEXTURE_2D, 0);

    if (!bound) {
        glDeleteTextures(1, &texture);
        api.destroy(display, image);
        out.error = "the EGLImage could not be bound to a texture";
        return out;
    }

    // EGL holds its own duplicate now, so this one is ours to close -- and must be, or the
    // plugin leaks one descriptor per frame for the life of the session.
    ::close(request.fd);
    request.fd = -1;

    out.texture = texture;
    out.eglImage = image;
    out.eglDisplay = display;
    return out;
}

FrameImport::Result FrameImport::import(Request& request) {
    Result out;

    if (request.fd < 0) {
        out.error = "no file descriptor to import";
        return out;
    }
    if (request.width == 0 || request.height == 0) {
        out.error = "the frame has no area to import";
        return out;
    }
    if (request.memorySize == 0) {
        // glImportMemoryFdEXT takes this as THE size of the imported object: too small reads as
        // a smaller buffer, too large fails the import outright.
        out.error = "the frame reports no memory size, which the import needs exactly";
        return out;
    }
    if (request.drmFormat == 0) {
        // Checked ahead of the dma_buf dispatch, not in the swizzle switch below: EGL is handed
        // this fourcc verbatim, and one that tolerates 0 draws garbage.
        out.error = "the frame reports no pixel format, so its channel order is unknown";
        return out;
    }
    if (!supports(request.handleType, &out.error)) {
        return out;
    }

    // Cleared first: a stale error from earlier in this context would otherwise be attributed
    // to whichever call below happens to be checked next.
    while (glGetError() != GL_NO_ERROR) {
    }

    if (request.handleType == HandleType::DmaBuf) return importDmaBuf(request);

    // GL_RGBA8 carries neither the channel order nor the alpha semantics, so both are decoded
    // here and applied as swizzles below. Before anything is allocated, so a format this cannot
    // lay out fails while the fd is still the caller's to close.
    bool swapRedAndBlue = false;
    bool alphaIsUndefined = false;
    switch (request.drmFormat) {
        case kDrmFormatXrgb8888:
            swapRedAndBlue = true;
            alphaIsUndefined = true;
            break;
        case kDrmFormatArgb8888:
            swapRedAndBlue = true;
            break;
        case kDrmFormatXbgr8888:
            alphaIsUndefined = true;
            break;
        case kDrmFormatAbgr8888:
            break;
        default:
            // Laid out as 8-bit RGBA it would import cleanly and draw garbage.
            out.error = "the daemon exported a pixel format this importer cannot lay out";
            return out;
    }

    const MemoryObjectApi& api = memoryObjectApi();

    GLuint memory = 0;
    api.create(1, &memory);
    if (memory == 0 || glGetError() != GL_NO_ERROR) {
        // A name AND an error means the object exists and is ours to destroy; every other
        // failure below already does this, and returning here was the one that did not.
        if (memory != 0) api.destroy(1, &memory);
        out.error = "glCreateMemoryObjectsEXT failed";
        return out;
    }

    // The daemon allocates every exportable image with a DEDICATED allocation -- the memory
    // belongs to that one image rather than being suballocated from a pool -- and the importer
    // has to say so before importing. Without it the import and the texture allocation both
    // succeed and the texture samples as blank: the driver treats the memory as an ordinary
    // pool, so nothing points at the pixels the daemon rendered.
    //
    // Missed at first because the in-process probe passed regardless, which is exactly the
    // shape of bug a same-process test cannot see.
    const GLint dedicated = GL_TRUE;
    api.parameteriv(memory, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
    if (glGetError() != GL_NO_ERROR) {
        api.destroy(1, &memory);
        out.error = "the memory object would not accept GL_DEDICATED_MEMORY_OBJECT_EXT";
        return out;
    }

    // The driver is handed a DUPLICATE and the caller's descriptor never leaves our hands.
    //
    // The spec transfers ownership only on a SUCCESSFUL import and says nothing about failure,
    // and drivers disagree: NVIDIA leaves the descriptor open (measured -- 200 failed imports
    // leaked 197 descriptors), while Mesa closes it on its backend path and leaks it on its
    // argument-validation paths. So "does this driver close on failure" is not even constant
    // within one driver, and nothing can be keyed on it.
    //
    // Duplicating makes the question decidable instead of guessed: the retained original pins
    // the open file, so afterwards the duplicate's identity can still be compared against it.
    const int gift = ::fcntl(request.fd, F_DUPFD_CLOEXEC, 0);
    if (gift < 0) {
        api.destroy(1, &memory);
        out.error = "the frame's descriptor could not be duplicated for import";
        return out;
    }

    api.importFd(memory, static_cast<GLuint64>(request.memorySize), GL_HANDLE_TYPE_OPAQUE_FD_EXT,
                 static_cast<GLint>(gift));
    if (glGetError() != GL_NO_ERROR) {
        // Compared while the original is still open -- it is the operand. A driver that took the
        // duplicate leaves it closed or reused, and either way it is not ours to close.
        if (sameOpenFile(gift, request.fd)) ::close(gift);
        api.destroy(1, &memory);
        out.error = "glImportMemoryFdEXT rejected the exported memory";
        // request.fd is untouched: a failed import never consumes the caller's descriptor now.
        return out;
    }

    // Imported. The driver owns the duplicate; this one was only ever ours.
    ::close(request.fd);
    request.fd = -1;

    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    api.texStorage(GL_TEXTURE_2D, 1, GL_RGBA8, static_cast<GLsizei>(request.width),
                   static_cast<GLsizei>(request.height), memory, 0);
    if (glGetError() != GL_NO_ERROR) {
        glBindTexture(GL_TEXTURE_2D, 0);
        glDeleteTextures(1, &texture);
        api.destroy(1, &memory);
        out.error = "glTexStorageMem2DEXT could not back a texture with the imported memory";
        return out;
    }
    // Swizzled rather than stored as BGRA because glTexStorageMem2DEXT takes a sized internal
    // format and there is no BGRA one; the swizzle is free at sample time.
    if (swapRedAndBlue) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
    }
    if (alphaIsUndefined) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_ONE);
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    out.texture = texture;
    out.memoryObject = memory;
    return out;
}

void FrameImport::release(const Result& imported) {
    if (imported.texture != 0) {
        const GLuint texture = imported.texture;
        glDeleteTextures(1, &texture);
    }
    // Destroyed against the display that created it, not against whatever is current here: an
    // EGLImage outlives its context, and asking the thread would silently skip the destroy and
    // pin the daemon's buffer for the session.
    if (imported.eglImage != nullptr && imported.eglDisplay != nullptr) {
        eglImageApi().destroy(static_cast<EGLDisplay>(imported.eglDisplay),
                              static_cast<EGLImageKHR>(imported.eglImage));
    }
    if (imported.memoryObject != 0) {
        const GLuint memory = imported.memoryObject;
        memoryObjectApi().destroy(1, &memory);
    }
}

}  // namespace wallpaperx
