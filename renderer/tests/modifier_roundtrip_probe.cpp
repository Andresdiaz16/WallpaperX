// Round-trips an exported frame through a real Vulkan importer.
//
// This is the check every other export test in this project is a proxy for. The rest
// assert that the export path's self-description is internally consistent (frame_export
// _probe), or that a LINEAR dma_buf's bytes sit where its metadata says (the contents
// check there, which only works because LINEAR rows are CPU-addressable). Neither can
// say anything about a *tiled* modifier, which is what real hardware actually picks --
// this machine's NVIDIA driver chooses a block-linear layout, and AMD/Intel choose
// compressed ones. Their bytes mean nothing to a CPU walking rows.
//
// So the importer here is a GPU. The exported fd is imported back as a second VkImage
// built with VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT and
// VkImageDrmFormatModifierExplicitCreateInfoEXT, described using *only* the modifier and
// plane layouts the frame published -- exactly what a compositor is given and exactly
// what it must be able to act on. The driver then detiles it on copy-out, and the result
// is compared against a readback of the original. If the published modifier is wrong, or
// a plane offset is, the pixels do not survive.
//
// Runs wherever dma_buf and DRM format modifiers are available. On NVIDIA that needs
// WALLPAPERX_FORCE_HANDLE_TYPE=dmabuf, which main() sets, because the shipping policy
// there is OPAQUE_FD -- a deliberate choice, not a capability limit.
//
// WHAT THIS ESTABLISHES, and what it does not. Each line below was settled by running
// the control, not by reasoning about it; a control that did not fire is recorded as
// such rather than quietly dropped.
//
//   modifier      Load-bearing on both backends. Substituting another supported
//                 modifier at import time makes all 36 rows differ (NVIDIA
//                 0x300000000606012 -> ...15). This runs as a permanent control below.
//   plane offset  Load-bearing on both. Offsetting the published value by 256 bytes
//                 makes all rows differ on Lavapipe and faults the GPU on NVIDIA
//                 (vkWaitForFences fails). NOT run permanently, precisely because
//                 provoking a device loss on every ctest run would take the rest of the
//                 suite down with it.
//   rowPitch      Load-bearing on LINEAR only. A +64 perturbation makes 35 of 36 rows
//                 differ on Lavapipe, and changes nothing on NVIDIA's block-linear
//                 modifier -- there the row stride is implied by the modifier, and the
//                 field the exporter publishes is simply not consulted. So this probe
//                 does not test rowPitch on tiled layouts; nothing here can.
//   queue-family  NOT established. exportFrame() releases to VK_QUEUE_FAMILY_FOREIGN_EXT
//   ownership     and acquireFromExternal() performs the matching acquire, so the pair
//                 is exercised and a malformed barrier would be caught by validation.
//                 But dropping the acquire entirely -- IGNORED/IGNORED with an UNDEFINED
//                 oldLayout -- still produces correct pixels on both backends, so this
//                 probe cannot tell a correct transfer from a missing one. The visible
//                 failure is expected to be AMD/Intel-only (compressed modifiers, where
//                 an unpaired release leaves metadata unresolved), which is hardware
//                 nobody here has. Treat the transfer as unverified.

#include "vulkan/device.h"
#include "support/validation_grade.h"

#include <atomic>
#include <memory>
#include "vulkan/frame_export.h"
#include "vulkan/memory.h"
#include "vulkan/memory_export.h"
#include "vulkan/offscreen.h"
#include "renderers/static_renderer.h"

#include "support/check.h"
#include "support/gpu_readback.h"
#include "support/test_image.h"

#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using wallpaperx::testing::check;
using wallpaperx::testing::countDifferingRows;
using wallpaperx::testing::failureCount;
using wallpaperx::testing::readbackImage;
using wallpaperx::testing::renderReference;
using wallpaperx::testing::variesInBothAxes;

// Same reasoning as frame_export_probe: not square, and not a round number of pixels
// wide, so a transposed or padding-blind interpretation cannot survive the comparison.
constexpr uint32_t kWidth = 68;
constexpr uint32_t kHeight = 36;

// What the source and imported images are both created with, taken from the offscreen
// target itself rather than restated. A DRM modifier is only valid for a specific usage,
// so a private copy of this mask stops matching the exported image's the moment a
// renderer needs another bit -- and the probe would then filter candidate modifiers
// against a usage the exported image was never created with and blame the round trip.
constexpr VkImageUsageFlags kUsage = wallpaperx::OffscreenTargetManager::kUsage;

