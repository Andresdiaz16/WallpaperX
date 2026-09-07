// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "drm_formats.h"
#include "memory.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace wallpaperx {

class IRenderer;

class Device;

// The layout the offscreen render pass leaves a target in (its finalLayout). Shared so a
// renderer drawing through that pass and the pass itself cannot name different layouts.
inline constexpr VkImageLayout kRenderedLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

struct OffscreenTarget {
    // Identifies this image to a consumer across process boundaries, so one that already
    // imported this buffer can recognise it instead of re-importing every frame. Unique for
    // the life of the process rather than a slot index: a ring rebuilt at a new size would
    // otherwise reuse indices for different images.
    uint64_t bufferId = 0;

    // Which memory layout the driver picked from the list this image was offered, or
    // kDrmModifierInvalid for an opaque-fd export, which has no modifier at all. Read once at
    // creation: a consumer's later report only invalidates images whose layout it excludes.
    uint64_t drmModifier = kDrmModifierInvalid;

    AllocatedImage image;
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;

    // The queue family the published frame's ownership was released to, or
    // VK_QUEUE_FAMILY_IGNORED when the graphics queue owns the image.
    //
    // The image's layout is deliberately NOT tracked alongside it. It is only ever needed
    // to name the acquire that reverses a release, so it exists exactly while a frame is
    // published; while we own the image the render pass discards whatever is there, and a
    // second field recording it could only drift.
    //
    // Mutable because it tracks GPU-side state of the image, not a logical property of
    // the target: render() takes a const reference and callers render repeatedly.
    //
    // NOT thread-safe, and a const OffscreenTarget& is therefore NOT safe to share
    // across threads: OffscreenTargetManager::render() writes this and the field below
    // through a const reference. Callers that render and export from different threads
    // (Step 6 task 2's DBus handlers) must serialise both under the same lock.
    mutable uint32_t externalOwner = VK_QUEUE_FAMILY_IGNORED;

    // What the release named as oldLayout, so the acquire can name the same. Meaningless
    // unless published().
    mutable VkImageLayout releasedFromLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    // Whether a rendered frame is currently in the consumer's hands: what makes an export
    // meaningful, and what a reclaim requires.
    bool published() const { return externalOwner != VK_QUEUE_FAMILY_IGNORED; }

    // Records the release that ends every render: ownership is now `queueFamily`'s and the
    // paired acquire must name `fromLayout` -> GENERAL. One call rather than two
    // assignments, so the two cannot be left describing different renders.
    void notifyPublished(uint32_t queueFamily, VkImageLayout fromLayout) const {
        externalOwner = queueFamily;
        releasedFromLayout = fromLayout;
    }

    // Records the acquire that reverses it. The image is ours again, in GENERAL.
    void notifyReclaimed() const {
        externalOwner = VK_QUEUE_FAMILY_IGNORED;
        releasedFromLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    }
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

    // The slot holding the frame this monitor last published. Stable until destroy(): the
    // slots are allocated once at create() and never grow, so a reference outlives any
    // number of renders.
    const OffscreenTarget& get(int monitorId) const;

    // Rotates to the next slot and returns it. With depth 1 this returns the same target,
    // which is what makes the draw-once path indifferent to being called or not.
    const OffscreenTarget& advance(int monitorId);

    // The slot the next render should draw into: the first one past the published frame
    // whose bufferId is absent from `held`, or the published slot itself while nothing has
    // been published. Null when every slot is held, which means every image on this monitor
    // is being read and there is nowhere to draw.
    //
    // A query, not a rotation: render() is what makes a slot current, so a render that the
    // caller then refuses or abandons leaves the published frame exactly where it was.
    const OffscreenTarget* nextUnheld(int monitorId, const std::vector<uint64_t>& held) const;

    uint32_t depth(int monitorId) const;

    // Renders `renderer` into `target`, submits it, and publishes the result: one
    // command buffer holding the acquire back from the previous publish, the render,
    // and the release that hands the finished frame to an external consumer.
    //
    // Publishing here rather than at export time is what keeps the target's recorded
    // ownership and layout describing the image as it actually is however many times
    // -- including zero -- a consumer fetches it. A producer drawing every display
    // frame otherwise accumulates renders that were never released.
    //
    // Owning the submit is what makes the bookkeeping correct: the target's tracked
    // state is only committed once the submit has actually succeeded. Doing it at
    // record time would leave a target claiming a transition that never ran if the
    // submit throws (device lost, OOM), and the next acquire would then name it.
    //
    // A successful render also makes `target` its monitor's current slot, so get() and
    // "the frame that was published" cannot name different images. That is why nothing
    // else moves the cursor: a caller that chose a slot and then did not draw into it
    // would otherwise leave the ring pointing at a blank one.
    void render(IRenderer& renderer, const OffscreenTarget& target);

    // Takes a published frame back: submits the acquire that reverses render()'s release,
    // leaving the image owned by the graphics queue in VK_IMAGE_LAYOUT_GENERAL.
    //
    // Anything on this device that touches the image outside render() -- a readback copy --
    // must call this first and then transition from GENERAL. Reading an image the graphics
    // queue does not own is undefined however the barrier names its layouts, and no
    // validation layer says so: ownership transfers are not tracked.
    //
    // The target stops being published, so nothing may export it until the next render.
    // Throws if it was not published, which is a caller bug -- only a successful render
    // publishes, and only one reclaim can follow one render.
    void reclaim(const OffscreenTarget& target);

    VkRenderPass renderPass() const { return m_renderPass; }

private:
    void createRenderPass();
    // Points the owning ring's cursor at `target`. Does nothing for a target this manager
    // does not own, which no shipping caller can produce.
    void makeCurrent(const OffscreenTarget& target);
    void destroyTarget(OffscreenTarget& target);
    // One device-idle wait for the whole batch, then each is freed.
    void destroyTargets(const std::vector<OffscreenTarget*>& images);
    // Frees one, assuming the caller has already waited. Never call directly.
    void dropTarget(OffscreenTarget& target);
    OffscreenTarget makeTarget(uint32_t width, uint32_t height);

    // One monitor's images and which of them holds the published frame. A vector sized once
    // at create() and never grown, so the references get() hands out stay valid.
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
