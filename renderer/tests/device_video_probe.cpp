// SPDX-License-Identifier: GPL-2.0-or-later
//
// The Device's creation record and its video-decode surface: S13.32-S13.36.
//
// WHY THIS FILE EXISTS. Every accessor commit 8f4b7d6 added to renderer/vulkan/device.h --
// enabledInstanceExtensions(), enabledDeviceExtensions(), enabledFeatures(),
// videoDecodeFamilies(), timelineSemaphore(), samplerYcbcrConversion() -- had no caller and no
// test anywhere in the tree. Their only consumer is the ffmpeg bridge, and it trusts them
// completely: it hands the two extension arrays and the feature chain straight to
// av_hwdevice_ctx_init(). So the property that matters is not "the accessor returns something"
// but "what it returns is what vkCreateInstance and vkCreateDevice were actually given", and
// the only way to know that is to read the creation calls themselves.
//
// HOW. The executable's definition of a Vulkan entry point preempts libvulkan's for every call
// site linked into it, including those inside wallpaperx-renderer.a; the real entry point is
// reached through RTLD_NEXT. That is the same technique ownership_transfer_probe and
// video_renderer_probe use, and it is required here because X.6 forbids a test-only seam in a
// production header -- there is no other way to see the argument a call was made with.
//
// FOUR ENTRIES, ONE BINARY. `enabled` and `waits` answer on any driver. `decode` answers
// differently on a driver with a hardware decoder and one without, and asserts the invariants
// that hold on both plus the ones only a decoder can show. `shared-decode-queue` needs a device
// whose graphics family also decodes, which neither driver here is, so it manufactures that
// report and skips where the driver has no video extensions at all.

#include "vulkan/device.h"
#include "vulkan/gpu_fault.h"

#include "support/check.h"
#include "support/validation_grade.h"

#include <dlfcn.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace {

using wallpaperx::Device;
using wallpaperx::DescriptorTier;
using wallpaperx::testing::check;
using wallpaperx::testing::fail;

constexpr int kNotVerifiedExitCode = 77;
bool g_notVerified = false;

// ------------------------------------------------------------------------- //
// What vkCreateInstance and vkCreateDevice were actually given.
// ------------------------------------------------------------------------- //

struct CreationRecord {
    bool seen = false;
    const char* const* extensionNames = nullptr;
    uint32_t extensionCount = 0;
    const void* featureChain = nullptr;
    std::vector<uint32_t> queueFamilies;
};

CreationRecord g_instanceCreation;
CreationRecord g_deviceCreation;

// Turns the graphics family into one that also decodes, so the aliasing case in
// Device::createLogicalDevice() -- decode family == graphics family, one queue, shared handle --
// can be reached on hardware that does not have it.
bool g_fakeDecodeOnGraphicsFamily = false;

// ------------------------------------------------------------------------- //
// Chain and extension helpers.
// ------------------------------------------------------------------------- //

std::vector<VkStructureType> chainTypes(const void* head) {
    std::vector<VkStructureType> types;
    for (const auto* node = static_cast<const VkBaseInStructure*>(head); node != nullptr; node = node->pNext) {
        types.push_back(node->sType);
    }
    return types;
}

const void* findInChain(const void* head, VkStructureType type) {
    for (const auto* node = static_cast<const VkBaseInStructure*>(head); node != nullptr; node = node->pNext) {
        if (node->sType == type) return node;
    }
    return nullptr;
}

bool chainHas(const void* head, VkStructureType type) { return findInChain(head, type) != nullptr; }

bool listedIn(const std::vector<const char*>& list, const char* name) {
    for (const char* entry : list) {
        if (std::strcmp(entry, name) == 0) return true;
    }
    return false;
}

// The four codecs a Vulkan decode queue can advertise that this Device knows about. VP9 is here
// on purpose although device.cpp deliberately never enables it: "videoDecodeOps names no codec
// whose extension is enabled" is only a claim worth making if the check can see a codec the
// table omits.
struct CodecEntry {
    const char* extension;
    VkVideoCodecOperationFlagBitsKHR op;
    const char* name;
};
constexpr std::array<CodecEntry, 4> kCodecs{{
    {VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME, VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR, "H.264"},
    {VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME, VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR, "H.265"},
    {VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME, VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR, "AV1"},
    {VK_KHR_VIDEO_DECODE_VP9_EXTENSION_NAME, VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR, "VP9"},
}};

// ------------------------------------------------------------------------- //
// S13.32 -- the record is the creation input
// ------------------------------------------------------------------------- //

