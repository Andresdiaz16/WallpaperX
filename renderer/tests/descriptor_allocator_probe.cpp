#include "vulkan/descriptor_heap.h"
#include "vulkan/device.h"
#include "support/validation_grade.h"

#include <string>

#include "vulkan/memory.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <vector>

namespace {

// "heap" is not a recognized WALLPAPERX_FORCE_TIER value (only "buffer"/"classic"
// are -- see tier_selection.cpp's parseTierOverride), so it has no real override
// effect; Heap tier is already top priority and gets selected automatically
// whenever the hardware/driver supports it. Given that, a non-Heap result here
// always means genuine lack of hardware support, never a forced override falling
// back -- so this must fail loudly everywhere except the one place that's
// actually expected to lack it: the Lavapipe ctest pass, which sets
// WALLPAPERX_ALLOW_NO_HEAP_TIER (see renderer/CMakeLists.txt). Returns nullopt to
// mean "proceed with the real test body"; a concrete bool means "return this
// immediately without running the body".
std::optional<bool> heapTierGuard(wallpaperx::Device& device, const char* label) {
    if (device.descriptorAllocator().tier() == wallpaperx::DescriptorTier::Heap) {
        return std::nullopt;
    }
    const char* allowNoHeapTier = std::getenv("WALLPAPERX_ALLOW_NO_HEAP_TIER");
    if (allowNoHeapTier && std::strcmp(allowNoHeapTier, "1") == 0) {
        std::cout << "descriptor_allocator_probe (" << label << "): skipped, hardware/driver has no Heap tier support"
                  << std::endl;
        return true;
    }
    std::cerr << "descriptor_allocator_probe (" << label << "): expected Heap tier but resolved to "
              << wallpaperx::toString(device.descriptorAllocator().tier())
              << " -- set WALLPAPERX_ALLOW_NO_HEAP_TIER=1 only when this is expected (e.g. running under Lavapipe)"
              << std::endl;
    return false;
}

// RAII test resource: creates an image + view + sampler (registered with the heap
// allocator when that tier is active) and cleans them all up on destruction, so no
// test exit path -- including the exception/catch path, which the old manual
// destroyTestResource() calls did NOT cover -- can leak a GPU allocation (a leak
// would trip VMA's fatal teardown assert). Move-only: it owns raw Vulkan handles.
class TestResource {
public:
    explicit TestResource(wallpaperx::Device& device) : m_device(&device) {
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

        m_image = device.memory().createImage(imageInfo, "descriptor-allocator-probe-image");

        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = m_image.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device.device(), &viewInfo, nullptr, &m_view) != VK_SUCCESS) {
            device.memory().destroyImage(m_image);
            throw std::runtime_error("descriptor_allocator_probe: failed to create image view");
        }
        if (auto* heap = dynamic_cast<wallpaperx::DescriptorHeapAllocator*>(&device.descriptorAllocator())) {
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
            throw std::runtime_error("descriptor_allocator_probe: failed to create sampler");
        }
        if (auto* heap = dynamic_cast<wallpaperx::DescriptorHeapAllocator*>(&device.descriptorAllocator())) {
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
        if (auto* heap = dynamic_cast<wallpaperx::DescriptorHeapAllocator*>(&m_device->descriptorAllocator())) {
            heap->unregisterImageView(m_view);
            heap->unregisterSampler(m_sampler);
        }
        if (m_sampler) vkDestroySampler(m_device->device(), m_sampler, nullptr);
        if (m_view) vkDestroyImageView(m_device->device(), m_view, nullptr);
        m_device->memory().destroyImage(m_image);
        m_device = nullptr;
    }

    wallpaperx::Device* m_device = nullptr;
    wallpaperx::AllocatedImage m_image;
    VkImageView m_view = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
};

