// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Scaffolding shared by the two probes that drive IDescriptorAllocator directly:
// descriptor_allocator_probe (ownership and capacity) and descriptor_inflight_probe
// (reuse while the GPU may still be reading). Both need the same sampled-image resource
// and the same rule for what a missing Heap tier means, and a second copy of either is
// how the two stop agreeing -- the guard in particular encodes a policy decision (a
// non-Heap result is a hardware fact only where the environment says so, and a
// regression everywhere else) that must not be re-derived per probe.
//
// Test-facing only: nothing in the renderer tree includes this.

#include "vulkan/descriptor_heap.h"
#include "vulkan/device.h"
#include "vulkan/memory.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <stdexcept>

namespace wallpaperx::testing {

// "heap" is not a recognized WALLPAPERX_FORCE_TIER value (only "buffer"/"classic" are --
// see tier_selection.cpp's parseTierOverride), so it has no real override effect; Heap
// tier is already top priority and gets selected automatically whenever the
// hardware/driver supports it. Given that, a non-Heap result here always means genuine
// lack of hardware support, never a forced override falling back -- so this must fail
// loudly everywhere except the one place that's actually expected to lack it: the
// Lavapipe ctest pass, which sets WALLPAPERX_ALLOW_NO_HEAP_TIER (see
// renderer/CMakeLists.txt). Returns nullopt to mean "proceed with the real test body"; a
// concrete bool means "return this immediately without running the body".
inline std::optional<bool> heapTierGuard(Device& device, const char* probe, const char* label) {
    if (device.descriptorAllocator().tier() == DescriptorTier::Heap) {
        return std::nullopt;
    }
    const char* allowNoHeapTier = std::getenv("WALLPAPERX_ALLOW_NO_HEAP_TIER");
    if (allowNoHeapTier && std::strcmp(allowNoHeapTier, "1") == 0) {
        std::cout << probe << " (" << label << "): skipped, hardware/driver has no Heap tier support" << std::endl;
        return true;
    }
    std::cerr << probe << " (" << label << "): expected Heap tier but resolved to "
              << toString(device.descriptorAllocator().tier())
              << " -- set WALLPAPERX_ALLOW_NO_HEAP_TIER=1 only when this is expected (e.g. running under Lavapipe)"
              << std::endl;
    return false;
}

// RAII test resource: creates an image + view + sampler (registered with the heap
// allocator when that tier is active) and cleans them all up on destruction, so no
// test exit path -- including the exception/catch path, which manual teardown calls did
// NOT cover -- can leak a GPU allocation (a leak would trip VMA's fatal teardown assert).
// Move-only: it owns raw Vulkan handles.
class TestResource {
public:
    explicit TestResource(Device& device) : m_device(&device) {
        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.extent = {4, 4, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        m_image = device.memory().createImage(imageInfo, "descriptor-probe-image");

        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = m_image.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device.device(), &viewInfo, nullptr, &m_view) != VK_SUCCESS) {
            device.memory().destroyImage(m_image);
            throw std::runtime_error("descriptor probe: failed to create image view");
        }
        if (auto* heap = dynamic_cast<DescriptorHeapAllocator*>(&device.descriptorAllocator())) {
            heap->registerImageView(m_view, viewInfo);
        }

        VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        if (vkCreateSampler(device.device(), &samplerInfo, nullptr, &m_sampler) != VK_SUCCESS) {
            cleanup();  // m_view is already created/registered -- release it before throwing
            throw std::runtime_error("descriptor probe: failed to create sampler");
        }
        if (auto* heap = dynamic_cast<DescriptorHeapAllocator*>(&device.descriptorAllocator())) {
            heap->registerSampler(m_sampler, samplerInfo);
        }
    }

    ~TestResource() { cleanup(); }

    TestResource(TestResource&& other) noexcept
        : m_device(other.m_device), m_image(other.m_image), m_view(other.m_view), m_sampler(other.m_sampler) {
        other.m_device = nullptr;
        other.m_image = {};
        other.m_view = VK_NULL_HANDLE;
        other.m_sampler = VK_NULL_HANDLE;
    }
    TestResource& operator=(TestResource&& other) noexcept {
        if (this != &other) {
            cleanup();
            m_device = other.m_device;
            m_image = other.m_image;
            m_view = other.m_view;
            m_sampler = other.m_sampler;
            other.m_device = nullptr;
            other.m_image = {};
            other.m_view = VK_NULL_HANDLE;
            other.m_sampler = VK_NULL_HANDLE;
        }
        return *this;
    }
    TestResource(const TestResource&) = delete;
    TestResource& operator=(const TestResource&) = delete;

    VkImageView view() const { return m_view; }
    VkSampler sampler() const { return m_sampler; }

private:
    void cleanup() {
        if (!m_device) return;  // moved-from -- owns nothing
        if (auto* heap = dynamic_cast<DescriptorHeapAllocator*>(&m_device->descriptorAllocator())) {
            heap->unregisterImageView(m_view);
            heap->unregisterSampler(m_sampler);
        }
        if (m_sampler) vkDestroySampler(m_device->device(), m_sampler, nullptr);
        if (m_view) vkDestroyImageView(m_device->device(), m_view, nullptr);
        m_device->memory().destroyImage(m_image);
        m_device = nullptr;
    }

    Device* m_device = nullptr;
    AllocatedImage m_image;
    VkImageView m_view = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
};

}  // namespace wallpaperx::testing