void runEnabledChecks(Device& device) {
    if (!g_instanceCreation.seen || !g_deviceCreation.seen) {
        fail("vkCreateInstance or vkCreateDevice was never seen by the interposer, so nothing was compared");
        return;
    }

    // GOAL: S13.32 -- what a caller reads back as enabled is the array Vulkan was given, not a
    //       copy made afterwards. ffmpeg is handed exactly these pointers.
    // WHY-THIS-SHAPE: comparing the CONTENTS of the two lists would pass for an accessor that
    //       returns a freshly built copy -- which is precisely the shape the header says it is
    //       not, and the shape that can drift from what was enabled. Pointer identity is the
    //       only comparison that distinguishes them, so both are asserted: the pointer, and the
    //       length beside it (a pointer alone would survive a truncation).
    // CONTROL: renderer/vulkan/device.cpp, `std::vector<const char*>(m_enabledDeviceExtensions)
    //       .swap(m_enabledDeviceExtensions);` inserted after vkCreateDevice succeeds -- a copy
    //       with identical contents in different storage, which is exactly the defect the
    //       pointer comparison exists for. Observed: "FAIL: enabledDeviceExtensions() is not the
    //       array vkCreateDevice was given", exit 1. A `push_back` there instead gives "FAIL:
    //       enabledDeviceExtensions() has 14 entries but vkCreateDevice was given 13", which is
    //       the length half. Both reverted.
    check(device.enabledInstanceExtensions().data() == g_instanceCreation.extensionNames,
          "enabledInstanceExtensions() is not the array vkCreateInstance was given");
    check(device.enabledInstanceExtensions().size() == g_instanceCreation.extensionCount,
          "enabledInstanceExtensions() has a different length from what vkCreateInstance was given");
    check(device.enabledDeviceExtensions().data() == g_deviceCreation.extensionNames,
          "enabledDeviceExtensions() is not the array vkCreateDevice was given");
    if (device.enabledDeviceExtensions().size() != g_deviceCreation.extensionCount) {
        std::fprintf(stderr, "FAIL: enabledDeviceExtensions() has %zu entries but vkCreateDevice was given %u\n",
                     device.enabledDeviceExtensions().size(), g_deviceCreation.extensionCount);
        fail("enabled device extension count differs from the creation input");
    }
    check(static_cast<const void*>(&device.enabledFeatures()) == g_deviceCreation.featureChain,
          "enabledFeatures() is not the head of the pNext chain vkCreateDevice was given");

    // A zero-length list would make the three identity checks above vacuous -- nullptr == nullptr
    // for a vector that never got a push_back. VK_KHR_external_memory_fd is unconditional in
    // createLogicalDevice(), so the device list is never empty on a Device that constructed.
    check(!device.enabledDeviceExtensions().empty(),
          "the enabled device extension list is empty, so comparing it against the creation input proves nothing");

    // GOAL: S13.33 -- the feature record IS the chain the driver was given, in the header's
    //       stated order, with a tier's structs linked exactly when that tier is in use and
    //       every linked struct's own flag set.
    // WHY-THIS-SHAPE: asserting only that the chain is non-empty, or only that the accessors
    //       return something, cannot see the defect this invariant exists for: a struct that is
    //       linked while its flag is clear (the driver is asked for nothing and the caller
    //       believes it was asked), or a tier's struct linked under a different tier. Walking
    //       the chain from the accessor and cross-checking it against descriptorTier() is what
    //       makes both visible. The opposite direction -- a flag set on a struct that is NOT
    //       linked -- is not reachable from outside the class and is recorded as uncovered in
    //       GOALS.md rather than pretended to.
    // CONTROL: renderer/vulkan/device.cpp, three of them, all reverted. (1) Swap the
    //       vulkan11/vulkan12 links: "FAIL: the feature chain does not begin features2 ->
    //       Vulkan11 -> Vulkan12 -> Vulkan13". (2) `m_features.maintenance5.maintenance5 =
    //       VK_FALSE` while still linking it: "FAIL: the maintenance5 struct is in the chain with
    //       its flag clear". (3) `} else if (useBuffer) {` widened to `} else {` with the flag
    //       set only for the Buffer tier, which links a tier struct under the Classic tier: two
    //       reds on the classic entry, and NOTHING on the default entry -- which is why `enabled`
    //       is registered once per tier.
    const std::vector<VkStructureType> chain = chainTypes(device.enabledFeatures().pNext);
    const bool orderOk = chain.size() >= 3 &&
                         chain[0] == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES &&
                         chain[1] == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES &&
                         chain[2] == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    check(orderOk, "the feature chain does not begin features2 -> Vulkan11 -> Vulkan12 -> Vulkan13");

    const void* head = device.enabledFeatures().pNext;
    const bool heapLinked = chainHas(head, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT);
    const bool maintenance5Linked = chainHas(head, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR);
    const bool untypedLinked = chainHas(head, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_UNTYPED_POINTERS_FEATURES_KHR);
    const bool bufferLinked = chainHas(head, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT);
    const DescriptorTier tier = device.descriptorTier();

    check(heapLinked == (tier == DescriptorTier::Heap),
          "the descriptor-heap feature struct is linked into the chain iff the Heap tier is in use -- it is not");
    check(maintenance5Linked == (tier == DescriptorTier::Heap),
          "the maintenance5 feature struct is linked into the chain iff the Heap tier is in use -- it is not");
    check(untypedLinked == (tier == DescriptorTier::Heap),
          "the untyped-pointers feature struct is linked into the chain iff the Heap tier is in use -- it is not");
    check(bufferLinked == (tier == DescriptorTier::Buffer),
          "the descriptor-buffer feature struct is linked into the chain iff the Buffer tier is in use -- it is not");

    // Linked and set together, or neither: a struct in the chain with its flag clear asks the
    // driver for nothing while every reader believes the feature was enabled.
    if (heapLinked) {
        const auto* heap = static_cast<const VkPhysicalDeviceDescriptorHeapFeaturesEXT*>(
            findInChain(head, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT));
        check(heap->descriptorHeap == VK_TRUE, "the descriptor-heap struct is in the chain with its flag clear");
    }
    if (maintenance5Linked) {
        const auto* m5 = static_cast<const VkPhysicalDeviceMaintenance5FeaturesKHR*>(
            findInChain(head, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR));
        check(m5->maintenance5 == VK_TRUE, "the maintenance5 struct is in the chain with its flag clear");
    }
    if (untypedLinked) {
        const auto* up = static_cast<const VkPhysicalDeviceShaderUntypedPointersFeaturesKHR*>(
            findInChain(head, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_UNTYPED_POINTERS_FEATURES_KHR));
        check(up->shaderUntypedPointers == VK_TRUE,
              "the untyped-pointers struct is in the chain with its flag clear");
    }
    if (bufferLinked) {
        const auto* db = static_cast<const VkPhysicalDeviceDescriptorBufferFeaturesEXT*>(
            findInChain(head, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT));
        check(db->descriptorBuffer == VK_TRUE, "the descriptor-buffer struct is in the chain with its flag clear");
    }

    // GOAL: S13.33 -- the two feature accessors read the chain, not a value remembered beside it.
    // CONTROL: renderer/vulkan/device.h, `bool timelineSemaphore() const { return false; }`.
    //       Observed: "FAIL: timelineSemaphore() disagrees with the Vulkan 1.2 features struct
    //       vkCreateDevice was given". Reverted.
    const auto* vulkan11 = static_cast<const VkPhysicalDeviceVulkan11Features*>(
        findInChain(head, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES));
    const auto* vulkan12 = static_cast<const VkPhysicalDeviceVulkan12Features*>(
        findInChain(head, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES));
    if (vulkan11 == nullptr || vulkan12 == nullptr) {
        fail("the Vulkan 1.1 and 1.2 feature structs are not both in the chain");
    } else {
        check(device.timelineSemaphore() == (vulkan12->timelineSemaphore == VK_TRUE),
              "timelineSemaphore() disagrees with the Vulkan 1.2 features struct vkCreateDevice was given");
        check(device.samplerYcbcrConversion() == (vulkan11->samplerYcbcrConversion == VK_TRUE),
              "samplerYcbcrConversion() disagrees with the Vulkan 1.1 features struct vkCreateDevice was given");
    }

    std::printf("device_video_probe (enabled): tier %s, %zu instance extensions, %zu device extensions, "
                "%zu structs in the feature chain\n",
                wallpaperx::toString(tier), device.enabledInstanceExtensions().size(),
                device.enabledDeviceExtensions().size(), chain.size());
}