bool testTierRejectsDoubleAllocate(const char* forceTier) {
    setenv("WALLPAPERX_FORCE_TIER", forceTier, 1);

    const bool ok = wallpaperx::testing::gradedRun(
        std::string("descriptor_allocator_probe (double-allocate, ") + forceTier + ")",
        [&](wallpaperx::testing::ValidationGrade& grade) {
    try {
        wallpaperx::Device device(/*enableValidation=*/true);
        grade.watch(device);
        TestResource a(device);
        TestResource b(device);

        wallpaperx::DescriptorHandle handleA = device.descriptorAllocator().allocate(a.view(), a.sampler());

        bool threw = false;
        try {
            device.descriptorAllocator().allocate(b.view(), b.sampler());
        } catch (const std::runtime_error&) {
            threw = true;
        }
        if (!threw) {
            std::cerr << "descriptor_allocator_probe (" << forceTier
                      << "): second allocate() should have thrown but silently succeeded" << std::endl;
            return false;
        }

        device.descriptorAllocator().free(handleA);
        wallpaperx::DescriptorHandle handleB = device.descriptorAllocator().allocate(b.view(), b.sampler());
        if (!handleB.valid()) {
            std::cerr << "descriptor_allocator_probe (" << forceTier
                      << "): allocate() after free() should succeed" << std::endl;
            return false;
        }

        device.descriptorAllocator().free(handleB);

        bool doubleFreeThrew = false;
        try {
            device.descriptorAllocator().free(handleB);  // double-free the same handle
        } catch (const std::runtime_error&) {
            doubleFreeThrew = true;
        }
        if (!doubleFreeThrew) {
            std::cerr << "descriptor_allocator_probe (" << forceTier << "): second free() should have thrown"
                      << std::endl;
            return false;
        }

    } catch (const std::exception& e) {
        std::cerr << "descriptor_allocator_probe (" << forceTier << ") failed: " << e.what() << std::endl;
        return false;
    }

    return true;
    });
    if (!ok) return false;
    std::cout << "descriptor_allocator_probe (" << forceTier << "): passed" << std::endl;
    return true;
}

bool testHeapTierUniformSlotsDoNotCollideWithImageSlots() {
    // No override needed -- Heap is already top priority and auto-selected
    // whenever supported (see heapTierGuard's comment above).
    unsetenv("WALLPAPERX_FORCE_TIER");

    const bool graded = wallpaperx::testing::gradedRun(
        "descriptor_allocator_probe (heap uniform)", [&](wallpaperx::testing::ValidationGrade& grade) {
    bool ok = true;
    try {
        wallpaperx::Device device(/*enableValidation=*/true);
        grade.watch(device);
        if (auto result = heapTierGuard(device, "heap uniform")) {
            return *result;
        }
        TestResource image1(device);

        wallpaperx::AllocatedBuffer uniformBuffer = device.memory().createBuffer(
            256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            wallpaperx::HostAccess::Write, "descriptor-allocator-probe-uniform");

        wallpaperx::DescriptorHandle imageHandle = device.descriptorAllocator().allocate(image1.view(), image1.sampler());
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
        // Reusing image1's SAME view+sampler here is deliberate: free() no longer
        // unregisters them, so a register-once / reuse-across-cycles caller works.
        // This is the regression guard for the old bug where free() dropped the
        // registration and this allocate() would have thrown "never registered".
        // allocateUniform() has no registration step, so reusing uniformBuffer is fine.
        wallpaperx::DescriptorHandle imageHandle2 = device.descriptorAllocator().allocate(image1.view(), image1.sampler());
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
    } catch (const std::exception& e) {
        std::cerr << "descriptor_allocator_probe (heap uniform) failed: " << e.what() << std::endl;
        return false;
    }

    return true;
    });
    if (!graded) return false;
    std::cout << "descriptor_allocator_probe (heap uniform): passed" << std::endl;
    return true;
}

bool testHeapTierRejectsDoubleFree() {
    unsetenv("WALLPAPERX_FORCE_TIER");

    const bool ok = wallpaperx::testing::gradedRun(
        "descriptor_allocator_probe (heap double-free)", [&](wallpaperx::testing::ValidationGrade& grade) {
    try {
        wallpaperx::Device device(/*enableValidation=*/true);
        grade.watch(device);
        if (auto result = heapTierGuard(device, "heap double-free")) {
            return *result;
        }

        TestResource a(device);
        TestResource b(device);
        TestResource c(device);

        wallpaperx::DescriptorHandle handleA = device.descriptorAllocator().allocate(a.view(), a.sampler());
        device.descriptorAllocator().free(handleA);

        bool threw = false;
        try {
            device.descriptorAllocator().free(handleA);  // double-free the same handle
        } catch (const std::runtime_error&) {
            threw = true;
        }
        if (!threw) {
            std::cerr << "descriptor_allocator_probe (heap double-free): second free() should have thrown"
                      << std::endl;
            return false;
        }

        // The double-free must not have corrupted the free-list into handing the
        // same slot to two independent, simultaneously-live allocations.
        wallpaperx::DescriptorHandle handleB = device.descriptorAllocator().allocate(b.view(), b.sampler());
        wallpaperx::DescriptorHandle handleC = device.descriptorAllocator().allocate(c.view(), c.sampler());
        const bool aliased = handleB.index == handleC.index;

        device.descriptorAllocator().free(handleB);
        device.descriptorAllocator().free(handleC);

        if (aliased) {
            std::cerr << "descriptor_allocator_probe (heap double-free): two live resources got the same slot"
                      << std::endl;
            return false;
        }
    } catch (const std::exception& e) {
        std::cerr << "descriptor_allocator_probe (heap double-free) failed: " << e.what() << std::endl;
        return false;
    }

    return true;
    });
    if (!ok) return false;
    std::cout << "descriptor_allocator_probe (heap double-free): passed" << std::endl;
    return true;
}

