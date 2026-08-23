// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The GL side of a frame-import test: a headless context on a NAMED GPU, and a way to read
// back what a shader SAMPLES from an imported texture.
//
// Both halves exist because of the same trap. An opaque fd is importable only by the driver
// that produced it, so a context on whatever EGL_DEFAULT_DISPLAY resolves to (llvmpipe here,
// even where Vulkan resolves to NVIDIA) fails the import for a reason that says nothing about
// the importer. And a texture swizzle applies to LOOKUPS only, so reading an imported texture
// straight out of its own memory cannot see the channel plumbing that decides whether the
// wallpaper's colours are right.
//
// GL_GLEXT_PROTOTYPES is defined below, which makes the core GL 3.0+ entry points (framebuffer
// objects, glGetStringi) callable directly. That is fine for a test binary linked against
// libGL; frame_import.cpp itself resolves through eglGetProcAddress instead, because the plugin
// runs inside a context it does not own.

#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace wallpaperx::testing {

// A surfaceless EGL context, owned, created on a NAMED device rather than on whatever
// EGL_DEFAULT_DISPLAY resolves to.
//
// This matters and was measured: the default display comes up on llvmpipe here even though
// Vulkan resolves to NVIDIA. Both mismatched pairings were tried and both fail at
// glTexStorageMem2DEXT -- including lavapipe into llvmpipe, which are both Mesa and both
// software.
//
// So the device is chosen by UUID: Vulkan reports deviceUUID, GL reports GL_DEVICE_UUID_EXT, and
// EXT_memory_object defines them to be comparable for exactly this purpose. Matching them is
// the difference between a test that grades the importer and one that grades which driver EGL
// happened to pick.
class HeadlessGl {
public:
    HeadlessGl() = default;

    ~HeadlessGl() { tearDown(); }

    HeadlessGl(const HeadlessGl&) = delete;
    HeadlessGl& operator=(const HeadlessGl&) = delete;

    // Brings up a context on the EGL device whose GL_DEVICE_UUID_EXT equals `wantedUuid`, and
    // fails rather than falling back: a context on the wrong device would fail the import with
    // a driver mismatch, which reads as "this host cannot" when the truth is "this harness
    // chose badly".
    bool createMatching(const std::array<uint8_t, 16>& wantedUuid, std::string& why) {
        auto queryDevices = reinterpret_cast<PFNEGLQUERYDEVICESEXTPROC>(eglGetProcAddress("eglQueryDevicesEXT"));
        auto getPlatformDisplay =
            reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
        if (queryDevices == nullptr || getPlatformDisplay == nullptr) {
            why = "this EGL does not offer EGL_EXT_platform_device, so a specific GPU cannot be asked for";
            return false;
        }

        // An all-zero wanted UUID means the caller never learned the exporting GPU's identity.
        // Matching on it would bind whichever device also answers zeros, and the import failure
        // that follows would be charged to the importer rather than to this harness.
        if (wantedUuid == std::array<uint8_t, 16>{}) {
            why = "the exporting device's UUID is all zeros, so there is no identity to match a GL context against";
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
        why = "no EGL device matches the wanted GPU's UUID (tried: " + (tried.empty() ? "none" : tried) + ")";
        return false;
    }

    std::string renderer() const {
        const GLubyte* name = glGetString(GL_RENDERER);
        return name != nullptr ? reinterpret_cast<const char*>(name) : "(unknown)";
    }

    void tearDown() {
        if (m_display == EGL_NO_DISPLAY) return;
        eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (m_context != EGL_NO_CONTEXT) eglDestroyContext(m_display, m_context);
        eglTerminate(m_display);
        m_display = EGL_NO_DISPLAY;
        m_context = EGL_NO_CONTEXT;
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

// Reads an imported texture the way a compositor does: SAMPLED through a shader into an
// ordinary RGBA8 target, rather than read straight out of its own memory.
//
// That distinction is the whole reason this exists. The importer turns B8G8R8A8 memory into the
// colours that were rendered with GL_TEXTURE_SWIZZLE_R/B, and a swizzle applies to texture
// LOOKUPS only -- a framebuffer read of the texture itself bypasses it completely. So a raw read
// cannot see the channel plumbing at all, and this is the only view of the texture that can.
//
// texelFetch rather than texture(): no filtering, no coordinate rounding, one texel per
// destination pixel, so a mismatch is a channel or a layout and never a sample position.
//
// Returns an empty vector and sets `why` when the harness itself could not run -- a caller must
// report that as its own fault, not as a verdict on the importer.
inline std::vector<unsigned char> readSampled(GLuint texture, uint32_t width, uint32_t height, std::string& why) {
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

    // Drained first: GL keeps errors queued until read, so one left by the caller would be
    // attributed to this sampling and turn the pixel check into a skip about the wrong thing.
    while (glGetError() != GL_NO_ERROR) {
    }

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
        why = std::string("this harness's own sampling shader would not compile: ") + log.data();
        glDeleteShader(shader);
        return 0;
    };

    // Every exit below runs this, so a failure partway through frees everything this function
    // made -- the descriptor and object accounting a caller measures is taken against it. It
    // does NOT restore the caller's bindings: they are zeroed, and the sampled texture keeps the
    // filter and wrap this set. A caller that had bindings worth keeping must save them itself.
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
        why = std::string("this harness's own sampling program would not link: ") + log.data();
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
        why = "this harness could not build a framebuffer to sample the imported texture into";
        tidy();
        return pixels;
    }

    // NEAREST, and not for filtering's sake: the default minification filter wants mipmaps, and
    // an imported texture has exactly one level -- left alone it is an incomplete texture and
    // every lookup returns black, which would read as a wrong import.
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

}  // namespace wallpaperx::testing
