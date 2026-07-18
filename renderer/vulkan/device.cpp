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
    createInstance(enableValidation);
    selectPhysicalDevice();
    determineTier();
    createLogicalDevice();
    m_memory.init(m_instance, m_physicalDevice, m_device, VK_API_VERSION_1_3);
    createDescriptorAllocator();
}

Device::~Device() {
    m_descriptorAllocator.reset();
    m_memory.shutdown();

    if (m_device) vkDestroyDevice(m_device, nullptr);

    if (m_debugMessenger) {
        auto destroyFn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroyFn) destroyFn(m_instance, m_debugMessenger, nullptr);
    }

    if (m_instance) vkDestroyInstance(m_instance, nullptr);
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
        }
    }

    VkInstanceCreateInfo createInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    createInfo.pApplicationInfo = &appInfo;
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

    for (VkPhysicalDevice candidate : devices) {
        uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> extensions(extCount);
        vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extCount, extensions.data());

        if (!hasExtension(extensions, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)) {
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
        }
    }

    if (best == VK_NULL_HANDLE) {
        throw std::runtime_error("Device: no GPU with VK_KHR_external_memory_fd found");
    }

    m_physicalDevice = best;
    m_info.gpuName = bestProps.deviceName;
    m_info.vendor = vendorFromId(bestProps.vendorID);

    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> extensions(extCount);
    vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &extCount, extensions.data());

    m_info.hasDescriptorHeap = hasExtension(extensions, VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME);
    m_info.hasDescriptorBuffer = hasExtension(extensions, VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME);
    m_info.hasExternalMemoryDmaBuf = hasExtension(extensions, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &queueFamilyCount, queueFamilies.data());

    for (uint32_t i = 0; i < queueFamilyCount; ++i) {
        if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            m_graphicsQueueFamily = i;
            break;
        }
    }
    if (m_graphicsQueueFamily == UINT32_MAX) {
        throw std::runtime_error("Device: no graphics-capable queue family found");
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
    if (useHeap) {
        deviceExtensions.push_back(VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME);
    } else if (useBuffer) {
        deviceExtensions.push_back(VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME);
    }

    VkPhysicalDeviceVulkan12Features vulkan12Features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    vulkan12Features.bufferDeviceAddress = (useHeap || useBuffer) ? VK_TRUE : VK_FALSE;

    VkPhysicalDeviceDescriptorBufferFeaturesEXT descriptorBufferFeatures{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT};
    descriptorBufferFeatures.descriptorBuffer = VK_TRUE;
    descriptorBufferFeatures.pNext = &vulkan12Features;

    VkPhysicalDeviceDescriptorHeapFeaturesEXT descriptorHeapFeatures{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT};
    descriptorHeapFeatures.descriptorHeap = VK_TRUE;
    descriptorHeapFeatures.pNext = &vulkan12Features;

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

}
