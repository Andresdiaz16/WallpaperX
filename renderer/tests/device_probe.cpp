// SPDX-License-Identifier: GPL-2.0-or-later

#include "vulkan/device.h"
#include "support/validation_grade.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>

namespace {

using wallpaperx::DescriptorTier;

// forceEnv: the WALLPAPERX_FORCE_TIER value to set (nullptr = unset -> automatic).
// expectedTier: the tier the selection MUST resolve to on any hardware, or nullopt
// when the result legitimately depends on what the GPU supports (so we only smoke-
// test it). Asserting the tier -- not just "didn't crash" -- verifies the env var
// actually drives the real allocator, which the pure selectTier unit test can't.
bool probeTier(const char* forceEnv, std::optional<DescriptorTier> expectedTier) {
    if (forceEnv) {
        setenv("WALLPAPERX_FORCE_TIER", forceEnv, 1);
    } else {
        unsetenv("WALLPAPERX_FORCE_TIER");
    }

    const char* label = forceEnv ? forceEnv : "default";

    return wallpaperx::testing::gradedRun(
        std::string("device_probe (") + label + ")", [&](wallpaperx::testing::ValidationGrade& grade) {
        try {
            wallpaperx::Device device(/*enableValidation=*/true);
            grade.watch(device);
            const auto& info = device.info();
            const DescriptorTier tier = device.descriptorAllocator().tier();
            std::cout << "GPU: " << info.gpuName
                  << " | Vendor: " << wallpaperx::toString(info.vendor)
                  << " | Descriptor tier: " << wallpaperx::toString(tier)
                  << std::endl;

            // GOAL: S2.6 -- WALLPAPERX_FORCE_TIER reaches the real Device: classic yields Classic, and
            //       buffer never yields Heap.
            // CONTROL: renderer/vulkan/device.cpp determineTier, the override replaced by std::nullopt ->
            //       on device_probe_lavapipe "device_probe (classic): forced tier not honored -- expected
            //       Classic but got Heap" and "device_probe (buffer): forcing buffer must not select
            //       Heap". Reverted.
            if (expectedTier && tier != *expectedTier) {
            std::cerr << "device_probe (" << label << "): forced tier not honored -- expected "
                      << wallpaperx::toString(*expectedTier) << " but got " << wallpaperx::toString(tier)
                      << std::endl;
            return false;
            }
            // Forcing buffer must never fall UP to Heap (it can fall down to Classic if
            // the buffer tier is unsupported, which is hardware-dependent).
            if (forceEnv && std::strcmp(forceEnv, "buffer") == 0 && tier == DescriptorTier::Heap) {
            std::cerr << "device_probe (buffer): forcing buffer must not select Heap" << std::endl;
            return false;
            }
        } catch (const std::exception& e) {
            std::cerr << "device_probe (" << label << ") failed: " << e.what() << std::endl;
            return false;
        }
        return true;
    });
}

}

int main() {
    bool ok = true;
    // The default resolves to whatever the GPU supports, so it is only smoke-tested. Forcing
    // classic MUST yield Classic on any hardware; buffer is checked for the "never Heap"
    // invariant inside.
    ok &= probeTier(nullptr, std::nullopt);
    ok &= probeTier("buffer", std::nullopt);
    ok &= probeTier("classic", DescriptorTier::Classic);
    return ok ? 0 : 1;
}
