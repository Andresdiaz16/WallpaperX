// SPDX-License-Identifier: GPL-2.0-or-later

// The exporter's half of the dma_buf modifier negotiation: what a consumer reports it can
// import must constrain what the daemon actually hands out.
//
// The chain is SetImportModifiers over the bus -> WallpaperManager::setImportModifiers ->
// Memory::setConsumerModifiers -> the intersection inside
// Memory::createExportableImage(), which is where the decision is made. This probe grades
// that decision at its source, through the same calls the daemon makes: create an
// offscreen target, export it, and read the modifier the export published.
//
// WHAT THIS CAN AND CANNOT SHOW ON THE AVAILABLE HARDWARE. The failure mode negotiation
// exists to prevent -- an exporter and an importer that overlap only partially, so the
// exporter picks a layout the importer cannot read -- is not reproducible here: Lavapipe
// offers exactly one modifier (LINEAR) and NVIDIA offers several, but no consumer on this
// machine reports a *different* set, and none of the modifiers is multi-plane. What IS
// reproducible is the mechanism: that narrowing the reported set changes which layout the
// driver is offered, and that a set sharing nothing with the driver's is refused instead
// of quietly ignored. Both are asserted below; the first needs two usable modifiers and
// reports itself unverified where there is only one.

#include "vulkan/device.h"
#include "vulkan/frame_export.h"
#include "vulkan/memory.h"
#include "vulkan/memory_export.h"
#include "vulkan/offscreen.h"
#include "renderers/static_renderer.h"

#include "support/check.h"
#include "support/test_image.h"
#include "support/validation_grade.h"

#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using wallpaperx::testing::check;

constexpr uint32_t kWidth = 68;
constexpr uint32_t kHeight = 36;

constexpr int kNotVerifiedExitCode = 77;
bool g_notVerified = false;

// Set once one dma_buf export has demonstrably worked. Everything main()'s catch could
// see after that is a failure of the code under test, not of the host -- without this
// flag, a throw from any later step would be routed to the skip channel and a broken
// exporter would report as "this driver could not answer", which is exactly the
// inconclusive-reads-as-green trap the skip channel exists to remove.
bool g_exportEstablished = false;

// A modifier no driver defines: the vendor field (bits 56-63) is 0xFE, which is not an
// assigned DRM vendor. Used as the consumer set that shares nothing with the driver's.
constexpr uint64_t kUnrealModifier = 0xFE00000000001234ull;

std::string hex(uint64_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0x%llx", static_cast<unsigned long long>(value));
    return buffer;
}

// Renders a fresh target for `monitorId` and returns the modifier its export published.
//
// A fresh target per call, never a reused one: the modifier is chosen at image creation,
// so re-exporting an existing target would report the set that was in force when it was
// built and say nothing about the one under test.
uint64_t exportedModifier(wallpaperx::Device& device, wallpaperx::OffscreenTargetManager& targets,
                          wallpaperx::IRenderer& renderer, int monitorId) {
    const wallpaperx::OffscreenTarget& target = targets.create(monitorId, kWidth, kHeight);
    targets.render(renderer, target);
    const wallpaperx::ExportedFrame frame = wallpaperx::exportFrame(device, target);
    struct FdGuard {
        int fd;
        ~FdGuard() {
            if (fd >= 0) close(fd);
        }
    } guard{frame.fd};
    if (frame.type != wallpaperx::ExportedFrame::HandleType::DmaBuf) {
        throw std::runtime_error("the export did not produce a dma_buf handle");
    }
    return frame.modifier;
}

