#pragma once

#include "memory.h"

#include <cstdint>
#include <unordered_map>

namespace wallpaperx {

class Device;

struct OffscreenTarget {
    AllocatedImage image;
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
};

class OffscreenTargetManager {
public:
    static constexpr VkFormat kFormat = VK_FORMAT_B8G8R8A8_UNORM;

    explicit OffscreenTargetManager(Device& device);
    ~OffscreenTargetManager();

    OffscreenTargetManager(const OffscreenTargetManager&) = delete;
    OffscreenTargetManager& operator=(const OffscreenTargetManager&) = delete;

    const OffscreenTarget& create(int monitorId, uint32_t width, uint32_t height);
    void destroy(int monitorId);
    bool has(int monitorId) const;
    const OffscreenTarget& get(int monitorId) const;

    VkRenderPass renderPass() const { return m_renderPass; }

private:
    void createRenderPass();
    void destroyTarget(OffscreenTarget& target);

    Device& m_device;
    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    std::unordered_map<int, OffscreenTarget> m_targets;
};

}
