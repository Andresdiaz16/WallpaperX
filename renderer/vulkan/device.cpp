// SPDX-License-Identifier: GPL-2.0-or-later

#include "device.h"

#include "descriptor_buffer.h"
#include "descriptor_classic.h"
#include "descriptor_heap.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
#include <stdexcept>
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
constexpr uint64_t kSubmitTimeoutSeconds = 10;
constexpr uint64_t kSubmitTimeoutNs = kSubmitTimeoutSeconds * 1'000'000'000ull;

// Ceiling on batches submitted but not yet known complete. Not a tuning knob: it is what
// bounds the pending list, and reaching it makes the next submit wait for the oldest batch.
constexpr size_t kMaxSubmissionsInFlight = 16;

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
    std::cerr << "[vulkan] " << data->pMessage << std::endl;

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

Device::Device(bool enableValidation) {
    try {
        createInstance(enableValidation);
        selectPhysicalDevice();
        determineTier();
        createLogicalDevice();
        const bool bufferDeviceAddressEnabled = m_selectedTier == DescriptorTier::Heap || m_selectedTier == DescriptorTier::Buffer;
        m_memory.init(m_instance, m_physicalDevice, m_device, VK_API_VERSION_1_3, bufferDeviceAddressEnabled,
                      m_info.hasImageDrmFormatModifier);
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

    // Everything is leaked, deliberately, once anything has been abandoned -- including when
    // the GPU has since caught up, because what was abandoned is unreachable now, and
    // vkDestroyDevice requires every child object to be gone while VMA aborts the process
    // outright if an allocation is still live.
    if (m_abandoned) {
        std::fprintf(stderr,
                     "Device: %s, so the Vulkan device and everything it owns is leaked rather than destroyed\n",
                     idle ? "resources were abandoned to a GPU that had not finished reading them"
                          : "the GPU did not finish the work submitted to it");
        return;
    }

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

    if (m_debugMessenger) {
        auto destroyFn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroyFn) destroyFn(m_instance, m_debugMessenger, nullptr);
        m_debugMessenger = VK_NULL_HANDLE;
    }

    if (m_instance) {
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

    std::vector<const char*> instanceExtensions;
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
                std::cerr << "[vulkan] validation layer present but VK_EXT_validation_features is not -- "
                             "running without synchronization validation"
                          << std::endl;
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

        const bool isBetter = best == VK_NULL_HANDLE || deviceRank(props.deviceType) > deviceRank(bestProps.deviceType);

        if (isBetter) {
            best = candidate;
            bestProps = props;
            bestGraphicsQueueFamily = graphicsQueueFamily;
        }
    }

    if (best == VK_NULL_HANDLE) {
        throw std::runtime_error("Device: no GPU with VK_KHR_external_memory_fd and a graphics queue found");
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

    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> extensions(extCount);
    vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &extCount, extensions.data());

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

    std::vector<const char*> deviceExtensions{VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME};
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

    VkPhysicalDeviceVulkan12Features vulkan12Features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    vulkan12Features.bufferDeviceAddress = (useHeap || useBuffer) ? VK_TRUE : VK_FALSE;
    // The heap-tier fragment shader (fullscreen_heap.frag) indexes the descriptor
    // heap arrays with nonuniformEXT, which requires this feature to be enabled.
    vulkan12Features.shaderSampledImageArrayNonUniformIndexing = useHeap ? VK_TRUE : VK_FALSE;

    VkPhysicalDeviceShaderUntypedPointersFeaturesKHR untypedPointersFeatures{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_UNTYPED_POINTERS_FEATURES_KHR};
    untypedPointersFeatures.shaderUntypedPointers = VK_TRUE;
    untypedPointersFeatures.pNext = &vulkan12Features;

    VkPhysicalDeviceDescriptorBufferFeaturesEXT descriptorBufferFeatures{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT};
    descriptorBufferFeatures.descriptorBuffer = VK_TRUE;
    descriptorBufferFeatures.pNext = &vulkan12Features;

    VkPhysicalDeviceMaintenance5FeaturesKHR maintenance5Features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
    maintenance5Features.maintenance5 = VK_TRUE;
    maintenance5Features.pNext = &untypedPointersFeatures;

    VkPhysicalDeviceDescriptorHeapFeaturesEXT descriptorHeapFeatures{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT};
    descriptorHeapFeatures.descriptorHeap = VK_TRUE;
    descriptorHeapFeatures.pNext = &maintenance5Features;

    void* featureChain = &vulkan12Features;
    if (useHeap) {
        featureChain = &descriptorHeapFeatures;
    } else if (useBuffer) {
        featureChain = &descriptorBufferFeatures;
    }

    const float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = m_graphicsQueueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &queuePriority;

    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.pNext = featureChain;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
    deviceInfo.ppEnabledExtensionNames = deviceExtensions.data();

    if (const VkResult r = vkCreateDevice(m_physicalDevice, &deviceInfo, nullptr, &m_device); r != VK_SUCCESS) {
        throwVkFailure("Device: vkCreateDevice", r);
    }

    vkGetDeviceQueue(m_device, m_graphicsQueueFamily, 0, &m_graphicsQueue);
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
}

