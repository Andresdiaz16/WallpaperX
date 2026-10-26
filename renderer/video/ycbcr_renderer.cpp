// SPDX-License-Identifier: GPL-2.0-or-later

#include "ycbcr_renderer.h"

#include "../renderers/cover_crop.h"
#include "../vulkan/format_features.h"
#include "../vulkan/gpu_fault.h"
#include "../vulkan/image_barrier.h"
#include "../vulkan/offscreen.h"
#include "../vulkan/shader_module.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <variant>

namespace wallpaperx {

namespace {

// Mirrors the push_constant block in ycbcr_draw.frag, offset for offset.
struct DrawPush {
    float matrix[3][4];
    float offset[4];
    float srcPerDst[2];
    float srcOffset[2];
    float chromaShift[2];
};
static_assert(offsetof(DrawPush, srcPerDst) == 64);
static_assert(offsetof(DrawPush, chromaShift) == 80);
static_assert(sizeof(DrawPush) == 88);

constexpr const char* kComponent = "YcbcrRenderer";
constexpr VkImageSubresourceRange kWholeImage{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
constexpr VkImageSubresourceLayers kBaseLayer{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
constexpr VkImageAspectFlags kPlaneAspects[3] = {VK_IMAGE_ASPECT_PLANE_0_BIT, VK_IMAGE_ASPECT_PLANE_1_BIT,
                                                 VK_IMAGE_ASPECT_PLANE_2_BIT};
constexpr VkImageLayout kSampledLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

constexpr VkFormatFeatureFlags2 kSampledFeatures =
    VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT;

// A decoder's planes are sampled where they are; a host picture's are uploaded first.
VkFormatFeatureFlags2 planeFeatures(DecodedFrame::Storage storage) {
    return storage == DecodedFrame::Storage::DeviceImage ? kSampledFeatures
                                                         : kSampledFeatures | VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT;
}

std::variant<ycbcr::Planes, std::string> planesFor(const Device& device, AVPixelFormat format,
                                                   DecodedFrame::Storage storage) {
    return ycbcr::planesFor(device, format, storage, planeFeatures(storage));
}

// Chroma texels per luma pixel along an axis subsampled by `shift`.
float chromaScale(uint32_t shift) { return 1.0f / static_cast<float>(1u << shift); }

}  // namespace

YcbcrRenderer::Shape YcbcrRenderer::shapeOf(const DecodedFrame& frame) {
    const bool onDevice = frame.storage() == DecodedFrame::Storage::DeviceImage;
    return Shape{frame.storage(), frame.pixelFormat(), frame.extent(), frame.visible(),
                 onDevice ? frame.imageExtent() : VkExtent2D{}};
}

bool YcbcrRenderer::readsDeviceImage(const Device& device, AVPixelFormat swFormat, VkFormat imageFormat) {
    const std::variant<ycbcr::Planes, std::string> planes =
        planesFor(device, swFormat, DecodedFrame::Storage::DeviceImage);
    const ycbcr::Planes* accepted = std::get_if<ycbcr::Planes>(&planes);
    return accepted != nullptr && ycbcr::deviceFormatFor(accepted->layout) == imageFormat;
}

YcbcrRenderer::YcbcrRenderer(Device& device, DecodedFrame&& first, const std::string& shaderDir)
    : m_device(device), m_shape(shapeOf(first)) {
    m_device.assertGpuThread("YcbcrRenderer::YcbcrRenderer");

    const bool onDevice = m_shape.storage == DecodedFrame::Storage::DeviceImage;
    const std::variant<ycbcr::Planes, std::string> planes = planesFor(m_device, m_shape.format, m_shape.storage);
    if (const auto* refusal = std::get_if<std::string>(&planes)) {
        const std::string message = std::string(kComponent) + ": " + *refusal;
        // The decoder asks readsDeviceImage() before decoding on the device, so a device image
        // refused here is the decoder's defect. A host picture refused is content this device
        // cannot show, not a fault of the device.
        if (onDevice) throw std::logic_error(message);
        throw WallpaperContentError(message);
    }
    m_planes = std::get<ycbcr::Planes>(planes);
    const ycbcr::Layout& layout = m_planes.layout;

    const VkExtent2D coded = m_shape.extent;
    const VkRect2D visible = m_shape.visible;
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(m_device.physicalDevice(), &properties);
    const uint32_t limit = properties.limits.maxImageDimension2D;
    if (coded.width == 0 || coded.height == 0 || coded.width > limit || coded.height > limit) {
        throw WallpaperContentError(std::string(kComponent) + ": a " + std::to_string(coded.width) + "x" +
                                    std::to_string(coded.height) + " picture cannot be held as an image on " +
                                    m_device.info().gpuName);
    }
    if (visible.offset.x < 0 || visible.offset.y < 0 || visible.extent.width == 0 || visible.extent.height == 0 ||
        static_cast<uint64_t>(visible.offset.x) + visible.extent.width > coded.width ||
        static_cast<uint64_t>(visible.offset.y) + visible.extent.height > coded.height) {
        throw WallpaperContentError(std::string(kComponent) +
                                    ": the stream's visible rectangle lies outside its coded picture");
    }
    m_chromaRegion = ycbcr::planeRegion(visible, layout.chromaShiftX, layout.chromaShiftY);
    // Where each plane's image starts in its plane and how large it is: a decoder's image holds
    // the whole coded picture and can hold more, an upload plane only what the visible picture uses.
    VkRect2D lumaImage = visible;
    VkRect2D chromaImage = m_chromaRegion;
    if (onDevice) {
        checkDecoderImage(first.deviceImage());
        const VkExtent2D image = m_shape.imageExtent;
        lumaImage = {{0, 0}, image};
        chromaImage = {{0, 0}, {image.width >> layout.chromaShiftX, image.height >> layout.chromaShiftY}};
    }
    m_chromaOrigin = {static_cast<float>(chromaImage.offset.x), static_cast<float>(chromaImage.offset.y)};
    const float chromaMinX = static_cast<float>(m_chromaRegion.offset.x - chromaImage.offset.x) + 0.5f;
    const float chromaMinY = static_cast<float>(m_chromaRegion.offset.y - chromaImage.offset.y) + 0.5f;
    const Constants constants{
        {static_cast<float>(visible.extent.width) - 0.5f, static_cast<float>(visible.extent.height) - 0.5f},
        {static_cast<float>(visible.offset.x - lumaImage.offset.x),
         static_cast<float>(visible.offset.y - lumaImage.offset.y)},
        {1.0f / static_cast<float>(lumaImage.extent.width), 1.0f / static_cast<float>(lumaImage.extent.height)},
        {chromaScale(layout.chromaShiftX), chromaScale(layout.chromaShiftY)},
        {chromaMinX, chromaMinY},
        {chromaMinX + static_cast<float>(m_chromaRegion.extent.width) - 1.0f,
         chromaMinY + static_cast<float>(m_chromaRegion.extent.height) - 1.0f},
        {1.0f / static_cast<float>(chromaImage.extent.width), 1.0f / static_cast<float>(chromaImage.extent.height)},
    };
    if (!(formatFeatures(m_device.physicalDevice(), OffscreenTargetManager::kFormat, VK_IMAGE_TILING_OPTIMAL) &
          VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT)) {
        throw WallpaperContentError(std::string(kComponent) + ": " + m_device.info().gpuName + " cannot draw into " +
                                    formatName(OffscreenTargetManager::kFormat));
    }

    m_bindings[0] = Binding{0, VK_COMPONENT_SWIZZLE_IDENTITY};
    m_bindings[1] = Binding{layout.cbPlane, layout.cbComponent};
    m_bindings[2] = Binding{layout.crPlane, layout.crComponent};

    // Refused before anything is created, so a stream that cannot be shown costs nothing.
    static_cast<void>(ycbcr::colourMapFor(first, layout, m_planes.luma.sampleToCode, kComponent));

    try {
        createSampling();
        createPipeline(shaderDir, constants);
        if (!onDevice) {
            createUploadPlanes();
            m_sources.push_back(makeSource(VK_NULL_HANDLE, false));
        }
        show(std::move(first));
    } catch (...) {
        // Only a host picture's upload can have reached the GPU, and a throw from its submit
        // means it did not; anything else was never submitted against.
        if (m_device.safeToFree("a video renderer's planes, views, pipeline and upload buffers, built part way")) {
            releaseObjects();
        }
        throw;
    }
}

YcbcrRenderer::~YcbcrRenderer() {
    // Every draw of the shown picture reads these, and nothing here records which was last.
    if (m_device.safeToFree("a video renderer's plane views, descriptor sets, sampler, pipeline, upload planes "
                            "and upload buffers")) {
        releaseObjects();
    }
}

void YcbcrRenderer::releaseObjects() noexcept {
    const VkDevice vk = m_device.device();
    for (Source& source : m_sources) destroySource(source);
    m_sources.clear();
    if (m_pipeline) vkDestroyPipeline(vk, m_pipeline, nullptr);
    if (m_renderPass) vkDestroyRenderPass(vk, m_renderPass, nullptr);
    if (m_pipelineLayout) vkDestroyPipelineLayout(vk, m_pipelineLayout, nullptr);
    if (m_setLayout) vkDestroyDescriptorSetLayout(vk, m_setLayout, nullptr);
    if (m_sampler) vkDestroySampler(vk, m_sampler, nullptr);
    m_pipeline = VK_NULL_HANDLE;
    m_renderPass = VK_NULL_HANDLE;
    m_pipelineLayout = VK_NULL_HANDLE;
    m_setLayout = VK_NULL_HANDLE;
    m_sampler = VK_NULL_HANDLE;
    for (UploadPlane& plane : m_uploadPlanes) m_device.memory().destroyImage(plane.image);
    m_uploadPlanes.clear();
    for (Staging& staging : m_staging) m_device.memory().destroyBuffer(staging.buffer);
    m_shownSet = VK_NULL_HANDLE;
}

void YcbcrRenderer::createSampling() {
    const VkDevice vk = m_device.device();

    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (const VkResult r = vkCreateSampler(vk, &sampler, nullptr, &m_sampler); r != VK_SUCCESS) {
        throwVkFailure("YcbcrRenderer: vkCreateSampler", r);
    }

    // Sets of this renderer's own, each written once, on every descriptor tier: the Device's
    // allocator holds one sampled image on two of its three tiers, and rewriting it waits for the
    // GPU, where a draw here samples three images and a different set every picture.
    VkDescriptorSetLayoutBinding bindings[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[i].pImmutableSamplers = &m_sampler;
    }
    VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout.bindingCount = 3;
    layout.pBindings = bindings;
    if (const VkResult r = vkCreateDescriptorSetLayout(vk, &layout, nullptr, &m_setLayout); r != VK_SUCCESS) {
        throwVkFailure("YcbcrRenderer: vkCreateDescriptorSetLayout", r);
    }
}

void YcbcrRenderer::createPipeline(const std::string& shaderDir, const Constants& constants) {
    const VkDevice vk = m_device.device();

    VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(DrawPush)};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &m_setLayout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    if (const VkResult r = vkCreatePipelineLayout(vk, &layout, nullptr, &m_pipelineLayout); r != VK_SUCCESS) {
        throwVkFailure("YcbcrRenderer: vkCreatePipelineLayout", r);
    }

    // The targets' own pass with the load discarded: the draw and, where it leaves part of the
    // target bare, a clear cover every pixel.
    m_renderPass = createOffscreenRenderPass(vk, VK_ATTACHMENT_LOAD_OP_DONT_CARE, kComponent);

    const ShaderModule vertModule(vk, shaderDir + "/ycbcr_convert.vert.spv", kComponent);
    const ShaderModule fragModule(vk, shaderDir + "/ycbcr_draw.frag.spv", kComponent);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule.get();
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule.get();
    stages[1].pName = "main";
    // ycbcr_draw.frag's constant_id 0 to 13, in the order Constants holds them.
    static_assert(sizeof(Constants) == 14 * sizeof(float));
    std::array<VkSpecializationMapEntry, 14> entries{};
    for (uint32_t i = 0; i < entries.size(); ++i) entries[i] = {i, i * uint32_t{sizeof(float)}, sizeof(float)};
    const VkSpecializationInfo specialization{static_cast<uint32_t>(entries.size()), entries.data(), sizeof(Constants),
                                              &constants};
    stages[1].pSpecializationInfo = &specialization;

    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    // Set per draw: the placed region of each target.
    VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;
    const VkDynamicState dynamic[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamic;
    VkPipelineRasterizationStateCreateInfo rasterizer{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;

    VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeline.stageCount = 2;
    pipeline.pStages = stages;
    pipeline.pVertexInputState = &vertexInput;
    pipeline.pInputAssemblyState = &inputAssembly;
    pipeline.pViewportState = &viewportState;
    pipeline.pRasterizationState = &rasterizer;
    pipeline.pMultisampleState = &multisample;
    pipeline.pColorBlendState = &blend;
    pipeline.pDynamicState = &dynamicState;
    pipeline.layout = m_pipelineLayout;
    pipeline.renderPass = m_renderPass;
    pipeline.subpass = 0;
    const VkResult result = vkCreateGraphicsPipelines(vk, VK_NULL_HANDLE, 1, &pipeline, nullptr, &m_pipeline);
    if (result != VK_SUCCESS) {
        // Unclassified, as the shader modules are: a rejection may be the bundle's fault.
        throw std::runtime_error("YcbcrRenderer: vkCreateGraphicsPipelines failed with " + vkResultName(result));
    }
}

// Each holds only the texels the visible picture uses, so filtering never reads the codec's padding.
void YcbcrRenderer::createUploadPlanes() {
    const ycbcr::Layout& layout = m_planes.layout;
    for (uint32_t i = 0; i < layout.planeCount; ++i) {
        const ycbcr::PlaneFormat& format = i == 0 ? m_planes.luma : m_planes.chroma;
        UploadPlane plane{{}, i == 0 ? m_shape.visible : m_chromaRegion, format.texelBytes};
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format.format;
        info.extent = {plane.region.extent.width, plane.region.extent.height, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        plane.image = m_device.memory().createImage(info, "video-upload-plane");
        m_uploadPlanes.push_back(plane);
    }
    VkDeviceSize bytes = 0;
    for (const UploadPlane& plane : m_uploadPlanes) {
        bytes += static_cast<VkDeviceSize>(plane.region.extent.width) * plane.region.extent.height * plane.texelBytes;
    }
    for (Staging& staging : m_staging) {
        staging.buffer = m_device.memory().createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, HostAccess::Write,
                                                        "video-upload-staging");
    }
}

YcbcrRenderer::Source YcbcrRenderer::makeSource(VkImage image, bool decoderImage) {
    const VkDevice vk = m_device.device();
    Source source{image};
    try {
        for (uint32_t i = 0; i < 3; ++i) {
            const Binding& binding = m_bindings[i];
            const VkFormat format = binding.plane == 0 ? m_planes.luma.format : m_planes.chroma.format;
            // A plane of the decoder's image is viewed in that plane's own format, which needs the
            // view restricted to sampling: the image's other usages are the decoder's.
            VkImageViewUsageCreateInfo usage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
            usage.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.pNext = decoderImage ? &usage : nullptr;
            view.image = decoderImage ? image : m_uploadPlanes[binding.plane].image.image;
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = format;
            view.components.r = binding.r;
            view.subresourceRange = kWholeImage;
            if (decoderImage) view.subresourceRange.aspectMask = kPlaneAspects[binding.plane];
            if (const VkResult r = vkCreateImageView(vk, &view, nullptr, &source.views[i]); r != VK_SUCCESS) {
                throwVkFailure("YcbcrRenderer: vkCreateImageView (plane)", r);
            }
        }

        const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = 1;
        pool.poolSizeCount = 1;
        pool.pPoolSizes = &size;
        if (const VkResult r = vkCreateDescriptorPool(vk, &pool, nullptr, &source.pool); r != VK_SUCCESS) {
            throwVkFailure("YcbcrRenderer: vkCreateDescriptorPool", r);
        }
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = source.pool;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &m_setLayout;
        if (const VkResult r = vkAllocateDescriptorSets(vk, &allocate, &source.set); r != VK_SUCCESS) {
            throwVkFailure("YcbcrRenderer: vkAllocateDescriptorSets", r);
        }
    } catch (...) {
        destroySource(source);
        throw;
    }

    // A fresh set, so no pending batch can have bound it.
    VkDescriptorImageInfo images[3]{};
    VkWriteDescriptorSet writes[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        images[i].imageView = source.views[i];
        images[i].imageLayout = kSampledLayout;
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = source.set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(vk, 3, writes, 0, nullptr);
    return source;
}

void YcbcrRenderer::destroySource(Source& source) noexcept {
    const VkDevice vk = m_device.device();
    // The pool frees the set with it.
    if (source.pool) vkDestroyDescriptorPool(vk, source.pool, nullptr);
    for (VkImageView& view : source.views) {
        if (view) vkDestroyImageView(vk, view, nullptr);
        view = VK_NULL_HANDLE;
    }
    source.pool = VK_NULL_HANDLE;
    source.set = VK_NULL_HANDLE;
}

// What readsDeviceImage() and the plane formats' features assume of the decoder's image, which the
// decoder vouches for before it decodes on the device.
void YcbcrRenderer::checkDecoderImage(const DecodedFrame::DeviceImage& image) const {
    if (image.format != ycbcr::deviceFormatFor(m_planes.layout)) {
        throw std::logic_error("YcbcrRenderer: the decoder's image format does not match its pixel format");
    }
    if ((image.usage & VK_IMAGE_USAGE_SAMPLED_BIT) == 0) {
        throw std::logic_error("YcbcrRenderer: the decoder's image cannot be sampled");
    }
    if (image.tiling != VK_IMAGE_TILING_OPTIMAL) {
        throw std::logic_error("YcbcrRenderer: the decoder's image is not optimally tiled");
    }
}

// Every source is of the held picture's pool, and a pool's images live as long as any frame of it,
// which the held picture is: so a handle found here names the image its views were made for.
const YcbcrRenderer::Source& YcbcrRenderer::sourceFor(VkImage image) {
    for (const Source& source : m_sources) {
        if (source.image == image) return source;
    }
    m_sources.reserve(m_sources.size() + 1);
    m_sources.push_back(makeSource(image, true));
    return m_sources.back();
}

// The old pool's handles may be reused once its last frame is released, so its sources go before
// any of the new pool's is cached.
const YcbcrRenderer::Source& YcbcrRenderer::startPool(VkImage image) {
    // Reserved first, so the push below cannot throw: clear() keeps the capacity.
    m_sources.reserve(std::max<size_t>(m_sources.size(), 1));
    // The draws that read the old sources were all submitted by now, and a batch's fence covers
    // every batch before it on the queue.
    if (!m_sources.empty()) m_device.wait(m_device.lastSubmission());
    const Source fresh = makeSource(image, true);
    for (Source& old : m_sources) destroySource(old);
    m_sources.clear();
    m_sources.push_back(fresh);
    return m_sources.back();
}

void YcbcrRenderer::show(DecodedFrame&& frame) {
    m_device.assertGpuThread("YcbcrRenderer::show");
    if (shapeOf(frame) != m_shape) {
        throw std::invalid_argument("YcbcrRenderer: the frame's shape differs from the one this renderer was built "
                                    "for; build a new renderer for it");
    }
    // Everything that can refuse the frame happens before anything shown changes.
    const ycbcr::ColourMap colour = ycbcr::colourMapFor(frame, m_planes.layout, m_planes.luma.sampleToCode, kComponent);
    const std::array<float, 2> siting = ycbcr::chromaSiting(frame.colour().chromaLocation, m_planes.layout);
    const VkRect2D visible = m_shape.visible;
    const std::array<float, 2> chromaShift{
        (static_cast<float>(visible.offset.x) + siting[0]) * chromaScale(m_planes.layout.chromaShiftX) -
            m_chromaOrigin[0],
        (static_cast<float>(visible.offset.y) + siting[1]) * chromaScale(m_planes.layout.chromaShiftY) -
            m_chromaOrigin[1]};
    if (m_shape.storage == DecodedFrame::Storage::DeviceImage) {
        const DecodedFrame::DeviceImage image = frame.deviceImage();
        const bool samePool = m_held && m_held->imagePool() == frame.imagePool();
        if (!samePool) checkDecoderImage(image);
        m_shownSet = samePool ? sourceFor(image.image).set : startPool(image.image).set;
        m_held = std::move(frame);
    } else {
        upload(frame);
        m_shownSet = m_sources.front().set;
    }
    m_colour = colour;
    m_chromaShift = chromaShift;
}

void YcbcrRenderer::upload(const DecodedFrame& frame) {
    const DecodedFrame::HostPlanes planes = frame.hostPlanes();
    if (planes.count < m_uploadPlanes.size()) {
        throw std::logic_error("YcbcrRenderer: a host frame has fewer planes than its pixel format");
    }
    Staging& staging = m_staging[m_nextStaging];
    // The upload from two pictures ago has almost always run by now; wait() returns at once then.
    m_device.wait(staging.lastUpload);

    // Each plane's region, rows packed tightly, plane after plane, whatever ffmpeg's stride and its sign.
    auto* out = static_cast<uint8_t*>(staging.buffer.mapped);
    VkDeviceSize offset = 0;
    for (size_t i = 0; i < m_uploadPlanes.size(); ++i) {
        const UploadPlane& plane = m_uploadPlanes[i];
        const VkExtent2D extent = plane.region.extent;
        const size_t rowBytes = static_cast<size_t>(extent.width) * plane.texelBytes;
        const ptrdiff_t stride = planes.stride[i];
        const uint8_t* first = planes.data[i] + stride * plane.region.offset.y +
                               static_cast<ptrdiff_t>(plane.region.offset.x) * plane.texelBytes;
        if (stride == static_cast<ptrdiff_t>(rowBytes)) {
            std::memcpy(out + offset, first, rowBytes * extent.height);
            offset += rowBytes * extent.height;
            continue;
        }
        for (uint32_t y = 0; y < extent.height; ++y) {
            std::memcpy(out + offset, first + stride * y, rowBytes);
            offset += rowBytes;
        }
    }
    m_device.memory().flushForHostWrite(staging.buffer);

    staging.lastUpload = m_device.submit([&](VkCommandBuffer cmd) {
        // What a plane held was read by draws submitted before this, which the source scope waits
        // out, and is discarded.
        std::array<VkImageMemoryBarrier, 3> barriers{};
        const auto count = static_cast<uint32_t>(m_uploadPlanes.size());
        for (uint32_t i = 0; i < count; ++i) {
            barriers[i] = imageLayoutBarrier(m_uploadPlanes[i].image.image, VK_IMAGE_LAYOUT_UNDEFINED,
                                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        }
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                             nullptr, 0, nullptr, count, barriers.data());
        VkDeviceSize bufferOffset = 0;
        for (const UploadPlane& plane : m_uploadPlanes) {
            const VkExtent2D extent = plane.region.extent;
            VkBufferImageCopy copy{};
            copy.bufferOffset = bufferOffset;
            copy.imageSubresource = kBaseLayer;
            copy.imageExtent = {extent.width, extent.height, 1};
            vkCmdCopyBufferToImage(cmd, staging.buffer.buffer, plane.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   1, &copy);
            bufferOffset += static_cast<VkDeviceSize>(extent.width) * extent.height * plane.texelBytes;
        }
        // Visible to every later draw: those are submitted after this batch.
        for (uint32_t i = 0; i < count; ++i) {
            barriers[i] = imageLayoutBarrier(m_uploadPlanes[i].image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                             kSampledLayout, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        }
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                             0, nullptr, count, barriers.data());
    });
    m_nextStaging = (m_nextStaging + 1) % static_cast<uint32_t>(m_staging.size());
}

void YcbcrRenderer::place(const Placement& placement) {
    if (!positionModeShowsVideo(placement.mode)) {
        throw std::invalid_argument(std::string("YcbcrRenderer: a video cannot be drawn in position mode ") +
                                    toString(placement.mode));
    }
    if (!placementAllowed(placement)) {
        throw std::invalid_argument("YcbcrRenderer: a zoom or offset outside what Free mode allows");
    }
    m_placement = placement;
}

void YcbcrRenderer::submit(Device& device, const std::function<void(VkCommandBuffer)>& recordBatch) {
    if (!m_held) {
        IRenderer::submit(device, recordBatch);
        return;
    }
    // The draw reads the image after the decoder's write of it and before its next one.
    static_cast<void>(m_held->submit(
        [&](VkCommandBuffer cmd, const DecodedFrame::DeviceImage& image) {
            m_recording = &image;
            struct Done {
                const DecodedFrame::DeviceImage*& recording;
                ~Done() { recording = nullptr; }
            } done{m_recording};
            recordBatch(cmd);
        },
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT));
}

VkImageLayout YcbcrRenderer::record(VkCommandBuffer cmd, const OffscreenTarget& target) {
    const DecodedFrame::DeviceImage* source = m_recording;
    if (m_held && source == nullptr) {
        throw std::logic_error("YcbcrRenderer: a device picture is recorded only through submit()");
    }
    // The decoder's writes made visible from wherever they were made, and the image back exactly
    // as found afterwards, ordered before whatever the decoder submits next.
    if (source != nullptr) {
        const VkImageMemoryBarrier toSampled = imageLayoutBarrier(source->image, source->layout, kSampledLayout,
                                                                  VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &toSampled);
    }

    const VkRect2D visible = m_shape.visible;
    const std::optional<Region> region =
        placedRegion(m_placement, visible.extent.width, visible.extent.height, target.width, target.height);
    const bool covers = region && region->target.x0 == 0 && region->target.y0 == 0 &&
                        region->target.x1 == static_cast<int32_t>(target.width) &&
                        region->target.y1 == static_cast<int32_t>(target.height);

    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = m_renderPass;
    begin.framebuffer = target.framebuffer;
    begin.renderArea = {{0, 0}, {target.width, target.height}};
    vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
    if (!covers) {
        VkClearAttachment clear{};
        clear.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        clear.colorAttachment = 0;
        clear.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        const VkClearRect whole{{{0, 0}, {target.width, target.height}}, 0, 1};
        vkCmdClearAttachments(cmd, 1, &clear, 1, &whole);
    }
    if (region) {
        // The exact edges, not the whole-texel crop: a cut edge then moves smoothly, as a still's does.
        const Area& from = region->exact;
        const Crop& to = region->target;
        const double perDstX = static_cast<double>(from.x1 - from.x0) / (to.x1 - to.x0);
        const double perDstY = static_cast<double>(from.y1 - from.y0) / (to.y1 - to.y0);
        DrawPush push{};
        std::memcpy(push.matrix, m_colour.matrix, sizeof push.matrix);
        std::memcpy(push.offset, m_colour.offset, sizeof push.offset);
        push.srcPerDst[0] = static_cast<float>(perDstX);
        push.srcPerDst[1] = static_cast<float>(perDstY);
        push.srcOffset[0] = static_cast<float>(from.x0 - to.x0 * perDstX);
        push.srcOffset[1] = static_cast<float>(from.y0 - to.y0 * perDstY);
        push.chromaShift[0] = m_chromaShift[0];
        push.chromaShift[1] = m_chromaShift[1];

        const VkViewport viewport{static_cast<float>(to.x0),
                                  static_cast<float>(to.y0),
                                  static_cast<float>(to.x1 - to.x0),
                                  static_cast<float>(to.y1 - to.y0),
                                  0.0f,
                                  1.0f};
        const VkRect2D scissor{{to.x0, to.y0}, {static_cast<uint32_t>(to.x1 - to.x0), static_cast<uint32_t>(to.y1 - to.y0)}};
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &m_shownSet, 0, nullptr);
        vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(DrawPush), &push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    vkCmdEndRenderPass(cmd);

    if (source != nullptr) {
        const VkImageMemoryBarrier back = imageLayoutBarrier(source->image, kSampledLayout, source->layout, 0,
                                                             VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &back);
    }
    return kRenderedLayout;
}

}
