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

    // What one submission waits on and what it signals. All empty is the ordinary case:
    // no ordering beyond this queue, where submission order and the barriers in the batch
    // are enough.
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

    // Names one submitted batch, for a caller that has to know when it finished.
    //
    // Deliberately carries no handle. The command buffer and the fence belong to the Device
    // for as long as the GPU may still be reading them, so there is nothing here a caller
    // could free, reset or submit again. Serials are never reused, which is what makes a
    // ticket the Device no longer tracks mean "that work is done" rather than "unknown".
    class SubmitTicket {
    public:
        SubmitTicket() = default;

    private:
        friend class Device;
        explicit SubmitTicket(uint64_t serial) : m_serial(serial) {}
        uint64_t m_serial = 0;  // 0: names nothing, and wait() on it returns at once
    };

    // Records `record` into a command buffer, submits it, and returns without waiting for the
    // GPU. Throws GpuFailure on any Vulkan failure, and std::invalid_argument if scope.wait
    // and scope.waitStages differ in length. Also refuses, without touching the queue, while a
    // batch that was already given the full wait bound has still not finished: one stuck batch
    // holds up everything behind it, so the next caller is told rather than queued behind it.
    //
    // The returned ticket may be dropped: a caller that only needs the work to happen, in
    // order, before whatever it submits next, needs nothing from it. Ordering against later
    // batches on this queue comes from submission order and the barriers in them, not from
    // the caller having waited.
    //
    // Anything the batch reads -- images, buffers, pipelines, descriptors -- must stay alive
    // until it completes. wait() and waitForSubmissions() are how a caller that is about to
    // destroy one of those gets there.
    SubmitTicket submit(const std::function<void(VkCommandBuffer)>& record, const SubmitScope& scope);

    // submit() with no semaphores: for work whose only ordering is against other batches on
    // this queue, which submission order and the barriers in them already give.
    SubmitTicket submit(const std::function<void(VkCommandBuffer)>& record) { return submit(record, {}); }

    // submit() with no semaphores, waited on before it returns. For work whose result the
    // host reads next: a readback that maps the buffer it copied into, an upload whose
    // staging buffer is freed at the end of the call.
    void immediateSubmit(const std::function<void(VkCommandBuffer)>& record);

    // Blocks until `ticket`'s batch has completed. Returns at once for a batch that already
    // has, including one submitted long enough ago that the Device has forgotten it.
    //
    // Bounded, unlike the unconditional fence wait this replaced: a wedged or lost GPU makes
    // this throw GpuFailure rather than parking the render worker forever, which is the one
    // failure the daemon has no other way out of. The bound is far above any batch this
    // renderer submits, so a merely slow GPU cannot trip it.
    void wait(SubmitTicket ticket);

    // Blocks until every batch submitted through this Device has completed. What a caller
    // about to destroy something a submission may still be reading needs; same bound and
    // same failure as wait().
    void waitForSubmissions();

    // waitForSubmissions() for a caller that cannot throw. False means the bound expired with
    // work still in flight, and then the caller MUST NOT free anything those batches read:
    // from a destructor, leaking is the only choice that is not corruption, since the GPU is
    // still reading what it was about to free.
    //
    // A false answer is remembered. Once anything has been abandoned this Device tears itself
    // down by leaking too, because vkDestroyDevice requires every child object to be gone and
    // the submitted work to have completed, and neither is true any more.
    [[nodiscard]] bool drainForTeardown() noexcept;

    // Names the most recent batch submitted, or nothing at all when there has been none.
    // What a caller records when it stops using something the GPU may be reading: no batch
    // submitted after this point can reach it, so this is the last one that could.
    SubmitTicket lastSubmission() const;

    // Whether `ticket`'s batch has completed, without blocking. Only ever goes from false to
    // true, and retires whatever has finished on the way.
    bool hasCompleted(SubmitTicket ticket);

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

    // One batch the GPU may still be executing. Its command buffer and fence return to the
    // free lists only once the fence has signalled, so a handle is in exactly one of three
    // places at any moment -- a pending batch, a free list, or destroyed -- and reusing one
    // the GPU is still reading is not expressible rather than merely unlikely.
    struct Pending {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        uint64_t serial = 0;
        // Whether a wait already gave this batch the full bound and it did not finish. Kept on
        // the batch rather than beside the list so it cannot outlive what it describes:
        // retiring the batch takes the record with it, which is what lets a GPU that catches
        // up be told from one that is wedged.
        bool waitExpired = false;
    };

    // Moves every batch whose fence has signalled onto the free lists. Cheap enough to run
    // on every submit: one vkGetFenceStatus per outstanding batch, and there are at most
    // kMaxSubmissionsInFlight of those.
    void retireCompleted();
    // retireCompleted() where a throw is not allowed, answering whether nothing is left
    // outstanding. A retire that could not run answers "outstanding", the safe direction.
    bool retireCompletedNoThrow() noexcept;
    bool isPending(uint64_t serial) const;
    // Whether some batch has already been given the full wait bound without finishing.
    bool stalled() const;
    [[noreturn]] void throwStalled(const char* what) const;
    void waitFences(const VkFence* fences, uint32_t count, const char* what);
    VkCommandBuffer takeCommandBuffer();
    VkFence takeFence();
    // Frees a command buffer and fence that no submission accepted. Destroyed rather than
    // recycled: after a vkQueueSubmit that returned VK_ERROR_DEVICE_LOST the driver's hold on
    // the fence is unknowable, and this path is rare enough that one allocation is cheaper
    // than reasoning about it.
    void discardSubmitObjects(VkCommandBuffer cmd, VkFence fence) noexcept;

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

    // Batches submitted and not yet known complete, oldest first. Touched only from the GPU
    // thread, so no lock: assertGpuThread() is what keeps that true.
    std::vector<Pending> m_pending;
    std::vector<VkCommandBuffer> m_freeCommandBuffers;
    // Every fence here is unsignalled -- retireCompleted() resets one before it lands.
    std::vector<VkFence> m_freeFences;
    uint64_t m_nextSerial = 1;

    // Whether something the GPU may still have been reading was left unfreed because a
    // teardown drain expired. Never cleared: the handles are gone by then, so a GPU that
    // later catches up does not make them destroyable, and destroying the device or the VMA
    // allocator around them is a validation error and a hard abort respectively.
    bool m_abandoned = false;
};

}
