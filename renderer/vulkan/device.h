// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "descriptor_allocator.h"
#include "gpu_fault.h"
#include "memory.h"
#include "tier_selection.h"

#include <array>
#include <cstdint>
#include <functional>
#include <vector>
#include <memory>
#include <stdexcept>
#include <string>
#include <atomic>
#include <thread>

namespace wallpaperx {

struct DeviceInfo {
    std::string gpuName;
    GpuVendor vendor = GpuVendor::Other;

    // Names the physical GPU across APIs and processes: an importer compares it against
    // GL_DEVICE_UUID_EXT, since an opaque fd is only importable on the device that made it.
    std::array<uint8_t, VK_UUID_SIZE> deviceUuid{};
    bool hasDescriptorHeap = false;
    bool hasDescriptorBuffer = false;
    bool hasExternalMemoryDmaBuf = false;

    // Whether a dma_buf frame can be described to an importer. Tracked separately
    // from hasExternalMemoryDmaBuf because the two degrade differently: without the
    // dma_buf extension a non-NVIDIA GPU cannot export at all, but without the
    // modifier extension it can still export and render -- the frame just carries no
    // layout metadata. Conflating them turns the second case into a hard failure.
    bool hasImageDrmFormatModifier = false;

    // Whether a binary semaphore can be exported as an OPAQUE_FD for the importing process to
    // wait on. Without it the only cross-process ordering available is a host fence, which is a
    // happens-before through the host while the acquire it must order is on the device -- so a
    // consumer reads a frame whose queue-family release it never acquired, which is undefined.
    //
    // The extension being present is NOT enough and must not be substituted: Lavapipe advertises
    // it and supports SYNC_FD only, which GL_EXT_semaphore_fd cannot import.
    bool hasExternalSemaphoreFd = false;

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

    // Whether a validator was actually listening. Needed alongside the count below,
    // because 0 warnings is equally true of a clean run and an unwatched one.
    //
    // A snapshot accessor and a reset used to sit here too; every reader now goes
    // through validationCounter(), so they were dead surface in a header the whole
    // renderer includes.
    bool validationLayerActive() const noexcept { return m_validationLayerActive; }

    // The counter itself, shared so it stays readable after this Device is gone.
    //
    // Not an over-generalisation: the layer's most valuable complaints are the ones it
    // makes during teardown -- a resource still in flight, an object destroyed twice --
    // and those are emitted from ~Device. A caller holding only an accessor could never
    // read them, because by the time destruction has finished there is nothing left to
    // ask. Taking the handle before destruction and reading it after is the only way to
    // judge the teardown path.
    std::shared_ptr<const std::atomic<int>> validationCounter() const { return m_validationWarnings; }

    // Called by the debug messenger callback, which is a free function because Vulkan
    // requires a C entry point -- it reaches the right Device through pUserData. Public
    // only for that reason; nothing else should call it.
    void noteValidationWarning() noexcept { m_validationWarnings->fetch_add(1, std::memory_order_relaxed); }

    Memory& memory() { return m_memory; }
    IDescriptorAllocator& descriptorAllocator() { return *m_descriptorAllocator; }

    VkCommandPool commandPool() const { return m_commandPool; }

    // What one submission waits on and what it signals. All empty is immediateSubmit's case:
    // no ordering beyond this queue, and the caller blocked on the host fence.
    //
    // A signal semaphore is the only way to order work in another queue or another process
    // against this submission. A host fence cannot stand in for it: the fence is a
    // happens-before through the host, while the consumer's acquire happens on the device.
    struct SubmitScope {
        // Paired by index: wait[i] is waited on at waitStages[i], so the two must be the
        // same length. Vulkan takes them as separate arrays and silently misreads a short one.
        std::vector<VkSemaphore> wait;
        std::vector<VkPipelineStageFlags> waitStages;
        std::vector<VkSemaphore> signal;
    };

    // Records and submits `record`, then waits for it. Throws GpuFailure on any Vulkan
    // failure, and std::invalid_argument if scope.wait and scope.waitStages differ in length.
    void submit(const std::function<void(VkCommandBuffer)>& record, const SubmitScope& scope);

    // submit() with no semaphores. Every caller that renders once per wallpaper wants this.
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
    // Written from the debug callback, which the loader may invoke on any thread, and
    // shared so validationCounter() can outlive this object.
    std::shared_ptr<std::atomic<int>> m_validationWarnings = std::make_shared<std::atomic<int>>(0);
    bool m_validationLayerActive = false;
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
