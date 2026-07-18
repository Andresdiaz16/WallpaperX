#include "vulkan/descriptor_heap.h"
#include "vulkan/device.h"
#include "vulkan/memory.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {

struct TestResource {
    wallpaperx::AllocatedImage image;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
};

TestResource createTestResource(wallpaperx::Device& device) {
    TestResource resource;

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

    resource.image = device.memory().createImage(imageInfo, "descriptor-allocator-probe-image");

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = resource.image.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    if (vkCreateImageView(device.device(), &viewInfo, nullptr, &resource.view) != VK_SUCCESS) {
        throw std::runtime_error("descriptor_allocator_probe: failed to create image view");
    }

    if (auto* heap = dynamic_cast<wallpaperx::DescriptorHeapAllocator*>(&device.descriptorAllocator())) {
        heap->registerImageView(resource.view, viewInfo);
    }

    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;

    if (vkCreateSampler(device.device(), &samplerInfo, nullptr, &resource.sampler) != VK_SUCCESS) {
        throw std::runtime_error("descriptor_allocator_probe: failed to create sampler");
    }

    if (auto* heap = dynamic_cast<wallpaperx::DescriptorHeapAllocator*>(&device.descriptorAllocator())) {
        heap->registerSampler(resource.sampler, samplerInfo);
    }

    return resource;
}

void destroyTestResource(wallpaperx::Device& device, TestResource& resource) {
    if (auto* heap = dynamic_cast<wallpaperx::DescriptorHeapAllocator*>(&device.descriptorAllocator())) {
        heap->unregisterImageView(resource.view);
        heap->unregisterSampler(resource.sampler);
    }
    vkDestroySampler(device.device(), resource.sampler, nullptr);
    vkDestroyImageView(device.device(), resource.view, nullptr);
    device.memory().destroyImage(resource.image);
}