// ------------------------------------------------------------------------- //
// S13.34 / S13.36 -- the decode surface
// ------------------------------------------------------------------------- //

// Every invariant that must hold whether or not this device has a decoder. Shared by the
// `decode` and `shared-decode-queue` entries so the manufactured configuration is held to the
// same rules as the real one.
void runDecodeInvariants(Device& device) {
    const auto& families = device.videoDecodeFamilies();
    const VkVideoCodecOperationFlagsKHR reported = device.info().videoDecodeOps;

    // GOAL: S13.34 -- videoDecodeFamilies() is empty exactly when info().videoDecodeOps is zero,
    //       which is the header's own wording. Absence is a reported capability, not a failure.
    // WHY-THIS-SHAPE: asserting "there is a decoder" would be a check only the real GPU can pass
    //       and would report the software driver -- where having none is correct -- as a
    //       failure. The equivalence holds on both, and it is the thing a caller relies on:
    //       VideoDecoder::openDecoder() branches on videoDecodeOps alone and would then index a
    //       list that disagrees with it.
    // CONTROL: renderer/vulkan/device.cpp, delete `m_info.videoDecodeOps |= decode.ops;` from the
    //       family readback, so the families are listed while nothing is reported. Observed ten
    //       reds, this one first: "FAIL: a decode family is listed while info().videoDecodeOps is
    //       zero". Reverted.
    check(families.empty() == (reported == 0),
          families.empty() ? "info().videoDecodeOps is non-zero while no decode family is listed"
                           : "a decode family is listed while info().videoDecodeOps is zero");

    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device.physicalDevice(), &familyCount, nullptr);

    VkVideoCodecOperationFlagsKHR union_ = 0;
    for (const Device::VideoDecodeFamily& decode : families) {
        union_ |= decode.ops;
        check(decode.ops != 0, "a listed decode family advertises no codec operation at all");
        check(decode.family < familyCount, "a listed decode family index is not a real queue family");
        check(decode.queue != VK_NULL_HANDLE, "a listed decode family has no queue handle");
        check((decode.ops & reported) == decode.ops,
              "a decode family advertises a codec operation that info().videoDecodeOps does not report");

        // GOAL: S13.36 -- the decode queue handle is graphicsQueue() exactly when the decode
        //       family is the graphics family. Vulkan forbids creating a family's queue twice,
        //       so the shared case must hand back the queue the Device already owns.
        // CONTROL: see the shared-decode-queue entry, which is where this branch is reachable at
        //       all: removing addQueue()'s dedupe `return` makes it red there and changes
        //       nothing here.
        if (decode.family == device.graphicsQueueFamily()) {
            check(decode.queue == device.graphicsQueue(),
                  "a decode family that IS the graphics family does not share the graphics queue handle");
        } else {
            check(decode.queue != device.graphicsQueue(),
                  "a decode family that is not the graphics family was handed the graphics queue");
        }
    }
    check(union_ == reported,
          "info().videoDecodeOps is not the union of what the listed decode families advertise");

    // GOAL: S13.34 -- videoDecodeOps names no codec whose extension is absent from the enabled
    //       list. The header's claim is that it is derived from what vkCreateDevice was given,
    //       so it "cannot name a codec whose extension is not enabled".
    // WHY-THIS-SHAPE: the table below includes VP9, which device.cpp deliberately never enables
    //       because its extension needs a feature of its own. A check that only walked the three
    //       enabled codecs could not see a reported bit outside them, and "a codec no session
    //       can be created for" is exactly the wrong answer that looks like a right one.
    // CONTROL: renderer/vulkan/device.cpp, `VideoDecodeFamily decode{family, ops & enabledOps,
    //       ...}` changed to `ops | VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR`. Observed:
    //       "FAIL: info().videoDecodeOps reports VP9 but VK_KHR_video_decode_vp9 is not enabled".
    //       Reverted.
    VkVideoCodecOperationFlagsKHR accountedFor = 0;
    for (const CodecEntry& codec : kCodecs) {
        const bool enabled = listedIn(device.enabledDeviceExtensions(), codec.extension);
        const bool claimed = (reported & codec.op) != 0;
        accountedFor |= codec.op;
        if (claimed && !enabled) {
            std::fprintf(stderr, "FAIL: info().videoDecodeOps reports %s but %s is not enabled\n", codec.name,
                         codec.extension);
            fail("a reported codec has no enabled extension behind it");
        }
        if (enabled && !claimed) {
            std::fprintf(stderr, "FAIL: %s is enabled but info().videoDecodeOps does not report %s\n",
                         codec.extension, codec.name);
            fail("an enabled codec extension is not reported");
        }
    }
    check((reported & ~accountedFor) == 0,
          "info().videoDecodeOps reports a codec operation this probe's table does not name");

    const bool videoQueue = listedIn(device.enabledDeviceExtensions(), VK_KHR_VIDEO_QUEUE_EXTENSION_NAME);
    const bool decodeQueue = listedIn(device.enabledDeviceExtensions(), VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME);
    check((reported != 0) == (videoQueue && decodeQueue),
          "the two video queue extensions are enabled iff a decode capability is reported -- they are not");

    // GOAL: S13.36 -- no queue family is given two VkDeviceQueueCreateInfo entries. That is
    //       invalid usage, and the dedupe in addQueue() is the only thing preventing it once a
    //       decode family coincides with the graphics family.
    // WHY-THIS-SHAPE: read off the create-info array vkCreateDevice was handed rather than
    //       inferred from the reported families, because the create infos are built before the
    //       enabled-codec list is known and so are not derivable from what is reported.
    // CONTROL: renderer/vulkan/device.cpp, addQueue()'s dedupe `return` changed to `break`.
    //       Observed under the shared-decode-queue entry: "FAIL: a queue family was given two
    //       VkDeviceQueueCreateInfo entries" plus the layer's own
    //       VUID-VkDeviceCreateInfo-queueFamilyIndex-02802, while the plain decode entry stayed
    //       green. Reverted.
    if (!g_deviceCreation.seen) {
        fail("vkCreateDevice was never seen, so the queue create infos were not read");
    } else {
        std::vector<uint32_t> seen;
        bool duplicate = false;
        for (uint32_t family : g_deviceCreation.queueFamilies) {
            for (uint32_t other : seen) {
                if (other == family) duplicate = true;
            }
            seen.push_back(family);
        }
        check(!duplicate, "a queue family was given two VkDeviceQueueCreateInfo entries");
        for (const Device::VideoDecodeFamily& decode : families) {
            bool present = false;
            for (uint32_t family : g_deviceCreation.queueFamilies) {
                if (family == decode.family) present = true;
            }
            check(present, "a reported decode family had no queue created for it");
        }
    }
}

