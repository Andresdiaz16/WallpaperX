// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Test-only pieces shared by the probes that run ffmpeg's Vulkan decoder: the validation oracle
// that excuses ffmpeg's one known message, the filter that makes a GPU with a decoder report
// none, and a scratch directory of the process's own. Both probes must agree on what is
// excused, so there is one definition.

#include "check.h"
#include "validation_grade.h"
#include "vulkan/device.h"

#include <stdlib.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace wallpaperx::testing {

// ffmpeg's Vulkan decoder binds one reference slot per DPB picture where this driver's profile
// wants a single layered image. Reproducible with the stock `ffmpeg -hwaccel vulkan` command
// line, so it is a defect in a dependency. Named exactly, so any other complaint still fails.
inline constexpr const char* kExcusedDecoderMessageId = "VUID-VkVideoBeginCodingInfoKHR-flags-07244";

// A Device's validation messages graded with that one id excused, on a messenger of this
// oracle's own. attach() right after the Device is built, detach() before it is destroyed,
// reconcile() after -- the last is what catches a message outside the messenger's window.
class DecoderValidationOracle {
public:
    DecoderValidationOracle() = default;
    DecoderValidationOracle(const DecoderValidationOracle&) = delete;
    DecoderValidationOracle& operator=(const DecoderValidationOracle&) = delete;

    void attach(Device& device, ValidationGrade& grade) {
        m_deviceCounter = device.validationCounter();
        // The verdict is the UNEXPECTED count, not the Device's. Everything else about the
        // grade -- the waiver, the "layer installed but not attached" case -- is the shared policy.
        grade.adopt(m_unexpected, device.validationLayerActive());
        m_instance = device.instance();
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
        if (create != nullptr) {
            VkDebugUtilsMessengerCreateInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            info.messageSeverity =
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            info.pfnUserCallback = &DecoderValidationOracle::callback;
            info.pUserData = this;
            if (create(m_instance, &info, nullptr, &m_messenger) != VK_SUCCESS) m_messenger = VK_NULL_HANDLE;
        }
        if (m_messenger == VK_NULL_HANDLE && device.validationLayerActive()) {
            fail("a validator is attached to the Device but no messenger of the probe's own could be installed, so "
                 "nothing classified its messages");
        }
    }

    void detach() noexcept {
        if (m_messenger == VK_NULL_HANDLE) return;
        auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy != nullptr) destroy(m_instance, m_messenger, nullptr);
        m_messenger = VK_NULL_HANDLE;
    }

    // GOAL: the excused-message list must not become a hole. Every validation message the
    //       Device counted has to be one this messenger saw and classified; anything left over
    //       arrived outside its window -- instance creation, or teardown, where the layer reports
    //       objects still in flight and allocations never freed.
    // WHY-THIS-SHAPE: the messenger cannot exist during vkCreateInstance or vkDestroyInstance, so
    //       comparing totals is what recovers those windows. Asserting only "no unexpected
    //       messages" would leave a teardown complaint ungraded.
    // CONTROL: renderer/vulkan/device.cpp, destroyResources() with the `vkDestroyCommandPool`
    //       call removed, so the layer reports a live object at vkDestroyDevice. Observed on
    //       video_decoder_probe (content): "FAIL: 1 validation message(s) were counted by the
    //       Device but never reached this probe's messenger". Reverted.
    void reconcile(const char* probe) const {
        if (!m_deviceCounter) return;
        const int deviceTotal = m_deviceCounter->load();
        const int classified = m_excused.load() + m_unexpected->load();
        if (deviceTotal > classified) {
            std::fprintf(stderr,
                         "FAIL: %s: %d validation message(s) were counted by the Device but never reached this "
                         "probe's messenger, so they came from instance creation or teardown\n",
                         probe, deviceTotal - classified);
            fail("a validation message was emitted outside the window this probe classifies");
        }
        if (m_excused.load() > 0) {
            std::printf("%s: %d message(s) matching %s were excused -- ffmpeg's own Vulkan decoder, reproducible "
                        "with the stock ffmpeg command line\n",
                        probe, m_excused.load(), kExcusedDecoderMessageId);
        }
    }

