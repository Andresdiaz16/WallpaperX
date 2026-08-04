#include "vulkan/device.h"
#include "vulkan/validation_probe.h"
#include "vulkan/offscreen.h"
#include "renderers/static_renderer.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

std::string writeTestImage() {
    std::filesystem::create_directories(WALLPAPERX_TEST_IMAGE_DIR);
    const std::string path = std::string(WALLPAPERX_TEST_IMAGE_DIR) + "/test_red.png";

    const int w = 8;
    const int h = 8;
    std::vector<unsigned char> pixels(static_cast<size_t>(w) * h * 4);
    for (int i = 0; i < w * h; ++i) {
        pixels[i * 4 + 0] = 255;
        pixels[i * 4 + 1] = 0;
        pixels[i * 4 + 2] = 0;
        pixels[i * 4 + 3] = 255;
    }

    if (!stbi_write_png(path.c_str(), w, h, 4, pixels.data(), w * 4)) {
        throw std::runtime_error("static_renderer_probe: failed to write test image");
    }
    return path;
}

bool testRenderValidImage() {
    wallpaperx::resetValidationWarningCount();

    try {
        const std::string imagePath = writeTestImage();

        wallpaperx::Device device(/*enableValidation=*/true);
        wallpaperx::OffscreenTargetManager targets(device);
        const auto& target = targets.create(0, 64, 64);

        wallpaperx::StaticRenderer renderer(device, targets.renderPass(), imagePath, WALLPAPERX_SHADER_DIR);

        targets.render(renderer, target);

        // Readback transition, issued outside OffscreenTargetManager::render(). Any
        // such out-of-band transition has to be reported back to the target, or its
        // tracked layout goes stale and a later export declares a layout the image has
        // since left -- silently, because validation skips the layout check on an
        // ownership release.
        device.immediateSubmit([&](VkCommandBuffer cmd) {
            VkImageMemoryBarrier toTransferSrc{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            toTransferSrc.oldLayout = wallpaperx::kRenderedLayout;
            toTransferSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            toTransferSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toTransferSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toTransferSrc.image = target.image.image;
            toTransferSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            toTransferSrc.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            toTransferSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                  0, 0, nullptr, 0, nullptr, 1, &toTransferSrc);
        });
        target.notifyLayout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

        wallpaperx::AllocatedBuffer readback = device.memory().createBuffer(
            static_cast<VkDeviceSize>(target.width) * target.height * 4,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT, wallpaperx::HostAccess::Read, "readback");

        device.immediateSubmit([&](VkCommandBuffer copyCmd) {
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {target.width, target.height, 1};
            vkCmdCopyImageToBuffer(copyCmd, target.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    readback.buffer, 1, &region);
        });

        // immediateSubmit has waited on the copy's fence; make the device writes
        // visible to the host (no-op on coherent memory) before reading them.
        device.memory().invalidateForHostRead(readback);

        const unsigned char* pixels = static_cast<const unsigned char*>(readback.mapped);
        const unsigned char b = pixels[0];
        const unsigned char g = pixels[1];
        const unsigned char r = pixels[2];
        const unsigned char a = pixels[3];

        device.memory().destroyBuffer(readback);

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

    const int warnings = wallpaperx::validationWarningCount();
    if (warnings > 0) {
        std::cerr << "static_renderer_probe: " << warnings << " Vulkan validation warning(s)" << std::endl;
        return false;
    }

    std::cout << "static_renderer_probe: passed, zero validation warnings" << std::endl;
    return true;
}

// From TEST.md's QA pass (2026-07-18): confirms StaticRenderer's own error path
// (as opposed to stb_image's) throws a catchable, clear exception for a missing
// file, rather than crashing or leaving partially-constructed state.
bool testMissingImageThrows() {
    wallpaperx::resetValidationWarningCount();

    try {
        wallpaperx::Device device(/*enableValidation=*/true);
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

    if (wallpaperx::validationWarningCount() > 0) {
        std::cerr << "static_renderer_probe (missing image): validation warnings" << std::endl;
        return false;
    }

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