bool run(wallpaperx::testing::ValidationGrade& grade) {
    const std::string imagePath = wallpaperx::testing::writeGradientPng(
        std::string(WALLPAPERX_TEST_IMAGE_DIR) + "/consumer_modifier_gradient.png", 16, 16);

    wallpaperx::Device device(/*enableValidation=*/true);
    grade.watch(device);

    if (!device.info().hasExternalMemoryDmaBuf || !device.info().hasImageDrmFormatModifier) {
        g_notVerified = true;
        std::cout << "consumer_modifier_probe: NOT VERIFIED -- this driver has no dma_buf with a describable "
                     "layout, so there is no modifier to negotiate"
                  << std::endl;
        return true;
    }

    wallpaperx::OffscreenTargetManager targets(device);
    wallpaperx::StaticRenderer renderer(device, targets.renderPass(), imagePath, WALLPAPERX_SHADER_DIR);

    // The set the driver itself can both export and import for exactly the images the
    // daemon creates. Asked with OffscreenTargetManager's own format, usage and extent
    // rather than a private copy, for the reason that constant's comment gives: a private
    // mask stops matching the moment a renderer needs another bit, and the probe would
    // then filter candidates against a usage no exported image was ever created with.
    const std::vector<uint64_t> usable = wallpaperx::usableDrmModifiers(
        device.physicalDevice(), wallpaperx::OffscreenTargetManager::kFormat, VK_IMAGE_TYPE_2D,
        wallpaperx::OffscreenTargetManager::kUsage, {kWidth, kHeight, 1},
        VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT);
    std::cout << "consumer_modifier_probe: " << device.info().gpuName << " offers " << usable.size()
              << " usable modifier(s)" << std::endl;

    // --- What the driver picks when no consumer has reported. ---
    const uint64_t unnegotiated = exportedModifier(device, targets, renderer, /*monitorId=*/0);
    g_exportEstablished = true;
    // NO CONTROL, stated rather than left to be assumed. Making this go red needs the
    // exporter to publish a modifier outside the exportable-AND-importable set, and the
    // one production edit that would do it -- dropping IMPORTABLE from
    // createExportableImage's feature mask -- changes nothing on either driver here,
    // because both advertise the same modifiers in both directions (tried; the probe
    // stayed green). The only remaining way to fail it is to alter the probe's own query,
    // which tests the check's input rather than the code. Kept because it is the premise
    // the narrowing check below rests on, and cheap; read it as unverified, not as
    // established.
    check(std::find(usable.begin(), usable.end(), unnegotiated) != usable.end(),
          "with no consumer report, the exported modifier is one this driver listed as usable");
    std::cout << "consumer_modifier_probe: unnegotiated choice " << hex(unnegotiated) << std::endl;

    // GOAL: a reported set that shares nothing with the driver's usable set is REFUSED,
    //       not ignored. False if an image is created anyway: the daemon then exports a
    //       layout the consumer said it cannot read, the compositor's import fails, and
    //       GetStatus still reports has_frame with no error -- a blank wallpaper with
    //       nothing anywhere naming a cause.
    // WHY-THIS-SHAPE: a modifier with an unassigned DRM vendor byte, so no driver can
    //       legitimately offer it and the disjointness holds on every backend. The
    //       obvious alternative -- picking a real modifier this driver happens not to
    //       support -- is not portable: what one driver omits another offers, so the case
    //       would silently stop being disjoint on the next GPU.
    // CONTROL: the `if (agreed.empty()) throw ...` block disabled in
    //       Memory::createExportableImage (renderer/vulkan/memory.cpp), leaving
    //       `modifiers = std::move(agreed)` to hand the driver an empty list. The two
    //       ICDs go red on different lines, which is why both checks are here: Lavapipe
    //       creates the image anyway -> "a consumer set that shares no modifier with this
    //       driver is refused"; NVIDIA still throws, but from vkCreateImage further down
    //       -> "the refusal names the disagreement rather than failing somewhere
    //       downstream", plus 10 validation warnings. Also observed with the whole
    //       consumer-modifier block disabled: both ICDs red on the first line. Reverted.
    device.memory().setConsumerModifiers({kUnrealModifier});
    bool refused = false;
    std::string refusal;
    try {
        (void)exportedModifier(device, targets, renderer, /*monitorId=*/1);
    } catch (const std::exception& e) {
        refused = true;
        refusal = e.what();
    }
    check(refused, "a consumer set that shares no modifier with this driver is refused, not silently ignored");
    if (refused) {
        check(refusal.find("share no DRM format modifier") != std::string::npos,
              "the refusal names the disagreement rather than failing somewhere downstream");
        if (refusal.find("share no DRM format modifier") == std::string::npos) {
            std::cerr << "  the refusal said: " << refusal << std::endl;
        }
    }

    // The unnegotiated default is restored by clearing the set, so a later render is not
    // stuck with the refusal. Asserted rather than assumed: if clearing did not restore
    // it, every check after a failed negotiation would be measuring a dead exporter.
    // In its own try, not left to main()'s catch: the set in force here is the disjoint
    // one, so a setter that ignored an empty vector would leave this export throwing --
    // and main() would report the run as a skip over a defect it had just provoked.
    device.memory().setConsumerModifiers({});
    bool restored = false;
    try {
        restored = exportedModifier(device, targets, renderer, /*monitorId=*/2) == unnegotiated;
    } catch (const std::exception& e) {
        std::cerr << "  the export after clearing the reported set threw: " << e.what() << std::endl;
    }
    check(restored, "clearing the reported set restores the unnegotiated choice");

    // GOAL: the reported set actually NARROWS what the driver is offered -- the exporter
    //       never publishes a modifier the consumer did not report.
    // WHY-THIS-SHAPE: the substitute is chosen to differ from the unnegotiated choice
    //       measured above, in this same run, rather than from a hardcoded list. The
    //       simpler form -- report one arbitrary usable modifier and assert the export
    //       matches -- goes vacuous exactly when the arbitrary pick happens to be the one
    //       the driver would have chosen anyway, which is what a "pick the first usable"
    //       implementation gives on most drivers. Requiring the reported one to differ
    //       from the free choice is what makes a passing result evidence that the report
    //       was consulted at all.
    // COVERAGE-ASSERT: `narrowed != unnegotiated` is asserted before the comparison, so a
    //       future edit to how the substitute is picked cannot quietly reduce this to
    //       "the driver chose what it was going to choose".
    // CONTROL: the `if (!m_consumerModifiers.empty()) { ... modifiers = std::move(agreed); }`
    //       block deleted from Memory::createExportableImage (renderer/vulkan/memory.cpp),
    //       so the full usable list is offered regardless of the report. Observed red on
    //       NVIDIA.
    uint64_t narrowed = 0;
    bool haveSubstitute = false;
    for (uint64_t candidate : usable) {
        if (candidate == unnegotiated) continue;
        narrowed = candidate;
        haveSubstitute = true;
        break;
    }
    if (!haveSubstitute) {
        // Scoped to this claim alone. Lavapipe has one usable modifier, so there is no
        // second layout to report and nothing about narrowing can be observed -- but the
        // refusal check above ran and held, and reporting the whole probe as unverified
        // would deny that.
        g_notVerified = true;
        std::cout << "consumer_modifier_probe: NOT VERIFIED -- this driver offers no usable modifier other than "
                  << hex(unnegotiated)
                  << ", so there is no second layout to report and narrowing cannot be observed" << std::endl;
        return wallpaperx::testing::failureCount() == 0;
    }

    check(narrowed != unnegotiated, "the reported modifier differs from the one the driver picks unprompted");
    device.memory().setConsumerModifiers({narrowed});
    const uint64_t negotiated = exportedModifier(device, targets, renderer, /*monitorId=*/3);
    const bool honoured = negotiated == narrowed;
    check(honoured, "the exported modifier is the one the consumer reported, not the one the driver preferred");
    if (!honoured) {
        std::cerr << "  reported " << hex(narrowed) << " but the export published " << hex(negotiated)
                  << " (unprompted choice is " << hex(unnegotiated) << ")" << std::endl;
    }
    std::cout << "consumer_modifier_probe: reported " << hex(narrowed) << ", exported " << hex(negotiated)
              << std::endl;

    return wallpaperx::testing::failureCount() == 0;
}

}  // namespace

