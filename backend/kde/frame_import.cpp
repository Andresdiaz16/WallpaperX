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
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <optional>
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
    using GetBytesFn = void (*)(GLenum, GLubyte*);

    CreateFn create = nullptr;
    DeleteFn destroy = nullptr;
    ImportFdFn importFd = nullptr;
    ParameterivFn parameteriv = nullptr;
    TexStorageFn texStorage = nullptr;
    GetBytesFn getBytes = nullptr;

    bool complete() const {
        // parameteriv included: without it the dedicated flag cannot be set, and an import that
        // skips the flag succeeds and samples blank.
        //
        // getBytes included so the device check cannot be skipped. GL_EXT_memory_object mandates
        // it, so absent here means a driver advertising the extension without implementing it.
        return create != nullptr && destroy != nullptr && importFd != nullptr && parameteriv != nullptr &&
               texStorage != nullptr && getBytes != nullptr;
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
        resolved.getBytes =
            reinterpret_cast<MemoryObjectApi::GetBytesFn>(eglGetProcAddress("glGetUnsignedBytevEXT"));
        return resolved;
    }();
    return api;
}

// The GL_EXT_semaphore / GL_EXT_semaphore_fd side, resolved once like the memory-object side and
// for the same reason. glWaitSemaphoreEXT is the only thing in OpenGL that can express a
// queue-family acquire and a layout transition, which is why the whole ownership handover hangs
// off these four entry points.
struct SemaphoreApi {
    using GenFn = void (*)(GLsizei, GLuint*);
    using DeleteFn = void (*)(GLsizei, const GLuint*);
    using ImportFdFn = void (*)(GLuint, GLenum, GLint);
    using WaitFn = void (*)(GLuint, GLuint, const GLuint*, GLuint, const GLuint*, const GLenum*);

    GenFn generate = nullptr;
    DeleteFn destroy = nullptr;
    ImportFdFn importFd = nullptr;
    WaitFn wait = nullptr;

    bool complete() const {
        return generate != nullptr && destroy != nullptr && importFd != nullptr && wait != nullptr;
    }
};

const SemaphoreApi& semaphoreApi() {
    static const SemaphoreApi api = [] {
        SemaphoreApi resolved;
        resolved.generate = reinterpret_cast<SemaphoreApi::GenFn>(eglGetProcAddress("glGenSemaphoresEXT"));
        resolved.destroy = reinterpret_cast<SemaphoreApi::DeleteFn>(eglGetProcAddress("glDeleteSemaphoresEXT"));
        resolved.importFd = reinterpret_cast<SemaphoreApi::ImportFdFn>(eglGetProcAddress("glImportSemaphoreFdEXT"));
        resolved.wait = reinterpret_cast<SemaphoreApi::WaitFn>(eglGetProcAddress("glWaitSemaphoreEXT"));
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
// belongs to something else.
//
// There is NO fstat fallback, and that is the point. Every opaque export from this driver shares
// one anonymous inode, so fstat reports "same file" for descriptors naming different frames --
// measured, see PLAN.md. Using it where kcmp is unavailable would answer "still yours" about a
// descriptor belonging to another frame, and closing that inside plasmashell frees a number
// another thread is using.
//
// Answering "not ours" instead leaks one descriptor per failed import. Where kcmp cannot answer
// at all -- no CONFIG_KCMP, or a seccomp policy returning EPERM -- that repeats for every refusal,
// so a budget bounds it and supports() refuses the path once spent.
//
// A budget rather than a latch, and NEVER cleared: the count only rises on a host where kcmp
// cannot answer, which is a property of the kernel build or the seccomp policy and does not change
// while the process runs. Clearing it on a successful import would key it on evidence about a
// different question and leave the leak unbounded, since successes are the norm even there.
//
// 16 is what this pays to survive the sporadic refusal that a latch killed the session for. Once
// spent, the path is refused for good and the desktop says why.
constexpr int kAbandonBudget = 16;
std::atomic<int> g_abandonedDescriptors{0};

// Shared by the memory path and the sync path: a refused semaphore import has the same
// undecidable descriptor as a refused memory import, and is answered the same way.
constexpr const char* kKcmpBudgetSpent =
    "this kernel will not answer kcmp, so refused imports cannot reclaim their descriptors";

bool kcmpBudgetSpent() { return g_abandonedDescriptors.load(std::memory_order_relaxed) >= kAbandonBudget; }

// An imported texture has one level, and the default minification filter wants a mipmap chain,
// which leaves it incomplete and sampling black. MAX_LEVEL is the fix; the filters keep the two
// import paths delivering textures that sample alike. Call with the texture bound.
void completeImportedTexture() {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
}

// Hex, so a device mismatch can name the two GPUs instead of only asserting they differ.
std::string uuidText(const uint8_t* bytes, size_t count) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(count * 2);
    for (size_t i = 0; i < count; ++i) {
        out.push_back(kHex[bytes[i] >> 4]);
        out.push_back(kHex[bytes[i] & 0xf]);
    }
    return out;
}

bool sameOpenFile(int candidate, int reference) {
    const long compared = ::syscall(SYS_kcmp, ::getpid(), ::getpid(), KCMP_FILE, candidate, reference);
    if (compared >= 0) return compared == 0;
    if (errno == ENOSYS || errno == EPERM) g_abandonedDescriptors.fetch_add(1, std::memory_order_relaxed);
    return false;
}

// One value with two named states, rather than a bool beside a reason that can disagree with it:
// nullptr is "this context can", anything else is why it cannot.
bool granted(const char* reason, std::string* whyNot) {
    if (reason == nullptr) return true;
    if (whyNot != nullptr) *whyNot = reason;
    return false;
}

// The EGLImage side, for dma_buf. A dma_buf cannot go through GL memory objects: that needs
// GL_EXT_memory_object_dma_buf, which neither the NVIDIA driver nor Mesa advertises here.
struct EglImageApi {
    using CreateFn = EGLImageKHR (*)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLAttrib*);
    using DestroyFn = EGLBoolean (*)(EGLDisplay, EGLImageKHR);
    using TargetTextureFn = void (*)(GLenum, GLeglImageOES);
    using QueryModifiersFn = EGLBoolean (*)(EGLDisplay, EGLint, EGLint, EGLuint64KHR*, EGLBoolean*, EGLint*);

    CreateFn create = nullptr;
    DestroyFn destroy = nullptr;
    TargetTextureFn targetTexture = nullptr;

    // Not part of complete(): importing works without it, and only the modifier report needs
    // it. Folding it in would refuse the whole path on a driver missing just the query.
    QueryModifiersFn queryModifiers = nullptr;

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
        resolved.queryModifiers =
            reinterpret_cast<EglImageApi::QueryModifiersFn>(eglGetProcAddress("eglQueryDmaBufModifiersEXT"));
        return resolved;
    }();
    return api;
}