void Device::assertGpuThread(const char* what) const {
    if (std::this_thread::get_id() == m_gpuThread) return;

    std::cerr << "[vulkan] FATAL: " << (what ? what : "GPU call")
              << " was made from a thread other than the one that created the Device. Vulkan submission is "
                 "not free-threaded and this Device shares one command pool, so this is a data race. In the "
                 "daemon this means a DBus handler reached the GPU directly instead of going through the "
                 "render worker."
              << std::endl;
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

    // Checked, not assumed: Vulkan reads pWaitDstStageMask as waitSemaphoreCount entries, so a
    // short stage list is an out-of-bounds read the validation layers do not always catch.
    if (scope.wait.size() != scope.waitStages.size()) {
        throw std::invalid_argument("Device::submit: one wait stage is required per wait semaphore");
    }

    retireCompleted();
    // Refused before the queue is touched. A batch that has already had the full bound holds
    // up everything behind it, so accepting more work would only queue renders behind a GPU
    // that is not running them, and the wedge would surface a submission cap later rather than
    // now. Retiring first is what lets a GPU that catches up be accepted again.
    if (stalled()) throwStalled("Device::submit");

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

    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    submitInfo.waitSemaphoreCount = static_cast<uint32_t>(scope.wait.size());
    submitInfo.pWaitSemaphores = scope.wait.empty() ? nullptr : scope.wait.data();
    submitInfo.pWaitDstStageMask = scope.waitStages.empty() ? nullptr : scope.waitStages.data();
    submitInfo.signalSemaphoreCount = static_cast<uint32_t>(scope.signal.size());
    submitInfo.pSignalSemaphores = scope.signal.empty() ? nullptr : scope.signal.data();

    if (const VkResult r = vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, fence); r != VK_SUCCESS) {
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

    for (const Pending& pending : m_pending) {
        // A serial this Device no longer tracks is one whose fence has already been seen
        // signalled -- serials are issued in order and never reused -- so there is nothing
        // left to wait for.
        if (pending.serial != ticket.m_serial) continue;
        // Checked per batch rather than for any stalled batch at all, so waiting on work that
        // has already finished still returns rather than failing on someone else's stall.
        if (pending.waitExpired) throwStalled("Device::wait");
        waitFences(&pending.fence, 1, "Device::wait");
        break;
    }
    retireCompleted();
}

void Device::waitForSubmissions() {
    assertGpuThread("Device::waitForSubmissions");
    if (m_pending.empty()) return;
    if (stalled()) throwStalled("Device::waitForSubmissions");

    std::vector<VkFence> fences;
    fences.reserve(m_pending.size());
    for (const Pending& pending : m_pending) fences.push_back(pending.fence);

    waitFences(fences.data(), static_cast<uint32_t>(fences.size()), "Device::waitForSubmissions");
    retireCompleted();
}

