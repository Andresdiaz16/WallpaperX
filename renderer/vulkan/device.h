// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "descriptor_allocator.h"
#include "gpu_fault.h"
#include "memory.h"
#include "tier_selection.h"

#include <array>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <vector>
#include <memory>
#include <stdexcept>
#include <string>
#include <atomic>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>

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

    // Which codecs this device's video decode queue can decode, as codec-operation bits: a
    // caller asks `videoDecodeOps & VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR`. Zero means
    // there is no hardware decoder to hand a file to -- no decode queue family (Lavapipe), no
    // per-codec extension, or a feature the decoder relies on missing -- and the file has to
    // be decoded in software. Non-zero answers only the codec; whether a particular file's
    // profile and size are accepted is the decoder's own question, asked per file.
    //
    // Derived from what vkCreateDevice was actually given, after it succeeded, so it cannot
    // name a codec whose extension is not enabled.
    VkVideoCodecOperationFlagsKHR videoDecodeOps = 0;
};

class Device {
public:
    // `onLost` runs once, on the thread whose call first saw VK_ERROR_DEVICE_LOST, before that
    // call returns. It must not block or call back into this Device; a throw from it is swallowed.
    explicit Device(bool enableValidation, std::function<void()> onLost = {});
    ~Device();

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    VkInstance instance() const { return m_instance; }
    VkPhysicalDevice physicalDevice() const { return m_physicalDevice; }
    VkDevice device() const { return m_device; }
    VkQueue graphicsQueue() const { return m_graphicsQueue; }
    uint32_t graphicsQueueFamily() const { return m_graphicsQueueFamily; }

    // One queue family that can decode video, with a queue on it. `ops` is what the family
    // decodes among the codecs whose extension is enabled, never zero. `queue` is
    // graphicsQueue() itself where the graphics family decodes: Vulkan forbids creating a
    // family's queue twice, and one is all a single decoder thread needs.
    struct VideoDecodeFamily {
        uint32_t family = UINT32_MAX;
        VkVideoCodecOperationFlagsKHR ops = 0;
        VkQueue queue = VK_NULL_HANDLE;
    };

    // Every decode family this device was created with; empty exactly when
    // info().videoDecodeOps is zero. Nothing in this Device submits to these queues. They exist
    // to be handed to a decoder library, which also submits to the graphics queue. Unless
    // queuesInternallySynchronized(), every queue is externally synchronised and that library
    // has to submit on the GPU thread.
    //
    // What that library submits is invisible here: wait(), waitForSubmissions(), safeToFree()
    // and teardown cover only batches this Device submitted, so a decoder's outstanding work is
    // the decoder's to drain before anything it read or wrote is freed.
    const std::vector<VideoDecodeFamily>& videoDecodeFamilies() const { return m_videoDecodeFamilies; }

    // Whether every queue this device has was created internally synchronised
    // (VK_KHR_internally_synchronized_queues), so a decoder library may submit to it from
    // another thread while this Device submits from the GPU thread. Only asked for where there
    // is a decoder to share the queues with. Read off the feature chain the library is given,
    // which is also how the library learns to fetch its queues the same way.
    bool queuesInternallySynchronized() const {
        return m_features.internallySynchronizedQueues.internallySynchronizedQueues == VK_TRUE;
    }

    // Whether VK_KHR_video_maintenance2 is enabled, which a decoder library given this device's
    // extension list takes as leave to pass each picture's parameter sets inline rather than
    // through session parameters it builds and caches.
    bool videoInlineParameters() const { return m_features.videoMaintenance2.videoMaintenance2 == VK_TRUE; }

    const DeviceInfo& info() const { return m_info; }
    DescriptorTier descriptorTier() const { return m_selectedTier; }

    // What this device was created with, for a library that has to be told -- ffmpeg's Vulkan
    // context wants the enabled extensions and the enabled feature chain. These are the very
    // arrays and structs vkCreateInstance and vkCreateDevice were given, not copies made
    // afterwards, so they cannot disagree with what was enabled. The feature chain is walked
    // by pNext from the head, in the fixed order the EnabledFeatures comment gives.
    const std::vector<const char*>& enabledInstanceExtensions() const { return m_enabledInstanceExtensions; }
    const std::vector<const char*>& enabledDeviceExtensions() const { return m_enabledDeviceExtensions; }
    const VkPhysicalDeviceFeatures2& enabledFeatures() const { return m_features.features2; }