bool testHeapTierExhaustion() {
    unsetenv("WALLPAPERX_FORCE_TIER");

    const bool ok = wallpaperx::testing::gradedRun(
        "descriptor_allocator_probe (heap exhaustion)", [&](wallpaperx::testing::ValidationGrade& grade) {
    try {
        wallpaperx::Device device(/*enableValidation=*/true);
        grade.watch(device);
        if (auto result = heapTierGuard(device, "heap exhaustion")) {
            return *result;
        }

        // resources ends up with one MORE entry than handles: the
        // (kMaxDescriptorSlots + 1)th resource is created to attempt (and fail)
        // one allocation past capacity, so it never gets a handle. Both vectors
        // are driven off their own .size() below -- never zip them together by
        // index.
        std::vector<TestResource> resources;
        std::vector<wallpaperx::DescriptorHandle> handles;
        resources.reserve(wallpaperx::kMaxDescriptorSlots + 1);
        handles.reserve(wallpaperx::kMaxDescriptorSlots);

        for (uint32_t i = 0; i < wallpaperx::kMaxDescriptorSlots; ++i) {
            resources.push_back(TestResource(device));
            handles.push_back(device.descriptorAllocator().allocate(resources.back().view(), resources.back().sampler()));
        }

        // The (kMaxDescriptorSlots + 1)th allocation must fail cleanly, not
        // corrupt state or succeed silently past the documented capacity.
        resources.push_back(TestResource(device));
        bool threw = false;
        try {
            device.descriptorAllocator().allocate(resources.back().view(), resources.back().sampler());
        } catch (const std::runtime_error&) {
            threw = true;
        }

        for (size_t i = 0; i < handles.size(); ++i) {
            device.descriptorAllocator().free(handles[i]);
        }
        // resources clean themselves up (RAII) when the vector goes out of scope.

        if (!threw) {
            std::cerr << "descriptor_allocator_probe (heap exhaustion): allocate() past kMaxDescriptorSlots "
                         "should have thrown"
                      << std::endl;
            return false;
        }
    } catch (const std::exception& e) {
        std::cerr << "descriptor_allocator_probe (heap exhaustion) failed: " << e.what() << std::endl;
        return false;
    }

    return true;
    });
    if (!ok) return false;
    std::cout << "descriptor_allocator_probe (heap exhaustion): passed" << std::endl;
    return true;
}

bool testHeapTierAllowsMultipleAllocate() {
    unsetenv("WALLPAPERX_FORCE_TIER");

    const bool ok = wallpaperx::testing::gradedRun(
        "descriptor_allocator_probe (heap multi-slot)", [&](wallpaperx::testing::ValidationGrade& grade) {
    try {
        wallpaperx::Device device(/*enableValidation=*/true);
        grade.watch(device);
        if (auto result = heapTierGuard(device, "heap multi-slot")) {
            return *result;
        }
        TestResource a(device);
        TestResource b(device);

        wallpaperx::DescriptorHandle handleA = device.descriptorAllocator().allocate(a.view(), a.sampler());
        wallpaperx::DescriptorHandle handleB = device.descriptorAllocator().allocate(b.view(), b.sampler());

        if (!handleA.valid() || !handleB.valid() || handleA.index == handleB.index) {
            std::cerr << "descriptor_allocator_probe (heap): expected two distinct valid slots" << std::endl;
            return false;
        }

        device.descriptorAllocator().free(handleA);
        device.descriptorAllocator().free(handleB);
    } catch (const std::exception& e) {
        std::cerr << "descriptor_allocator_probe (heap multi-slot) failed: " << e.what() << std::endl;
        return false;
    }

    return true;
    });
    if (!ok) return false;
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
    ok &= testHeapTierRejectsDoubleFree();
    ok &= testHeapTierExhaustion();
    return ok ? 0 : 1;
}
