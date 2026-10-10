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
#include <mutex>
#include <thread>
#include <vector>

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

    // Every semaphore name glCreateSemaphoresNV handed back (the timeline imports), every name passed
    // to glDeleteSemaphoresEXT and every name passed to glWaitSemaphoreEXT, failed waits included,
    // each with the EGL context current at the call.
    struct NamedCall {
        GLuint name;
        EGLContext context;
        // For a creation: semaphoresDeleted's length at that moment, taken under the same lock, so
        // a delete of this name from that index on is a delete of this semaphore, never of an
        // earlier one that held the name.
        size_t deletesBefore = 0;
    };
    std::mutex callsMutex;
    std::vector<NamedCall> timelinesCreated;  // guarded by callsMutex
    std::vector<NamedCall> semaphoresDeleted;  // guarded by callsMutex
    std::vector<NamedCall> semaphoresWaited;  // guarded by callsMutex

    // While recordOrder is set, every wrapped call in the one order the recorder's lock gave them, with
    // the calling thread: what one context did before another touched the same name. glFlush and
    // glFinish are seen only as Qt and the plugin resolve them, through eglGetProcAddress. FrameEnd is
    // no GL call: a harness marks with it where a thread is about to end a scene-graph frame.
    enum class Call { CreateTimeline, Import, SetValue, Wait, Flush, Finish, Delete, FrameEnd };
    struct OrderedCall {
        Call call;
        GLuint name;  // 0 for glFlush and glFinish
        std::thread::id thread;
    };
    std::atomic<bool> recordOrder{false};
    std::vector<OrderedCall> callOrder;  // guarded by callsMutex
    // Set once glFlush or glFinish was handed out wrapped. Where eglGetProcAddress has no core GL
    // names, Qt falls back to dlsym and the recorder never sees either call.
    std::atomic<bool> flushWrapped{false};
    std::atomic<bool> finishWrapped{false};
};

inline GlSemaphoreFaults g_glSemaphoreFaults;

namespace gl_fault_detail {

using ProcAddress = __eglMustCastToProperFunctionPointerType (*)(const char*);
using WaitFn = void (*)(GLuint, GLuint, const GLuint*, GLuint, const GLuint*, const GLenum*);
using CreateFn = void (*)(GLsizei, GLuint*);
using DeleteFn = void (*)(GLsizei, const GLuint*);
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

// Caller holds callsMutex.
inline void orderLocked(GlSemaphoreFaults::Call call, GLuint name) {
    if (g_glSemaphoreFaults.recordOrder.load()) {
        g_glSemaphoreFaults.callOrder.push_back({call, name, std::this_thread::get_id()});
    }
}
inline void order(GlSemaphoreFaults::Call call, GLuint name) {
    std::lock_guard<std::mutex> lock(g_glSemaphoreFaults.callsMutex);
    orderLocked(call, name);
}

// A call that touches a name another context may rely on is recorded before it is made, and one that
// completes something (an import, a flush, a finish) after it returns: the recorded order then never
// puts a completion ahead of a touch it did not precede.
using ImportFdFn = void (*)(GLuint, GLenum, GLint);
using ParameterUi64vFn = void (*)(GLuint, GLenum, const GLuint64*);
using VoidFn = void (*)();

inline void loggingImport(GLuint semaphore, GLenum handleType, GLint fd) {
    static const ImportFdFn real = realGl<ImportFdFn>("glImportSemaphoreFdEXT");
    if (real != nullptr) real(semaphore, handleType, fd);
    order(GlSemaphoreFaults::Call::Import, semaphore);
}

inline void loggingSetValue(GLuint semaphore, GLenum pname, const GLuint64* value) {
    static const ParameterUi64vFn real = realGl<ParameterUi64vFn>("glSemaphoreParameterui64vEXT");
    order(GlSemaphoreFaults::Call::SetValue, semaphore);
    if (real != nullptr) real(semaphore, pname, value);
}

inline void loggingFlush() {
    static const VoidFn real = realGl<VoidFn>("glFlush");
    if (real != nullptr) real();
    order(GlSemaphoreFaults::Call::Flush, 0);
}

inline void loggingFinish() {
    static const VoidFn real = realGl<VoidFn>("glFinish");
    if (real != nullptr) real();
    order(GlSemaphoreFaults::Call::Finish, 0);
}

inline void faultingWait(GLuint semaphore, GLuint buffers, const GLuint* bufferList, GLuint textures,
                         const GLuint* textureList, const GLenum* layouts) {
    GlSemaphoreFaults& faults = g_glSemaphoreFaults;
    faults.waitsSeen.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(faults.callsMutex);
        faults.semaphoresWaited.push_back({semaphore, eglGetCurrentContext()});
        orderLocked(GlSemaphoreFaults::Call::Wait, semaphore);
    }
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
    if (real == nullptr) return;
    real(n, semaphores);
    std::lock_guard<std::mutex> lock(g_glSemaphoreFaults.callsMutex);
    for (GLsizei i = 0; i < n; ++i) {
        g_glSemaphoreFaults.timelinesCreated.push_back(
            {semaphores[i], eglGetCurrentContext(), g_glSemaphoreFaults.semaphoresDeleted.size()});
        orderLocked(GlSemaphoreFaults::Call::CreateTimeline, semaphores[i]);
    }
}

inline void loggingDelete(GLsizei n, const GLuint* semaphores) {
    static const DeleteFn real = realGl<DeleteFn>("glDeleteSemaphoresEXT");
    {
        std::lock_guard<std::mutex> lock(g_glSemaphoreFaults.callsMutex);
        for (GLsizei i = 0; i < n; ++i) {
            g_glSemaphoreFaults.semaphoresDeleted.push_back({semaphores[i], eglGetCurrentContext()});
            orderLocked(GlSemaphoreFaults::Call::Delete, semaphores[i]);
        }
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
        } else if (std::strcmp(name, "glDeleteSemaphoresEXT") == 0) {
            wrapper = reinterpret_cast<__eglMustCastToProperFunctionPointerType>(&detail::loggingDelete);
        } else if (std::strcmp(name, "glImportSemaphoreFdEXT") == 0) {
            wrapper = reinterpret_cast<__eglMustCastToProperFunctionPointerType>(&detail::loggingImport);
        } else if (std::strcmp(name, "glSemaphoreParameterui64vEXT") == 0) {
            wrapper = reinterpret_cast<__eglMustCastToProperFunctionPointerType>(&detail::loggingSetValue);
        }
        const bool core = std::strcmp(name, "glFlush") == 0 || std::strcmp(name, "glFinish") == 0;
        if (core) {
            wrapper = reinterpret_cast<__eglMustCastToProperFunctionPointerType>(
                std::strcmp(name, "glFlush") == 0 ? &detail::loggingFlush : &detail::loggingFinish);
        }
        // Only where the driver has the real one: a wrapper for a name the driver lacks would make
        // the plugin believe in a capability this host does not have.
        if (wrapper != nullptr) {
            if (detail::realProcAddress() == nullptr || detail::realProcAddress()(name) == nullptr) return nullptr;
            wallpaperx::testing::GlSemaphoreFaults& faults = wallpaperx::testing::g_glSemaphoreFaults;
            if (!core) {
                faults.resolvedThroughHere.store(true);
            } else if (std::strcmp(name, "glFlush") == 0) {
                faults.flushWrapped.store(true);
            } else {
                faults.finishWrapped.store(true);
            }
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