// Takes the display rather than fetching one, so a false can only ever mean "this display does
// not advertise it" -- answering for a missing display blamed the driver for an extension it has.
bool hasEglExtension(EGLDisplay display, const char* wanted) {
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

// Everything supports(), supportsSync() and importableModifiers() decide that a context cannot
// change its mind about: which extensions it advertises and whether the entry points resolve.
// Each field carries the whole answer in `granted`'s form.
//
// The one place that asks the driver these questions. Anything else deriving its own answer from
// the extension strings is a second copy of this that nothing keeps in step.
struct ContextCapabilities {
    // The display the EGL answers below were asked of, EGL_NO_DISPLAY when the thread had none.
    // Carried rather than re-read by the callers: a second eglGetCurrentDisplay() would be a
    // separate observation that can disagree with the one these answers describe.
    EGLDisplay display = EGL_NO_DISPLAY;

    const char* dmaBuf = nullptr;
    const char* opaqueFd = nullptr;
    const char* sync = nullptr;
    // Whether the layouts this context can import are enumerable, which is not the same question
    // as whether it can import: the query is a separate entry point, and the two refusals cost
    // different things -- no dma_buf frames at all, versus a daemon left on its unnegotiated
    // default. So each carries its own reason rather than sharing one.
    const char* modifierQuery = nullptr;
};

ContextCapabilities probeCapabilities(EGLDisplay display) {
    ContextCapabilities out;
    out.display = display;

    if (display == EGL_NO_DISPLAY) {
        // Not the same fact as a missing extension, and the difference sends someone to a
        // different place: a GLX-backed Qt -- the default on X11 -- has no current EGL display
        // even with a live GL context, so the driver may advertise both extensions below to a
        // display this thread simply does not hold.
        out.dmaBuf = "no current EGL display: this GL context is not EGL-backed, so nothing here can import a dma_buf";
        out.modifierQuery =
            "no current EGL display: this GL context is not EGL-backed, so nothing can be asked which layouts "
            "it imports";
    } else {
        // Asked once and answered for two capabilities below.
        const bool modifiersAdvertised = hasEglExtension(display, "EGL_EXT_image_dma_buf_import_modifiers");

        if (!hasEglExtension(display, "EGL_EXT_image_dma_buf_import")) {
            out.dmaBuf = "this EGL does not advertise EGL_EXT_image_dma_buf_import";
        } else if (!modifiersAdvertised) {
            // The modifier is not optional here: the daemon refuses to export a dma_buf without an
            // explicit one, so an EGL that cannot be told the modifier cannot describe the image.
            out.dmaBuf = "this EGL does not advertise EGL_EXT_image_dma_buf_import_modifiers";
        } else if (!hasGlExtension("GL_OES_EGL_image")) {
            out.dmaBuf = "this GL context cannot bind an EGLImage to a texture (no GL_OES_EGL_image)";
        } else if (!eglImageApi().complete()) {
            out.dmaBuf = "the EGLImage entry points could not be resolved";
        }

        if (!modifiersAdvertised) {
            out.modifierQuery = "this driver does not advertise EGL_EXT_image_dma_buf_import_modifiers";
        } else if (eglImageApi().queryModifiers == nullptr) {
            out.modifierQuery = "eglQueryDmaBufModifiersEXT could not be resolved";
        }
    }

    // The opaque-fd and sync answers below read the GL context, not EGL, so they stand on their
    // own where there is no EGL display -- which is exactly where the opaque path is what runs.

    if (!hasGlExtension("GL_EXT_memory_object")) {
        out.opaqueFd = "this GL context does not advertise GL_EXT_memory_object";
    } else if (!hasGlExtension("GL_EXT_memory_object_fd")) {
        out.opaqueFd = "this GL context does not advertise GL_EXT_memory_object_fd";
    } else if (!memoryObjectApi().complete()) {
        out.opaqueFd = "the GL_EXT_memory_object_fd entry points could not be resolved";
    }

    if (!hasGlExtension("GL_EXT_semaphore")) {
        out.sync = "this GL context does not advertise GL_EXT_semaphore";
    } else if (!hasGlExtension("GL_EXT_semaphore_fd")) {
        out.sync = "this GL context does not advertise GL_EXT_semaphore_fd";
    } else if (!semaphoreApi().complete()) {
        out.sync = "the GL_EXT_semaphore_fd entry points could not be resolved";
    }

    return out;
}

// Cached per CONTEXT and never per process: plasmashell can hold several, and what one advertises
// is not an answer for another. Worth caching at all because walking the driver's extension list
// costs microseconds and the render thread asks these questions for every frame.
//
// The key is the current EGL display, the EGL context handle AND the number of GL extensions,
// because none alone identifies a context here. Under a GLX-backed Qt -- the default on X11 --
// eglGetCurrentContext() is EGL_NO_CONTEXT for every context, so the handle discriminates
// nothing; and eglDestroyContext frees the handle, so a later context can be handed the same
// address. The count moves with the driver, the API version and the profile, which between them
// decide every answer above. What it cannot separate is two contexts advertising different lists
// of the same length.
//
// The display is in the key so the refusals cached for a thread that had none are dropped the
// moment it acquires one, rather than outliving the state that produced them.
//
// With no context current the count reads 0 and every answer is a refusal, which is the truth --
// so that state needs no special case, and is cached like any other.
//
// thread_local rather than a locked table: a context is current on one thread at a time, so the
// key check that keeps this correct is also what keeps it lock-free on the compositor's render
// thread. Returned by value so a recompute cannot invalidate a reference a caller still holds.
ContextCapabilities capabilities() {
    struct Cached {
        EGLContext context;
        GLint extensionCount;
        // The display it was probed against lives in `answers`, so there is one copy of it.
        ContextCapabilities answers;
    };
    thread_local std::optional<Cached> cached;

    const EGLDisplay display = eglGetCurrentDisplay();
    const EGLContext context = eglGetCurrentContext();
    GLint extensionCount = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &extensionCount);

    if (!cached.has_value() || cached->answers.display != display || cached->context != context ||
        cached->extensionCount != extensionCount) {
        cached = Cached{context, extensionCount, probeCapabilities(display)};
    }
    return cached->answers;
}

}  // namespace

