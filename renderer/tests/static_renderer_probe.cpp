#include "vulkan/device.h"
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

}

int main() {
    wallpaperx::resetValidationWarningCount();

    try {
        const std::string imagePath = writeTestImage();

        wallpaperx::Device device(/*enableValidation=*/true);
        wallpaperx::OffscreenTargetManager targets(device);
        const auto& target = targets.create(0, 64, 64);

        wallpaperx::StaticRenderer renderer(device, targets.renderPass(), imagePath, WALLPAPERX_SHADER_DIR);

        device.immediateSubmit([&](VkCommandBuffer cmd) {
            renderer.render(cmd, target.framebuffer, target.width, target.height);

            VkImageMemoryBarrier toTransferSrc{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            toTransferSrc.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
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

        wallpaperx::AllocatedBuffer readback = device.memory().createBuffer(
            static_cast<VkDeviceSize>(target.width) * target.height * 4,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT, /*hostVisible=*/true, "readback");

        device.immediateSubmit([&](VkCommandBuffer copyCmd) {
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {target.width, target.height, 1};
            vkCmdCopyImageToBuffer(copyCmd, target.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    readback.buffer, 1, &region);
        });

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
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "static_renderer_probe failed: " << e.what() << std::endl;
        return 1;
    }

    const int warnings = wallpaperx::validationWarningCount();
    if (warnings > 0) {
        std::cerr << "static_renderer_probe: " << warnings << " Vulkan validation warning(s)" << std::endl;
        return 1;
    }

    std::cout << "static_renderer_probe: passed, zero validation warnings" << std::endl;
    return 0;
}
