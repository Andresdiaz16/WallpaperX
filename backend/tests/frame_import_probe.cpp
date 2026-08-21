// SPDX-License-Identifier: GPL-2.0-or-later

// S12.1's import half: a frame the renderer exported becomes a GL texture whose pixels are the
// ones that were rendered. The export half already has frame_export_probe, which maps the fd on
// the CPU; this drives the same frame through the path the compositor actually uses.
//
// The oracle is the same frame read two ways -- Vulkan reads the target back, GL reads the
// imported texture back, and the two must agree byte for byte. That is stronger than comparing
// against a constant: a stride mistake, a transposed read or a wrong format all survive "is it
// roughly the right colour" and none of them survive this.
//
// WHY IT CAN SKIP: an opaque fd is importable only by the driver that produced it. This binary
// pairs a Vulkan device with a GL context, and there is no guarantee the two resolve to the same
// driver -- a headless EGL context on this machine comes up on llvmpipe even where Vulkan
// resolves to NVIDIA. When they do not match, or when the context lacks the memory-object
// extensions, this reports NOT VERIFIED rather than failing: an unimportable pairing says
// nothing about whether the importer is correct.

#include "frame_import.h"

#include "support/gpu_readback.h"
#include "support/test_image.h"
#include "support/validation_grade.h"
#include "tests/test_report.h"
#include "vulkan/device.h"
#include "vulkan/frame_export.h"
#include "vulkan/offscreen.h"
#include "renderers/static_renderer.h"

// The framebuffer entry points are GL 3.0 core, and gl.h declares only GL 1.x unless this
// is set. Fine for a probe linked against libGL; the importer itself resolves through
// eglGetProcAddress instead, because the plugin runs inside a context it does not own.
#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

namespace {

using wallpaperx::testing::check;
using wallpaperx::testing::notVerified;
using wallpaperx::testing::renderReference;
using wallpaperx::testing::variesInBothAxes;

constexpr const char* kGoals = "S12.1, S12.2 (a frame the daemon exported reaches the screen as the pixels that "
                               "were rendered, imported by the path the driver actually supports)";

// Not square, and not a round number of pixels wide, for the reason frame_export_probe gives:
// a square target passes with width and height transposed, and a power-of-two width hides a
// stride that ignores padding.
constexpr uint32_t kWidth = 68;
constexpr uint32_t kHeight = 36;

// A surfaceless EGL context, owned, created on a NAMED device rather than on whatever
// EGL_DEFAULT_DISPLAY resolves to.
//
// This matters and was measured: the default display comes up on llvmpipe here even though
// Vulkan resolves to NVIDIA, and an opaque fd is importable only by the driver that produced
// it. Both mismatched pairings were tried and both fail at glTexStorageMem2DEXT -- including
// lavapipe into llvmpipe, which are both Mesa and both software.
//
// So the device is chosen by UUID: Vulkan reports deviceUUID, GL reports GL_DEVICE_UUID_EXT,
// and EXT_memory_object defines them to be comparable for exactly this purpose. Matching them
// is the difference between a probe that grades the importer and one that grades which driver
// EGL happened to pick.
class HeadlessGl {
public:
    ~HeadlessGl() {
        if (m_display == EGL_NO_DISPLAY) return;
        eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (m_context != EGL_NO_CONTEXT) eglDestroyContext(m_display, m_context);
        eglTerminate(m_display);
    }