bool FrameImport::supports(HandleType type, std::string* whyNot) {
    if (type == HandleType::DmaBuf) return granted(capabilities().dmaBuf, whyNot);

    // Outside the cache, deliberately: this one is spent at runtime by sameOpenFile, when kcmp
    // cannot answer and a refused import cannot work out which descriptor the driver kept.
    if (kcmpBudgetSpent()) return granted(kKcmpBudgetSpent, whyNot);
    return granted(capabilities().opaqueFd, whyNot);
}

bool FrameImport::supportsSync(std::string* whyNot) {
    if (kcmpBudgetSpent()) return granted(kKcmpBudgetSpent, whyNot);
    return granted(capabilities().sync, whyNot);
}

// GL_LAYOUT_GENERAL_EXT because that is what the producer's release transitioned to. OpenGL has
// no token meaning "whatever Vulkan last used", so the two sides naming the same layout is the
// whole of the agreement -- and nothing checks it: an ownership transfer's halves are invisible
// to the validation layers.
bool FrameImport::acquire(Request& request, unsigned int texture, std::string& error) {
    const SemaphoreApi& api = semaphoreApi();

    for (int& syncFd : request.syncFds) {
        if (syncFd < 0) continue;

        GLuint semaphore = 0;
        api.generate(1, &semaphore);
        if (semaphore == 0 || glGetError() != GL_NO_ERROR) {
            if (semaphore != 0) api.destroy(1, &semaphore);
            error = "glGenSemaphoresEXT failed";
            return false;
        }

        // Duplicated for the same undecidable reason as the memory import: the extension transfers
        // the descriptor on success and says nothing about failure, so the original is retained to
        // give kcmp something to compare against afterwards.
        const int gift = ::fcntl(syncFd, F_DUPFD_CLOEXEC, 0);
        if (gift < 0) {
            api.destroy(1, &semaphore);
            error = "the frame's sync descriptor could not be duplicated for import";
            return false;
        }

        api.importFd(semaphore, GL_HANDLE_TYPE_OPAQUE_FD_EXT, static_cast<GLint>(gift));
        if (glGetError() != GL_NO_ERROR) {
            api.destroy(1, &semaphore);
            if (sameOpenFile(gift, syncFd)) ::close(gift);
            error = "glImportSemaphoreFdEXT rejected the frame's sync descriptor";
            return false;
        }

        const GLuint waited = texture;
        const GLenum layout = GL_LAYOUT_GENERAL_EXT;
        api.wait(semaphore, 0, nullptr, 1, &waited, &layout);
        const bool ok = glGetError() == GL_NO_ERROR;

        // Deleted straight after: GL keeps a deleted object alive until the commands referring to
        // it have run, so the queued wait is unaffected.
        api.destroy(1, &semaphore);
        ::close(syncFd);
        syncFd = -1;

        if (!ok) {
            error = "glWaitSemaphoreEXT would not acquire the frame";
            return false;
        }
    }
    return true;
}

