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

#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
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
        const wallpaperx::FrameImport::Result imported = wallpaperx::FrameImport::import(request);
        const bool importedOk = imported.texture != 0;
        check(importedOk, "an exported frame imports into a GL context on the same device");
        if (!importedOk) {
            // The fd is still ours on a failed import, by the contract in frame_import.h.
            if (frame.fd >= 0) ::close(frame.fd);
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

        // GOAL: S12.1 -- the imported texture holds the pixels that were rendered.
        // CONTROL: `request.width` and `request.height` swapped at the import site
        // CONTROL, dma_buf path: the plane pitch replaced by an assumed `width * 4` in
        //       FrameImport::importDmaBuf -> "FAIL: the imported texture holds the frame that
        //       was rendered, byte for byte", 7101 of 9792 bytes differing on llvmpipe.
        //       Observed 2026-08-21 on the lavapipe entry. Reverted.
        // NOT COVERED, measured rather than assumed: forcing the DRM modifier to 0 changes
        //       nothing here, because lavapipe renders LINEAR and 0 IS linear. The modifier
        //       plumbing is therefore exercised but not graded, and would need a device that
        //       picks a tiled or compressed modifier -- which this machine has none of, since
        //       its only other Vulkan device is NVIDIA and that exports an opaque fd.
        // Compared both ways round, because the two import paths report channels differently and
        // the difference is not a defect. The memory-object path lays GL_RGBA8 over the raw
        // bytes, so a framebuffer read returns them in memory order -- the same order the Vulkan
        // readback is in. The EGLImage path is told the fourcc, so EGL interprets the channels
        // and a read comes back as RGBA against a B8G8R8A8 render. Requiring the raw order would
        // fail the dma_buf path for being correct.
        const auto swappedRedBlue = [](const std::vector<unsigned char>& in) {
            std::vector<unsigned char> out = in;
            for (size_t i = 0; i + 3 < out.size(); i += 4) std::swap(out[i], out[i + 2]);
            return out;
        };
        const bool same = got.size() == expected.size() &&
                          (got == expected || got == swappedRedBlue(expected));
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