    // Brings up a context on the EGL device whose GL_DEVICE_UUID_EXT equals `wantedUuid`, and
    // fails rather than falling back: a context on the wrong device would skip with a driver
    // mismatch, which reads as "this host cannot" when the truth is "this probe chose badly".
    bool createMatching(const std::array<uint8_t, 16>& wantedUuid, std::string& why) {
        auto queryDevices = reinterpret_cast<PFNEGLQUERYDEVICESEXTPROC>(eglGetProcAddress("eglQueryDevicesEXT"));
        auto getPlatformDisplay =
            reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
        if (queryDevices == nullptr || getPlatformDisplay == nullptr) {
            why = "this EGL does not offer EGL_EXT_platform_device, so a specific GPU cannot be asked for";
            return false;
        }

        std::array<EGLDeviceEXT, 16> devices{};
        EGLint deviceCount = 0;
        if (queryDevices(static_cast<EGLint>(devices.size()), devices.data(), &deviceCount) != EGL_TRUE) {
            why = "eglQueryDevicesEXT failed";
            return false;
        }

        std::string tried;
        for (EGLint i = 0; i < deviceCount; ++i) {
            if (!bringUp(getPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, devices[i], nullptr))) continue;

            std::array<uint8_t, 16> uuid{};
            if (glUuid(uuid) && uuid == wantedUuid) return true;

            if (!tried.empty()) tried += ", ";
            tried += renderer();
            tearDown();
        }
        why = "no EGL device matches the Vulkan device's UUID (tried: " + (tried.empty() ? "none" : tried) + ")";
        return false;
    }

    std::string renderer() const {
        const GLubyte* name = glGetString(GL_RENDERER);
        return name != nullptr ? reinterpret_cast<const char*>(name) : "(unknown)";
    }

private:
    bool bringUp(EGLDisplay display) {
        if (display == EGL_NO_DISPLAY) return false;
        m_display = display;
        EGLint major = 0;
        EGLint minor = 0;
        if (eglInitialize(m_display, &major, &minor) != EGL_TRUE) {
            m_display = EGL_NO_DISPLAY;
            return false;
        }
        if (eglBindAPI(EGL_OPENGL_API) != EGL_TRUE) {
            tearDown();
            return false;
        }
        EGLint configAttrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
        EGLConfig config{};
        EGLint configs = 0;
        if (eglChooseConfig(m_display, configAttrs, &config, 1, &configs) != EGL_TRUE || configs < 1) {
            tearDown();
            return false;
        }
        EGLint contextAttrs[] = {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 5, EGL_NONE};
        m_context = eglCreateContext(m_display, config, EGL_NO_CONTEXT, contextAttrs);
        if (m_context == EGL_NO_CONTEXT) {
            tearDown();
            return false;
        }
        if (eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, m_context) != EGL_TRUE) {
            tearDown();
            return false;
        }
        return true;
    }

    void tearDown() {
        if (m_display == EGL_NO_DISPLAY) return;
        eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (m_context != EGL_NO_CONTEXT) eglDestroyContext(m_display, m_context);
        eglTerminate(m_display);
        m_display = EGL_NO_DISPLAY;
        m_context = EGL_NO_CONTEXT;
    }

    // GL_DEVICE_UUID_EXT comes from GL_EXT_memory_object, so a context without that extension
    // has no UUID to compare -- and could not have imported anything either.
    static bool glUuid(std::array<uint8_t, 16>& out) {
        using GetUnsignedBytevFn = void (*)(GLenum, GLubyte*);
        auto getBytes = reinterpret_cast<GetUnsignedBytevFn>(eglGetProcAddress("glGetUnsignedBytevEXT"));
        if (getBytes == nullptr) return false;
        while (glGetError() != GL_NO_ERROR) {
        }
        getBytes(GL_DEVICE_UUID_EXT, out.data());
        return glGetError() == GL_NO_ERROR;
    }

    EGLDisplay m_display = EGL_NO_DISPLAY;
    EGLContext m_context = EGL_NO_CONTEXT;
};

// How many descriptors this process holds. /proc/self/fd is the only view that counts what the
// driver opened as well as what this code did, which is the point: the fd accounting under test
// crosses into GL and EGL.
int openFdCount() {
    DIR* dir = ::opendir("/proc/self/fd");
    if (dir == nullptr) return -1;
    int count = 0;
    while (::readdir(dir) != nullptr) ++count;
    ::closedir(dir);
    return count;
}