// The dma_buf path. The fd, the modifier and the per-plane offsets and pitches describe the
// image to EGL, which hands back an EGLImage that a texture is then pointed at.
//
// Unlike the memory-object path, EGL does NOT take the fd: eglCreateImage dups what it needs.
// So on success this closes the fd itself, which is what makes both paths honour the same
// contract -- a successful import consumes it. On failure it is left for the caller, as there.
//
// `displayHandle` comes from the grant that established there is a display at all, rather than
// from a fresh lookup here that could answer differently with nowhere to report it.
FrameImport::Result FrameImport::importDmaBuf(Request& request, void* displayHandle) {
    Result out;

    if (request.planes.empty()) {
        out.error = "a dma_buf frame carries no plane layout, so EGL cannot be told where the pixels are";
        return out;
    }
    // The same limit the exporter selects modifiers against, so the two cannot disagree about
    // what is exportable. Importing only the first plane of a compressed layout would draw the
    // image without its compression metadata -- garbage, not a degraded picture.
    if (request.planes.size() > kMaxExportedPlanes) {
        out.error = "this frame has more than one memory plane, which the importer does not describe yet";
        return out;
    }

    const EglImageApi& api = eglImageApi();
    const EGLDisplay display = static_cast<EGLDisplay>(displayHandle);

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
    if (bound) completeImportedTexture();
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

std::vector<uint64_t> FrameImport::importableModifiers(uint32_t drmFormat, std::string* whyNot) {
    const auto refuse = [whyNot](const char* reason) {
        if (whyNot != nullptr) *whyNot = reason;
        return std::vector<uint64_t>{};
    };

    const ContextCapabilities capable = capabilities();
    if (!granted(capable.modifierQuery, whyNot)) return {};

    // Granted implies a display: the probe refuses this capability outright when it had none.
    const EGLDisplay display = capable.display;
    const EglImageApi& api = eglImageApi();

    // Two calls: the first asks how many there are, the second fills them in. Asking for a
    // count of 0 with null buffers is how the extension reports the size.
    EGLint count = 0;
    if (api.queryModifiers(display, static_cast<EGLint>(drmFormat), 0, nullptr, nullptr, &count) != EGL_TRUE) {
        return refuse("eglQueryDmaBufModifiersEXT refused to report a count for this format");
    }
    if (count <= 0) {
        return refuse("this driver imports no modifier for the format the daemon exports");
    }

    std::vector<EGLuint64KHR> raw(static_cast<size_t>(count));
    std::vector<EGLBoolean> externalOnly(static_cast<size_t>(count));
    if (api.queryModifiers(display, static_cast<EGLint>(drmFormat), count, raw.data(), externalOnly.data(), &count) !=
        EGL_TRUE) {
        return refuse("eglQueryDmaBufModifiersEXT refused to list the modifiers");
    }
    // Only shrinks: growing would add zeros, and zero is LINEAR -- a layout never listed.
    const size_t written = count < 0 ? 0 : static_cast<size_t>(count);
    raw.resize(std::min(written, raw.size()));
    externalOnly.resize(raw.size());

    std::vector<uint64_t> usable;
    usable.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        if (externalOnly[i] == EGL_TRUE) continue;
        usable.push_back(static_cast<uint64_t>(raw[i]));
    }
    if (usable.empty()) {
        return refuse("every modifier this driver imports for that format is external-only");
    }
    return usable;
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
    // Checked before anything is allocated, so the caller's descriptors are all still its own.
    const bool mustAcquire = std::any_of(request.syncFds.begin(), request.syncFds.end(),
                                         [](int syncFd) { return syncFd >= 0; });
    if (mustAcquire && !supportsSync(&out.error)) {
        return out;
    }

    // Cleared first: a stale error from earlier in this context would otherwise be attributed
    // to whichever call below happens to be checked next.
    while (glGetError() != GL_NO_ERROR) {
    }

    out = request.handleType == HandleType::DmaBuf ? importDmaBuf(request, capabilities().display)
                                                   : importOpaqueFd(request);
    if (out.texture == 0) return out;

    if (mustAcquire) {
        std::string error;
        if (!acquire(request, out.texture, error)) {
            // The texture is sound but was never acquired, and handing it back would sample memory
            // whose ownership transfer is half done -- exactly what this exists to prevent.
            release(out);
            Result refused;
            refused.error = std::move(error);
            return refused;
        }
    }
    return out;
}

