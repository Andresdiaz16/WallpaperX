// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Failing the plugin's GL semaphore calls from the test binary, without the driver refusing an
// import. A refused glImportSemaphoreFdEXT is the one failure after which the plugin gives up the
// producer's hand-over for the whole context, so a test that needs a hand-over to FAIL -- and to
// go on failing, or to fail once and recover -- must fail something else: the wait, or the
// timeline's setup.
//
// The plugin resolves these entry points through eglGetProcAddress, so this defines
// eglGetProcAddress in the executable, hands back a wrapper for the few names below and forwards
// everything else; and glGetStringi, to hide one extension in one context. The executable must
// export each symbol it relies on (--export-dynamic-symbol in its CMake entry), or the plugin,
// loaded at runtime, binds the driver's. Include from exactly one translation unit of an
// executable. Nothing in backend/kde changes.
//
// The plugin caches what it resolved for the life of the process, so each wrapper reads its
// fault per call. Armed and read on the thread that renders, which in these harnesses is the one
// that pumps the scene; atomics only so a stray call from another thread is not a data race.

#include <EGL/egl.h>
#include <GL/gl.h>

#include <dlfcn.h>

#include <atomic>
#include <cstring>

namespace wallpaperx::testing {

struct GlSemaphoreFaults {
    // The next n glWaitSemaphoreEXT calls queue no wait and raise GL_INVALID_ENUM instead: a
    // hand-over whose semaphore imported and whose wait failed.
    std::atomic<int> failWaits{0};
    std::atomic<int> waitsFailed{0};
    std::atomic<int> waitsSeen{0};
    // glCreateSemaphoresNV hands back no name, and glSemaphoreParameterivNV is given a type the
    // driver rejects -- the timeline's setup failing before any import is attempted.
    std::atomic<bool> failTimelineCreate{false};
    std::atomic<bool> failTimelineType{false};
    std::atomic<int> timelineSetupsFailed{0};
    // Set once the plugin asked for any wrapped name, so a harness can tell "the fault never
    // fired" from "the interposer never landed".
    std::atomic<bool> resolvedThroughHere{false};
    // The EGL context in which GL_NV_timeline_semaphore is hidden from the extension list, as on a
    // driver without it: a second context in the same process that cannot wait on a timeline.
    // Needs glGetStringi exported as well; EGL_NO_CONTEXT hides nothing.
    std::atomic<EGLContext> hideTimelineIn{EGL_NO_CONTEXT};
};

inline GlSemaphoreFaults g_glSemaphoreFaults;

namespace gl_fault_detail {

using ProcAddress = __eglMustCastToProperFunctionPointerType (*)(const char*);
using WaitFn = void (*)(GLuint, GLuint, const GLuint*, GLuint, const GLuint*, const GLenum*);
using CreateFn = void (*)(GLsizei, GLuint*);
using ParameterivFn = void (*)(GLuint, GLenum, const GLint*);

inline ProcAddress realProcAddress() {
    static const ProcAddress real = reinterpret_cast<ProcAddress>(::dlsym(RTLD_NEXT, "eglGetProcAddress"));
    return real;
}

template <class Fn>
Fn realGl(const char* name) {
    const ProcAddress real = realProcAddress();
    return real == nullptr ? nullptr : reinterpret_cast<Fn>(real(name));
}

// Any call the driver rejects will do; this one changes no state.
inline void raiseGlError() { glEnable(0xFFFFu); }

inline void faultingWait(GLuint semaphore, GLuint buffers, const GLuint* bufferList, GLuint textures,
                         const GLuint* textureList, const GLenum* layouts) {
    GlSemaphoreFaults& faults = g_glSemaphoreFaults;
    faults.waitsSeen.fetch_add(1);
    int armed = faults.failWaits.load();
    while (armed > 0 && !faults.failWaits.compare_exchange_weak(armed, armed - 1)) {
    }
    if (armed > 0) {
        faults.waitsFailed.fetch_add(1);
        raiseGlError();
        return;
    }
    static const WaitFn real = realGl<WaitFn>("glWaitSemaphoreEXT");
    if (real != nullptr) real(semaphore, buffers, bufferList, textures, textureList, layouts);
}

inline void faultingCreate(GLsizei n, GLuint* semaphores) {
    static const CreateFn real = realGl<CreateFn>("glCreateSemaphoresNV");
    if (g_glSemaphoreFaults.failTimelineCreate.load()) {
        g_glSemaphoreFaults.timelineSetupsFailed.fetch_add(1);
        for (GLsizei i = 0; i < n; ++i) semaphores[i] = 0;
        return;
    }
    if (real != nullptr) real(n, semaphores);
}

inline void faultingParameteriv(GLuint semaphore, GLenum name, const GLint* value) {
    static const ParameterivFn real = realGl<ParameterivFn>("glSemaphoreParameterivNV");
    if (real == nullptr) return;
    if (g_glSemaphoreFaults.failTimelineType.load()) {
        g_glSemaphoreFaults.timelineSetupsFailed.fetch_add(1);
        const GLint notAType = 0x7fff;
        real(semaphore, name, &notAType);
        return;
    }
    real(semaphore, name, value);
}

}  // namespace gl_fault_detail

}  // namespace wallpaperx::testing

extern "C" __eglMustCastToProperFunctionPointerType eglGetProcAddress(const char* name) {
    namespace detail = wallpaperx::testing::gl_fault_detail;
    if (name != nullptr) {
        __eglMustCastToProperFunctionPointerType wrapper = nullptr;
        if (std::strcmp(name, "glWaitSemaphoreEXT") == 0) {
            wrapper = reinterpret_cast<__eglMustCastToProperFunctionPointerType>(&detail::faultingWait);
        } else if (std::strcmp(name, "glCreateSemaphoresNV") == 0) {
            wrapper = reinterpret_cast<__eglMustCastToProperFunctionPointerType>(&detail::faultingCreate);
        } else if (std::strcmp(name, "glSemaphoreParameterivNV") == 0) {
            wrapper = reinterpret_cast<__eglMustCastToProperFunctionPointerType>(&detail::faultingParameteriv);
        }
        // Only where the driver has the real one: a wrapper for a name the driver lacks would make
        // the plugin believe in a capability this host does not have.
        if (wrapper != nullptr) {
            if (detail::realProcAddress() == nullptr || detail::realProcAddress()(name) == nullptr) return nullptr;
            wallpaperx::testing::g_glSemaphoreFaults.resolvedThroughHere.store(true);
            return wrapper;
        }
    }
    const detail::ProcAddress real = detail::realProcAddress();
    return real == nullptr ? nullptr : real(name);
}

// The plugin walks the extension list with glGetStringi, which it calls as a dynamic symbol. Renamed
// rather than dropped, so the count the plugin reads first still matches the entries it walks.
extern "C" const GLubyte* glGetStringi(GLenum name, GLuint index) {
    using Fn = const GLubyte* (*)(GLenum, GLuint);
    static const Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "glGetStringi"));
    if (real == nullptr) return nullptr;
    const GLubyte* answer = real(name, index);
    const EGLContext hidden = wallpaperx::testing::g_glSemaphoreFaults.hideTimelineIn.load();
    if (name == GL_EXTENSIONS && hidden != EGL_NO_CONTEXT && answer != nullptr && eglGetCurrentContext() == hidden &&
        std::strcmp(reinterpret_cast<const char*>(answer), "GL_NV_timeline_semaphore") == 0) {
        return reinterpret_cast<const GLubyte*>("GL_WALLPAPERX_hidden_timeline_semaphore");
    }
    return answer;
}