int main() {
    // Must precede anything that calls externalMemoryHandleType(), which resolves this
    // once into a function-local static and never re-reads it. On NVIDIA it is what makes
    // the probe run at all -- the shipping policy there is OPAQUE_FD, which carries no
    // modifier and has nothing to negotiate. On Mesa it is a no-op.
    setenv("WALLPAPERX_FORCE_HANDLE_TYPE", "dmabuf", 1);

    const bool ok = wallpaperx::testing::gradedRun("consumer_modifier_probe", [](auto& grade) {
        try {
            return run(grade);
        } catch (const std::exception& e) {
            // Two verdicts from one catch, and the split is the point. Before any export
            // has worked, a throw means this host cannot produce a dma_buf at all -- a
            // host limitation, and charging it to the renderer is the attribution error
            // X.3 forbids. After one HAS worked, the same throw is the exporter failing on
            // a set it was asked to honour, and routing that to the skip channel would let
            // a broken negotiation exit 77 and read as "not applicable here".
            std::cerr << "consumer_modifier_probe: a dma_buf export threw: " << e.what() << std::endl;
            if (g_exportEstablished) {
                check(false, "no export after the first one threw");
                return false;
            }
            g_notVerified = true;
            return wallpaperx::testing::failureCount() == 0;
        }
    });

    if (!ok) {
        wallpaperx::testing::reportFailures("consumer_modifier_probe");
        return 1;
    }
    if (g_notVerified) {
        std::cout << "consumer_modifier_probe: NOT VERIFIED on this driver -- reported to ctest as a skip, not a "
                     "pass"
                  << std::endl;
        return kNotVerifiedExitCode;
    }
    std::cout << "consumer_modifier_probe: passed" << std::endl;
    return 0;
}