// Set when the probe could not make its central claim on this machine -- the driver has
// no dma_buf with a describable layout at all, or it offers no second modifier for the
// control to substitute. Reported to ctest as a SKIP (see kNotVerifiedExitCode and the
// SKIP_RETURN_CODE property in CMakeLists.txt) rather than as a pass, because an
// inconclusive run and a verified one are not the same result and a green tick cannot
// tell them apart.
bool g_notVerified = false;

// Matches ctest's own convention for a skipped test; the SKIP_RETURN_CODE property is
// what makes ctest act on it.
constexpr int kNotVerifiedExitCode = 77;

std::string writeTestImage() {
    // Same gradient as frame_export_probe's, for the same reason (support/test_image.h),
    // written to a filename of this probe's own.
    return wallpaperx::testing::writeGradientPng(
        std::string(WALLPAPERX_TEST_IMAGE_DIR) + "/modifier_roundtrip_gradient.png", 16, 16);
}

// A memory type the driver will accept for both the image's requirements and the
// imported fd. Returns UINT32_MAX if the two do not intersect, which is itself a
// finding: it means the exporter produced a handle this device cannot bind.
uint32_t chooseMemoryType(VkPhysicalDevice physicalDevice, uint32_t imageBits, uint32_t fdBits) {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &props);
    const uint32_t usable = imageBits & fdBits;
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if (usable & (1u << i)) return i;
    }
    return UINT32_MAX;
}

// One imported image plus the memory backing it, so both are released together even
// when a check fails partway through.
struct ImportedImage {
    // Where an import stopped. The control below needs this, not just a bool: a
    // rejection at vkCreateImage means the driver refused to *describe* an image that
    // way and no dma_buf was ever interpreted, which is a much weaker statement than a
    // rejection at bind time or a successful import that produced wrong pixels.
    // Collapsing the two lets the control report "the modifier mattered" for a run in
    // which no import happened at all.
    // NotAttempted is the default on purpose. The control's pass criterion keys off
    // this, and an enum whose default was a real stage let *any* failure occurring
    // before importFrame ran (a failed dup, a non-dma_buf export) read as though the
    // import had been attempted and rejected.
    //
    // Ok is assigned on the success path, so `stage` alone is a complete account of what
    // happened and nothing else needs to track it. A parallel importAttempted bool used
    // to sit alongside this and contradicted it: it was set true after importFrame
    // returned, while a successful import left stage at NotAttempted, so the failure
    // diagnostic could report "import was never attempted" about an import that finished.
    enum class Stage { NotAttempted, Ok, CreateImage, QueryFd, MemoryType, Allocate, Bind };

    VkDevice device = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    Stage stage = Stage::NotAttempted;
    std::string error;  // non-empty means the import failed

    // What the importing device says this image needs, once it has been created. Zero
    // until then. Reported rather than acted on here: whether a mismatch against the
    // exporter's published memorySize is a defect depends on whether the import used the
    // frame's own modifier, which only the caller knows.
    VkDeviceSize requiredSize = 0;

    ~ImportedImage() {
        if (image) vkDestroyImage(device, image, nullptr);
        if (memory) vkFreeMemory(device, memory, nullptr);
    }
    ImportedImage() = default;
    ImportedImage(const ImportedImage&) = delete;
    ImportedImage& operator=(const ImportedImage&) = delete;
};

