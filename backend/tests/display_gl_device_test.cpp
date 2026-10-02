// SPDX-License-Identifier: GPL-2.0-or-later

// Whether the GL that display entries get under the nested compositor is on the real GPU. Every
// import entry on an opaque-fd host skips, rather than fails, when its GL context is on another
// device, so a nested compositor that pushed clients onto software GL would turn the whole NVIDIA
// import path from run to skipped with nothing red. This entry is what goes red instead.

#include "tests/test_report.h"

#include <EGL/egl.h>
#include <GL/gl.h>
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using wallpaperx::testing::check;
using wallpaperx::testing::notVerified;
using Uuid = std::array<uint8_t, VK_UUID_SIZE>;

std::string hex(const Uuid& uuid) {
    std::string out;
    char byte[3];
    for (uint8_t b : uuid) {
        std::snprintf(byte, sizeof byte, "%02x", b);
        out += byte;
    }
    return out;
}

// The UUID and name of every Vulkan device that is not a CPU implementation.
struct HardwareDevice {
    Uuid uuid{};
    std::string name;
};
bool hardwareDevices(std::vector<HardwareDevice>& out, std::string& why) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&info, nullptr, &instance) != VK_SUCCESS) {
        why = "no Vulkan instance could be created";
        return false;
    }
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());
    for (VkPhysicalDevice device : devices) {
        VkPhysicalDeviceIDProperties ids{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &ids};
        vkGetPhysicalDeviceProperties2(device, &properties);
        if (properties.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) continue;
        HardwareDevice found;
        std::copy(std::begin(ids.deviceUUID), std::end(ids.deviceUUID), found.uuid.begin());
        found.name = properties.properties.deviceName;
        out.push_back(found);
    }
    vkDestroyInstance(instance, nullptr);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    wallpaperx::testing::g_testName = "display_gl_device_test";
    const char* kGoal = "the nested compositor's GL is on the real GPU";

    std::vector<HardwareDevice> gpus;
    std::string why;
    if (!hardwareDevices(gpus, why) || gpus.empty()) {
        notVerified(kGoal, gpus.empty() && why.empty()
                               ? "Vulkan finds only CPU devices here, so there is no real GPU for GL to be on"
                               : why);
        return wallpaperx::testing::reportNotVerifiedOrPassed("goal(s) could not be exercised (listed above)", "");
    }
    QGuiApplication app(argc, argv);
    if (QGuiApplication::platformName() != QLatin1String("wayland")) {
        notVerified(kGoal, "Qt is on '" + QGuiApplication::platformName().toStdString() +
                               "', not the nested compositor (isolated_run.sh says why above)");
        return wallpaperx::testing::reportNotVerifiedOrPassed("goal(s) could not be exercised (listed above)", "");
    }

    QOpenGLContext context;
    QOffscreenSurface surface;
    surface.create();
    const bool current = context.create() && context.makeCurrent(&surface);
    check(current, "a GL context can be made current under the nested compositor");
    if (!current) return wallpaperx::testing::reportCheckFailures() ? 1 : 0;

    const auto* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const std::string glRenderer = renderer != nullptr ? renderer : "(unknown)";
    std::printf("display_gl_device_test: GL under the nested compositor is '%s'\n", glRenderer.c_str());

    // GOAL: the GL context a display entry gets is on one of this host's hardware Vulkan devices.
    // WHY-THIS-SHAPE: compared by GL_DEVICE_UUID_EXT, not by renderer name, which differs between GL
    //       and Vulkan for the same GPU. A context that cannot answer is red too: without
    //       EXT_memory_object no import entry can import at all.
    // CONTROL: observed 2026-10-01, the entry run with __EGL_VENDOR_LIBRARY_FILENAMES listing Mesa's
    //       vendor file before NVIDIA's, so GL came up on the AMD integrated GPU (radeonsi) while
    //       Vulkan stayed on the NVIDIA one -> "FAIL: the nested compositor's GL context is on a
    //       hardware Vulkan device". Mesa's file alone hides NVIDIA's Vulkan too, and skips instead.
    Uuid glUuid{};
    using GetUnsignedBytevFn = void (*)(GLenum, GLubyte*);
    auto getBytes = reinterpret_cast<GetUnsignedBytevFn>(eglGetProcAddress("glGetUnsignedBytevEXT"));
    bool answered = false;
    if (getBytes != nullptr) {
        while (glGetError() != GL_NO_ERROR) {
        }
        constexpr GLenum kDeviceUuidExt = 0x9597;  // GL_DEVICE_UUID_EXT
        getBytes(kDeviceUuidExt, glUuid.data());
        answered = glGetError() == GL_NO_ERROR;
    }
    bool matched = false;
    for (const HardwareDevice& gpu : gpus) matched = matched || (answered && gpu.uuid == glUuid);
    check(matched, "the nested compositor's GL context is on a hardware Vulkan device");
    if (!matched) {
        std::fprintf(stderr, "  GL '%s' reports device %s\n", glRenderer.c_str(),
                     answered ? hex(glUuid).c_str() : "(no GL_DEVICE_UUID_EXT)");
        for (const HardwareDevice& gpu : gpus) std::fprintf(stderr, "  Vulkan '%s' is %s\n", gpu.name.c_str(), hex(gpu.uuid).c_str());
    }
    context.doneCurrent();
    if (wallpaperx::testing::reportCheckFailures()) return 1;
    return wallpaperx::testing::reportNotVerifiedOrPassed("goal(s) could not be exercised (listed above)", "");
}
