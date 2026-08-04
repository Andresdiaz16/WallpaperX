#pragma once

#include "descriptor_allocator.h"
#include "memory.h"
#include "tier_selection.h"

#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace wallpaperx {

// A validation-warning counter and its three read accessors used to live here and in
// device.cpp. Every caller was a test, so both the state and the accessors were test-only
// code compiled into the shipped renderer; moving just the declarations to a test-facing
// header did not change that. They are gone. Tests now count warnings by reading the
// "[vulkan] " lines Device's debug callback already writes to stderr -- see
// renderer/tests/support/validation_probe.h, which no production translation unit is on
// the include path for. Nothing here needs to be added back to serve a test.

struct DeviceInfo {
    std::string gpuName;
    GpuVendor vendor = GpuVendor::Other;
    bool hasDescriptorHeap = false;
    bool hasDescriptorBuffer = false;
    bool hasExternalMemoryDmaBuf = false;

    // Whether a dma_buf frame can be described to an importer. Tracked separately
    // from hasExternalMemoryDmaBuf because the two degrade differently: without the
    // dma_buf extension a non-NVIDIA GPU cannot export at all, but without the
    // modifier extension it can still export and render -- the frame just carries no
    // layout metadata. Conflating them turns the second case into a hard failure.
    bool hasImageDrmFormatModifier = false;

    // VK_EXT_queue_family_foreign: lets the exported image's queue-family ownership
    // be released to a non-Vulkan consumer (a compositor). Without it the release
    // falls back to VK_QUEUE_FAMILY_EXTERNAL, which is core since 1.1.
    bool hasQueueFamilyForeign = false;
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
    DescriptorTier descriptorTier() const { return m_selectedTier; }
    Memory& memory() { return m_memory; }
    IDescriptorAllocator& descriptorAllocator() { return *m_descriptorAllocator; }

    VkCommandPool commandPool() const { return m_commandPool; }

    void immediateSubmit(const std::function<void(VkCommandBuffer)>& record);

    // Aborts unless called from the thread that created this Device.
    //
    // Vulkan queue submission is not free-threaded and this Device shares one command
    // pool, so every caller into the GPU has to be on the same thread. In the daemon
    // that is the render worker, and the rule it enforces -- no DBus handler touches
    // the GPU -- is what keeps the single event loop responsive. A rule that lives only
    // in comments is one refactor from being broken silently, and the symptom would be
    // a data race: intermittent, unreproducible, and invisible to every existing test.
    // This turns it into an immediate, located failure under any test that trips it.
    //
    // Aborts rather than throws on purpose. A wrong-thread call is a programming error,
    // not a runtime condition; throwing would let a caller's catch turn it into a
    // routine "render failed" and hide exactly what needs to be loud.
    void assertGpuThread(const char* what) const;

    // Re-binds the above to the calling thread. Vulkan permits use from any thread as
    // long as access is externally synchronised, so the affinity check is stricter than
    // the spec -- deliberately, because "one designated thread" is a rule a reviewer can
    // check and "externally synchronised" is not. This is the escape hatch for the one
    // legitimate handoff: an owner that has *joined* the render thread and is now tearing
    // the device down from its own. The caller must guarantee no other thread can touch
    // the Device; calling it while the worker still runs re-points the check at the wrong
    // thread and disables the very protection it exists for.
    void adoptGpuThread() { m_gpuThread = std::this_thread::get_id(); }

private:
    void createInstance(bool enableValidation);
    void selectPhysicalDevice();
    void determineTier();
    void createLogicalDevice();
    void createDescriptorAllocator();
    void createCommandPool();
    void destroyResources();

    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;
    uint32_t m_graphicsQueueFamily = UINT32_MAX;

    // The thread that constructed this Device, and therefore the only one allowed to
    // touch it. Captured rather than configured: whoever created the device is by
    // definition the GPU thread.
    std::thread::id m_gpuThread = std::this_thread::get_id();

    DeviceInfo m_info;
    DescriptorTier m_selectedTier = DescriptorTier::Classic;
    Memory m_memory;
    std::unique_ptr<IDescriptorAllocator> m_descriptorAllocator;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
};

}