void runDecodeChecks(Device& device) {
    runDecodeInvariants(device);

    const auto& families = device.videoDecodeFamilies();
    if (families.empty()) {
        // Not a skip: "this driver has no decoder, reports none, and still works" is the whole of
        // S13.34's second half and it was just graded above. What is not answered here is the
        // shape of a real decode family, and saying so is the honest form.
        std::printf("device_video_probe (decode): %s reports no hardware video decoder -- the absence-is-a-"
                    "capability half is graded; the family shape is not answerable on this driver\n",
                    device.info().gpuName.c_str());
        return;
    }

    std::printf("device_video_probe (decode): %s reports codec operations 0x%x across %zu decode "
                "families (graphics family %u)\n",
                device.info().gpuName.c_str(), device.info().videoDecodeOps, families.size(),
                device.graphicsQueueFamily());
    for (const Device::VideoDecodeFamily& decode : families) {
        std::printf("  decode family %u, ops 0x%x, %s the graphics queue\n", decode.family, decode.ops,
                    decode.queue == device.graphicsQueue() ? "sharing" : "not sharing");
    }

    // A decoder library is told these three gate the decode queue; without them
    // createLogicalDevice() enables no codec at all, so a device that reports one must have them.
    check(device.timelineSemaphore(),
          "a decode capability is reported without timelineSemaphore, which the decode queue depends on");
}