bool Device::drainForTeardown() noexcept {
    if (!m_device) return true;
    if (retireCompletedNoThrow()) return true;

    // Skipped once a batch has already had the full bound: nothing else can get past it on
    // this one queue, so a second bound would only add to how long teardown takes to give the
    // same answer. A batch that does finish is retired above, and this stops applying.
    if (!stalled()) {
        // A destructor has nowhere to report a failed allocation to, so the fences go in a
        // fixed array; submit() caps m_pending at its size. Were that ever untrue the wait
        // would cover a prefix, and the retire below -- which polls every batch -- would still
        // report the rest as outstanding.
        std::array<VkFence, kMaxSubmissionsInFlight> fences{};
        const uint32_t count = static_cast<uint32_t>(std::min(m_pending.size(), fences.size()));
        for (uint32_t i = 0; i < count; ++i) fences[i] = m_pending[i].fence;

        const VkResult r = vkWaitForFences(m_device, count, fences.data(), VK_TRUE, kSubmitTimeoutNs);
        // A lost device abandons its work rather than finishing it, so no wait will ever say
        // otherwise and its objects are the application's to destroy -- which is the only way
        // a lost device can be torn down at all.
        if (r == VK_ERROR_DEVICE_LOST) return true;

        const bool idle = retireCompletedNoThrow();
        if (r == VK_TIMEOUT) {
            for (Pending& batch : m_pending) batch.waitExpired = true;
        }
        if (idle) return true;
    }

    m_abandoned = true;
    return false;
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

bool Device::stalled() const {
    for (const Pending& batch : m_pending) {
        if (batch.waitExpired) return true;
    }
    return false;
}

void Device::throwStalled(const char* what) const {
    throw GpuFailure(std::string(what) + ": the GPU still has not finished a batch it was given " +
                     std::to_string(kSubmitTimeoutSeconds) + " seconds to complete");
}

bool Device::isPending(uint64_t serial) const {
    for (const Pending& pending : m_pending) {
        if (pending.serial == serial) return true;
    }
    return false;
}

void Device::waitFences(const VkFence* fences, uint32_t count, const char* what) {
    const VkResult r = vkWaitForFences(m_device, count, fences, VK_TRUE, kSubmitTimeoutNs);
    if (r == VK_SUCCESS) return;

    if (r == VK_TIMEOUT) {
        // The batch stays pending and keeps its command buffer and fence: it is still the
        // GPU's, and recycling either now is how a wedged GPU turns into corruption. Marked
        // so that nothing spends the bound on it a second time -- the next submit is refused
        // outright, and teardown leaks rather than freeing what the GPU is still reading.
        for (Pending& batch : m_pending) {
            if (std::find(fences, fences + count, batch.fence) != fences + count) batch.waitExpired = true;
        }
        throw GpuFailure(std::string(what) + ": the GPU did not finish a submitted batch within " +
                         std::to_string(kSubmitTimeoutSeconds) + " seconds");
    }

    // No device-idle wait before rethrowing. The batch stays pending, so nothing here recycles
    // a command buffer or fence the GPU may still hold, and the two results that reach this
    // line want opposite things: a lost device has abandoned the work, while an out-of-memory
    // failure leaves it running for as long as it takes -- which is the unbounded wait this
    // whole path exists to avoid. A caller about to free what the batch reads goes through
    // drainForTeardown(), which is bounded and says whether freeing is safe.
    throwVkFailure(std::string(what) + ": vkWaitForFences", r);
}

void Device::retireCompleted() {
    if (m_pending.empty()) return;

    // Reserved before anything moves, so the loop below cannot throw part way and leave a
    // handle recorded in two places at once.
    m_freeCommandBuffers.reserve(m_freeCommandBuffers.size() + m_pending.size());
    m_freeFences.reserve(m_freeFences.size() + m_pending.size());

    size_t kept = 0;
    for (size_t i = 0; i < m_pending.size(); ++i) {
        const Pending batch = m_pending[i];
        // VK_NOT_READY is still running. VK_ERROR_DEVICE_LOST is neither: the batch stays
        // pending forever, because after a loss nothing says whether the driver is done with
        // these two objects, and only teardown -- which waits for idle first -- frees them.
        if (vkGetFenceStatus(m_device, batch.fence) != VK_SUCCESS) {
            m_pending[kept++] = batch;
            continue;
        }
        vkResetFences(m_device, 1, &batch.fence);
        m_freeFences.push_back(batch.fence);
        m_freeCommandBuffers.push_back(batch.cmd);
    }
    m_pending.resize(kept);
}

bool Device::retireCompletedNoThrow() noexcept {
    try {
        retireCompleted();
    } catch (...) {
        // Only the free lists' reserve can throw. Nothing was retired, so the batches are
        // reported as still outstanding, which is what a caller that cannot throw must assume.
        return false;
    }
    return m_pending.empty();
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