// Reads an imported texture back through a framebuffer object. glGetTexImage would be shorter
// but is not available on GLES-shaped contexts, and the plugin's own context may be one.
std::vector<unsigned char> readTexture(GLuint texture, uint32_t width, uint32_t height, std::string& why) {
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);

    std::vector<unsigned char> pixels;
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        why = "the imported texture could not be attached to a framebuffer to read back";
    } else {
        pixels.resize(static_cast<size_t>(width) * height * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA, GL_UNSIGNED_BYTE,
                     pixels.data());
        if (glGetError() != GL_NO_ERROR) {
            why = "glReadPixels failed on the imported texture";
            pixels.clear();
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    return pixels;
}

// Reads the imported texture the way a compositor does: SAMPLED through a shader into an
// ordinary RGBA8 target, rather than read straight out of its own memory as readTexture does.
//
// The difference between the two is the whole reason this exists. The importer turns
// B8G8R8A8 memory into the colours that were rendered with GL_TEXTURE_SWIZZLE_R/B, and a
// texture swizzle applies to texture LOOKUPS only -- a framebuffer read of the texture itself
// bypasses it completely. So readTexture cannot see the channel plumbing at all, on either
// path, and this is the only view of the texture that can.
//
// texelFetch rather than texture(): no filtering, no coordinate rounding, one texel per
// destination pixel, so a mismatch here is a channel or a layout and never a sample position.
std::vector<unsigned char> readSampled(GLuint texture, uint32_t width, uint32_t height, std::string& why) {
    static const char* const kVertex = "#version 330 core\n"
                                       "void main() {\n"
                                       "  vec2 corner = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
                                       "  gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);\n"
                                       "}\n";
    static const char* const kFragment = "#version 330 core\n"
                                         "uniform sampler2D source;\n"
                                         "out vec4 colour;\n"
                                         "void main() {\n"
                                         "  colour = texelFetch(source, ivec2(gl_FragCoord.xy), 0);\n"
                                         "}\n";

    std::vector<unsigned char> pixels;
    GLuint vertex = 0;
    GLuint fragment = 0;
    GLuint program = 0;
    GLuint destination = 0;
    GLuint fbo = 0;
    GLuint vao = 0;

    const auto compile = [&why](GLenum stage, const char* source) -> GLuint {
        GLuint shader = glCreateShader(stage);
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);
        GLint ok = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (ok == GL_TRUE) return shader;
        std::array<char, 512> log{};
        glGetShaderInfoLog(shader, static_cast<GLsizei>(log.size() - 1), nullptr, log.data());
        why = std::string("this probe's own sampling shader would not compile: ") + log.data();
        glDeleteShader(shader);
        return 0;
    };

    // Every exit below runs this, so a failure partway through leaves the context as it was
    // found -- the descriptor and object accounting later in this file is measured against it.
    const auto tidy = [&] {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glUseProgram(0);
        glBindVertexArray(0);
        glBindTexture(GL_TEXTURE_2D, 0);
        if (vao != 0) glDeleteVertexArrays(1, &vao);
        if (fbo != 0) glDeleteFramebuffers(1, &fbo);
        if (destination != 0) glDeleteTextures(1, &destination);
        if (program != 0) glDeleteProgram(program);
        if (vertex != 0) glDeleteShader(vertex);
        if (fragment != 0) glDeleteShader(fragment);
    };

    vertex = compile(GL_VERTEX_SHADER, kVertex);
    fragment = vertex != 0 ? compile(GL_FRAGMENT_SHADER, kFragment) : 0;
    if (vertex == 0 || fragment == 0) {
        tidy();
        return pixels;
    }
    program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        std::array<char, 512> log{};
        glGetProgramInfoLog(program, static_cast<GLsizei>(log.size() - 1), nullptr, log.data());
        why = std::string("this probe's own sampling program would not link: ") + log.data();
        tidy();
        return pixels;
    }

    glGenTextures(1, &destination);
    glBindTexture(GL_TEXTURE_2D, destination);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(width), static_cast<GLsizei>(height), 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, destination, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        why = "this probe could not build a framebuffer to sample the imported texture into";
        tidy();
        return pixels;
    }

    // NEAREST, and not for filtering's sake: the default minification filter wants mipmaps,
    // and an EGLImage-backed texture has exactly one level -- left alone it is an incomplete
    // texture and every lookup returns black, which would read as a wrong import.
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glUseProgram(program);
    glUniform1i(glGetUniformLocation(program, "source"), 0);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    glDrawArrays(GL_TRIANGLES, 0, 3);

    pixels.resize(static_cast<size_t>(width) * height * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA, GL_UNSIGNED_BYTE,
                 pixels.data());
    if (glGetError() != GL_NO_ERROR) {
        why = "sampling the imported texture into a framebuffer failed";
        pixels.clear();
    }
    tidy();
    return pixels;
}

