// SPDX-License-Identifier: GPL-2.0-or-later

#include "device.h"

#include "descriptor_buffer.h"
#include "descriptor_classic.h"
#include "descriptor_heap.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <stdexcept>
#include <utility>
#include <vector>

namespace wallpaperx {

const char* toString(DescriptorTier tier) {
    switch (tier) {
        case DescriptorTier::Heap: return "Heap";
        case DescriptorTier::Buffer: return "Buffer";
        case DescriptorTier::Classic: return "Classic";
    }
    return "Unknown";
}

namespace {

// How long a caller that must see its work finished waits before calling the GPU wedged.
// Unbounded -- what this was -- parks the render worker forever on a device that never
// completes, and the daemon has no other way to notice. Two to three orders of magnitude
// above anything submitted here (a fullscreen quad, a texture upload, an empty batch
// carrying a semaphore signal), so it cannot fire on a GPU that is merely slow.
constexpr std::chrono::seconds kSubmitTimeout{10};
constexpr uint64_t kSubmitTimeoutNs = std::chrono::nanoseconds(kSubmitTimeout).count();

// Ceiling on batches submitted but not yet known complete. Not a tuning knob: it is what
// bounds the pending list, and reaching it makes the next submit wait for the oldest batch.
constexpr size_t kMaxSubmissionsInFlight = 16;

// Each codec's decode extension and the operation bit a queue family advertises for it. One
// table, so the extension enabled and the capability reported cannot name different codecs.
// Only codecs whose extension needs no feature of its own belong here: VP9's needs
// videoDecodeVP9 chained at creation, so naming it by extension alone would advertise a
// codec no session can be created for.
constexpr std::array<std::pair<const char*, VkVideoCodecOperationFlagBitsKHR>, 3> kVideoDecodeCodecs{{
    {VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME, VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR},
    {VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME, VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR},
    {VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME, VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR},
}};

// Preference order among devices that already passed the capability checks. A rank, not a
// "discrete beats everything else" test: that form never displaced whatever enumerated first,
// so a software rasteriser ahead of an integrated GPU in the list kept the wallpaper on the CPU.
int deviceRank(VkPhysicalDeviceType type) {
    switch (type) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 4;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 2;
        // Last, never excluded: it is the only device in continuous integration, where the
        // software rasteriser is the whole point.
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return 1;
        default: return 0;
    }
}

bool hasExtension(const std::vector<VkExtensionProperties>& extensions, const char* name) {
    for (const auto& ext : extensions) {
        if (std::strcmp(ext.extensionName, name) == 0) return true;
    }
    return false;
}

// A failed query is reported, not thrown: everything an extension enables here is optional,
// so the device still builds without it. But it is reported, because an empty list also
// reads as "this device has none" -- and for the video extensions that silently selects
// software decode, a wrong answer that looks like a right one.
std::vector<VkExtensionProperties> enumerateDeviceExtensions(VkPhysicalDevice physicalDevice) {
    uint32_t count = 0;
    VkResult r = vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(r == VK_SUCCESS ? count : 0);
    if (r == VK_SUCCESS && count > 0) {
        r = vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, extensions.data());
        extensions.resize(r == VK_SUCCESS ? count : 0);
    }
    if (r != VK_SUCCESS) {
        std::fprintf(stderr,
                     "[vulkan] vkEnumerateDeviceExtensionProperties failed with %s; treating the device as having "
                     "no optional extension -- no dma_buf export, no descriptor heap or buffer, no hardware video "
                     "decode\n",
                     vkResultName(r).c_str());
    }
    return extensions;
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT types,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* userData) {
    if (severity < VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) return VK_FALSE;

    // Printed unconditionally. GENERAL is the loader's own channel and during instance
    // creation it carries host-configuration complaints -- "Failed to CreateInstance in
    // ICD 1. Skipping ICD.", malformed manifests. Those are exactly what an operator
    // needs to see when a machine misbehaves, so suppressing them at the messenger's
    // delivery mask (which is what excluding the type did) threw away the diagnostic in
    // order to avoid counting it.
    // One call, so the line reaches stderr in one write and no other thread's lands inside it.
    std::fprintf(stderr, "[vulkan] %s\n", data->pMessage);

    // Counted only when it is the validation layer talking about us. A stale ICD
    // manifest left over from an uninstalled driver is a host packaging problem, and
    // counting it made every probe fail as though the renderer had regressed.
    if ((types & VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT) != 0 &&
        (types & (VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                  VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)) == 0) {
        return VK_FALSE;
    }
    // pUserData is the messenger's own channel for this, set to the owning Device at
    // creation. It is what makes the count a property of the device rather than of the
    // process, and it is why no global is needed.
    if (auto* device = static_cast<Device*>(userData)) device->noteValidationWarning();
    return VK_FALSE;
}

VkDebugUtilsMessengerCreateInfoEXT debugMessengerInfo(void* owner) {
    VkDebugUtilsMessengerCreateInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    // Every type is delivered, including GENERAL: debugCallback decides what to COUNT,
    // and it needs to see the loader's messages in order to print them.
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = debugCallback;
    info.pUserData = owner;
    return info;
}

}

Device::Device(bool enableValidation, std::function<void()> onLost) : m_onLost(std::move(onLost)) {
    try {
        createInstance(enableValidation);
        selectPhysicalDevice();
        determineTier();
        createLogicalDevice();
        m_memory.init(m_instance, m_physicalDevice, m_device, VK_API_VERSION_1_3,
                      m_features.vulkan12.bufferDeviceAddress == VK_TRUE, m_info.hasImageDrmFormatModifier);
        createDescriptorAllocator();
        createCommandPool();
    } catch (...) {
        // A C++ destructor does not run for an object whose constructor throws,
        // so without this the instance/device/allocator created before the throw
        // point would leak (and Memory's leaked-allocation check would never even
        // run). destroyResources() is null-guarded, so it is safe on a partially
        // constructed Device.
        destroyResources();
        throw;
    }
}

