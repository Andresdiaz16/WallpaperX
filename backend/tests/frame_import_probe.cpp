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
#include <GL/gl.h>
#include <GL/glext.h>

#include <unistd.h>

#include <cstdio>
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

// A surfaceless EGL context, owned. Nothing here draws to a surface -- the texture is read back
// through a framebuffer object -- so no window, no pbuffer and no display server is needed.
class HeadlessGl {
public:
    ~HeadlessGl() {
        if (m_display == EGL_NO_DISPLAY) return;
        eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (m_context != EGL_NO_CONTEXT) eglDestroyContext(m_display, m_context);
        eglTerminate(m_display);
    }

    bool create(std::string& why) {
        m_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (m_display == EGL_NO_DISPLAY) {
            why = "no EGL display";
            return false;
        }
        EGLint major = 0;
        EGLint minor = 0;
        if (eglInitialize(m_display, &major, &minor) != EGL_TRUE) {
            why = "eglInitialize failed";
            m_display = EGL_NO_DISPLAY;
            return false;
        }
        if (eglBindAPI(EGL_OPENGL_API) != EGL_TRUE) {
            why = "this EGL does not offer desktop OpenGL";
            return false;
        }
        EGLint configAttrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
        EGLConfig config{};
        EGLint configs = 0;
        if (eglChooseConfig(m_display, configAttrs, &config, 1, &configs) != EGL_TRUE || configs < 1) {
            why = "no EGL config offering desktop OpenGL";
            return false;
        }
        EGLint contextAttrs[] = {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 5, EGL_NONE};
        m_context = eglCreateContext(m_display, config, EGL_NO_CONTEXT, contextAttrs);
        if (m_context == EGL_NO_CONTEXT) {
            why = "eglCreateContext failed for a 4.5 core context";
            return false;
        }
        // Surfaceless: needs EGL_KHR_surfaceless_context, which both stacks here advertise.
        if (eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, m_context) != EGL_TRUE) {
            why = "eglMakeCurrent failed without a surface";
            return false;
        }
        return true;
    }

    std::string renderer() const {
        const GLubyte* name = glGetString(GL_RENDERER);
        return name != nullptr ? reinterpret_cast<const char*>(name) : "(unknown)";
    }

private:
    EGLDisplay m_display = EGL_NO_DISPLAY;
    EGLContext m_context = EGL_NO_CONTEXT;
};

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

bool runImportChecks() {
    HeadlessGl gl;
    std::string why;
    if (!gl.create(why)) {
        notVerified(kGoals, "no headless GL context on this host -- " + why);
        return true;
    }

    if (!wallpaperx::FrameImport::supports(wallpaperx::FrameImport::HandleType::OpaqueFd, &why)) {
        notVerified(kGoals, std::string("this GL context (") + gl.renderer() + ") cannot import an opaque fd -- " + why);
        return true;
    }

    return wallpaperx::testing::gradedRun("frame_import_probe", [&](wallpaperx::testing::ValidationGrade& grade) {
        const std::string imagePath = wallpaperx::testing::writeGradientPng(
            std::string(WALLPAPERX_TEST_IMAGE_DIR) + "/frame_import_gradient.png", 16, 16);

        wallpaperx::Device device(/*enableValidation=*/true);
        grade.watch(device);
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

        if (frame.type != wallpaperx::ExportedFrame::HandleType::OpaqueFd) {
            if (frame.fd >= 0) ::close(frame.fd);
            notVerified(kGoals, "this device exported a dma_buf, which the importer does not handle yet -- run this "
                                "probe with WALLPAPERX_FORCE_HANDLE_TYPE=opaque to reach the path under test");
            return true;
        }

        wallpaperx::FrameImport::Request request;
        request.fd = frame.fd;
        request.width = kWidth;
        request.height = kHeight;
        request.memorySize = frame.memorySize;
        request.handleType = wallpaperx::FrameImport::HandleType::OpaqueFd;

        const wallpaperx::FrameImport::Result imported = wallpaperx::FrameImport::import(request);
        if (imported.texture == 0) {
            // The fd is still ours on a failed import, by the contract in frame_import.h.
            if (frame.fd >= 0) ::close(frame.fd);
            // A driver that cannot import its own sibling's memory is a pairing fault, not a
            // defect in the importer: this binary does not choose which driver EGL resolves to.
            notVerified(kGoals, std::string("the import failed on ") + gl.renderer() + " -- " + imported.error +
                                    " (an opaque fd is importable only by the driver that produced it, and this "
                                    "probe does not get to choose the pairing)");
            return true;
        }

        std::string readWhy;
        const std::vector<unsigned char> got = readTexture(imported.texture, kWidth, kHeight, readWhy);
        if (got.empty()) {
            wallpaperx::FrameImport::release(imported);
            notVerified(kGoals, "the imported texture could not be read back -- " + readWhy);
            return true;
        }

        // GOAL: S12.1 -- the imported texture holds the pixels that were rendered.
        // CONTROL: `request.width` and `request.height` swapped at the import site
        const bool same = got.size() == expected.size() && got == expected;
        check(same, "the imported texture holds the frame that was rendered, byte for byte");
        if (!same) {
            size_t differing = 0;
            for (size_t i = 0; i < got.size() && i < expected.size(); ++i) {
                if (got[i] != expected[i]) ++differing;
            }
            std::fprintf(stderr, "  %zu of %zu bytes differ (read %zu, expected %zu) on %s\n", differing,
                         expected.size(), got.size(), expected.size(), gl.renderer().c_str());
        }

        wallpaperx::FrameImport::release(imported);
        return true;
    });
}

}  // namespace

int main() {
    wallpaperx::testing::g_testName = "frame_import_probe";
    const bool ok = runImportChecks();
    if (!ok) return 1;
    if (wallpaperx::testing::reportCheckFailures()) return 1;
    if (wallpaperx::testing::reportHarnessFaults("while importing an exported frame")) return 1;
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