// The UUID of the GPU a Vulkan device is running on, which is what the GL context must match.
std::array<uint8_t, 16> vulkanDeviceUuid(VkPhysicalDevice physical) {
    VkPhysicalDeviceIDProperties ids{};
    ids.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &ids;
    vkGetPhysicalDeviceProperties2(physical, &properties);

    std::array<uint8_t, 16> uuid{};
    std::memcpy(uuid.data(), ids.deviceUUID, uuid.size());
    return uuid;
}

bool runImportChecks() {
    return wallpaperx::testing::gradedRun("frame_import_probe", [&](wallpaperx::testing::ValidationGrade& grade) {
        const std::string imagePath = wallpaperx::testing::writeGradientPng(
            std::string(WALLPAPERX_TEST_IMAGE_DIR) + "/frame_import_gradient.png", 16, 16);

        // Created BEFORE the GL context, because its UUID is what decides which GL device to
        // ask for. The other order forces a guess, and the guess is wrong on this machine.
        wallpaperx::Device device(/*enableValidation=*/true);
        grade.watch(device);

        HeadlessGl gl;
        std::string why;
        if (!gl.createMatching(vulkanDeviceUuid(device.physicalDevice()), why)) {
            notVerified(kGoals, "no GL context on the same device as the Vulkan one -- " + why);
            return true;
        }
        wallpaperx::OffscreenTargetManager targets(device);
        wallpaperx::StaticRenderer renderer(device, targets.renderPass(), imagePath, WALLPAPERX_SHADER_DIR);

        // From its own target, never exported: reading back the exported one would move the
        // export onto a release scope the daemon never uses.
        const std::vector<unsigned char> expected =
            renderReference(device, targets, renderer, /*monitorId=*/1, kWidth, kHeight);

        // COVERAGE-ASSERT: an image that is constant along an axis cannot distinguish a correct
        // import from a transposed or wrongly-strided one, so this check would keep passing for
        // exactly the builds it exists to catch.
        if (!variesInBothAxes(expected, kWidth, kHeight)) {
            notVerified(kGoals, "the rendered frame is constant along an axis, so comparing an imported copy of it "
                                "cannot detect a wrong stride or a transposed read");
            return true;
        }

        const auto& target = targets.create(0, kWidth, kHeight);
        targets.render(renderer, target);
        wallpaperx::ExportedFrame frame = wallpaperx::exportFrame(device, target);

        const bool isDmaBuf = frame.type == wallpaperx::ExportedFrame::HandleType::DmaBuf;

        wallpaperx::FrameImport::Request request;
        request.fd = frame.fd;
        request.width = kWidth;
        request.height = kHeight;
        request.memorySize = frame.memorySize;
        request.handleType = isDmaBuf ? wallpaperx::FrameImport::HandleType::DmaBuf
                                      : wallpaperx::FrameImport::HandleType::OpaqueFd;
        request.drmFormat = frame.drmFormat;
        request.drmModifier = frame.modifier;
        for (const auto& plane : frame.planes) {
            request.planes.emplace_back(plane.offset, plane.rowPitch);
        }

        // Which path runs is the device's choice, not this probe's: NVIDIA exports opaque by
        // policy and Mesa exports dma_buf. Both are under test, and the run says which it took
        // so a green log cannot be mistaken for covering both.
        std::printf("frame_import_probe: %s path, on %s\n", isDmaBuf ? "dma_buf" : "opaque fd",
                    gl.renderer().c_str());
        if (!wallpaperx::FrameImport::supports(request.handleType, &why)) {
            if (frame.fd >= 0) ::close(frame.fd);
            notVerified(kGoals, std::string("this context cannot import the handle type this device exports (") +
                                    (isDmaBuf ? "dma_buf" : "opaque fd") + ") -- " + why);
            return true;
        }

        // GOAL: S12.2 -- the import succeeds once the pairing is known good. Graded as a
        //       FAILURE, not a skip: every reason an import could legitimately be impossible
        //       here has been ruled out above, since the GL device's UUID equals the Vulkan
        //       device's and the context advertises the extensions the path needs. Treating a
        //       failure past this point as "could not be exercised" is how a broken importer
        //       reports itself as a skip -- measured, not imagined: transposing the requested
        //       dimensions made glTexStorageMem2DEXT fail and the probe went green-as-skipped
        //       with its own control applied.
        // CONTROL: `request.width` and `request.height` swapped at the import site -> "FAIL: an
        //       exported frame imports into a GL context on the same device", printing "on
        //       NVIDIA GeForce RTX 3080 Ti/PCIe/SSE2: glTexStorageMem2DEXT could not back a
        //       texture with the imported memory". Observed 2026-08-20 -- and observed BEFORE
        //       the fix producing a skip instead, which is what the fix was for. Reverted.
        // CONTROL, for the "chosen from what the driver advertises" half specifically: the
        //       handle-type branch in FrameImport::import forced each way round. `if (false)`,
        //       sending a dma_buf through the memory-object importer, reds the lavapipe entry
        //       alone; `if (true)`, sending an opaque fd through EGLImage, reds the NVIDIA entry
        //       alone. Neither wrong path is quietly importable, and it takes both entries to
        //       show it. Observed 2026-08-21. Both reverted.
        const int beforeImport = openFdCount();
        const wallpaperx::FrameImport::Result imported = wallpaperx::FrameImport::import(request);
        const bool importedOk = imported.texture != 0;
        check(importedOk, "an exported frame imports into a GL context on the same device");
        if (!importedOk) {
            // request, not frame: import() clears the one it consumed, and a failed import may
            // still have consumed it.
            if (request.fd >= 0) ::close(request.fd);
            std::fprintf(stderr, "  on %s: %s\n", gl.renderer().c_str(), imported.error.c_str());
            return true;
        }

        std::string readWhy;
        const std::vector<unsigned char> got = readTexture(imported.texture, kWidth, kHeight, readWhy);
        if (got.empty()) {
            wallpaperx::FrameImport::release(imported);
            notVerified(kGoals, "the imported texture could not be read back -- " + readWhy);
            return true;
        }

        // GOAL: S12.1 -- the imported texture holds the pixels that were rendered, in the layout
        //       they were rendered in: same geometry, same stride, same row order.
        // WHY-THIS-SHAPE: deliberately accepts EITHER channel order, and therefore grades
        //       LAYOUT ONLY -- it is not the check that establishes the colours are right, and
        //       must not be read as one. The two paths report channels differently for correct
        //       reasons: the memory-object path lays GL_RGBA8 over the raw bytes so a framebuffer
        //       read returns memory order (the order the Vulkan readback is in), while the
        //       EGLImage path is told the fourcc and reads back as RGBA against a B8G8R8A8
        //       render. Requiring one order here would fail one path for being correct. Colour is
        //       graded by the sampled check below, which is where the swizzle becomes visible.
        // CONTROL: `request.width` and `request.height` swapped at the import site
        // CONTROL, dma_buf path: the plane pitch replaced by an assumed `width * 4` in
        //       FrameImport::importDmaBuf -> "FAIL: the imported texture is laid out as the frame
        //       that was rendered", 7101 of 9792 bytes differing on llvmpipe -- and the sampled
        //       check below red with it. Re-run against this wording 2026-08-21 on the lavapipe
        //       entry. Reverted.
        // NOT COVERED, measured rather than assumed: forcing the DRM modifier to 0 changes
        //       nothing here, because lavapipe renders LINEAR and 0 IS linear. The modifier
        //       plumbing is therefore exercised but not graded, and would need a device that
        //       picks a tiled or compressed modifier -- which this machine has none of, since
        //       its only other Vulkan device is NVIDIA and that exports an opaque fd.
        const auto swappedRedBlue = [](const std::vector<unsigned char>& in) {
            std::vector<unsigned char> out = in;
            for (size_t i = 0; i + 3 < out.size(); i += 4) std::swap(out[i], out[i + 2]);
            return out;
        };
        const std::vector<unsigned char> asColours = swappedRedBlue(expected);
        const bool same = got.size() == expected.size() && (got == expected || got == asColours);
        check(same, "the imported texture is laid out as the frame that was rendered");
        if (!same) {
            size_t differing = 0;
            for (size_t i = 0; i < got.size() && i < expected.size(); ++i) {
                if (got[i] != expected[i]) ++differing;
            }
            std::fprintf(stderr, "  %zu of %zu bytes differ (read %zu, expected %zu) on %s\n", differing,
                         expected.size(), got.size(), expected.size(), gl.renderer().c_str());
        }

        // GOAL: S12.1 -- what a compositor SAMPLES from the imported texture is the colour that
        //       was rendered, channel for channel. False if red and blue arrive transposed, which
        //       is a wallpaper with the wrong colours on every screen -- the defect the importer's
        //       swizzle exists to fix, and one that was hit for real on the first cross-process
        //       frame.
        // WHY-THIS-SHAPE: sampled through a shader rather than read out of the texture's own
        //       memory, and asserted against ONE order rather than either. The obvious simpler
        //       form -- reuse the framebuffer read above -- cannot answer this at all: a texture
        //       swizzle applies to lookups and not to a framebuffer read, so the raw read returns
        //       the same bytes whether the swizzle is set or not. Measured: with the swizzle
        //       deleted the whole 40-test suite stayed green.
        //       The expected order is the same on BOTH paths and that is not a coincidence -- one
        //       gets there by swizzle and the other by telling EGL the fourcc, and both are ways
        //       of saying the same thing. So one expectation grades both, and neither path can
        //       reach it by accident.
        // CONTROL: the swizzle in FrameImport::import disabled (`if (false)`) -> "FAIL: sampling
        //       the imported texture gives the colours that were rendered", 4788 of 9792 bytes
        //       differing, "first at pixel (0,0): sampled 0,16,16,255; rendered 16,16,0,255", on
        //       the NVIDIA opaque entry and correctly green on the dma_buf one, which does not
        //       use the swizzle. Observed 2026-08-21. Reverted.
        // CONTROL: B8G8R8A8_UNORM mapped to XBGR8888 instead of XRGB8888 in
        //       drmFourccForVkFormat -- an exporter that lies about its channel order -> the same
        //       FAIL on BOTH entries. Before this check, that mutation left all 40 tests green.
        //       Observed 2026-08-21. Reverted.
        std::string sampledWhy;
        const std::vector<unsigned char> sampled = readSampled(imported.texture, kWidth, kHeight, sampledWhy);
        if (sampled.empty()) {
            // The probe's own machinery, not the importer's: attributed as a skip so a broken
            // harness cannot print as a failure of the code under test.
            notVerified(kGoals, "the imported texture could not be sampled -- " + sampledWhy);
        } else {
            const bool coloursMatch = sampled == asColours;
            check(coloursMatch, "sampling the imported texture gives the colours that were rendered");
            if (!coloursMatch) {
                size_t differing = 0;
                size_t firstAt = sampled.size();
                for (size_t i = 0; i < sampled.size() && i < asColours.size(); ++i) {
                    if (sampled[i] == asColours[i]) continue;
                    ++differing;
                    if (firstAt == sampled.size()) firstAt = i;
                }
                if (firstAt < sampled.size()) {
                    const size_t pixel = (firstAt / 4) * 4;
                    std::fprintf(stderr,
                                 "  %zu of %zu bytes differ, first at pixel (%zu,%zu): sampled %u,%u,%u,%u; rendered "
                                 "%u,%u,%u,%u -- on %s\n",
                                 differing, asColours.size(), (pixel / 4) % kWidth, (pixel / 4) / kWidth,
                                 sampled[pixel], sampled[pixel + 1], sampled[pixel + 2], sampled[pixel + 3],
                                 asColours[pixel], asColours[pixel + 1], asColours[pixel + 2], asColours[pixel + 3],
                                 gl.renderer().c_str());
                }
            }
        }

        wallpaperx::FrameImport::release(imported);

        // GOAL: S12.3 -- a successful import consumes the fd exactly once, and a failed one
        //       leaves it with the caller. Both halves matter and they fail in opposite
        //       directions: consuming twice closes a descriptor this process may have reused,
        //       and consuming neither leaks one per frame on a process that runs for a login
        //       session.
        // WHY-THIS-SHAPE: counted from /proc/self/fd rather than by inspecting the importer,
        //       because the two paths dispose of the fd differently -- GL takes ownership,
        //       EGL dups and the importer closes its own -- and the property that must hold is
        //       about the process, not about which call was made.
        // CONTROL: the `::close(request.fd)` deleted from FrameImport::importDmaBuf -> "FAIL:
        //       importing and releasing repeatedly does not accumulate descriptors", printing
        //       "45 open fds before, 47 after one cycle, 54 after eight" -- one per frame, which
        //       is what the loop exists to show. Observed 2026-08-21 on the lavapipe entry.
        //       Reverted.
        // CONTROL, the other half: `::close(request.fd)` added to the width==0 refusal in
        //       FrameImport::import -> "FAIL: a failed import leaves its file descriptor with the
        //       caller, neither closed nor leaked", printing "55 open fds before the failed
        //       import, 54 after". Observed 2026-08-21 on both entries. Reverted.
        // COVERAGE-ASSERT: every count below comes from /proc/self/fd, which returns -1 when it
        //       cannot be read. Left ungraded, every comparison here is -1 against -1 and the
        //       whole phase passes having counted nothing; asserted so that becomes a skip.

        // A failure that never reaches the driver: the fd must come back untouched.
        wallpaperx::FrameImport::Request rejected;
        rejected.fd = ::dup(STDIN_FILENO);
        rejected.width = 0;  // refused before anything is imported
        rejected.height = kHeight;
        rejected.memorySize = 4096;
        rejected.handleType = request.handleType;
        const int beforeRejected = openFdCount();
        const wallpaperx::FrameImport::Result none = wallpaperx::FrameImport::import(rejected);
        const int afterRejected = openFdCount();
        check(none.texture == 0, "an import that cannot proceed produces no texture");
        if (beforeRejected < 0 || afterRejected < 0) {
            notVerified(kGoals, "/proc/self/fd could not be read, so no descriptor was counted -- reported as "
                                "uncounted rather than as a passing comparison of -1 against -1");
            if (rejected.fd >= 0) ::close(rejected.fd);
            return true;
        }
        check(afterRejected == beforeRejected,
              "a failed import leaves its file descriptor with the caller, neither closed nor leaked");

        // GOAL: a failure AFTER the driver has taken the descriptor still tells the caller the
        //       descriptor is gone. The case above is refused before any driver call, so it
        //       grades the early half only: on the opaque path GL takes the fd at
        //       glImportMemoryFdEXT and two later steps can still fail. A caller told the fd is
        //       still its own closes a number the process has since reused -- inside a
        //       compositor, its own socket.
        // WHY-THIS-SHAPE: asserted by asking whether the descriptor is STILL OPEN, not by
        //       counting. Counting cannot see this: the caller's second close simply fails with
        //       EBADF and the total is unchanged, which is exactly what a count-based version of
        //       this check did -- it passed with the defect present, measured.
        // CONTROL: `request.fd = -1` deleted from FrameImport::import after the importFd check ->
        //       "FAIL: a failed import that consumed the descriptor says so...", printing
        //       "import failed and returned fd 51, which is already closed". Observed
        //       2026-08-21. Reverted.
        {
            const auto& lateTarget = targets.create(0, kWidth, kHeight);
            targets.render(renderer, lateTarget);
            wallpaperx::ExportedFrame lateFrame = wallpaperx::exportFrame(device, lateTarget);

            wallpaperx::FrameImport::Request late = request;
            late.fd = lateFrame.fd;
            late.planes.clear();
            for (const auto& plane : lateFrame.planes) late.planes.emplace_back(plane.offset, plane.rowPitch);
            // Transposed, which is what this file's own control uses to make the texture
            // allocation fail while the memory import succeeds.
            late.width = kHeight;
            late.height = kWidth;
            const int lateFd = late.fd;

            const wallpaperx::FrameImport::Result lateResult = wallpaperx::FrameImport::import(late);
            if (lateResult.texture != 0) {
                wallpaperx::FrameImport::release(lateResult);
                if (late.fd >= 0) ::close(late.fd);
                notVerified(kGoals, "the transposed import unexpectedly succeeded, so a failure past the memory "
                                    "import could not be staged on this driver");
            } else {
                // Either it says it consumed the fd, or the fd it handed back is genuinely open.
                const bool stillOpen = ::fcntl(lateFd, F_GETFD) != -1;
                const bool honest = late.fd < 0 || stillOpen;
                check(honest, "a failed import that consumed the descriptor says so, rather than handing back a "
                              "number the caller would close twice");
                if (!honest) {
                    std::fprintf(stderr, "  import failed and returned fd %d, which is already closed\n", lateFd);
                }
                if (late.fd >= 0 && stillOpen) ::close(late.fd);
            }
        }

        // The property that matters is that the count does not grow PER FRAME. A driver may hold
        // one descriptor of its own for an imported allocation and release it only with the
        // context -- measured here, NVIDIA does exactly that -- and a single import cannot tell
        // that apart from a leak. A wallpaper that changes every few seconds can.
        int afterFirstCycle = 0;
        for (int cycle = 0; cycle < 8; ++cycle) {
            const auto& spare = targets.create(0, kWidth, kHeight);
            targets.render(renderer, spare);
            wallpaperx::ExportedFrame extra = wallpaperx::exportFrame(device, spare);

            // Metadata from THIS export: exportFrame recomputes it per image.
            wallpaperx::FrameImport::Request again = request;
            again.fd = extra.fd;
            again.memorySize = extra.memorySize;
            again.drmFormat = extra.drmFormat;
            again.drmModifier = extra.modifier;
            again.planes.clear();
            for (const auto& plane : extra.planes) again.planes.emplace_back(plane.offset, plane.rowPitch);

            const wallpaperx::FrameImport::Result round = wallpaperx::FrameImport::import(again);
            if (round.texture == 0) {
                if (again.fd >= 0) ::close(again.fd);
                notVerified(kGoals, "a repeat import failed, so descriptor growth could not be measured: " +
                                        round.error);
                return true;
            }
            wallpaperx::FrameImport::release(round);
            if (cycle == 0) afterFirstCycle = openFdCount();
        }

        const int atEnd = openFdCount();
        if (beforeImport < 0 || afterFirstCycle < 0 || atEnd < 0) {
            notVerified(kGoals, "/proc/self/fd stopped answering partway through the import cycles, so growth could "
                                "not be measured");
            return true;
        }
        const bool steady = atEnd <= afterFirstCycle;
        check(steady, "importing and releasing repeatedly does not accumulate descriptors");
        if (!steady) {
            std::fprintf(stderr, "  %d open fds before, %d after one cycle, %d after eight\n", beforeImport,
                         afterFirstCycle, atEnd);
        }
        return true;
    });
}

}  // namespace

int main() {
    wallpaperx::testing::g_testName = "frame_import_probe";
    // Everything this drives reports failure by throwing, and without a boundary a host with no
    // Vulkan ends in std::terminate -- a failure carrying no reason.
    bool ok = false;
    try {
        ok = runImportChecks();
    } catch (const std::exception& e) {
        wallpaperx::testing::harnessFailure(std::string("the probe could not build its preconditions: ") +
                                            e.what());
    }
    // Both reports run before the verdict, or a fault's reason line is never printed.
    const bool failed = wallpaperx::testing::reportCheckFailures();
    const bool faulted = wallpaperx::testing::reportHarnessFaults("while importing an exported frame");
    if (!ok || failed || faulted) return 1;
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
