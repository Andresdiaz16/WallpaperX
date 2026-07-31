#include "device.h"

#include "descriptor_buffer.h"
#include "descriptor_classic.h"
#include "descriptor_heap.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace wallpaperx {

namespace {
std::atomic<int> g_validationWarningCount{0};
}

void resetValidationWarningCount() { g_validationWarningCount = 0; }
int validationWarningCount() { return g_validationWarningCount; }

const char* toString(DescriptorTier tier) {
    switch (tier) {
        case DescriptorTier::Heap: return "Heap";
        case DescriptorTier::Buffer: return "Buffer";
        case DescriptorTier::Classic: return "Classic";
    }
    return "Unknown";
}

namespace {

bool hasExtension(const std::vector<VkExtensionProperties>& extensions, const char* name) {
    for (const auto& ext : extensions) {
        if (std::strcmp(ext.extensionName, name) == 0) return true;
    }
    return false;
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        std::cerr << "[vulkan] " << data->pMessage << std::endl;
        ++g_validationWarningCount;
    }
    return VK_FALSE;
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

    VkInstanceCreateInfo createInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    createInfo.pApplicationInfo = &appInfo;
    if (syncValidationAvailable) {
        createInfo.pNext = &validationFeatures;
    }
    createInfo.enabledLayerCount = static_cast<uint32_t>(instanceLayers.size());
    createInfo.ppEnabledLayerNames = instanceLayers.data();
    createInfo.enabledExtensionCount = static_cast<uint32_t>(instanceExtensions.size());
    createInfo.ppEnabledExtensionNames = instanceExtensions.data();

    if (vkCreateInstance(&createInfo, nullptr, &m_instance) != VK_SUCCESS) {
        throw std::runtime_error("Device: vkCreateInstance failed");
    }

    if (validationAvailable) {
        auto createFn = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
        if (createFn) {
            VkDebugUtilsMessengerCreateInfoEXT messengerInfo{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            messengerInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                             VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            messengerInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                         VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                         VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            messengerInfo.pfnUserCallback = debugCallback;
            createFn(m_instance, &messengerInfo, nullptr, &m_debugMessenger);
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

        const bool isBetter = best == VK_NULL_HANDLE ||
            (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU &&
             bestProps.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);

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

    if (vkCreateDevice(m_physicalDevice, &deviceInfo, nullptr, &m_device) != VK_SUCCESS) {
        throw std::runtime_error("Device: vkCreateDevice failed");
    }

    vkGetDeviceQueue(m_device, m_graphicsQueueFamily, 0, &m_graphicsQueue);
}

void Device::createDescriptorAllocator() {
    switch (m_selectedTier) {
        case DescriptorTier::Heap:
            m_descriptorAllocator = std::make_unique<DescriptorHeapAllocator>(m_physicalDevice, m_device, m_memory);
            break;
        case DescriptorTier::Buffer:
            m_descriptorAllocator = std::make_unique<DescriptorBufferAllocator>(m_physicalDevice, m_device, m_memory);
            break;
        case DescriptorTier::Classic:
            m_descriptorAllocator = std::make_unique<ClassicDescriptorAllocator>(m_device);
            break;
    }
}

void Device::createCommandPool() {
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = m_graphicsQueueFamily;

    if (vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool) != VK_SUCCESS) {
        throw std::runtime_error("Device: failed to create command pool");
    }
}

void Device::immediateSubmit(const std::function<void(VkCommandBuffer)>& record) {
    VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.commandPool = m_commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(m_device, &allocInfo, &cmd) != VK_SUCCESS) {
        throw std::runtime_error("Device::immediateSubmit: failed to allocate command buffer");
    }

    VkFence fence = VK_NULL_HANDLE;

    // Free the command buffer and fence on every exit path -- including the
    // caller's record() lambda throwing, or any of the checks below. Captures the
    // handles by value and cmd/fence by reference so it cleans up whatever exists
    // at scope exit. (A local class can't touch Device's private members, so it
    // takes the device/pool handles by value.)
    struct Cleanup {
        VkDevice device;
        VkCommandPool pool;
        VkCommandBuffer& cmd;
        VkFence& fence;
        ~Cleanup() {
            if (fence) vkDestroyFence(device, fence, nullptr);
            if (cmd) vkFreeCommandBuffers(device, pool, 1, &cmd);
        }
    } cleanup{m_device, m_commandPool, cmd, fence};

    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
        throw std::runtime_error("Device::immediateSubmit: vkBeginCommandBuffer failed");
    }

    record(cmd);

    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        throw std::runtime_error("Device::immediateSubmit: vkEndCommandBuffer failed");
    }

    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(m_device, &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
        throw std::runtime_error("Device::immediateSubmit: failed to create fence");
    }

    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;

    if (vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, fence) != VK_SUCCESS) {
        throw std::runtime_error("Device::immediateSubmit: vkQueueSubmit failed");
    }

    if (vkWaitForFences(m_device, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        throw std::runtime_error("Device::immediateSubmit: vkWaitForFences failed");
    }
}

}