    // Read off the feature chain rather than remembered beside it.
    bool timelineSemaphore() const { return m_features.vulkan12.timelineSemaphore == VK_TRUE; }
    bool samplerYcbcrConversion() const { return m_features.vulkan11.samplerYcbcrConversion == VK_TRUE; }

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
    //
    // A wait is one object holding the semaphore and the stage it is waited at, built only by a
    // constructor that takes both: an aggregate would let `{semaphore}` compile with the stage
    // zeroed, and a zero stage mask is no execution dependency at all -- the consumer reads
    // while the producer still writes, and not every driver refuses it. A timeline semaphore
    // likewise travels with its value; one in a binary list is undefined and unrefused.
    struct SubmitScope {
        struct Wait {
            Wait(VkSemaphore semaphore, VkPipelineStageFlags stage) : semaphore(semaphore), stage(stage) {}
            VkSemaphore semaphore;
            VkPipelineStageFlags stage;
        };
        struct TimelineWait {
            TimelineWait(VkSemaphore semaphore, uint64_t value, VkPipelineStageFlags stage)
                : semaphore(semaphore), value(value), stage(stage) {}
            VkSemaphore semaphore;
            uint64_t value;
            VkPipelineStageFlags stage;
        };
        struct TimelineSignal {
            TimelineSignal(VkSemaphore semaphore, uint64_t value) : semaphore(semaphore), value(value) {}
            VkSemaphore semaphore;
            uint64_t value;
        };

        // Room for more of each kind than any caller adds (a draw of a decoded picture adds one
        // timeline wait and one signal). Fixed, so a scope built on every draw allocates nothing;
        // adding past it throws std::length_error before anything is submitted.
        static constexpr size_t kCapacity = 4;

        template <class T>
        class List {
            static_assert(std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>);

        public:
            List() {}
            List(std::initializer_list<T> items) {
                for (const T& item : items) push_back(item);
            }
            List(const List& other) : m_size(other.m_size) {
                std::uninitialized_copy(other.begin(), other.end(), m_items);
            }
            List& operator=(const List& other) {
                if (this == &other) return *this;
                m_size = other.m_size;
                std::uninitialized_copy(other.begin(), other.end(), m_items);
                return *this;
            }

            template <class... Args>
            T& emplace_back(Args&&... args) {
                if (m_size == kCapacity) throw std::length_error("Device::SubmitScope: more entries than it holds");
                T* item = std::construct_at(m_items + m_size, std::forward<Args>(args)...);
                ++m_size;
                return *item;
            }
            void push_back(const T& item) { emplace_back(item); }

            const T* begin() const { return m_items; }
            const T* end() const { return m_items + m_size; }
            size_t size() const { return m_size; }
            bool empty() const { return m_size == 0; }

        private:
            // A union, so the entries need no default constructor: they deliberately have none.
            union {
                T m_items[kCapacity];
            };
            size_t m_size = 0;
        };

        List<Wait> wait;
        List<VkSemaphore> signal;
        List<TimelineWait> timelineWait;
        List<TimelineSignal> timelineSignal;
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
    // GPU. Throws GpuFailure on any Vulkan failure, and std::invalid_argument for a wait whose
    // stage mask is zero. Also refuses, without touching the queue, while a
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

    // submit() of an empty batch that also signals a timeline semaphore of this Device's, so that
    // hasCompleted() off the GPU thread learns of this batch, and of every one before it, once the
    // GPU has run it rather than at the GPU thread's next look. Throws as submit() does, and
    // GpuFailure when the semaphore cannot be made.
    SubmitTicket submitMarker();

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
    //
    // Throws without waiting when the batch is still outstanding and some batch has already
    // been given the full bound, whichever one that is: nothing gets past a wedged batch on
    // this queue, so a second bound would only cost the caller another ten seconds.
    void wait(SubmitTicket ticket);

    // Blocks until every batch submitted through this Device has completed. What a caller
    // about to destroy something a submission may still be reading needs; same bound and
    // same failure as wait().
    void waitForSubmissions();