// ------------------------------------------------------------------------- //
// S13.35 -- a wait carries its stage
// ------------------------------------------------------------------------- //

void runWaitTypeChecks() {
    using Wait = Device::SubmitScope::Wait;
    using TimelineWait = Device::SubmitScope::TimelineWait;
    using TimelineSignal = Device::SubmitScope::TimelineSignal;

    // GOAL: S13.35 -- neither the stage nor (for a timeline) the value can be omitted at the
    //       call site, because the type has no way to express it.
    // WHY-THIS-SHAPE: these are runtime checks on compile-time traits, not static_asserts. A
    //       static_assert that fires is a build failure, and a build failure reports as "the
    //       probe did not compile" with no line saying which property was lost; a check that
    //       fires prints the property by name and still lets the rest of the run report.
    // CONTROL: renderer/vulkan/device.h, give Wait's constructor a default
    //       `VkPipelineStageFlags stage = 0`. Observed: "FAIL: SubmitScope::Wait can be built
    //       from a semaphore alone, so a stage can be omitted". Reverted.
    check(!std::is_default_constructible_v<Wait>, "SubmitScope::Wait is default-constructible, so it can be empty");
    check(!std::is_aggregate_v<Wait>, "SubmitScope::Wait is an aggregate, so {semaphore} compiles with a zero stage");
    check(!std::is_constructible_v<Wait, VkSemaphore>,
          "SubmitScope::Wait can be built from a semaphore alone, so a stage can be omitted");

    check(!std::is_default_constructible_v<TimelineWait>, "SubmitScope::TimelineWait is default-constructible");
    check(!std::is_aggregate_v<TimelineWait>, "SubmitScope::TimelineWait is an aggregate");
    check(!std::is_constructible_v<TimelineWait, VkSemaphore>,
          "SubmitScope::TimelineWait can be built from a semaphore alone");
    check(!std::is_constructible_v<TimelineWait, VkSemaphore, uint64_t>,
          "SubmitScope::TimelineWait can be built without a stage, which orders nothing");

    check(!std::is_default_constructible_v<TimelineSignal>, "SubmitScope::TimelineSignal is default-constructible");
    check(!std::is_aggregate_v<TimelineSignal>, "SubmitScope::TimelineSignal is an aggregate");
    check(!std::is_constructible_v<TimelineSignal, VkSemaphore>,
          "SubmitScope::TimelineSignal can be built without a value, which is undefined in a binary list");
}