bool testTierRejectsDoubleAllocate(const char* forceTier) {
    setenv("WALLPAPERX_FORCE_TIER", forceTier, 1);
    wallpaperx::resetValidationWarningCount();

    try {
        wallpaperx::Device device(/*enableValidation=*/true);
        TestResource a = createTestResource(device);
        TestResource b = createTestResource(device);

        wallpaperx::DescriptorHandle handleA = device.descriptorAllocator().allocate(a.view, a.sampler);

        bool threw = false;
        try {
            device.descriptorAllocator().allocate(b.view, b.sampler);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        if (!threw) {
            std::cerr << "descriptor_allocator_probe (" << forceTier
                      << "): second allocate() should have thrown but silently succeeded" << std::endl;
            destroyTestResource(device, a);
            destroyTestResource(device, b);
            return false;
        }

        device.descriptorAllocator().free(handleA);
        wallpaperx::DescriptorHandle handleB = device.descriptorAllocator().allocate(b.view, b.sampler);
        if (!handleB.valid()) {
            std::cerr << "descriptor_allocator_probe (" << forceTier
                      << "): allocate() after free() should succeed" << std::endl;
            destroyTestResource(device, a);
            destroyTestResource(device, b);
            return false;
        }

        device.descriptorAllocator().free(handleB);
        destroyTestResource(device, a);
        destroyTestResource(device, b);
    } catch (const std::exception& e) {
        std::cerr << "descriptor_allocator_probe (" << forceTier << ") failed: " << e.what() << std::endl;
        return false;
    }

    if (wallpaperx::validationWarningCount() > 0) {
        std::cerr << "descriptor_allocator_probe (" << forceTier << "): validation warnings" << std::endl;
        return false;
    }

    std::cout << "descriptor_allocator_probe (" << forceTier << "): passed" << std::endl;
    return true;
}

bool testHeapTierUniformSlotsDoNotCollideWithImageSlots() {
    setenv("WALLPAPERX_FORCE_TIER", "heap", 1);
    wallpaperx::resetValidationWarningCount();

    bool ok = true;
    try {
        wallpaperx::Device device(/*enableValidation=*/true);
        TestResource image1 = createTestResource(device);
        TestResource image2 = createTestResource(device);

        wallpaperx::AllocatedBuffer uniformBuffer = device.memory().createBuffer(
            256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            /*hostVisible=*/true, "descriptor-allocator-probe-uniform");

        wallpaperx::DescriptorHandle imageHandle = device.descriptorAllocator().allocate(image1.view, image1.sampler);
        wallpaperx::DescriptorHandle uniformHandle =
            device.descriptorAllocator().allocateUniform(uniformBuffer.buffer, 256);

        if (!imageHandle.valid() || !uniformHandle.valid()) {
            std::cerr << "descriptor_allocator_probe (heap uniform): expected both handles valid" << std::endl;
            ok = false;
        }
        // Image indices live in [0, kMaxDescriptorSlots); uniform indices must never
        // land in that range, or free() cannot tell the two allocations apart.
        if (imageHandle.index >= wallpaperx::kMaxDescriptorSlots) {
            std::cerr << "descriptor_allocator_probe (heap uniform): image index out of expected range"
                      << std::endl;
            ok = false;
        }
        if (uniformHandle.index < wallpaperx::kMaxDescriptorSlots) {
            std::cerr << "descriptor_allocator_probe (heap uniform): uniform index collides with image range"
                      << std::endl;
            ok = false;
        }

        device.descriptorAllocator().free(imageHandle);
        device.descriptorAllocator().free(uniformHandle);

        // Re-allocating each type should reuse its own free-list, not the other's.
        // image1's registration was dropped by free(), so the second image
        // allocation uses image2 (mirrors testTierRejectsDoubleAllocate's pattern);
        // allocateUniform() has no registration step, so reusing uniformBuffer is fine.
        wallpaperx::DescriptorHandle imageHandle2 = device.descriptorAllocator().allocate(image2.view, image2.sampler);
        wallpaperx::DescriptorHandle uniformHandle2 =
            device.descriptorAllocator().allocateUniform(uniformBuffer.buffer, 256);
        if (imageHandle2.index != imageHandle.index || uniformHandle2.index != uniformHandle.index) {
            std::cerr << "descriptor_allocator_probe (heap uniform): re-allocate did not reuse freed slot"
                      << std::endl;
            ok = false;
        }

        device.descriptorAllocator().free(imageHandle2);
        device.descriptorAllocator().free(uniformHandle2);
        device.memory().destroyBuffer(uniformBuffer);
        destroyTestResource(device, image1);
        destroyTestResource(device, image2);
    } catch (const std::exception& e) {
        std::cerr << "descriptor_allocator_probe (heap uniform) failed: " << e.what() << std::endl;
        return false;
    }

    if (!ok) return false;

    if (wallpaperx::validationWarningCount() > 0) {
        std::cerr << "descriptor_allocator_probe (heap uniform): validation warnings" << std::endl;
        return false;
    }

    std::cout << "descriptor_allocator_probe (heap uniform): passed" << std::endl;
    return true;
}

bool testHeapTierAllowsMultipleAllocate() {
    setenv("WALLPAPERX_FORCE_TIER", "heap", 1);
    wallpaperx::resetValidationWarningCount();

    try {
        wallpaperx::Device device(/*enableValidation=*/true);
        TestResource a = createTestResource(device);
        TestResource b = createTestResource(device);

        wallpaperx::DescriptorHandle handleA = device.descriptorAllocator().allocate(a.view, a.sampler);
        wallpaperx::DescriptorHandle handleB = device.descriptorAllocator().allocate(b.view, b.sampler);

        if (!handleA.valid() || !handleB.valid() || handleA.index == handleB.index) {
            std::cerr << "descriptor_allocator_probe (heap): expected two distinct valid slots" << std::endl;
            destroyTestResource(device, a);
            destroyTestResource(device, b);
            return false;
        }

        device.descriptorAllocator().free(handleA);
        device.descriptorAllocator().free(handleB);
        destroyTestResource(device, a);
        destroyTestResource(device, b);
    } catch (const std::exception& e) {
        std::cerr << "descriptor_allocator_probe (heap multi-slot) failed: " << e.what() << std::endl;
        return false;
    }

    if (wallpaperx::validationWarningCount() > 0) {
        std::cerr << "descriptor_allocator_probe (heap multi-slot): validation warnings" << std::endl;
        return false;
    }

    std::cout << "descriptor_allocator_probe (heap multi-slot): passed" << std::endl;
    return true;
}

}

int main() {
    bool ok = true;
    ok &= testTierRejectsDoubleAllocate("classic");
    ok &= testTierRejectsDoubleAllocate("buffer");
    ok &= testHeapTierAllowsMultipleAllocate();
    ok &= testHeapTierUniformSlotsDoNotCollideWithImageSlots();
    return ok ? 0 : 1;
}
