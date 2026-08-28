// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "memory.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace wallpaperx {

class IRenderer;

class Device;

// The layout the offscreen render pass leaves a target in (its finalLayout). Shared
// so the render pass and the export path's barrier cannot drift apart.
inline constexpr VkImageLayout kRenderedLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

struct OffscreenTarget {
    // Identifies this image to a consumer across process boundaries, so one that already
    // imported this buffer can recognise it instead of re-importing every frame. Unique for
    // the life of the process rather than a slot index: a ring rebuilt at a new size would
    // otherwise reuse indices for different images.
    uint64_t bufferId = 0;

    AllocatedImage image;
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;

    // The layout the export path believes this image is in: UNDEFINED from creation,
    // whatever a renderer left it in after a submitted render, GENERAL once released
    // to an external consumer. exportFrame()'s release barrier reads it as oldLayout,
    // so tracking it is what keeps that barrier from declaring a layout the image was
    // never in -- a target can legitimately be exported before it is ever rendered.
    //
    // Mutable because it tracks GPU-side state of the image, not a logical property of
    // the target: exportFrame() takes a const reference and callers export repeatedly.
    //
    // NOT thread-safe, and a const OffscreenTarget& is therefore NOT safe to share
    // across threads: both OffscreenTargetManager::render() and exportFrame() write
    // this through a const reference. Callers that render and export from different
    // threads (Step 6 task 2's DBus handlers) must serialise both under the same lock
    // -- guarding only the export side leaves the write unsynchronised.
    mutable VkImageLayout exportLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    // The queue family this image's ownership was released to by exportFrame(), or
    // VK_QUEUE_FAMILY_IGNORED when the graphics queue still owns it.
    //
    // Separate from exportLayout on purpose. Ownership is not derivable from the
    // layout: GENERAL is both what a release leaves behind *and* a layout a renderer
    // may legitimately end in (a compute write), so using the layout as the
    // "already released" sentinel would silently skip the release for such a renderer.
    // It also has to be paired -- a release with no matching acquire before the next
    // render is where a driver's compressed-modifier metadata resolve goes wrong.
    mutable uint32_t externalOwner = VK_QUEUE_FAMILY_IGNORED;

    // What the release named as oldLayout, so the acquire can name the same. exportLayout
    // is no use for that -- the release overwrites it to GENERAL.
    mutable VkImageLayout releasedFromLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    // Records that the image is now in `layout`. Anything that transitions the image
    // outside OffscreenTargetManager::render() / exportFrame() -- a readback barrier,
    // for instance -- must call this, or the export path's barrier will declare a
    // layout the image has since left.
    void notifyLayout(VkImageLayout layout) const { exportLayout = layout; }
};

class OffscreenTargetManager {
public:
    static constexpr VkFormat kFormat = VK_FORMAT_B8G8R8A8_UNORM;

    // The usage every offscreen target is created with. Public because a DRM format
    // modifier is only valid for a specific usage: anything querying which modifiers
    // could describe one of these images has to ask about exactly this set, and a
    // second copy of the mask in another file silently stops matching the first the
    // moment a renderer needs a new bit.
    //
    // TRANSFER_SRC so the rendered frame can be copied out (readback for tests, and the
    // natural expectation for a frame we intend to export/hand off).
    static constexpr VkImageUsageFlags kUsage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    explicit OffscreenTargetManager(Device& device);
    ~OffscreenTargetManager();

    OffscreenTargetManager(const OffscreenTargetManager&) = delete;
    OffscreenTargetManager& operator=(const OffscreenTargetManager&) = delete;

    // `depth` images for this monitor, rotated by advance(). One is right for a wallpaper
    // drawn once; a moving one needs enough that the producer can draw while the consumer
    // still holds the frame it is showing. Replaces any existing entry, waiting for idle.
    const OffscreenTarget& create(int monitorId, uint32_t width, uint32_t height, uint32_t depth = 1);
    void destroy(int monitorId);
    bool has(int monitorId) const;

    // The slot currently being drawn into. Stable until destroy(): the slots are allocated
    // once at create() and never grow, so a reference outlives any number of advance() calls.
    const OffscreenTarget& get(int monitorId) const;

    // Rotates to the next slot and returns it. With depth 1 this returns the same target,
    // which is what makes the draw-once path indifferent to being called or not.
    const OffscreenTarget& advance(int monitorId);

    uint32_t depth(int monitorId) const;

    // Renders `renderer` into `target` and submits it.
    //
    // Owning the submit is what makes the layout bookkeeping correct: the target's
    // tracked layout is only committed once the submit has actually succeeded. Doing
    // it at record time would leave a target claiming a transition that never ran if
    // the submit throws (device lost, OOM) -- and the next export would then declare
    // that layout to the driver on an image still in its old one.
    void render(IRenderer& renderer, const OffscreenTarget& target);

    VkRenderPass renderPass() const { return m_renderPass; }

private:
    void createRenderPass();
    void destroyTarget(OffscreenTarget& target);
    // One device-idle wait for the whole batch, then each is freed.
    void destroyTargets(const std::vector<OffscreenTarget*>& images);
    // Frees one, assuming the caller has already waited. Never call directly.
    void dropTarget(OffscreenTarget& target);
    OffscreenTarget makeTarget(uint32_t width, uint32_t height);

    // One monitor's images and which of them is current. A vector sized once at create() and
    // never grown, so the references get() hands out stay valid.
    //
    // NOT named `slots`: Qt defines that as an empty macro, and this header is included from
    // the plugin's Qt translation units, where the member would silently disappear.
    struct Ring {
        std::vector<OffscreenTarget> images;
        size_t current = 0;
    };

    Device& m_device;
    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    std::unordered_map<int, Ring> m_targets;

    // Never reset, and starts at 1 so 0 stays available as "no buffer".
    uint64_t m_nextBufferId = 1;
};

}
