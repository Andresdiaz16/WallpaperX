#pragma once

#include <vulkan/vulkan.h>

namespace wallpaperx {

class IRenderer {
public:
    virtual ~IRenderer() = default;
    virtual void render(VkCommandBuffer cmd, VkFramebuffer framebuffer, uint32_t width, uint32_t height) = 0;
};

}
