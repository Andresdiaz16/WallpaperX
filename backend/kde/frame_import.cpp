// SPDX-License-Identifier: GPL-2.0-or-later

#include "frame_import.h"

#include <EGL/egl.h>
#include <GL/gl.h>
#include <GL/glext.h>

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

}  // namespace

bool FrameImport::supports(HandleType type, std::string* whyNot) {
    if (type == HandleType::DmaBuf) {
        // Not implemented rather than not supported, and the two must not be confused: the
        // driver here does advertise EGL_EXT_image_dma_buf_import, so this is a gap in this
        // file. GL_EXT_memory_object_dma_buf -- what the plan originally called for -- is
        // advertised by neither the NVIDIA driver nor llvmpipe on this machine.
        setWhyNot(whyNot, "importing a dma_buf is not implemented yet; it needs the EGLImage path "
                          "(eglCreateImageKHR with EGL_LINUX_DMA_BUF_EXT), not GL memory objects");
        return false;
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

FrameImport::Result FrameImport::import(const Request& request) {
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

    const MemoryObjectApi& api = memoryObjectApi();

    // Cleared first: a stale error from earlier in this context would otherwise be attributed
    // to whichever call below happens to be checked next.
    while (glGetError() != GL_NO_ERROR) {
    }

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
    if (imported.memoryObject != 0) {
        const MemoryObjectApi& api = memoryObjectApi();
        if (api.destroy != nullptr) {
            const GLuint memory = imported.memoryObject;
            api.destroy(1, &memory);
        }
    }
}

}  // namespace wallpaperx