// Builds a VkImage over the exported fd, described entirely by `modifier` and `planes`.
//
// `modifier` is a parameter rather than being read from the frame so a control can pass
// a deliberately wrong one -- which is the only way to show this round trip is sensitive
// to the modifier at all, rather than passing because the driver ignored it.
//
// TAKES OWNERSHIP OF `fd` UNCONDITIONALLY, on every path, success or failure.
//
// This used to split ownership with the caller -- "the driver owns it once
// vkAllocateMemory succeeds, you own it otherwise" -- and that contract was already
// broken by its own implementation: a vkBindImageMemory failure set an error *after*
// vkAllocateMemory had consumed the fd, so the caller dutifully closed a descriptor the
// driver owned, and vkFreeMemory later closed it a second time. On a machine where the
// number had been recycled that silently kills an unrelated fd, and the probe then
// reports a fabricated failure somewhere else entirely.
//
// A conditional ownership contract has to be re-derived correctly at every call site
// and after every edit. Making it unconditional deletes the class of bug: callers
// hand over a dup() and never think about it again.
void importFrame(wallpaperx::Device& device, int fd, uint64_t modifier,
                 const std::vector<wallpaperx::ExportedFramePlane>& planes, ImportedImage& out) {
    out.device = device.device();

    std::vector<VkSubresourceLayout> planeLayouts(planes.size());
    for (size_t i = 0; i < planes.size(); ++i) {
        // Only offset and rowPitch may be set; size/arrayPitch/depthPitch must be zero
        // for an explicit-modifier image (VUID-VkImageDrmFormatModifierExplicitCreateInfoEXT
        // -size-02267). Anything else here is rejected, which is why the exporter only
        // ever publishes those two fields per plane.
        planeLayouts[i].offset = static_cast<VkDeviceSize>(planes[i].offset);
        planeLayouts[i].rowPitch = static_cast<VkDeviceSize>(planes[i].rowPitch);
    }

    VkImageDrmFormatModifierExplicitCreateInfoEXT explicitInfo{
        VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
    explicitInfo.drmFormatModifier = modifier;
    explicitInfo.drmFormatModifierPlaneCount = static_cast<uint32_t>(planeLayouts.size());
    explicitInfo.pPlaneLayouts = planeLayouts.data();

    VkExternalMemoryImageCreateInfo externalImageInfo{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    externalImageInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    externalImageInfo.pNext = &explicitInfo;

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.pNext = &externalImageInfo;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = wallpaperx::OffscreenTargetManager::kFormat;
    imageInfo.extent = {kWidth, kHeight, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    imageInfo.usage = kUsage;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    // UNDEFINED is mandatory for an imported image. The contents are then preserved not
    // by the initial layout but by the queue-family acquire below, which pairs with the
    // release exportFrame() performed -- that pairing is the whole contract.
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    // Closed on every path that does not reach a successful vkAllocateMemory; released
    // (ownership handed to the driver) at the one point that does.
    struct FdOwner {
        int fd;
        ~FdOwner() {
            if (fd >= 0) close(fd);
        }
        int release() {
            const int held = fd;
            fd = -1;
            return held;
        }
    } owner{fd};

    if (vkCreateImage(device.device(), &imageInfo, nullptr, &out.image) != VK_SUCCESS) {
        out.image = VK_NULL_HANDLE;
        out.stage = ImportedImage::Stage::CreateImage;
        out.error = "vkCreateImage rejected the published modifier/plane layout";
        return;
    }

    auto pfnGetMemoryFdProperties = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(
        vkGetDeviceProcAddr(device.device(), "vkGetMemoryFdPropertiesKHR"));
    if (!pfnGetMemoryFdProperties) {
        out.stage = ImportedImage::Stage::QueryFd;
        out.error = "vkGetMemoryFdPropertiesKHR entry point missing";
        return;
    }

    // Queried before the fd is handed to vkAllocateMemory, which consumes it.
    VkMemoryFdPropertiesKHR fdProps{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    if (pfnGetMemoryFdProperties(device.device(), VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fdProps) !=
        VK_SUCCESS) {
        out.stage = ImportedImage::Stage::QueryFd;
        out.error = "vkGetMemoryFdPropertiesKHR failed for the exported fd";
        return;
    }

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device.device(), out.image, &requirements);
    out.requiredSize = requirements.size;

    const uint32_t memoryType =
        chooseMemoryType(device.physicalDevice(), requirements.memoryTypeBits, fdProps.memoryTypeBits);
    if (memoryType == UINT32_MAX) {
        out.stage = ImportedImage::Stage::MemoryType;
        out.error = "no memory type satisfies both the imported image and the exported fd";
        return;
    }

    // Dedicated: the exporter allocated the image's memory dedicated
    // (VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT), and an import must match, or the
    // driver is entitled to assume a suballocation that does not exist.
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.image = out.image;

    VkImportMemoryFdInfoKHR importInfo{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
    importInfo.pNext = &dedicated;
    importInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    importInfo.fd = owner.fd;

    // A dedicated allocation must be exactly the bound image's requirement
    // (VUID-VkMemoryDedicatedAllocateInfo-image-01433 and the allocationSize rules that
    // go with it), so allocationSize comes from vkGetImageMemoryRequirements and never
    // from the exporter's published memorySize.
    //
    // Whether that published size is large enough is checked by the caller, not here.
    // It used to be checked at this point and made the import fail, which is wrong on
    // the control path: there the image is deliberately created with a *substituted*
    // modifier, so its requirement legitimately differs from the exporter's allocation
    // for the real one (LINEAR at a 512-byte pitch over 36 rows needs 18432 bytes where
    // the block-linear export allocated 16384). Correct driver behaviour then aborted
    // the import before the dma_buf was ever handed over, and the control failed red
    // having tested nothing.
    VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocInfo.pNext = &importInfo;
    allocInfo.allocationSize = requirements.size;
    allocInfo.memoryTypeIndex = memoryType;

    if (vkAllocateMemory(device.device(), &allocInfo, nullptr, &out.memory) != VK_SUCCESS) {
        out.memory = VK_NULL_HANDLE;
        out.stage = ImportedImage::Stage::Allocate;
        out.error = "vkAllocateMemory rejected the imported fd";
        return;
    }
    owner.release();  // the driver owns the fd from here on and closes it with the memory

    if (vkBindImageMemory(device.device(), out.image, out.memory, 0) != VK_SUCCESS) {
        out.stage = ImportedImage::Stage::Bind;
        out.error = "vkBindImageMemory failed for the imported image";
        return;
    }

    out.stage = ImportedImage::Stage::Ok;
}

// Takes ownership of the imported image back from the external consumer and leaves it
// in TRANSFER_SRC_OPTIMAL, ready to be copied out.
//
// srcQueueFamily must be the family exportFrame() released to, or the transfer is
// unpaired and the contents are formally undefined. It is derived the same way rather
// than hard-coded, so the two cannot drift.
//
// Correctly formed, but not something this probe can prove is necessary -- see the
// queue-family line in the header. Removing it entirely still yields correct pixels on
// both available backends. It is written properly because the spec requires it and
// because AMD/Intel are expected to need it, not because a check here would notice.
void acquireFromExternal(wallpaperx::Device& device, VkImage image) {
    const uint32_t srcQueueFamily =
        device.info().hasQueueFamilyForeign ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_EXTERNAL;

    device.immediateSubmit([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier acquire{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        acquire.srcAccessMask = 0;  // the release side made the writes available
        acquire.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        // GENERAL is what the release transitioned the image to. Naming anything else
        // (including UNDEFINED, which would be legal but discards the contents) makes
        // the acquire fail to preserve the very pixels this probe is checking.
        acquire.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        acquire.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        acquire.srcQueueFamilyIndex = srcQueueFamily;
        acquire.dstQueueFamilyIndex = device.graphicsQueueFamily();
        acquire.image = image;
        acquire.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &acquire);
    });
}

// A modifier that is NOT the one the exporter chose, but that this device could
// legitimately have used for the same image and that describes the same number of
// planes. Returns kDrmModifierInvalid when no such alternative exists, in which case the
// control cannot run and says so.
//
// Two constraints beyond "different from the chosen one", both learned by review:
//
//   planeCount   must equal the exported frame's, because frame.planes is what gets
//                handed to VkImageDrmFormatModifierExplicitCreateInfoEXT. This used to
//                be hardcoded to 1, which on AMD/Intel -- whose compressed modifiers are
//                two-plane, and which the probe header calls the case the round trip
//                matters most for -- meant no candidate ever matched and the control
//                silently skipped, reporting green having tested nothing.
//   features     must include IMPORTABLE, not just EXPORTABLE. The substitute is used on
//                the import side; a driver may advertise a modifier as exportable and
//                not importable, and the resulting vkCreateImage rejection would be
//                attributed to modifier insensitivity rather than to the capability.
uint64_t someOtherUsableModifier(VkPhysicalDevice physicalDevice, uint64_t chosen, size_t planeCount) {
    const std::vector<uint64_t> usable = wallpaperx::usableDrmModifiers(
        physicalDevice, wallpaperx::OffscreenTargetManager::kFormat, VK_IMAGE_TYPE_2D, kUsage, {kWidth, kHeight, 1},
        VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT);

    const std::vector<VkDrmFormatModifierPropertiesEXT> all =
        wallpaperx::queryDrmModifierList(physicalDevice, wallpaperx::OffscreenTargetManager::kFormat);
    for (uint64_t candidate : usable) {
        if (candidate == chosen) continue;
        for (const auto& props : all) {
            if (props.drmFormatModifier == candidate && props.drmFormatModifierPlaneCount == planeCount) {
                return candidate;
            }
        }
    }
    return wallpaperx::kDrmModifierInvalid;
}

// Renders the shared test image into a fresh target owned by `targets`.
//
// Every exported buffer in this probe gets its own target. Sharing one would mean two
// VkImages aliasing a single dma_buf and, worse, two acquires against a single release:
// exportFrame() issues exactly one release to VK_QUEUE_FAMILY_FOREIGN_EXT and
// early-returns on every later call, so a second import's acquire is unpaired and its
// contents are undefined *regardless of the modifier it was told*. The wrong-modifier
// control did exactly that, and would have kept reporting success even if the import had
// become completely modifier-blind.
const wallpaperx::OffscreenTarget& renderFreshTarget(wallpaperx::OffscreenTargetManager& targets,
                                                     wallpaperx::IRenderer& renderer, int monitorId) {
    const wallpaperx::OffscreenTarget& target = targets.create(monitorId, kWidth, kHeight);
    targets.render(renderer, target);
    return target;
}

// Which of the two claims this process is being asked to establish.
//
// Two entries rather than one because they are verifiable on different hardware and a
// single verdict cannot report that honestly. The round trip needs one usable modifier;
// modifier sensitivity needs two, so that a wrong one can be substituted. Lavapipe has
// exactly one (LINEAR), so it verifies the first and cannot verify the second -- and
// collapsing both into one result means either claiming the control ran when it did not,
// or reporting the whole probe as unverified when the round trip demonstrably passed.
enum class Mode { RoundTrip, Sensitivity };

// Held rather than snapshotted, and assigned right after the Device is built rather than
// at the end of run(). The previous shape did both wrong: it copied the count out while
// the Device was still alive (missing every teardown complaint) from a statement placed
// after a `return` that the RoundTrip mode never reaches -- so for one of the two modes
// this probe ships, the warning check could not fail at all.
std::shared_ptr<const std::atomic<int>> g_validationCounter;
bool g_validationLayerActive = false;

int run(Mode mode) {

    const std::string imagePath = writeTestImage();
    wallpaperx::Device device(/*enableValidation=*/true);
    // Before the first early return below, so every path out of run() carries it.
    g_validationCounter = device.validationCounter();
    g_validationLayerActive = device.validationLayerActive();

    if (!device.info().hasExternalMemoryDmaBuf || !device.info().hasImageDrmFormatModifier) {
        g_notVerified = true;
        std::cout << "modifier_roundtrip_probe: NOT VERIFIED -- this driver lacks "
                  << (device.info().hasExternalMemoryDmaBuf ? "VK_EXT_image_drm_format_modifier"
                                                            : "VK_EXT_external_memory_dma_buf")
                  << ", so there is no dma_buf with a describable layout to round-trip" << std::endl;
        return 0;
    }

    wallpaperx::OffscreenTargetManager targets(device);
    wallpaperx::StaticRenderer renderer(device, targets.renderPass(), imagePath, WALLPAPERX_SHADER_DIR);

    // --- The reference, from a target that is never exported. ---
    //
    // A separate target, deliberately; renderReference() carries the reasoning, which
    // this probe used to hold a second verbatim copy of. If StaticRenderer ever stops
    // being deterministic, variesInBothAxes() below is not the check that catches it --
    // the round trip simply starts failing, which is the correct direction to fail in.
    const std::vector<unsigned char> reference =
        renderReference(device, targets, renderer, /*monitorId=*/1, kWidth, kHeight);

    // Both axes, not just "some byte differs". A frame with identical rows compares
    // equal to an import that got the row stride or the plane offset wrong, so a
    // one-axis check would let the headline assertion pass on a broken round trip.
    check(variesInBothAxes(reference, kWidth, kHeight),
          "the reference frame varies along both axes, so a wrong stride or offset cannot compare equal to it");

    // Exports `target` and round-trips it through an importer told `modifier`. Returns
    // how many rows came back wrong, or -1 if the import never completed.
    struct Outcome {
        long rows = -1;
        ImportedImage::Stage stage = ImportedImage::Stage::NotAttempted;
        std::string error;
        uint64_t modifier = 0;
        size_t planes = 0;
        uint64_t memorySize = 0;    // what the exporter published
        uint64_t requiredSize = 0;  // what the importing device asked for, 0 if never asked
    };
    const auto roundTrip = [&](const wallpaperx::OffscreenTarget& target, bool useWrongModifier) -> Outcome {
        Outcome outcome;

        // Exported straight from the layout the render left behind -- no readback in
        // between -- so exportFrame() takes the same release scope the daemon does.
        const wallpaperx::ExportedFrame frame = wallpaperx::exportFrame(device, target);
        struct FdGuard {
            int fd;
            ~FdGuard() {
                if (fd >= 0) close(fd);
            }
        } exported{frame.fd};

        if (frame.type != wallpaperx::ExportedFrame::HandleType::DmaBuf) {
            outcome.error = "the export did not produce a dma_buf handle";
            return outcome;
        }
        if (frame.planes.empty()) {
            outcome.error = "the exported dma_buf frame carries no plane layout";
            return outcome;
        }
        outcome.modifier = frame.modifier;
        outcome.planes = frame.planes.size();
        outcome.memorySize = frame.memorySize;

        uint64_t modifier = frame.modifier;
        if (useWrongModifier) {
            modifier = someOtherUsableModifier(device.physicalDevice(), frame.modifier, frame.planes.size());
            if (modifier == wallpaperx::kDrmModifierInvalid) {
                outcome.error = "no alternative usable modifier";
                return outcome;
            }
            outcome.modifier = modifier;
        }

        // dup() because importFrame takes ownership of whatever it is handed; the
        // FdGuard above keeps the probe's own copy. A failed dup is a hard failure and
        // never a silently-skipped import: passing -1 through would make every import
        // fail at the fd query, which the control would then have counted as "the
        // modifier mattered" while nothing about the modifier was exercised.
        const int dupFd = dup(frame.fd);
        if (dupFd < 0) {
            outcome.error = "dup() of the exported fd failed";
            return outcome;
        }

        ImportedImage imported;
        importFrame(device, dupFd, modifier, frame.planes, imported);
        outcome.stage = imported.stage;
        outcome.requiredSize = imported.requiredSize;
        if (!imported.error.empty()) {
            outcome.error = imported.error;
            return outcome;
        }

        acquireFromExternal(device, imported.image);
        const std::vector<unsigned char> pixels = readbackImage(device, imported.image, kWidth, kHeight);
        outcome.rows = static_cast<long>(countDifferingRows(reference, pixels, kWidth, kHeight));
        return outcome;
    };

    // --- Claim 1: the round trip. ---
    if (mode == Mode::RoundTrip) {
        const Outcome real = roundTrip(renderFreshTarget(targets, renderer, 0), /*useWrongModifier=*/false);
        if (!real.error.empty()) {
            check(false, "a Vulkan importer accepts the published layout: " + real.error);
            return failureCount();
        }
        std::cout << "modifier_roundtrip_probe: " << device.info().gpuName << " chose modifier 0x" << std::hex
                  << real.modifier << std::dec << " with " << real.planes << " plane(s), memorySize " << real.memorySize
                  << std::endl;
        check(real.rows == 0,
              "an image imported from nothing but the fd and the published modifier/plane layout "
              "contains the exported pixels");
        if (real.rows > 0) {
            std::cerr << "  " << real.rows << " of " << kHeight << " rows differ after the round trip" << std::endl;
        }

        // The exporter's published memorySize against what the importing device actually
        // needed for the same image under the same modifier. Asserted only here, because
        // only here do the two describe the same layout -- the control substitutes a
        // modifier, so a difference there is expected and means nothing.
        //
        // A lower bound, not equality. memorySize comes from VMA's dedicated allocation,
        // which is legitimately larger than the image strictly needs (frame_export_probe
        // asserts only `memorySize >= minBytes` for the same reason, and NVIDIA backs a
        // 16 KiB allocation with a 64 KiB dma_buf). The defect is the other direction: a
        // consumer sizing its allocation from an under-reported value reads past the end
        // of the object.
        check(real.memorySize >= real.requiredSize,
              "the exporter's published memorySize is at least what an importing device needs for the same image");
        if (real.memorySize < real.requiredSize) {
            std::cerr << "  published memorySize " << real.memorySize << " but the importing device requires "
                      << real.requiredSize
                      << " -- a consumer sizing its allocation from the published value would be short" << std::endl;
        }
        // Printed, not asserted as equality. A 2026-08-03 QA sweep found the two are in
        // fact equal at every extent from 1x1 to 3840x2160 on this NVIDIA driver, which
        // is worth knowing but is not something to pin: a driver or allocator that
        // rounded a dedicated allocation up would be behaving legally, and asserting
        // equality would fail the probe red on hardware nobody here has for a
        // non-defect. Both directions that *are* defects already have assertions -- an
        // under-report is caught above, and a memorySize larger than the real object is
        // caught by the lseek gate in dbus_service_test before it can SIGBUS an mmap.
        std::cout << "modifier_roundtrip_probe: importer requires " << real.requiredSize << " for the same image ("
                  << (real.memorySize == real.requiredSize
                          ? "exactly the published memorySize"
                          : "published memorySize is larger by " + std::to_string(real.memorySize - real.requiredSize) +
                                " bytes")
                  << ")" << std::endl;
        return failureCount();
    }

    // --- Claim 2: the same round trip, on its own buffer, told the WRONG modifier. ---
    //
    // Its own target, so the release/acquire pair stays 1:1 and the result is
    // attributable to the substituted modifier rather than to an unpaired acquire on a
    // buffer a previous import already took ownership of.
    const Outcome control = roundTrip(renderFreshTarget(targets, renderer, 2), /*useWrongModifier=*/true);
    if (control.error == "no alternative usable modifier") {
        // NOT VERIFIED, not a pass, and scoped to this claim alone. A driver with one
        // usable modifier cannot be asked whether the importer honours the one it is
        // told, and reporting that as green is what let a modifier-blind importer ship
        // under a tick. It says nothing about the round trip, which is why that is a
        // separate ctest entry and still passes here.
        g_notVerified = true;
        std::cout << "modifier_roundtrip_probe: NOT VERIFIED -- this driver offers no second " << control.planes
                  << "-plane modifier usable for this format, usage and extent, so there is no wrong value to "
                     "substitute and modifier sensitivity was not tested"
                  << std::endl;
        return failureCount();
    }

    // The control counts as having fired if the substituted modifier demonstrably
    // influenced the outcome. The question is NOT "was the dma_buf memory interpreted",
    // which is what this used to ask and which scores affirmative evidence as none:
    //
    //   NotAttempted   we never reached importFrame (failed dup, non-dma_buf export, no
    //                  plane layout). Nothing about the modifier was exercised.
    //   QueryFd        vkGetMemoryFdPropertiesKHR failed; the modifier played no part.
    //   MemoryType     no memory type satisfies both the image and the fd. A capability
    //                  mismatch, not a statement about the layout.
    //   CreateImage    COUNTS. The driver refused to describe an image this way, and on
    //                  radv/anv that is exactly how modifier sensitivity is reported:
    //                  they validate pPlaneLayouts against the modifier at creation time
    //                  and return VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT.
    //                  The substitute is drawn from usableDrmModifiers with format,
    //                  type, usage, extent and importability already known-good, so the
    //                  only thing left that differs from the passing round trip above is
    //                  the (modifier, plane layout) pair. Excluding this failed the probe
    //                  red on every AMD and Intel GPU while the driver was demonstrating
    //                  precisely what the control asks about.
    //   Allocate/Bind  COUNT. The fd was handed over under this modifier and rejected.
    //
    // A successful import with differing pixels is the other form of positive evidence,
    // and the one NVIDIA produces.
    const bool modifierChangedTheOutcome = control.stage == ImportedImage::Stage::CreateImage ||
                                           control.stage == ImportedImage::Stage::Allocate ||
                                           control.stage == ImportedImage::Stage::Bind;
    const bool importedAndWrong = control.error.empty() && control.rows != 0;
    check(importedAndWrong || modifierChangedTheOutcome,
          "importing with a different but equally usable modifier yields wrong pixels, or is rejected because of the "
          "modifier it was told -- otherwise nothing about modifier sensitivity was tested");
    if (!importedAndWrong && !modifierChangedTheOutcome) {
        std::cerr << "  the control stopped at a stage the substituted modifier had no part in ("
                  << (control.stage == ImportedImage::Stage::NotAttempted ? "import was never attempted"
                                                                          : "rejected for a reason unrelated to the "
                                                                            "layout")
                  << "), so this run says nothing about whether the importer honours the modifier it is told"
                  << std::endl;
    }
    std::cout << "modifier_roundtrip_probe: substituted modifier 0x" << std::hex << control.modifier
              << std::dec << " -> "
              << (!control.error.empty() ? control.error : std::to_string(control.rows) + " row(s) differ")
              << std::endl;

    return failureCount();
}

}  // namespace

int main(int argc, char** argv) {
    // argv[1] selects which of the two claims this process establishes; see Mode. It is
    // required rather than defaulted, so adding a third claim cannot silently inherit
    // whichever one happened to be the default.
    if (argc < 2) {
        std::cerr << "modifier_roundtrip_probe: expected 'roundtrip' or 'sensitivity' as argv[1]" << std::endl;
        return 1;
    }
    const std::string what = argv[1];
    Mode mode;
    if (what == "roundtrip") {
        mode = Mode::RoundTrip;
    } else if (what == "sensitivity") {
        mode = Mode::Sensitivity;
    } else {
        std::cerr << "modifier_roundtrip_probe: unknown mode '" << what << "'" << std::endl;
        return 1;
    }

    // Set before anything calls externalMemoryHandleType(), which resolves
    // WALLPAPERX_FORCE_HANDLE_TYPE exactly ONCE into a function-local static and never
    // re-reads it. So this must happen here, and a second setenv later in the process
    // would be ignored -- a probe wanting to compare the two handle types has to be two
    // processes, not two setenv calls.
    //
    // On NVIDIA this is what makes the probe run at all: the shipping policy there is
    // OPAQUE_FD, which has no modifier and nothing to import from. On Mesa it is a no-op
    // -- dma_buf is already the default.
    setenv("WALLPAPERX_FORCE_HANDLE_TYPE", "dmabuf", 1);

    try {
        const int failures = run(mode);
        if (failures != 0) {
            std::cerr << "modifier_roundtrip_probe (" << what << "): " << failures << " check(s) failed" << std::endl;
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "modifier_roundtrip_probe (" << what << ") failed: " << e.what() << std::endl;
        return 1;
    }

    // Graded here, after run() returned and therefore after ~Device, which is what makes
    // the layer's teardown complaints countable.
    bool waivedNoValidator = false;
    if (!g_validationLayerActive) {
        // Honours the same waiver every other probe does, through the same single
        // definition. Without it this binary was the one TSan-tier entry that still went
        // red on a host without the layer package -- the exact condition the waiver was
        // widened to accept, and the reason the CMake comment claiming "every test reads
        // it" was false.
        if (wallpaperx::testing::validationWaived()) {
            std::cout << "modifier_roundtrip_probe (" << what
                      << "): WALLPAPERX_ALLOW_NO_VALIDATION=1 and no validator is loaded -- the GPU-side "
                         "oracle is DISABLED for this run" << std::endl;
            // Falls through to the g_notVerified check below rather than returning 0.
            // Returning here reported an inconclusive run as a pass: under the TSan tier
            // the waiver is set on every test, so a driver that could not attempt the
            // substitution at all exited 0 instead of the skip code, and ctest recorded
            // a claim the run never established. The waiver excuses the missing
            // validator; it says nothing about whether the subject was verified.
            waivedNoValidator = true;
        } else {
            std::cerr << "modifier_roundtrip_probe (" << what
                      << "): VK_LAYER_KHRONOS_validation is not loaded, so a zero-warning result "
                         "validates nothing" << std::endl;
            return 1;
        }
    }
    // Only meaningful when a validator was listening; the waived case has nothing to
    // count and must not be graded as though it did.
    const int warnings = waivedNoValidator ? 0 : (g_validationCounter ? g_validationCounter->load() : 0);
    if (warnings > 0) {
        std::cerr << "modifier_roundtrip_probe (" << what << "): " << warnings << " Vulkan validation warning(s)"
                  << std::endl;
        return 1;
    }

    // After the failure and warning checks, so a run that is both inconclusive and
    // broken still reports as broken.
    if (g_notVerified) {
        std::cout << "modifier_roundtrip_probe (" << what
                  << "): NOT VERIFIED on this driver -- reported to ctest as a skip, not a pass" << std::endl;
        return kNotVerifiedExitCode;
    }

    std::cout << "modifier_roundtrip_probe (" << what << "): passed, zero validation warnings" << std::endl;
    return 0;
}
