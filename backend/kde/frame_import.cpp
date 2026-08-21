// SPDX-License-Identifier: GPL-2.0-or-later

#include "frame_import.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>

#include <unistd.h>

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
        return create != nullptr && destroy != nullptr && importFd != nullptr && texStorage != nullptr;
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
bool hasGlExtension(const char* wanted) {
    using GetStringiFn = const GLubyte* (*)(GLenum, GLuint);
    static const auto getStringi = reinterpret_cast<GetStringiFn>(eglGetProcAddress("glGetStringi"));
    if (getStringi == nullptr) return false;

    GLint count = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &count);
    for (GLint i = 0; i < count; ++i) {
        const GLubyte* name = getStringi(GL_EXTENSIONS, static_cast<GLuint>(i));
        if (name != nullptr && std::strcmp(reinterpret_cast<const char*>(name), wanted) == 0) {
            return true;
        }
    }
    return false;
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
        // The EXT spelling, not KHR: it takes EGLAttrib (64-bit), which the modifier halves and
        // plane offsets need. The KHR form's EGLint attributes cannot carry them.
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
    if (!supports(request.handleType, &out.error)) {
        return out;
    }

    // Cleared first: a stale error from earlier in this context would otherwise be attributed
    // to whichever call below happens to be checked next.
    while (glGetError() != GL_NO_ERROR) {
    }

    if (request.handleType == HandleType::DmaBuf) return importDmaBuf(request);

    const MemoryObjectApi& api = memoryObjectApi();

    GLuint memory = 0;
    api.create(1, &memory);
    if (memory == 0 || glGetError() != GL_NO_ERROR) {
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
    if (api.parameteriv != nullptr) {
        const GLint dedicated = GL_TRUE;
        api.parameteriv(memory, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
        if (glGetError() != GL_NO_ERROR) {
            api.destroy(1, &memory);
            out.error = "the memory object would not accept GL_DEDICATED_MEMORY_OBJECT_EXT";
            return out;
        }
    }

    // Takes ownership of the fd on success -- the EXT_memory_object_fd spec is explicit that
    // the implementation closes it -- which is why the caller must not.
    api.importFd(memory, static_cast<GLuint64>(request.memorySize), GL_HANDLE_TYPE_OPAQUE_FD_EXT,
                 static_cast<GLint>(request.fd));
    if (glGetError() != GL_NO_ERROR) {
        api.destroy(1, &memory);
        out.error = "glImportMemoryFdEXT rejected the exported memory";
        return out;
    }
    // Gone from here on, whatever happens below.
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
    // The memory has a channel order and GL_RGBA8 storage does not carry it. The daemon renders
    // B8G8R8A8, so sampling the imported memory as RGBA returns red and blue transposed -- the
    // first cross-process frame came out with a green corner where it should have been red.
    // Swizzled rather than storing as BGRA because glTexStorageMem2DEXT takes a sized internal
    // format and there is no BGRA one; the swizzle is free at sample time.
    //
    // Keyed on the format the daemon reports rather than on the daemon being what it is today:
    // an X/ARGB fourcc is byte order B,G,R,A in memory, and an X/ABGR one is already RGBA.
    constexpr uint32_t kXrgb8888 = 'X' | ('R' << 8) | ('2' << 16) | ('4' << 24);
    constexpr uint32_t kArgb8888 = 'A' | ('R' << 8) | ('2' << 16) | ('4' << 24);
    if (request.drmFormat == kXrgb8888 || request.drmFormat == kArgb8888) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
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
    if (imported.eglImage != nullptr) {
        const EglImageApi& api = eglImageApi();
        EGLDisplay display = eglGetCurrentDisplay();
        if (api.destroy != nullptr && display != EGL_NO_DISPLAY) {
            api.destroy(display, static_cast<EGLImageKHR>(imported.eglImage));
        }
    }
    if (imported.memoryObject != 0) {
        const MemoryObjectApi& api = memoryObjectApi();
        if (api.destroy != nullptr) {
            const GLuint memory = imported.memoryObject;
            api.destroy(1, &memory);
        }
    }
}

}  // namespace wallpaperx