Device::~Device() {
    destroyResources();
}

void Device::destroyResources() {
    // Before anything is freed, because submit() does not wait: an executing batch is reading
    // its own command buffer and every image, buffer and descriptor the allocator below is
    // about to destroy.
    const bool idle = drainForTeardown();

    // Everything the device owns is leaked, deliberately, once anything has been abandoned --
    // including when the GPU has since caught up, because what was abandoned is unreachable
    // now, and vkDestroyDevice requires every child object to be gone while VMA aborts the
    // process outright if an allocation is still live.
    if (m_abandoned) {
        // The allocator among them: it owns descriptor heaps, pools and layouts a running batch
        // reads, and as a member it would otherwise be destroyed a moment later by ~Device --
        // freeing exactly what this leak exists to keep alive. Nothing can call
        // descriptorAllocator() afterwards; this runs from ~Device, and from the constructor's
        // catch, where nothing was ever submitted and so nothing is abandoned. Memory needs no
        // equivalent: it declares no destructor, so skipping shutdown() already leaks it.
        static_cast<void>(m_descriptorAllocator.release());
        std::fprintf(stderr,
                     "Device: %s, so the Vulkan device and everything it owns is leaked rather than destroyed\n",
                     idle ? "resources were abandoned to a GPU that had not finished reading them"
                          : "the GPU did not finish the work submitted to it");
    } else {
        if (m_device) {
            for (const Pending& batch : m_pending) vkDestroyFence(m_device, batch.fence, nullptr);
            for (VkFence fence : m_freeFences) vkDestroyFence(m_device, fence, nullptr);
        }
        m_pending.clear();
        m_freeFences.clear();
        // The pool owns them; destroying it below frees them all.
        m_freeCommandBuffers.clear();

        if (m_commandPool) {
            vkDestroyCommandPool(m_device, m_commandPool, nullptr);
            m_commandPool = VK_NULL_HANDLE;
        }
        m_descriptorAllocator.reset();
        m_memory.shutdown();

        if (m_device) {
            vkDestroyDevice(m_device, nullptr);
            m_device = VK_NULL_HANDLE;
        }
    }

    // Destroyed on both paths, and after the destructions above so their complaints are still
    // delivered: the messenger has no idle precondition, and its callback reaches this Device
    // through pUserData -- left alive on a leaked instance, the next message would read freed
    // memory.
    if (m_debugMessenger) {
        auto destroyFn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroyFn) destroyFn(m_instance, m_debugMessenger, nullptr);
        m_debugMessenger = VK_NULL_HANDLE;
    }

    // The instance is not destroyable while a leaked device is still its child.
    if (m_instance && !m_abandoned) {
        vkDestroyInstance(m_instance, nullptr);
        m_instance = VK_NULL_HANDLE;
    }
}