void runWaitChecks(Device& device) {
    runWaitTypeChecks();

    // Two binary semaphores, not one shared between the refused wait and the accepted one. A
    // binary semaphore's signal is consumed by the wait that takes it, so under the control that
    // removes the refusal the zero-stage submit swallows the signal and the accepted wait behind
    // it never completes -- the control run then hangs in the driver instead of terminating with
    // its red lines. One signal per wait keeps the control terminating.
    VkSemaphoreCreateInfo binaryInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore binary = VK_NULL_HANDLE;
    VkSemaphore binaryForAccepted = VK_NULL_HANDLE;
    if (vkCreateSemaphore(device.device(), &binaryInfo, nullptr, &binary) != VK_SUCCESS ||
        vkCreateSemaphore(device.device(), &binaryInfo, nullptr, &binaryForAccepted) != VK_SUCCESS) {
        fail("device_video_probe (waits): could not create the binary semaphores, so the refusal was not exercised");
        if (binary) vkDestroySemaphore(device.device(), binary, nullptr);
        return;
    }

    VkSemaphoreTypeCreateInfo timelineType{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    timelineType.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timelineType.initialValue = 7;
    VkSemaphoreCreateInfo timelineInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    timelineInfo.pNext = &timelineType;
    VkSemaphore timeline = VK_NULL_HANDLE;
    const bool haveTimeline =
        device.timelineSemaphore() &&
        vkCreateSemaphore(device.device(), &timelineInfo, nullptr, &timeline) == VK_SUCCESS;
    if (!haveTimeline) {
        std::printf("device_video_probe (waits): no timeline semaphore on this device -- the timeline half of the "
                    "zero-stage refusal is not answerable here\n");
    }

    // GOAL: S13.35 -- an explicitly zero stage is refused at submit with std::invalid_argument,
    //       before the queue is touched.
    // WHY-THIS-SHAPE: the semaphores are SATISFIED before the refusal is asked for -- the binary
    //       one by a prior submit that signals it, the timeline one by starting at a value the
    //       wait does not exceed. The obvious simpler form, waiting on a fresh unsignalled
    //       semaphore, passes on the strength of a hang the moment the guard is removed: the
    //       control run would park in the driver instead of printing a red line, which X.1
    //       forbids. Satisfied semaphores mean the control's submit completes and the check
    //       reports rather than stalls.
    // CONTROL: renderer/vulkan/device.cpp, delete both `if (entry.stage == 0) throw
    //       std::invalid_argument(...)` lines in submit(). Observed: "FAIL: a binary wait with a
    //       zero stage mask was accepted" and "FAIL: a timeline wait with a zero stage mask was
    //       accepted", exit 1, no hang. Reverted.
    // COVERAGE-ASSERT: refusals is asserted against the number of refusals that were ASKED for,
    //       so a future edit that stops building one of the two zero-stage scopes fails here
    //       instead of quietly halving what this grades.
    int refusalsAsked = 0;
    int refusals = 0;
    bool otherThrow = false;

    try {
        Device::SubmitScope signalScope;
        signalScope.signal.push_back(binary);
        signalScope.signal.push_back(binaryForAccepted);
        device.submit([](VkCommandBuffer) {}, signalScope);
        device.waitForSubmissions();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "device_video_probe (waits): could not signal the binary semaphores: %s\n", e.what());
        fail("setup for the zero-stage refusal did not complete");
        vkDestroySemaphore(device.device(), binary, nullptr);
        vkDestroySemaphore(device.device(), binaryForAccepted, nullptr);
        if (timeline) vkDestroySemaphore(device.device(), timeline, nullptr);
        return;
    }

    ++refusalsAsked;
    try {
        Device::SubmitScope zero;
        zero.wait.emplace_back(binary, 0);
        device.submit([](VkCommandBuffer) {}, zero);
        fail("a binary wait with a zero stage mask was accepted");
    } catch (const std::invalid_argument&) {
        ++refusals;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "device_video_probe (waits): binary zero-stage wait raised %s\n", e.what());
        otherThrow = true;
    }

    if (haveTimeline) {
        ++refusalsAsked;
        try {
            Device::SubmitScope zero;
            zero.timelineWait.emplace_back(timeline, 7, 0);
            device.submit([](VkCommandBuffer) {}, zero);
            fail("a timeline wait with a zero stage mask was accepted");
        } catch (const std::invalid_argument&) {
            ++refusals;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "device_video_probe (waits): timeline zero-stage wait raised %s\n", e.what());
            otherThrow = true;
        }
    }

    check(!otherThrow, "a zero-stage wait was refused with the wrong exception type");
    check(refusals == refusalsAsked, "not every zero-stage wait this probe built was refused");
    check(refusalsAsked >= 1, "no zero-stage wait was built, so the refusal was never asked for");

    // The same scopes with a real stage must go through, so the refusal above is the stage being
    // zero and not submit() refusing every scope that carries a wait.
    try {
        Device::SubmitScope ok;
        ok.wait.emplace_back(binaryForAccepted, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        if (haveTimeline) ok.timelineWait.emplace_back(timeline, 7, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        device.submit([](VkCommandBuffer) {}, ok);
        device.waitForSubmissions();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "device_video_probe (waits): a wait with a real stage was rejected: %s\n", e.what());
        fail("submit() refuses a wait even when its stage is non-zero");
    }

    std::printf("device_video_probe (waits): %d of %d zero-stage waits refused\n", refusals, refusalsAsked);

    vkDestroySemaphore(device.device(), binary, nullptr);
    vkDestroySemaphore(device.device(), binaryForAccepted, nullptr);
    if (timeline) vkDestroySemaphore(device.device(), timeline, nullptr);
}

}  // namespace