FrameImport::Result FrameImport::importOpaqueFd(Request& request) {
    Result out;

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

    // An opaque fd names memory on one particular GPU and is importable on no other. The way
    // this is reached in the field is a hybrid-graphics machine: the daemon prefers the discrete
    // card while plasmashell's context is on the integrated one, so every frame fails and the
    // driver's refusal names neither device.
    if (request.deviceUuid.size() != GL_UUID_SIZE_EXT) {
        out.error = "the frame does not say which GPU it was allocated on";
        return out;
    }
    std::array<GLubyte, GL_UUID_SIZE_EXT> contextUuid{};
    api.getBytes(GL_DEVICE_UUID_EXT, contextUuid.data());
    if (glGetError() != GL_NO_ERROR) {
        out.error = "this GL context would not say which GPU it runs on";
        return out;
    }
    if (!std::equal(contextUuid.begin(), contextUuid.end(), request.deviceUuid.begin())) {
        out.error = "the frame was rendered on GPU " + uuidText(request.deviceUuid.data(), request.deviceUuid.size()) +
                    " but the desktop draws on GPU " + uuidText(contextUuid.data(), contextUuid.size()) +
                    "; this frame cannot cross devices";
        return out;
    }

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
    // and drivers disagree: NVIDIA leaves the descriptor open, while Mesa closes it on its
    // backend path and leaks it on its argument-validation paths. So "does this driver close on
    // failure" is not even constant within one driver, and nothing can be keyed on it.
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
        // Destroyed first: a driver that took the descriptor on the failing call may release it
        // with the object, and closing before that frees a number the driver then closes again.
        api.destroy(1, &memory);
        // Compared AFTER the destroy and immediately before the close, never cached across it:
        // the destroy is what can change the answer. Another thread can take the freed number in
        // that window, but it cannot make this compare equal -- kcmp answers true only for the
        // same open file description as request.fd, and nothing outside this call holds that.
        if (sameOpenFile(gift, request.fd)) ::close(gift);
        out.error = kImportRejectedError;
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
    // Complete already, since glTexStorageMem2DEXT makes the texture immutable with one level.
    // Set anyway so both paths hand back a texture that samples the same way.
    completeImportedTexture();
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