void Device::createInstance(bool enableValidation) {
    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = "WallpaperX";
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.pEngineName = "wallpaperx-renderer";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.apiVersion = VK_API_VERSION_1_3;

    std::vector<const char*>& instanceExtensions = m_enabledInstanceExtensions;
    std::vector<const char*> instanceLayers;

    bool validationAvailable = false;
    bool syncValidationAvailable = false;
    if (enableValidation) {
        uint32_t layerCount = 0;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
        std::vector<VkLayerProperties> layers(layerCount);
        vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
        for (const auto& layer : layers) {
            if (std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
                validationAvailable = true;
                break;
            }
        }
        if (validationAvailable) {
            instanceLayers.push_back("VK_LAYER_KHRONOS_validation");
            instanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

            // VK_EXT_validation_features is provided by the layer, not the loader, and
            // it is deprecated in favour of VK_EXT_layer_settings -- so a stripped or
            // newer layer build may not expose it. Enabling it blind would fail
            // vkCreateInstance outright and take the whole renderer down; the correct
            // degradation is to run without synchronization validation.
            uint32_t layerExtCount = 0;
            vkEnumerateInstanceExtensionProperties("VK_LAYER_KHRONOS_validation", &layerExtCount, nullptr);
            std::vector<VkExtensionProperties> layerExts(layerExtCount);
            vkEnumerateInstanceExtensionProperties("VK_LAYER_KHRONOS_validation", &layerExtCount, layerExts.data());
            syncValidationAvailable = hasExtension(layerExts, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
            if (syncValidationAvailable) {
                instanceExtensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
            } else {
                std::fprintf(stderr, "[vulkan] validation layer present but VK_EXT_validation_features is not -- "
                                     "running without synchronization validation\n");
            }
        }
    }

    // The layer's default checks cover API usage, not GPU hazards: missing barriers,
    // layout hazards and destroy-while-in-flight are all silent unless synchronization
    // validation is opted into explicitly. Both ICDs we test on are poor witnesses for
    // sync bugs (Lavapipe largely serializes; NVIDIA is forgiving about layouts) while
    // RADV is not, so without this the first AMD user is the sync validator.
    // Thread-safety checks are on by default and need no opt-in here.
    constexpr VkValidationFeatureEnableEXT kSyncValidation =
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validationFeatures{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    validationFeatures.enabledValidationFeatureCount = 1;
    validationFeatures.pEnabledValidationFeatures = &kSyncValidation;

    // A second messenger description, chained into pNext, covering vkCreateInstance and
    // vkDestroyInstance themselves. The messenger created below cannot: it does not exist
    // until after vkCreateInstance returns, and destroyResources() destroys it before
    // vkDestroyInstance runs -- which is where the layer emits its object-leak report,
    // the single most valuable teardown complaint and the whole reason the counter is
    // shared so it can be read after ~Device. Without this the count was structurally
    // blind to it. Per the spec this description is used only for vkCreateInstance and
    // vkDestroyInstance, so it does not double-count what the standing messenger sees.
    // (An earlier revision quoted a 43 -> 156 measurement here as evidence of that. It
    // was taken before debugCallback began filtering GENERAL out of the count, so the
    // stated experiment no longer reproduces the stated numbers; the claim now rests on
    // the specification rather than on a figure a reader cannot check.)
    VkDebugUtilsMessengerCreateInfoEXT instanceMessenger = debugMessengerInfo(this);

    VkInstanceCreateInfo createInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    createInfo.pApplicationInfo = &appInfo;
    // No `else if (syncValidationAvailable)` branch: that flag is only ever assigned
    // inside `if (validationAvailable)` above, so "sync validation without the layer" is
    // not a reachable state and a fallback for it would be dead code that reads live.
    if (validationAvailable) {
        instanceMessenger.pNext = syncValidationAvailable ? &validationFeatures : nullptr;
        createInfo.pNext = &instanceMessenger;
    }
    createInfo.enabledLayerCount = static_cast<uint32_t>(instanceLayers.size());
    createInfo.ppEnabledLayerNames = instanceLayers.data();
    createInfo.enabledExtensionCount = static_cast<uint32_t>(instanceExtensions.size());
    createInfo.ppEnabledExtensionNames = instanceExtensions.data();

    if (const VkResult r = vkCreateInstance(&createInfo, nullptr, &m_instance); r != VK_SUCCESS) {
        throwVkFailure("Device: vkCreateInstance", r);
    }

    if (validationAvailable) {
        auto createFn = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
        if (createFn) {
            VkDebugUtilsMessengerCreateInfoEXT messengerInfo = debugMessengerInfo(this);
            // Set from the call SUCCEEDING, not from the layer being installed. The flag
            // answers "is anything feeding the counter", and every caller uses it to tell
            // a clean run from an unwatched one -- so recording it at layer-enumeration
            // time made it report true in exactly the case it exists to catch: createFn
            // null, or vkCreateDebugUtilsMessengerEXT failing, leaves no messenger and a
            // count that stays 0 no matter what the GPU did.
            m_validationLayerActive =
                createFn(m_instance, &messengerInfo, nullptr, &m_debugMessenger) == VK_SUCCESS &&
                m_debugMessenger != VK_NULL_HANDLE;
        }
    }
}

void Device::selectPhysicalDevice() {
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr);
    if (deviceCount == 0) {
        throw std::runtime_error("Device: no Vulkan-capable GPU found");
    }
    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(m_instance, &deviceCount, devices.data());

    VkPhysicalDevice best = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties bestProps{};
    uint32_t bestGraphicsQueueFamily = UINT32_MAX;

    for (VkPhysicalDevice candidate : devices) {
        uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> extensions(extCount);
        vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extCount, extensions.data());

        if (!hasExtension(extensions, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)) {
            continue;
        }

        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueFamilyCount, queueFamilies.data());

        uint32_t graphicsQueueFamily = UINT32_MAX;
        for (uint32_t i = 0; i < queueFamilyCount; ++i) {
            if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                graphicsQueueFamily = i;
                break;
            }
        }
        if (graphicsQueueFamily == UINT32_MAX) {
            continue;
        }

        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(candidate, &props);
        // The instance asks for 1.3, but a device may report lower than its instance, and this
        // renderer creates it with the 1.3 feature block chained, which is invalid on one that does.
        if (props.apiVersion < VK_API_VERSION_1_3) {
            continue;
        }

        const bool isBetter = best == VK_NULL_HANDLE || deviceRank(props.deviceType) > deviceRank(bestProps.deviceType);

        if (isBetter) {
            best = candidate;
            bestProps = props;
            bestGraphicsQueueFamily = graphicsQueueFamily;
        }
    }

    if (best == VK_NULL_HANDLE) {
        throw std::runtime_error(
            "Device: no Vulkan 1.3 GPU with VK_KHR_external_memory_fd and a graphics queue found");
    }

    m_physicalDevice = best;
    m_graphicsQueueFamily = bestGraphicsQueueFamily;
    m_info.gpuName = bestProps.deviceName;
    m_info.vendor = vendorFromId(bestProps.vendorID);

    VkPhysicalDeviceIDProperties idProps{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    props2.pNext = &idProps;
    vkGetPhysicalDeviceProperties2(m_physicalDevice, &props2);
    std::memcpy(m_info.deviceUuid.data(), idProps.deviceUUID, VK_UUID_SIZE);

    const std::vector<VkExtensionProperties> extensions = enumerateDeviceExtensions(m_physicalDevice);

    m_info.hasDescriptorHeap = hasExtension(extensions, VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME);
    m_info.hasDescriptorBuffer = hasExtension(extensions, VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME);
    m_info.hasExternalMemoryDmaBuf = hasExtension(extensions, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
    // Deliberately NOT folded into hasExternalMemoryDmaBuf: a driver with dma_buf but
    // no modifier support can still render and export, it just cannot describe the
    // layout. Requiring both would make externalMemoryHandleType() throw on such a
    // driver and take the wallpaper down entirely, trading a frame no compositor can
    // import for no frame at all.
    m_info.hasImageDrmFormatModifier = hasExtension(extensions, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
    m_info.hasQueueFamilyForeign = hasExtension(extensions, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);

    // The per-family codec list is only queryable through VK_KHR_video_queue, and chaining its
    // properties struct on a device without the extension is a validation error, not a no-op.
    if (hasExtension(extensions, VK_KHR_VIDEO_QUEUE_EXTENSION_NAME) &&
        hasExtension(extensions, VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME)) {
        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties2(m_physicalDevice, &familyCount, nullptr);
        std::vector<VkQueueFamilyVideoPropertiesKHR> video(
            familyCount, VkQueueFamilyVideoPropertiesKHR{VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR});
        std::vector<VkQueueFamilyProperties2> families(familyCount,
                                                       VkQueueFamilyProperties2{VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2});
        for (uint32_t i = 0; i < familyCount; ++i) families[i].pNext = &video[i];
        vkGetPhysicalDeviceQueueFamilyProperties2(m_physicalDevice, &familyCount, families.data());

        // Every decode family, not the first: a driver may split its codecs across families.
        for (uint32_t i = 0; i < familyCount; ++i) {
            if ((families[i].queueFamilyProperties.queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) == 0) continue;
            m_decodeFamilyOps.emplace_back(i, video[i].videoCodecOperations);
        }
    }
    // Gated on the handle type as well as the extension, unlike every flag above. Lavapipe
    // advertises VK_KHR_external_semaphore_fd and then fails vkCreateSemaphore with
    // VK_ERROR_INVALID_EXTERNAL_HANDLE for an OPAQUE_FD export -- it offers SYNC_FD only, which is
    // no use here because GL_EXT_semaphore_fd imports opaque descriptors and nothing else. The
    // extension alone therefore answers a different question from the one every reader asks.
    if (hasExtension(extensions, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME)) {
        VkPhysicalDeviceExternalSemaphoreInfo semaphoreQuery{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
        semaphoreQuery.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkExternalSemaphoreProperties semaphoreProps{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
        vkGetPhysicalDeviceExternalSemaphoreProperties(m_physicalDevice, &semaphoreQuery, &semaphoreProps);
        m_info.hasExternalSemaphoreFd =
            (semaphoreProps.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT) != 0;
    }

    if (m_info.hasDescriptorHeap) {
        // The heap tier doesn't just need VK_EXT_descriptor_heap: createLogicalDevice()
        // also enables VK_KHR_maintenance5 + VK_KHR_shader_untyped_pointers and the
        // maintenance5 / shaderUntypedPointers / shaderSampledImageArrayNonUniformIndexing
        // features. If any of those is missing, enabling them at vkCreateDevice would
        // fail the whole device instead of falling back, so gate heap availability on
        // all of them here (mirrors how descriptorBuffer is gated below).
        const bool hasHeapDeps =
            hasExtension(extensions, VK_KHR_MAINTENANCE_5_EXTENSION_NAME) &&
            hasExtension(extensions, VK_KHR_SHADER_UNTYPED_POINTERS_EXTENSION_NAME);

        VkPhysicalDeviceDescriptorHeapFeaturesEXT heapFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT};
        VkPhysicalDeviceMaintenance5FeaturesKHR maintenance5Features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
        VkPhysicalDeviceShaderUntypedPointersFeaturesKHR untypedPointersFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_UNTYPED_POINTERS_FEATURES_KHR};
        VkPhysicalDeviceVulkan12Features vulkan12Features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        heapFeatures.pNext = &maintenance5Features;
        maintenance5Features.pNext = &untypedPointersFeatures;
        untypedPointersFeatures.pNext = &vulkan12Features;
        VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features2.pNext = &heapFeatures;
        vkGetPhysicalDeviceFeatures2(m_physicalDevice, &features2);

        m_info.hasDescriptorHeap = hasHeapDeps && heapFeatures.descriptorHeap == VK_TRUE &&
                                   maintenance5Features.maintenance5 == VK_TRUE &&
                                   untypedPointersFeatures.shaderUntypedPointers == VK_TRUE &&
                                   vulkan12Features.shaderSampledImageArrayNonUniformIndexing == VK_TRUE;
    }
    if (m_info.hasDescriptorBuffer) {
        VkPhysicalDeviceDescriptorBufferFeaturesEXT bufferFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT};
        VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features2.pNext = &bufferFeatures;
        vkGetPhysicalDeviceFeatures2(m_physicalDevice, &features2);
        m_info.hasDescriptorBuffer = bufferFeatures.descriptorBuffer == VK_TRUE;
    }
}

void Device::determineTier() {
    m_selectedTier = selectTier(
        m_info.hasDescriptorHeap, m_info.hasDescriptorBuffer,
        parseTierOverride(std::getenv("WALLPAPERX_FORCE_TIER")));
}

void Device::createLogicalDevice() {
    const bool useHeap = m_selectedTier == DescriptorTier::Heap;
    const bool useBuffer = m_selectedTier == DescriptorTier::Buffer;

    std::vector<const char*>& deviceExtensions = m_enabledDeviceExtensions;
    deviceExtensions = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME};
    if (m_info.hasExternalMemoryDmaBuf) {
        deviceExtensions.push_back(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
    }
    if (m_info.hasImageDrmFormatModifier) {
        deviceExtensions.push_back(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
    }
    if (m_info.hasQueueFamilyForeign) {
        deviceExtensions.push_back(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
    }
    if (m_info.hasExternalSemaphoreFd) {
        deviceExtensions.push_back(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
    }
    if (useHeap) {
        deviceExtensions.push_back(VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME);
        deviceExtensions.push_back(VK_KHR_SHADER_UNTYPED_POINTERS_EXTENSION_NAME);
        // VK_EXT_descriptor_heap requires VK_KHR_maintenance5, which is also what
        // the heap-tier graphics pipeline's VkPipelineCreateFlags2CreateInfo needs.
        deviceExtensions.push_back(VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
    } else if (useBuffer) {
        deviceExtensions.push_back(VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME);
    }

    const std::vector<VkExtensionProperties> available = enumerateDeviceExtensions(m_physicalDevice);

    // A decoder library's frames carry timeline semaphores and synchronization2 access masks,
    // and it resets its result queries from the host: those three gate the decode queue. The
    // Y'CbCr conversion feature gates nothing and nothing here samples through one; it is
    // enabled when present because this chain is handed to ffmpeg, whose Vulkan code creates
    // Y'CbCr conversions of its own. All four are core and change nothing about how anything
    // already here behaves.
    VkPhysicalDeviceVulkan11Features supported11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features supported12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features supported13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    supported11.pNext = &supported12;
    supported12.pNext = &supported13;
    VkPhysicalDeviceFeatures2 supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    supported.pNext = &supported11;
    vkGetPhysicalDeviceFeatures2(m_physicalDevice, &supported);

    m_features.vulkan11.samplerYcbcrConversion = supported11.samplerYcbcrConversion;
    m_features.vulkan12.bufferDeviceAddress = (useHeap || useBuffer) ? VK_TRUE : VK_FALSE;
    // The heap-tier fragment shader (fullscreen_heap.frag) indexes the descriptor
    // heap arrays with nonuniformEXT, which requires this feature to be enabled.
    m_features.vulkan12.shaderSampledImageArrayNonUniformIndexing = useHeap ? VK_TRUE : VK_FALSE;
    m_features.vulkan12.timelineSemaphore = supported12.timelineSemaphore;
    m_features.vulkan12.hostQueryReset = supported12.hostQueryReset;
    m_features.vulkan13.synchronization2 = supported13.synchronization2;

    const bool decoderPrerequisites = m_features.vulkan12.timelineSemaphore == VK_TRUE &&
                                      m_features.vulkan12.hostQueryReset == VK_TRUE &&
                                      m_features.vulkan13.synchronization2 == VK_TRUE;
    // A codec is enabled when some decode family advertises it and its extension exists.
    std::vector<const char*> codecExtensions;
    if (decoderPrerequisites) {
        for (const auto& [extension, op] : kVideoDecodeCodecs) {
            const bool advertised = std::any_of(m_decodeFamilyOps.begin(), m_decodeFamilyOps.end(),
                                                [&](const auto& family) { return (family.second & op) != 0; });
            if (advertised && hasExtension(available, extension)) codecExtensions.push_back(extension);
        }
    }
    // A decode queue with no codec behind it would be a queue nothing can use.
    const bool enableDecode = !codecExtensions.empty();
    if (enableDecode) {
        deviceExtensions.push_back(VK_KHR_VIDEO_QUEUE_EXTENSION_NAME);
        deviceExtensions.push_back(VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME);
        deviceExtensions.insert(deviceExtensions.end(), codecExtensions.begin(), codecExtensions.end());
    }

    // The chain, in the order the header states. A tier's structs are linked and their flags
    // set together, so a struct that is out of the chain reads VK_FALSE.
    m_features.features2.pNext = &m_features.vulkan11;
    m_features.vulkan11.pNext = &m_features.vulkan12;
    m_features.vulkan12.pNext = &m_features.vulkan13;
    m_features.vulkan13.pNext = nullptr;
    if (useHeap) {
        m_features.vulkan13.pNext = &m_features.descriptorHeap;
        m_features.descriptorHeap.descriptorHeap = VK_TRUE;
        m_features.descriptorHeap.pNext = &m_features.maintenance5;
        m_features.maintenance5.maintenance5 = VK_TRUE;
        m_features.maintenance5.pNext = &m_features.untypedPointers;
        m_features.untypedPointers.shaderUntypedPointers = VK_TRUE;
        m_features.untypedPointers.pNext = nullptr;
    } else if (useBuffer) {
        m_features.vulkan13.pNext = &m_features.descriptorBuffer;
        m_features.descriptorBuffer.descriptorBuffer = VK_TRUE;
        m_features.descriptorBuffer.pNext = nullptr;
    }

    // One queue per family. A decode family that is the graphics family shares its queue: a
    // second create info for the same family is invalid, and one queue is all a single decoder
    // thread needs.
    const float queuePriority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queueInfos;
    auto addQueue = [&](uint32_t family) {
        for (const VkDeviceQueueCreateInfo& existing : queueInfos) {
            if (existing.queueFamilyIndex == family) return;
        }
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = family;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &queuePriority;
        queueInfos.push_back(queueInfo);
    };
    addQueue(m_graphicsQueueFamily);
    if (enableDecode) {
        for (const auto& [family, ops] : m_decodeFamilyOps) addQueue(family);
    }

    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.pNext = &m_features.features2;
    deviceInfo.queueCreateInfoCount = static_cast<uint32_t>(queueInfos.size());
    deviceInfo.pQueueCreateInfos = queueInfos.data();
    deviceInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
    deviceInfo.ppEnabledExtensionNames = deviceExtensions.data();

    if (const VkResult r = vkCreateDevice(m_physicalDevice, &deviceInfo, nullptr, &m_device); r != VK_SUCCESS) {
        throwVkFailure("Device: vkCreateDevice", r);
    }

    vkGetDeviceQueue(m_device, m_graphicsQueueFamily, 0, &m_graphicsQueue);

    // Read back off the extension list the device was just created with, so the codecs named
    // are exactly those whose extension is enabled. A family left with no enabled codec is
    // not listed, and its queue -- created above, since the list was not known then -- is
    // simply never fetched.
    VkVideoCodecOperationFlagsKHR enabledOps = 0;
    for (const auto& [extension, op] : kVideoDecodeCodecs) {
        if (std::find_if(deviceExtensions.begin(), deviceExtensions.end(),
                         [&](const char* e) { return std::strcmp(e, extension) == 0; }) != deviceExtensions.end()) {
            enabledOps |= op;
        }
    }
    for (const auto& [family, ops] : m_decodeFamilyOps) {
        VideoDecodeFamily decode{family, ops & enabledOps, VK_NULL_HANDLE};
        if (decode.ops == 0) continue;
        vkGetDeviceQueue(m_device, family, 0, &decode.queue);
        m_videoDecodeFamilies.push_back(decode);
        m_info.videoDecodeOps |= decode.ops;
    }
}

void Device::createDescriptorAllocator() {
    switch (m_selectedTier) {
        case DescriptorTier::Heap:
            m_descriptorAllocator = std::make_unique<DescriptorHeapAllocator>(*this);
            break;
        case DescriptorTier::Buffer:
            m_descriptorAllocator = std::make_unique<DescriptorBufferAllocator>(*this);
            break;
        case DescriptorTier::Classic:
            m_descriptorAllocator = std::make_unique<ClassicDescriptorAllocator>(*this);
            break;
    }
}

void Device::createCommandPool() {
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = m_graphicsQueueFamily;

    if (const VkResult r = vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool); r != VK_SUCCESS) {
        throwVkFailure("Device: vkCreateCommandPool", r);
    }

    // submit() enforces the cap before it adds, so this capacity is never exceeded and the
    // push after a successful vkQueueSubmit cannot throw -- which it must not, being the only
    // record that a command buffer and fence are in use.
    m_pending.reserve(kMaxSubmissionsInFlight);
    // The same, for the other end of that move. A fence or command buffer is created only when
    // the free list is empty and submit() has already established m_pending is under the cap,
    // so pending plus free never exceeds it -- which is what makes retireCompleted() unable to
    // allocate, and therefore able to answer "is the GPU idle" without an allocation deciding.
    m_freeCommandBuffers.reserve(kMaxSubmissionsInFlight);
    m_freeFences.reserve(kMaxSubmissionsInFlight);
}

void Device::assertGpuThread(const char* what) const {
    if (std::this_thread::get_id() == m_gpuThread) return;

    std::fprintf(stderr,
                 "[vulkan] FATAL: %s was made from a thread other than the one that created the Device. Vulkan "
                 "submission is not free-threaded and this Device shares one command pool, so this is a data race. "
                 "In the daemon this means a DBus handler reached the GPU directly instead of going through the "
                 "render worker.\n",
                 what ? what : "GPU call");
    std::abort();
}

void Device::immediateSubmit(const std::function<void(VkCommandBuffer)>& record) {
    // Asserted here too, though submit() asserts as well: the abort has to name the entry
    // point the caller used, not the one it happens to delegate to.
    assertGpuThread("Device::immediateSubmit");
    wait(submit(record, {}));
}

Device::SubmitTicket Device::submit(const std::function<void(VkCommandBuffer)>& record, const SubmitScope& scope) {
    assertGpuThread("Device::submit");

    // The type stops a stage being omitted; this stops one being passed as zero.
    for (const SubmitScope::Wait& entry : scope.wait) {
        if (entry.stage == 0) throw std::invalid_argument("Device::submit: a wait needs a non-zero stage mask");
    }
    for (const SubmitScope::TimelineWait& entry : scope.timelineWait) {
        if (entry.stage == 0) throw std::invalid_argument("Device::submit: a wait needs a non-zero stage mask");
    }

    retireCompleted();
    // Refused before the queue is touched. A batch a wait has already given up on holds up
    // everything behind it, so accepting more work would only queue renders behind a GPU
    // that is not running them, and the wedge would surface a submission cap later rather than
    // now. Retiring first is what lets a GPU that catches up be accepted again.
    if (const VkResult why = stalledResult(); why != VK_SUCCESS) throwStalled("Device::submit", why);

    // The only back-pressure a producer that never waits would otherwise have. Without it a
    // caller outrunning the GPU grows m_pending -- and the driver's own queue behind it --
    // without bound; with it, the producer paces itself against the oldest batch.
    while (m_pending.size() >= kMaxSubmissionsInFlight) {
        wait(SubmitTicket{m_pending.front().serial});
    }

    VkCommandBuffer cmd = takeCommandBuffer();

    // Gives the command buffer and fence back on every exit path that does not end in an
    // accepted submission -- including record() throwing. Disarmed once the batch is the
    // GPU's, after which they belong to m_pending until its fence signals.
    struct Discard {
        Device* device;
        VkCommandBuffer cmd;
        VkFence fence = VK_NULL_HANDLE;
        ~Discard() {
            if (device) device->discardSubmitObjects(cmd, fence);
        }
    } discard{this, cmd};

    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (const VkResult r = vkBeginCommandBuffer(cmd, &beginInfo); r != VK_SUCCESS) {
        throwVkFailure("Device::submit: vkBeginCommandBuffer", r);
    }

    record(cmd);

    if (const VkResult r = vkEndCommandBuffer(cmd); r != VK_SUCCESS) {
        throwVkFailure("Device::submit: vkEndCommandBuffer", r);
    }

    const VkFence fence = takeFence();
    discard.fence = fence;

    // Binary entries first, then timeline ones, in every array: Vulkan wants one flat list
    // per direction, with a value for each entry once any is a timeline. A binary entry's
    // value is ignored, so zero stands in for it.
    std::vector<VkSemaphore> waits;
    std::vector<VkPipelineStageFlags> waitStages;
    std::vector<uint64_t> waitValues;
    for (const SubmitScope::Wait& entry : scope.wait) {
        waits.push_back(entry.semaphore);
        waitStages.push_back(entry.stage);
        waitValues.push_back(0);
    }
    for (const SubmitScope::TimelineWait& entry : scope.timelineWait) {
        waits.push_back(entry.semaphore);
        waitStages.push_back(entry.stage);
        waitValues.push_back(entry.value);
    }
    std::vector<VkSemaphore> signals = scope.signal;
    std::vector<uint64_t> signalValues(scope.signal.size(), 0);
    for (const SubmitScope::TimelineSignal& entry : scope.timelineSignal) {
        signals.push_back(entry.semaphore);
        signalValues.push_back(entry.value);
    }

    VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    timeline.waitSemaphoreValueCount = static_cast<uint32_t>(waitValues.size());
    timeline.pWaitSemaphoreValues = waitValues.empty() ? nullptr : waitValues.data();
    timeline.signalSemaphoreValueCount = static_cast<uint32_t>(signalValues.size());
    timeline.pSignalSemaphoreValues = signalValues.empty() ? nullptr : signalValues.data();

    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    if (!scope.timelineWait.empty() || !scope.timelineSignal.empty()) submitInfo.pNext = &timeline;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    submitInfo.waitSemaphoreCount = static_cast<uint32_t>(waits.size());
    submitInfo.pWaitSemaphores = waits.empty() ? nullptr : waits.data();
    submitInfo.pWaitDstStageMask = waitStages.empty() ? nullptr : waitStages.data();
    submitInfo.signalSemaphoreCount = static_cast<uint32_t>(signals.size());
    submitInfo.pSignalSemaphores = signals.empty() ? nullptr : signals.data();

    if (const VkResult r = observe(vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, fence)); r != VK_SUCCESS) {
        throwVkFailure("Device::submit: vkQueueSubmit", r);
    }

    // Cannot reallocate, and must not: the batch is already the GPU's, so a throw here would
    // lose the only record of a command buffer and fence that are in use. The reserve in
    // createCommandPool() plus the cap above are what make this a plain move of two handles.
    discard.device = nullptr;
    m_pending.push_back(Pending{cmd, fence, m_nextSerial});
    return SubmitTicket{m_nextSerial++};
}

void Device::wait(SubmitTicket ticket) {
    assertGpuThread("Device::wait");
    // First, so the stalled test below is asked about work that is still outstanding now: a
    // batch that has finished falls out of the list here and this returns without consulting
    // it at all.
    retireCompleted();

    for (const Pending& pending : m_pending) {
        // A serial this Device no longer tracks is one whose fence has already been seen
        // signalled -- serials are issued in order and never reused -- so there is nothing
        // left to wait for.
        if (pending.serial != ticket.m_serial) continue;
        // Any stalled batch, not just this one: batches begin in submission order on this one
        // queue, so a batch still pending behind a wedged one cannot start, and giving it its
        // own full bound would park the caller for a second ten seconds to learn what is
        // already known.
        if (const VkResult why = stalledResult(); why != VK_SUCCESS) throwStalled("Device::wait", why);
        waitFences(&pending.fence, 1, "Device::wait");
        break;
    }
    retireCompleted();
}

void Device::waitForSubmissions() {
    assertGpuThread("Device::waitForSubmissions");
    // Before the stalled test, which would otherwise be answered from a list nothing has
    // refreshed -- rejecting a GPU that has since caught up for the life of the process.
    retireCompleted();
    if (m_pending.empty()) return;
    if (const VkResult why = stalledResult(); why != VK_SUCCESS) throwStalled("Device::waitForSubmissions", why);

    std::vector<VkFence> fences;
    fences.reserve(m_pending.size());
    for (const Pending& pending : m_pending) fences.push_back(pending.fence);

    waitFences(fences.data(), static_cast<uint32_t>(fences.size()), "Device::waitForSubmissions");
    retireCompleted();
}

bool Device::drainForTeardown() noexcept {
    assertGpuThread("Device::drainForTeardown");
    if (!m_device) return true;
    retireCompleted();
    if (m_pending.empty()) return true;

    // Skipped once a wait has already given up on a batch: nothing else can get past it on
    // this one queue, so a second bound would only add to how long teardown takes to give the
    // same answer. A batch that does finish is retired above, and this stops applying.
    if (stalledResult() == VK_SUCCESS) {
        // A destructor has nowhere to report a failed allocation to, so the fences go in a
        // fixed array; submit() caps m_pending at its size. Were that ever untrue the wait
        // would cover a prefix, and the retire below -- which polls every batch -- would still
        // report the rest as outstanding.
        std::array<VkFence, kMaxSubmissionsInFlight> fences{};
        const uint32_t count = static_cast<uint32_t>(std::min(m_pending.size(), fences.size()));
        for (uint32_t i = 0; i < count; ++i) fences[i] = m_pending[i].fence;

        const VkResult r = observe(vkWaitForFences(m_device, count, fences.data(), VK_TRUE, kSubmitTimeoutNs));
        // A lost device abandons its work rather than finishing it, so no wait will ever say
        // otherwise and its objects are the application's to destroy -- which is the only way
        // a lost device can be torn down at all.
        if (r == VK_ERROR_DEVICE_LOST) return true;

        retireCompleted();
        if (m_pending.empty()) return true;
        // Whatever ended the wait is recorded, not just a timeout: host memory exhaustion is
        // the realistic other one, and it abandons resources exactly as a timeout does. Left
        // unrecorded, submit() would go on accepting work against a device just declared
        // unrecoverable and the failure would surface as descriptor exhaustion. Only the
        // batches the retire above kept, which are the ones still unsignalled.
        if (r != VK_SUCCESS) {
            for (Pending& batch : m_pending) batch.waitOutcome = r;
        }
    }

    abandon();
    return false;
}

bool Device::safeToFree(const char* whatIsLeaked, std::optional<SubmitTicket> reader) noexcept {
    if (reader ? hasCompleted(*reader) : drainForTeardown()) return true;

    // Covers the poll above, which does not go through drainForTeardown(); on the other path
    // this is already set. Deliberately no waitOutcome stamp for the poll: nothing waited, so
    // it has learnt nothing about the queue that should refuse the next submit.
    abandon();
    std::fprintf(stderr, "Device: the GPU has not finished the work that reads %s, so it is leaked rather than freed\n",
                 whatIsLeaked);
    return false;
}

bool Device::lost() const {
    assertGpuThread("Device::lost");
    return m_lost;
}

VkResult Device::observe(VkResult result) noexcept {
    if (result != VK_ERROR_DEVICE_LOST || m_lost) return result;
    m_lost = true;
    if (m_onLost) {
        try {
            m_onLost();
        } catch (...) {
        }
    }
    return result;
}

Device::SubmitTicket Device::lastSubmission() const {
    assertGpuThread("Device::lastSubmission");
    // m_nextSerial starts at 1, so this is the null ticket until something is submitted.
    return SubmitTicket{m_nextSerial - 1};
}

bool Device::hasCompleted(SubmitTicket ticket) {
    assertGpuThread("Device::hasCompleted");
    // Checked before retiring, so a ticket old enough to have been forgotten costs no driver
    // call at all -- which is the common case for a caller polling one repeatedly.
    if (!isPending(ticket.m_serial)) return true;
    retireCompleted();
    return !isPending(ticket.m_serial);
}

VkResult Device::stalledResult() const {
    for (const Pending& batch : m_pending) {
        if (batch.waitOutcome != VK_SUCCESS) return batch.waitOutcome;
    }
    return VK_SUCCESS;
}

void Device::throwStalled(const char* what, VkResult why) const {
    if (why == VK_TIMEOUT) {
        throw GpuFailure(std::string(what) + ": the GPU still has not finished a batch it was given " +
                         std::to_string(kSubmitTimeout.count()) + " seconds to complete");
    }
    throw GpuFailure(std::string(what) + ": the GPU has not finished a submitted batch, and the wait for it "
                                         "failed with " + vkResultName(why));
}

bool Device::isPending(uint64_t serial) const {
    for (const Pending& pending : m_pending) {
        if (pending.serial == serial) return true;
    }
    return false;
}

void Device::waitFences(const VkFence* fences, uint32_t count, const char* what) {
    const VkResult r = observe(vkWaitForFences(m_device, count, fences, VK_TRUE, kSubmitTimeoutNs));
    if (r == VK_SUCCESS) return;

    if (r == VK_TIMEOUT) {
        // The batch stays pending and keeps its command buffer and fence: it is still the
        // GPU's, and recycling either now is how a wedged GPU turns into corruption. Marked
        // so that nothing spends the bound on it a second time -- the next submit is refused
        // outright, and teardown leaks rather than freeing what the GPU is still reading.
        //
        // Only the ones the driver still reports unsignalled: a wait for all of them expires on
        // the slowest, and stamping the rest would make a later wait on a batch that finished
        // long ago throw for someone else's stall.
        for (Pending& batch : m_pending) {
            if (std::find(fences, fences + count, batch.fence) == fences + count) continue;
            if (observe(vkGetFenceStatus(m_device, batch.fence)) != VK_SUCCESS) batch.waitOutcome = VK_TIMEOUT;
        }
        throw GpuFailure(std::string(what) + ": the GPU did not finish a submitted batch within " +
                         std::to_string(kSubmitTimeout.count()) + " seconds");
    }

    // No device-idle wait before rethrowing. The batch stays pending, so nothing here recycles
    // a command buffer or fence the GPU may still hold, and the two results that reach this
    // line want opposite things: a lost device has abandoned the work, while an out-of-memory
    // failure leaves it running for as long as it takes -- which is the unbounded wait this
    // whole path exists to avoid. A caller about to free what the batch reads goes through
    // drainForTeardown(), which is bounded and says whether freeing is safe.
    throwVkFailure(std::string(what) + ": vkWaitForFences", r);
}

void Device::retireCompleted() noexcept {
    if (m_pending.empty()) return;

    size_t kept = 0;
    for (size_t i = 0; i < m_pending.size(); ++i) {
        const Pending batch = m_pending[i];
        // VK_NOT_READY is still running. VK_ERROR_DEVICE_LOST is neither: the batch stays
        // pending forever, because after a loss nothing says whether the driver is done with
        // these two objects, and only teardown -- which waits for idle first -- frees them.
        if (observe(vkGetFenceStatus(m_device, batch.fence)) != VK_SUCCESS) {
            m_pending[kept++] = batch;
            continue;
        }
        vkResetFences(m_device, 1, &batch.fence);
        m_freeFences.push_back(batch.fence);
        m_freeCommandBuffers.push_back(batch.cmd);
    }
    m_pending.resize(kept);
}

VkCommandBuffer Device::takeCommandBuffer() {
    if (!m_freeCommandBuffers.empty()) {
        // In the executable state -- recorded and submitted once already -- which is why the
        // pool carries RESET_COMMAND_BUFFER_BIT: vkBeginCommandBuffer then resets it
        // implicitly instead of rejecting it.
        const VkCommandBuffer cmd = m_freeCommandBuffers.back();
        m_freeCommandBuffers.pop_back();
        return cmd;
    }

    VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.commandPool = m_commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (const VkResult r = vkAllocateCommandBuffers(m_device, &allocInfo, &cmd); r != VK_SUCCESS) {
        throwVkFailure("Device::submit: vkAllocateCommandBuffers", r);
    }
    return cmd;
}

VkFence Device::takeFence() {
    if (!m_freeFences.empty()) {
        const VkFence fence = m_freeFences.back();
        m_freeFences.pop_back();
        return fence;
    }

    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    if (const VkResult r = vkCreateFence(m_device, &fenceInfo, nullptr, &fence); r != VK_SUCCESS) {
        throwVkFailure("Device::submit: vkCreateFence", r);
    }
    return fence;
}

void Device::discardSubmitObjects(VkCommandBuffer cmd, VkFence fence) noexcept {
    if (fence) vkDestroyFence(m_device, fence, nullptr);
    if (cmd) vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
}

}
