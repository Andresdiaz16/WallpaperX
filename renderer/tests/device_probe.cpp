#include "vulkan/device.h"

#include <cstdlib>
#include <iostream>

namespace {

bool probeTier(const char* forceEnv) {
    if (forceEnv) {
        setenv("WALLPAPERX_FORCE_TIER", forceEnv, 1);
    } else {
        unsetenv("WALLPAPERX_FORCE_TIER");
    }

    wallpaperx::resetValidationWarningCount();

    try {
        wallpaperx::Device device(/*enableValidation=*/true);
        const auto& info = device.info();
        std::cout << "GPU: " << info.gpuName
                  << " | Vendor: " << wallpaperx::toString(info.vendor)
                  << " | Descriptor tier: " << wallpaperx::toString(device.descriptorAllocator().tier())
                  << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "device_probe (" << (forceEnv ? forceEnv : "default") << ") failed: " << e.what() << std::endl;
        return false;
    }

    const int warnings = wallpaperx::validationWarningCount();
    if (warnings > 0) {
        std::cerr << "device_probe (" << (forceEnv ? forceEnv : "default") << "): "
                  << warnings << " Vulkan validation warning(s)" << std::endl;
        return false;
    }

    return true;
}

}

int main() {
    bool ok = true;
    ok &= probeTier(nullptr);
    ok &= probeTier("heap");
    ok &= probeTier("buffer");
    ok &= probeTier("classic");
    return ok ? 0 : 1;
}