private:
    static VKAPI_ATTR VkBool32 VKAPI_CALL callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                   VkDebugUtilsMessageTypeFlagsEXT types,
                                                   const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                   void* userData) {
        auto* self = static_cast<DecoderValidationOracle*>(userData);
        // The same two filters Device::debugCallback applies, so the two counts are comparable.
        if (severity < VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) return VK_FALSE;
        if ((types & VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT) != 0 &&
            (types & (VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                      VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)) == 0) {
            return VK_FALSE;
        }
        const char* id = data->pMessageIdName != nullptr ? data->pMessageIdName : "";
        if (std::strcmp(id, kExcusedDecoderMessageId) == 0) {
            self->m_excused.fetch_add(1, std::memory_order_relaxed);
        } else {
            self->m_unexpected->fetch_add(1, std::memory_order_relaxed);
        }
        return VK_FALSE;
    }

    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_messenger = VK_NULL_HANDLE;
    std::shared_ptr<const std::atomic<int>> m_deviceCounter;
    std::atomic<int> m_excused{0};
    std::shared_ptr<std::atomic<int>> m_unexpected = std::make_shared<std::atomic<int>>(0);
};

using EnumerateDeviceExtensionsFn = VkResult (*)(VkPhysicalDevice, const char*, uint32_t*, VkExtensionProperties*);

// What `real` reports with every VK_KHR_video* extension removed, keeping the two-call protocol.
// For an interposed vkEnumerateDeviceExtensionProperties: a GPU with a decoder then builds a
// Device with none, which is how a software path is reached on the real GPU.
inline VkResult enumerateWithoutVideoExtensions(EnumerateDeviceExtensionsFn real, VkPhysicalDevice physicalDevice,
                                                const char* layerName, uint32_t* count,
                                                VkExtensionProperties* properties) {
    uint32_t total = 0;
    if (const VkResult r = real(physicalDevice, layerName, &total, nullptr); r != VK_SUCCESS) return r;
    std::vector<VkExtensionProperties> all(total);
    if (const VkResult r = real(physicalDevice, layerName, &total, all.data()); r != VK_SUCCESS) return r;

    std::vector<VkExtensionProperties> kept;
    kept.reserve(all.size());
    for (const VkExtensionProperties& entry : all) {
        if (std::strncmp(entry.extensionName, "VK_KHR_video", 12) == 0) continue;
        kept.push_back(entry);
    }
    if (properties == nullptr) {
        *count = static_cast<uint32_t>(kept.size());
        return VK_SUCCESS;
    }
    const uint32_t give = *count < kept.size() ? *count : static_cast<uint32_t>(kept.size());
    for (uint32_t i = 0; i < give; ++i) properties[i] = kept[i];
    *count = give;
    return give < kept.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

// A scratch directory of this process's own, removed with this object. Deliberately not a shared
// asset directory: the PNG probes hold RESOURCE_LOCKs these probes do not, and a directory-scoped
// cleanup would delete a fixture out from under a probe running in parallel.
class ScratchDir {
public:
    explicit ScratchDir(const char* prefix) {
        std::string pattern = (std::filesystem::temp_directory_path() / (std::string(prefix) + "-XXXXXX")).string();
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        if (const char* made = mkdtemp(buffer.data())) m_path = made;
    }
    ~ScratchDir() {
        if (m_path.empty()) return;
        std::error_code ignored;
        std::filesystem::remove_all(m_path, ignored);
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;

    bool ok() const { return !m_path.empty(); }
    std::string file(const char* name) const { return (std::filesystem::path(m_path) / name).string(); }

private:
    std::string m_path;
};

}  // namespace wallpaperx::testing