    // The whole of what a caller that cannot throw -- a destructor, above all -- has to do
    // before freeing something the GPU may still be reading. True means free it. False means
    // the GPU has not finished with it and freeing it would be corruption, so the caller MUST
    // leak it; the report naming `whatIsLeaked` is already printed and this Device is already
    // recorded as unteardownable by the time false comes back. Neither half is the caller's to
    // remember.
    //
    // Host bookkeeping is not covered by that and must still be done on a false answer --
    // releasing a descriptor slot, dropping a registration. Nothing there is a Vulkan object,
    // and holding a slot back strands a pool whose capacity is one for the life of the process
    // long after the GPU has recovered. The slot pool has its own guard against reuse under a
    // running batch.
    //
    // `reader` is the one batch that can be reading it, for a caller that knows which. It is
    // POLLED, never waited on: the precondition is that the caller has already waited for that
    // batch, so a second bound would only re-learn what its own wait reported. Without one,
    // every pending batch is given the full bound, because any of them might be the reader.
    [[nodiscard]] bool safeToFree(const char* whatIsLeaked,
                                  std::optional<SubmitTicket> reader = std::nullopt) noexcept;

    // For a caller that leaks objects made on this device on a judgement safeToFree() did not make --
    // a decoder whose library would wait on work this Device never submitted. Prints the report and
    // makes teardown leak the device too, as a false safeToFree() does: destroying a device while a
    // child of it lives is invalid even once the GPU has caught up.
    void recordLeak(const char* whatIsLeaked) noexcept;

    // recordLeak() for a caller that has left a thread running which still uses this Device: one
    // it told to stop and could wait for no longer. From then on no Device in this process is
    // freed (DeviceDeleter), and the process must end without running static destructors, which
    // would unload the driver and ffmpeg under that thread.
    void recordThreadLeftRunning(const char* whatIsLeaked) noexcept;
    // Whether recordThreadLeftRunning() has been called in this process. Any thread.
    static bool threadLeftRunning() noexcept;

    // Whether a queue submission or a fence query on this Device has returned
    // VK_ERROR_DEVICE_LOST. Permanent once true, as the loss is: nothing this Device submits can
    // run again. Object creation still succeeds on a lost device, so no creation call is asked.
    bool lost() const;

    // Names the most recent batch submitted, or nothing at all when there has been none.
    // What a caller records when it stops using something the GPU may be reading: no batch
    // submitted after this point can reach it, so this is the last one that could. Any thread.
    SubmitTicket lastSubmission() const;

    // Whether `ticket`'s batch has completed, without blocking. Only ever goes from false to
    // true. On the GPU thread it retires whatever has finished on the way; on any other it
    // answers from the GPU thread's last look and the last submitMarker() batch, so it can still
    // say false for a batch that has finished since -- never true for one that has not.
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
    // the Device but through lastSubmission() and hasCompleted(); calling it while the worker
    // still runs re-points the check at the wrong thread and disables the very protection it
    // exists for.
    void adoptGpuThread() { m_gpuThread.store(std::this_thread::get_id(), std::memory_order_relaxed); }

private:
    // Bounded wait for every batch still pending, for a caller that cannot throw. False means
    // the bound expired with work in flight, and its two callers -- safeToFree() and teardown
    // -- both respond by leaking, which is why this latches rather than leaving that to them.
    [[nodiscard]] bool drainForTeardown() noexcept;

    // Records that a Vulkan object or allocation some batch may still read has been orphaned.
    // Never cleared: the handle is unreachable by then, so a GPU that later catches up does
    // not make it destroyable again.
    void abandon() noexcept { m_abandoned = true; }

    // Passes `result` through, latching lost() on VK_ERROR_DEVICE_LOST. Every queue and fence
    // result goes through it.
    VkResult observe(VkResult result) noexcept;

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
        // How a wait for this batch ended without it having finished, and VK_SUCCESS while no
        // wait has given up on it -- VK_TIMEOUT for the full bound spent, another error for a
        // wait that could not be made at all. The result rather than a flag because the two
        // read the same to a caller and describe different faults to an operator. Kept on the
        // batch rather than beside the list so it cannot outlive what it describes: retiring
        // the batch takes the record with it, which is what lets a GPU that catches up be told
        // from one that is wedged.
        VkResult waitOutcome = VK_SUCCESS;
    };

