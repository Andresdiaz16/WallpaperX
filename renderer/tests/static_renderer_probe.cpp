// SPDX-License-Identifier: GPL-2.0-or-later

#include "vulkan/device.h"
#include "support/validation_grade.h"

#include "vulkan/offscreen.h"
#include "renderers/static_renderer.h"

#include "support/gpu_readback.h"
#include "support/test_image.h"

#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

std::string writeTestImage() {
    return wallpaperx::testing::writeSolidPng(
        std::string(WALLPAPERX_TEST_IMAGE_DIR) + "/test_red.png", 8, 8, {255, 0, 0, 255});
}

bool testRenderValidImage() {

    const bool ok = wallpaperx::testing::gradedRun(
        "static_renderer_probe", [&](wallpaperx::testing::ValidationGrade& grade) {
    try {
        const std::string imagePath = writeTestImage();

        wallpaperx::Device device(/*enableValidation=*/true);
        grade.watch(device);
        wallpaperx::OffscreenTargetManager targets(device);
        const auto& target = targets.create(0, 64, 64);

        wallpaperx::StaticRenderer renderer(device, targets.renderPass(), imagePath, WALLPAPERX_SHADER_DIR);

        targets.render(renderer, target);

        // The render published the frame, so the graphics queue no longer owns this image.
        // reclaimAndReadback() submits the acquire that takes it back before it copies --
        // see gpu_readback.h for why copying without it is undefined and unreportable.
        const std::vector<unsigned char> pixels =
            wallpaperx::testing::reclaimAndReadback(device, targets, target);

        const unsigned char b = pixels[0];
        const unsigned char g = pixels[1];
        const unsigned char r = pixels[2];
        const unsigned char a = pixels[3];

        std::cout << "static_renderer_probe: readback pixel (B,G,R,A) = ("
                  << int(b) << "," << int(g) << "," << int(r) << "," << int(a) << ")" << std::endl;

        if (r < 250 || g > 5 || b > 5 || a < 250) {
            std::cerr << "static_renderer_probe: rendered pixel does not match expected solid red" << std::endl;
            return false;
        }
    } catch (const std::exception& e) {
        std::cerr << "static_renderer_probe failed: " << e.what() << std::endl;
        return false;
    }

    return true;
    });
    if (!ok) return false;
    std::cout << "static_renderer_probe: passed, zero validation warnings" << std::endl;
    return true;
}

// From TEST.md's QA pass (2026-07-18): confirms StaticRenderer's own error path
// (as opposed to stb_image's) throws a catchable, clear exception for a missing
// file, rather than crashing or leaving partially-constructed state.
bool testMissingImageThrows() {

    const bool ok = wallpaperx::testing::gradedRun(
        "static_renderer_probe (missing image)", [&](wallpaperx::testing::ValidationGrade& grade) {
    try {
        wallpaperx::Device device(/*enableValidation=*/true);
        grade.watch(device);
        // renderPass() is valid immediately after construction (built in the
        // constructor, independent of create()) -- StaticRenderer fails at
        // stbi_load before it ever consumes the render pass, so no actual
        // offscreen target needs to exist for this test.
        wallpaperx::OffscreenTargetManager targets(device);

        bool threw = false;
        try {
            wallpaperx::StaticRenderer renderer(device, targets.renderPass(),
                                                 "/nonexistent/path/does-not-exist.png", WALLPAPERX_SHADER_DIR);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        if (!threw) {
            std::cerr << "static_renderer_probe (missing image): constructing with a nonexistent path "
                         "should have thrown"
                      << std::endl;
            return false;
        }
    } catch (const std::exception& e) {
        std::cerr << "static_renderer_probe (missing image) failed: " << e.what() << std::endl;
        return false;
    }

    return true;
    });
    if (!ok) return false;
    std::cout << "static_renderer_probe (missing image): passed" << std::endl;
    return true;
}

}

int main() {
    bool ok = true;
    ok &= testRenderValidImage();
    ok &= testMissingImageThrows();
    return ok ? 0 : 1;
}