// ------------------------------------------------------------------------- //
// Interposers
// ------------------------------------------------------------------------- //

extern "C" VkResult vkCreateInstance(const VkInstanceCreateInfo* info, const VkAllocationCallbacks* allocator,
                                     VkInstance* instance) {
    g_instanceCreation.seen = true;
    g_instanceCreation.extensionNames = info->ppEnabledExtensionNames;
    g_instanceCreation.extensionCount = info->enabledExtensionCount;
    g_instanceCreation.featureChain = info->pNext;

    using Fn = VkResult (*)(const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*);
    static const Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "vkCreateInstance"));
    if (real == nullptr) {
        std::fprintf(stderr, "device_video_probe: cannot resolve the real vkCreateInstance\n");
        std::abort();
    }
    return real(info, allocator, instance);
}

extern "C" VkResult vkCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* info,
                                   const VkAllocationCallbacks* allocator, VkDevice* device) {
    g_deviceCreation.seen = true;
    g_deviceCreation.extensionNames = info->ppEnabledExtensionNames;
    g_deviceCreation.extensionCount = info->enabledExtensionCount;
    g_deviceCreation.featureChain = info->pNext;
    g_deviceCreation.queueFamilies.clear();
    for (uint32_t i = 0; i < info->queueCreateInfoCount; ++i) {
        g_deviceCreation.queueFamilies.push_back(info->pQueueCreateInfos[i].queueFamilyIndex);
    }

    using Fn = VkResult (*)(VkPhysicalDevice, const VkDeviceCreateInfo*, const VkAllocationCallbacks*, VkDevice*);
    static const Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "vkCreateDevice"));
    if (real == nullptr) {
        std::fprintf(stderr, "device_video_probe: cannot resolve the real vkCreateDevice\n");
        std::abort();
    }
    return real(physicalDevice, info, allocator, device);
}