    // Moves every batch whose fence has signalled onto the free lists. Cheap enough to run
    // on every submit: one vkGetFenceStatus per outstanding batch, and there are at most
    // kMaxSubmissionsInFlight of those. noexcept is a contract the free lists' reserve in
    // createCommandPool() pays for -- pending plus free never exceeds the cap -- so that a
    // caller that cannot throw gets the state of the GPU and not the state of the heap.
    void retireCompleted() noexcept;
    bool isPending(uint64_t serial) const;
    // How the queue stopped making progress, or VK_SUCCESS while no outstanding batch has had
    // a wait give up on it -- so it answers "is it stalled" and "with what" in one value.
    VkResult stalledResult() const;
    [[noreturn]] void throwStalled(const char* what, VkResult why) const;
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
    // Filled once vkCreateDevice has succeeded, from the extension list it was given.
    std::vector<VideoDecodeFamily> m_videoDecodeFamilies;
    // What each decode family advertises, before the per-codec extensions are known. Only the
    // list above, derived after creation, says what can actually be decoded.
    std::vector<std::pair<uint32_t, VkVideoCodecOperationFlagsKHR>> m_decodeFamilyOps;

    // The arrays vkCreateInstance and vkCreateDevice were given. Kept as the record of what is
    // enabled; the strings are the header's literals, so they outlive everything.
    std::vector<const char*> m_enabledInstanceExtensions;
    std::vector<const char*> m_enabledDeviceExtensions;

    // The feature chain vkCreateDevice was given, kept whole so it can be read back. Linked
    // from features2 through vulkan11, vulkan12, vulkan13, the extension structs of the
    // descriptor tier in use, then internallySynchronizedQueues, then videoMaintenance2. A struct
    // is linked and its flags set in one place, or neither: that is what lets an unlinked struct read VK_FALSE
    // throughout, which every accessor above relies on. The links point into this object,
    // which is why the Device is not movable.
    struct EnabledFeatures {
        VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        VkPhysicalDeviceVulkan11Features vulkan11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
        VkPhysicalDeviceVulkan12Features vulkan12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceVulkan13Features vulkan13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceDescriptorHeapFeaturesEXT descriptorHeap{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT};
        VkPhysicalDeviceMaintenance5FeaturesKHR maintenance5{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
        VkPhysicalDeviceShaderUntypedPointersFeaturesKHR untypedPointers{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_UNTYPED_POINTERS_FEATURES_KHR};
        VkPhysicalDeviceDescriptorBufferFeaturesEXT descriptorBuffer{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT};
        VkPhysicalDeviceInternallySynchronizedQueuesFeaturesKHR internallySynchronizedQueues{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INTERNALLY_SYNCHRONIZED_QUEUES_FEATURES_KHR};
        VkPhysicalDeviceVideoMaintenance2FeaturesKHR videoMaintenance2{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_2_FEATURES_KHR};

        EnabledFeatures() = default;
        EnabledFeatures(const EnabledFeatures&) = delete;
        EnabledFeatures& operator=(const EnabledFeatures&) = delete;
    };
    EnabledFeatures m_features;

    // The thread that constructed this Device, and therefore the only one allowed to
    // touch it. Captured rather than configured: whoever created the device is by
    // definition the GPU thread.
    // Atomic because hasCompleted() reads it from a decoder's thread, which can still be running
    // when an owner adopts the Device for teardown.
    std::atomic<std::thread::id> m_gpuThread{std::this_thread::get_id()};

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
    // Written on the GPU thread only; atomic so lastSubmission() and hasCompleted() answer on
    // any thread. Every serial up to m_completedThrough has completed, as retireCompleted()
    // last found.
    std::atomic<uint64_t> m_nextSerial{1};
    std::atomic<uint64_t> m_completedThrough{0};
    // submitMarker()'s timeline semaphore, made at its first call, and the serial of the last batch
    // that signals it, which is also the value it signals. Written on the GPU thread, read on any:
    // the serial is stored after the semaphore.
    std::atomic<VkSemaphore> m_marker{VK_NULL_HANDLE};
    std::atomic<uint64_t> m_lastMarker{0};

    // Whether something the GPU may still have been reading was left unfreed. Written only by
    // abandon(); destroying the device or the VMA allocator around an orphan is a validation
    // error and a hard abort respectively, so teardown leaks instead once this is set.
    bool m_abandoned = false;

    // Written only by observe().
    bool m_lost = false;
    std::function<void()> m_onLost;
};

}
