// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Test-only helpers for producing a reference frame, getting its pixels onto the host,
// and deciding whether those pixels can serve as a reference at all.
//
// Header-only and outside the renderer library, for the same reason as
// exported_frame_map.h: nothing WallpaperX ships reads a rendered frame back into
// CPU memory. Only a test needs to.
//
// Exists because the readback was written three times across two probes with slightly
// different lifetimes, and the copy that got the exception safety right was the third
// one -- so the two that did not were leaking on exactly the failure paths a probe is
// built to provoke.

#include "vulkan/device.h"
#include "vulkan/memory.h"
#include "vulkan/offscreen.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace wallpaperx::testing {

inline constexpr uint32_t kBytesPerPixel = 4;  // B8G8R8A8_UNORM

// Copies an image already in VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL into a
// tightly-packed BGRA vector. The caller owns the transition, because the correct
// source scope for it depends on what last wrote the image and only the caller knows.
//
// The staging buffer is released by a guard rather than a trailing call. That is not
// stylistic: immediateSubmit() throws on a failed submit and the vector allocation can
// throw bad_alloc, and a leaked VMA allocation aborts inside VMA's teardown assertion
// naming an unrelated buffer -- replacing the failure the probe was reporting with a
// SIGABRT that points somewhere else. Observed exactly that way while running a control
// that fed an importer a wrong plane offset.
inline std::vector<unsigned char> readbackImage(Device& device, VkImage image, uint32_t width, uint32_t height) {
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * kBytesPerPixel;
    AllocatedBuffer staging =
        device.memory().createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, HostAccess::Read, "test-readback");

    struct Guard {
        Device& device;
        AllocatedBuffer& buffer;
        ~Guard() { device.memory().destroyBuffer(buffer); }
    } guard{device, staging};

    device.immediateSubmit([&](VkCommandBuffer cmd) {
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {width, height, 1};
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.buffer, 1, &region);
    });

    device.memory().invalidateForHostRead(staging);
    std::vector<unsigned char> pixels(static_cast<size_t>(bytes));
    std::memcpy(pixels.data(), staging.mapped, pixels.size());
    return pixels;
}

// Whether a reference frame can actually discriminate a layout mistake.
//
// Both axes, not either. A frame that varies only along x has identical rows, so a
// whole-row offset shift or a misread row stride leaves every row comparing equal; a
// frame that varies only along y cannot catch a within-row error. A uniform frame
// catches nothing at all and still compares equal to a completely wrong import.
//
// Callers assert this on their reference before trusting a comparison against it, so
// that a change to the renderer or the test image turns into a loud failure rather
// than a check that quietly stops checking.
inline bool variesInBothAxes(const std::vector<unsigned char>& pixels, uint32_t width, uint32_t height) {
    const size_t stride = static_cast<size_t>(width) * kBytesPerPixel;
    if (pixels.size() < stride * height) return false;

    // Horizontal variation is looked for in ANY row, not only the first. Checking row 0
    // alone rejects a perfectly discriminating reference whose top scanline happens to be
    // flat -- a border, a letterboxed scale, or a clear colour on the first row -- and the
    // caller then fails the build claiming the check "has lost its power" about an image
    // that would have caught a stride error fine.
    bool rowVaries = false;
    for (uint32_t y = 0; y < height && !rowVaries; ++y) {
        const unsigned char* row = pixels.data() + static_cast<size_t>(y) * stride;
        for (size_t i = kBytesPerPixel; i < stride; ++i) {
            if (row[i] != row[i % kBytesPerPixel]) {
                rowVaries = true;
                break;
            }
        }
    }
    // EVERY adjacent pair of rows must differ -- not "some row differs from row 0".
    //
    // The anchored form accepts an image whose first scanline is flat and whose other
    // 35 rows are identical to each other: row 1 differs from row 0, so it passes, while
    // rows 1..35 carry no vertical structure at all. Shifting the import down a row
    // inside that block compares equal for every one of them. Requiring each neighbour
    // pair to differ is what actually rules the shape out, and it implies the anchored
    // condition, so this is strictly stricter and never admits an image the old form
    // rejected.
    //
    // Not too strict for a legitimate reference: the probes upscale a 16x16 gradient
    // with VK_FILTER_LINEAR, so consecutive destination rows sample distinct source
    // coordinates and always differ. A reference with flat bands (a letterbox) would be
    // rejected here -- correctly, since a row shift inside a flat band is exactly what
    // this cannot detect.
    bool columnVaries = height > 1;
    for (uint32_t y = 1; y < height; ++y) {
        const unsigned char* previous = pixels.data() + static_cast<size_t>(y - 1) * stride;
        const unsigned char* current = pixels.data() + static_cast<size_t>(y) * stride;
        if (std::memcmp(previous, current, stride) == 0) {
            columnVaries = false;
            break;
        }
    }
    return rowVaries && columnVaries;
}