// Reports the graphics family as also decoding H.264, so Device::createLogicalDevice()'s aliasing
// case -- one queue for a family that is both -- is reached on hardware that does not have it.
// Only the pQueueFamilyProperties pass is edited; the count-only pass is forwarded untouched.
extern "C" void vkGetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice physicalDevice, uint32_t* count,
                                                          VkQueueFamilyProperties2* properties) {
    using Fn = void (*)(VkPhysicalDevice, uint32_t*, VkQueueFamilyProperties2*);
    static const Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "vkGetPhysicalDeviceQueueFamilyProperties2"));
    if (real == nullptr) {
        std::fprintf(stderr, "device_video_probe: cannot resolve the real vkGetPhysicalDeviceQueueFamilyProperties2\n");
        std::abort();
    }
    real(physicalDevice, count, properties);
    if (!g_fakeDecodeOnGraphicsFamily || properties == nullptr) return;

    for (uint32_t i = 0; i < *count; ++i) {
        if ((properties[i].queueFamilyProperties.queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) continue;
        properties[i].queueFamilyProperties.queueFlags |= VK_QUEUE_VIDEO_DECODE_BIT_KHR;
        for (auto* node = static_cast<VkBaseOutStructure*>(properties[i].pNext); node != nullptr;
             node = node->pNext) {
            if (node->sType != VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR) continue;
            auto* video = reinterpret_cast<VkQueueFamilyVideoPropertiesKHR*>(node);
            video->videoCodecOperations |= VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
        }
        return;
    }
}

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode != "enabled" && mode != "decode" && mode != "waits" && mode != "shared-decode-queue") {
        std::fprintf(stderr, "usage: %s enabled|decode|waits|shared-decode-queue\n", argv[0]);
        return 1;
    }
    g_fakeDecodeOnGraphicsFamily = mode == "shared-decode-queue";

    const bool graded = wallpaperx::testing::gradedRun(
        "device_video_probe (" + mode + ")", [&](wallpaperx::testing::ValidationGrade& grade) {
            try {
                Device device(/*enableValidation=*/true);
                grade.watch(device);

                if (mode == "enabled") {
                    runEnabledChecks(device);
                } else if (mode == "decode") {
                    runDecodeChecks(device);
                } else if (mode == "waits") {
                    runWaitChecks(device);
                } else {
                    // GOAL: S13.36 -- a decode family that IS the graphics family shares the
                    //       graphics queue handle, and gets no second queue create info.
                    // WHY-THIS-SHAPE: neither driver available here puts decode on the graphics
                    //       family (the RTX 3080 Ti decodes on family 3, graphics is family 0),
                    //       so the branch is unreachable without manufacturing the report. The
                    //       simpler alternative -- assert the property on whatever families the
                    //       driver happens to have -- grades nothing at all on this hardware,
                    //       because the aliasing branch is never taken.
                    // CONTROL: renderer/vulkan/device.cpp, addQueue()'s dedupe `return` changed
                    //       to `break`. Observed with this entry: "FAIL: a queue family was given
                    //       two VkDeviceQueueCreateInfo entries" plus the layer's
                    //       VUID-VkDeviceCreateInfo-queueFamilyIndex-02802; the plain decode
                    //       entry stayed green throughout. Reverted.
                    if (device.info().videoDecodeOps == 0) {
                        std::printf("device_video_probe (shared-decode-queue): %s enables no video decode "
                                    "extension, so a decode family cannot be manufactured on it\n",
                                    device.info().gpuName.c_str());
                        g_notVerified = true;
                    } else {
                        bool aliased = false;
                        for (const Device::VideoDecodeFamily& decode : device.videoDecodeFamilies()) {
                            if (decode.family == device.graphicsQueueFamily()) aliased = true;
                        }
                        // COVERAGE-ASSERT: without this the entry passes on a run where the
                        // manufactured decode bit never reached the Device -- the invariants
                        // below would all hold vacuously over the driver's real families.
                        check(aliased,
                              "the manufactured decode bit did not reach the Device, so the aliasing case was "
                              "never exercised");
                        runDecodeInvariants(device);
                        std::printf("device_video_probe (shared-decode-queue): graphics family %u also reported as "
                                    "decoding; %zu decode families listed\n",
                                    device.graphicsQueueFamily(), device.videoDecodeFamilies().size());
                    }
                }
            } catch (const std::exception& e) {
                // Reported, not rethrown: an escaping exception would skip the validation grade in
                // gradedRun's frame and print no verdict for the checks that did run.
                fail(std::string("device_video_probe threw: ") + e.what());
            }
            return wallpaperx::testing::failureCount() == 0;
        });

    const int failures = wallpaperx::testing::reportFailures("device_video_probe");
    if (!graded || failures > 0) return 1;
    if (g_notVerified) {
        std::printf("device_video_probe (%s): NOT VERIFIED on this driver -- reported to ctest as a skip, not a "
                    "pass\n",
                    mode.c_str());
        return kNotVerifiedExitCode;
    }
    std::printf("device_video_probe (%s): all checks passed, zero validation warnings\n", mode.c_str());
    return 0;
}
