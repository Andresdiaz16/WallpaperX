#include "vulkan/device.h"

#include <atomic>
#include <memory>
#include "vulkan/offscreen.h"

#include <iostream>

int main() {

    std::shared_ptr<const std::atomic<int>> validationCounter;
    try {
        wallpaperx::Device device(/*enableValidation=*/true);
        // Taken inside the Device's scope and graded after it: the validation
        // layer's teardown complaints are emitted from ~Device, so a check that
        // ran before destruction would miss the half most worth having.
        validationCounter = device.validationCounter();
        wallpaperx::OffscreenTargetManager targets(device);

        const auto& monitor0 = targets.create(0, 1920, 1080);
        const auto& monitor1 = targets.create(1, 2560, 1440);

        if (monitor0.image.image == VK_NULL_HANDLE || monitor0.view == VK_NULL_HANDLE ||
            monitor0.framebuffer == VK_NULL_HANDLE) {
            std::cerr << "offscreen_probe: monitor 0 target has null handles" << std::endl;
            return 1;
        }
        if (monitor1.image.image == VK_NULL_HANDLE || monitor1.view == VK_NULL_HANDLE ||
            monitor1.framebuffer == VK_NULL_HANDLE) {
            std::cerr << "offscreen_probe: monitor 1 target has null handles" << std::endl;
            return 1;
        }
        if (!targets.has(0) || !targets.has(1)) {
            std::cerr << "offscreen_probe: has() reported missing target" << std::endl;
            return 1;
        }

        std::cout << "offscreen targets: monitor 0 " << monitor0.width << "x" << monitor0.height
                  << ", monitor 1 " << monitor1.width << "x" << monitor1.height << std::endl;

        targets.destroy(0);
        if (targets.has(0)) {
            std::cerr << "offscreen_probe: monitor 0 still present after destroy()" << std::endl;
            return 1;
        }

        const auto& monitor1Resized = targets.create(1, 1280, 720);
        if (monitor1Resized.width != 1280 || monitor1Resized.height != 720) {
            std::cerr << "offscreen_probe: re-create() for an existing monitor did not replace its target" << std::endl;
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "offscreen_probe failed: " << e.what() << std::endl;
        return 1;
    }

    const int warnings = (validationCounter ? validationCounter->load() : 0);
    if (warnings > 0) {
        std::cerr << "offscreen_probe: " << warnings << " Vulkan validation warning(s)" << std::endl;
        return 1;
    }

    std::cout << "offscreen_probe: passed, zero validation warnings" << std::endl;
    return 0;
}