// Takes a rendered target back from its consumer and copies its pixels out into a
// tightly-packed BGRA buffer. The single readback path in this suite: every probe that
// reads a rendered frame back goes through here.
//
// reclaim() first, and that is the whole point of this function existing. Every completed
// render() releases queue-family ownership of the target to an external consumer, so from
// the graphics queue's point of view the image is not ours and is in no particular layout.
// Copying from it regardless is undefined however the barrier names its layouts, and
// nothing reports it: the validation layer does not track ownership, and neither driver
// available here (NVIDIA block-linear, Lavapipe LINEAR) corrupts an unowned read, so the
// pixels come back correct and the suite stays green. reclaim() submits the acquire that
// reverses the release, leaving the image ours in GENERAL.
//
// The barrier below therefore names GENERAL literally rather than reading a tracked field.
// reclaim()'s post-condition is a compile-time constant, so there is nothing to read and
// nothing that can go stale. Its source scope covers everything because what last wrote the
// image is the renderer, and IRenderer::record may have written it as a colour attachment,
// a compute store or a blit -- a scope narrower than the widest of those makes the writes
// unavailable to the copy, and again no layer says so.
//
// The target is left unpublished, which is what makes it unsafe to export until the next
// render: it is the caller's until then.
inline std::vector<unsigned char> reclaimAndReadback(Device& device, OffscreenTargetManager& targets,
                                                     const OffscreenTarget& target) {
    targets.reclaim(target);

    device.immediateSubmit([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier toTransferSrc{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toTransferSrc.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        toTransferSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toTransferSrc.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        toTransferSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toTransferSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransferSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransferSrc.image = target.image.image;
        toTransferSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &toTransferSrc);
    });

    // readbackImage() owns its staging buffer through a guard, so a failed submit here
    // reports the submit failure instead of leaking an allocation and aborting inside
    // VMA's teardown assertion naming an unrelated buffer.
    return readbackImage(device, target.image.image, target.width, target.height);
}

// Renders `renderer` into a fresh target owned by `targets` and copies its pixels out
// through Vulkan into a tightly-packed BGRA buffer.
//
// The target is created here rather than taken as a parameter, and is deliberately NOT
// the target whose exported fd gets compared against the result. Reading a target back
// reclaims it, so the target stops being published; a probe that read back the same target
// it exports would be exporting memory the graphics queue owns, which is the defect the
// export is supposed to be free of, and would grade its own epilogue instead.
//
// Comparing across two targets is sound because the renderers here are deterministic:
// same shader, same source image, same extent, nothing time- or randomness-dependent.
inline std::vector<unsigned char> renderReference(Device& device, OffscreenTargetManager& targets,
                                                  IRenderer& renderer, int monitorId, uint32_t width,
                                                  uint32_t height) {
    const OffscreenTarget& target = targets.create(monitorId, width, height);
    targets.render(renderer, target);
    return reclaimAndReadback(device, targets, target);
}

// How many rows of two tightly-packed BGRA buffers of the same geometry differ.
inline size_t countDifferingRows(const std::vector<unsigned char>& a, const std::vector<unsigned char>& b,
                                 uint32_t width, uint32_t height) {
    const size_t stride = static_cast<size_t>(width) * kBytesPerPixel;
    if (a.size() < stride * height || b.size() < stride * height) return height;
    size_t differing = 0;
    for (uint32_t y = 0; y < height; ++y) {
        if (std::memcmp(a.data() + y * stride, b.data() + y * stride, stride) != 0) ++differing;
    }
    return differing;
}

}  // namespace wallpaperx::testing
