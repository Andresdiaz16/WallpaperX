#pragma once

#include "descriptor_allocator.h"
#include "memory.h"
#include "tier_selection.h"

#include <functional>
#include <memory>
#include <string>

namespace wallpaperx {

void resetValidationWarningCount();
int validationWarningCount();

struct DeviceInfo {
    std::string gpuName;
    GpuVendor vendor = GpuVendor::Other;
    bool hasDescriptorHeap = false;
    bool hasDescriptorBuffer = false;
    bool hasExternalMemoryDmaBuf = false;
};

class Device {
public:
    explicit Device(bool enableValidation);
    ~Device();

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    VkInstance instance() const { return m_instance; }
    VkPhysicalDevice physicalDevice() const { return m_physicalDevice; }
    VkDevice device() const { return m_device; }
    VkQueue graphicsQueue() const { return m_graphicsQueue; }
    uint32_t graphicsQueueFamily() const { return m_graphicsQueueFamily; }

    const DeviceInfo& info() const { return m_info; }
    Memory& memory() { return m_memory; }
    IDescriptorAllocator& descriptorAllocator() { return *m_descriptorAllocator; }

    VkCommandPool commandPool() const { return m_commandPool; }

    void immediateSubmit(const std::function<void(VkCommandBuffer)>& record);

private:
    void createInstance(bool enableValidation);
    void selectPhysicalDevice();
    void determineTier();
    void createLogicalDevice();
    void createDescriptorAllocator();
    void createCommandPool();

    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;
    uint32_t m_graphicsQueueFamily = UINT32_MAX;

    DeviceInfo m_info;
    DescriptorTier m_selectedTier = DescriptorTier::Classic;
    Memory m_memory;
    std::unique_ptr<IDescriptorAllocator> m_descriptorAllocator;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
};

}
