#include "static_renderer.h"

#include "../vulkan/offscreen.h"

#include "../vulkan/descriptor_buffer.h"
#include "../vulkan/descriptor_classic.h"
#include "../vulkan/descriptor_heap.h"
#include "../vulkan/device.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace wallpaperx {

namespace {
struct Vertex {
    float pos[2];
    float uv[2];
};
}

StaticRenderer::StaticRenderer(Device& device, VkRenderPass renderPass, const std::string& imagePath, const std::string& shaderDir)
    : m_device(device), m_renderPass(renderPass) {
    m_heapAllocator = dynamic_cast<DescriptorHeapAllocator*>(&m_device.descriptorAllocator());

    try {
        loadAndUploadTexture(imagePath);
        createSampler();
        m_descriptorHandle = m_device.descriptorAllocator().allocate(m_textureView, m_sampler);
        createQuad();
        createPipeline(renderPass, shaderDir);
    } catch (...) {
        cleanup();
        throw;
    }
}

StaticRenderer::~StaticRenderer() {
    cleanup();
}

void StaticRenderer::cleanup() {
    VkDevice device = m_device.device();
    vkDeviceWaitIdle(device);

    if (m_pipeline) vkDestroyPipeline(device, m_pipeline, nullptr);
    if (m_pipelineLayout) vkDestroyPipelineLayout(device, m_pipelineLayout, nullptr);
    m_pipeline = VK_NULL_HANDLE;
    m_pipelineLayout = VK_NULL_HANDLE;

    // cleanup() runs from both ~StaticRenderer() (implicitly noexcept -- an
    // escaping exception here would call std::terminate(), an uncontrolled
    // process abort) and the constructor's catch block (where a new exception
    // from cleanup() would replace, and hide, the real construction failure).
    // free() can throw (e.g. on a double-free bug), so it must never be allowed
    // to escape this function.
    try {
        m_device.descriptorAllocator().free(m_descriptorHandle);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "StaticRenderer::cleanup: descriptorAllocator().free() failed: %s\n", e.what());
    }
    m_device.memory().destroyBuffer(m_vertexBuffer);

    if (m_heapAllocator) {
        m_heapAllocator->unregisterImageView(m_textureView);
        m_heapAllocator->unregisterSampler(m_sampler);
    }

    if (m_sampler) vkDestroySampler(device, m_sampler, nullptr);
    if (m_textureView) vkDestroyImageView(device, m_textureView, nullptr);
    m_sampler = VK_NULL_HANDLE;
    m_textureView = VK_NULL_HANDLE;
    m_device.memory().destroyImage(m_texture);
}

// The largest decoded image this renderer will accept, in bytes of RGBA.
//
// 256 MiB is 8192x8192, which is four times the pixel count of an 8K display -- generous
// for a wallpaper, and small enough that the staging copy plus the texture cannot
// dominate a machine. The bound is on *decoded* size because that is the cost, and the
// file gives no honest hint of it: a 995 KB PNG of 16000x16000 decodes to 1.0 GB, and
// with the staging buffer the daemon peaked at +2.0 GB RSS and was OOM-killed under a
// 1.2 GB limit. Nothing else stopped it -- the only previous ceiling was stb_image's own
// INT_MAX overflow guard at roughly 23000x23000, far past the point where the machine is
// already in trouble.
constexpr uint64_t kMaxDecodedBytes = 256ull * 1024 * 1024;

void StaticRenderer::loadAndUploadTexture(const std::string& imagePath) {
    // Checked from the header alone, before decoding, because a limit applied after
    // stbi_load has already paid the whole cost it exists to prevent. stbi_info reads
    // dimensions without allocating the pixels.
    int probeWidth = 0;
    int probeHeight = 0;
    int probeChannels = 0;
    if (!stbi_info(imagePath.c_str(), &probeWidth, &probeHeight, &probeChannels)) {
        throw std::runtime_error(
            "StaticRenderer: not a decodable image " + imagePath + ": " + stbi_failure_reason());
    }
    if (probeWidth <= 0 || probeHeight <= 0) {
        throw std::runtime_error("StaticRenderer: image reports non-positive dimensions " + imagePath);
    }
    const uint64_t decodedBytes = static_cast<uint64_t>(probeWidth) * static_cast<uint64_t>(probeHeight) * 4;
    if (decodedBytes > kMaxDecodedBytes) {
        throw std::runtime_error("StaticRenderer: " + imagePath + " decodes to " +
                                 std::to_string(decodedBytes / (1024 * 1024)) + " MiB (" +
                                 std::to_string(probeWidth) + "x" + std::to_string(probeHeight) +
                                 "), over the " + std::to_string(kMaxDecodedBytes / (1024 * 1024)) +
                                 " MiB limit");
    }

    int imageWidth = 0;
    int imageHeight = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load(imagePath.c_str(), &imageWidth, &imageHeight, &channels, STBI_rgb_alpha);
    if (!pixels) {
        throw std::runtime_error(
            "StaticRenderer: failed to load image " + imagePath + ": " + stbi_failure_reason());
    }

    const VkDeviceSize imageSize = static_cast<VkDeviceSize>(imageWidth) * imageHeight * 4;

    AllocatedBuffer staging = m_device.memory().createBuffer(
        imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, HostAccess::Write, "static-renderer-staging");
    std::memcpy(staging.mapped, pixels, static_cast<size_t>(imageSize));
    m_device.memory().flushForHostWrite(staging);
    stbi_image_free(pixels);

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent = {static_cast<uint32_t>(imageWidth), static_cast<uint32_t>(imageHeight), 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    m_texture = m_device.memory().createImage(imageInfo, "static-renderer-texture");

    m_device.immediateSubmit([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier toTransferDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toTransferDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toTransferDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransferDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransferDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransferDst.image = m_texture.image;
        toTransferDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        toTransferDst.srcAccessMask = 0;
        toTransferDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              0, 0, nullptr, 0, nullptr, 1, &toTransferDst);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {static_cast<uint32_t>(imageWidth), static_cast<uint32_t>(imageHeight), 1};

        vkCmdCopyBufferToImage(cmd, staging.buffer, m_texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        VkImageMemoryBarrier toShaderRead{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toShaderRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toShaderRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toShaderRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toShaderRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toShaderRead.image = m_texture.image;
        toShaderRead.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        toShaderRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toShaderRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                              0, 0, nullptr, 0, nullptr, 1, &toShaderRead);
    });

    m_device.memory().destroyBuffer(staging);

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = m_texture.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    if (vkCreateImageView(m_device.device(), &viewInfo, nullptr, &m_textureView) != VK_SUCCESS) {
        throw std::runtime_error("StaticRenderer: failed to create texture image view");
    }

    if (m_heapAllocator) {
        m_heapAllocator->registerImageView(m_textureView, viewInfo);
    }
}

void StaticRenderer::createSampler() {
    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.maxLod = 0.0f;

    if (vkCreateSampler(m_device.device(), &samplerInfo, nullptr, &m_sampler) != VK_SUCCESS) {
        throw std::runtime_error("StaticRenderer: failed to create sampler");
    }

    if (m_heapAllocator) {
        m_heapAllocator->registerSampler(m_sampler, samplerInfo);
    }
}

void StaticRenderer::createQuad() {
    const Vertex vertices[6] = {
        {{-1.0f, -1.0f}, {0.0f, 0.0f}},
        {{1.0f, -1.0f}, {1.0f, 0.0f}},
        {{-1.0f, 1.0f}, {0.0f, 1.0f}},
        {{1.0f, -1.0f}, {1.0f, 0.0f}},
        {{1.0f, 1.0f}, {1.0f, 1.0f}},
        {{-1.0f, 1.0f}, {0.0f, 1.0f}},
    };
    const VkDeviceSize size = sizeof(vertices);

    m_vertexBuffer = m_device.memory().createBuffer(
        size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, HostAccess::Write, "quad-vertex-buffer");
    std::memcpy(m_vertexBuffer.mapped, vertices, size);
    m_device.memory().flushForHostWrite(m_vertexBuffer);
}

std::vector<uint32_t> StaticRenderer::readSpirv(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        throw std::runtime_error("StaticRenderer: failed to open shader file " + path);
    }
    const std::streamoff rawSize = file.tellg();
    // tellg() returns -1 on a stat failure even for an open stream; casting that
    // to size_t would request a ~16 EB allocation. A zero-byte file would build a
    // 0-word module (codeSize == 0), which is invalid. Reject both, plus non-word
    // sizes, before allocating.
    if (rawSize <= 0 || static_cast<size_t>(rawSize) % sizeof(uint32_t) != 0) {
        throw std::runtime_error("StaticRenderer: shader file is not a valid SPIR-V module: " + path);
    }
    const size_t size = static_cast<size_t>(rawSize);
    std::vector<uint32_t> buffer(size / sizeof(uint32_t));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(size));
    return buffer;
}

VkShaderModule StaticRenderer::createShaderModule(VkDevice device, const std::vector<uint32_t>& spirv) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = spirv.size() * sizeof(uint32_t);
    info.pCode = spirv.data();

    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS) {
        throw std::runtime_error("StaticRenderer: failed to create shader module");
    }
    return module;
}

void StaticRenderer::createPipeline(VkRenderPass renderPass, const std::string& shaderDir) {
    const bool isHeapTier = m_heapAllocator != nullptr;

    auto vertSpirv = readSpirv(shaderDir + "/fullscreen.vert.spv");
    auto fragSpirv = readSpirv(shaderDir + (isHeapTier ? "/fullscreen_heap.frag.spv" : "/fullscreen_set.frag.spv"));

    VkShaderModule vertModule = createShaderModule(m_device.device(), vertSpirv);
    VkShaderModule fragModule = VK_NULL_HANDLE;
    try {
        fragModule = createShaderModule(m_device.device(), fragSpirv);
    } catch (...) {
        // vertModule already succeeded; without this it leaks when the fragment
        // module fails (bad SPIR-V, OOM).
        vkDestroyShaderModule(m_device.device(), vertModule, nullptr);
        throw;
    }

    VkPipelineShaderStageCreateInfo vertStage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertStage.module = vertModule;
    vertStage.pName = "main";

    VkPipelineShaderStageCreateInfo fragStage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    fragStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragStage.module = fragModule;
    fragStage.pName = "main";

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{vertStage, fragStage};

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 2> attributes{
        VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, pos)},
        VkVertexInputAttributeDescription{1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
    };

    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
    vertexInput.pVertexAttributeDescriptions = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.blendEnable = VK_FALSE;
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &colorBlendAttachment;

    std::array<VkDynamicState, 2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipelineInfo.stageCount = static_cast<uint32_t>(stages.size());
    pipelineInfo.pStages = stages.data();
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;

    VkPipelineCreateFlags2CreateInfo flags2Info{VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO};

    if (isHeapTier) {
        flags2Info.flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT;
        pipelineInfo.pNext = &flags2Info;
        pipelineInfo.layout = VK_NULL_HANDLE;
    } else {
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        if (auto* classic = dynamic_cast<ClassicDescriptorAllocator*>(&m_device.descriptorAllocator())) {
            setLayout = classic->setLayout();
        } else if (auto* buffer = dynamic_cast<DescriptorBufferAllocator*>(&m_device.descriptorAllocator())) {
            setLayout = buffer->setLayout();
            pipelineInfo.flags = VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
        }

        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &setLayout;

        if (vkCreatePipelineLayout(m_device.device(), &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
            vkDestroyShaderModule(m_device.device(), vertModule, nullptr);
            vkDestroyShaderModule(m_device.device(), fragModule, nullptr);
            throw std::runtime_error("StaticRenderer: failed to create pipeline layout");
        }
        pipelineInfo.layout = m_pipelineLayout;
    }

    VkResult result = vkCreateGraphicsPipelines(m_device.device(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline);

    vkDestroyShaderModule(m_device.device(), vertModule, nullptr);
    vkDestroyShaderModule(m_device.device(), fragModule, nullptr);

    if (result != VK_SUCCESS) {
        throw std::runtime_error("StaticRenderer: failed to create graphics pipeline");
    }
}

VkImageLayout StaticRenderer::record(VkCommandBuffer cmd, const OffscreenTarget& target) {
    const VkFramebuffer framebuffer = target.framebuffer;
    const uint32_t width = target.width;
    const uint32_t height = target.height;

    VkClearValue clearColor{};
    clearColor.color = {{0.0f, 0.0f, 0.0f, 1.0f}};

    VkRenderPassBeginInfo rpBegin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rpBegin.renderPass = m_renderPass;
    rpBegin.framebuffer = framebuffer;
    rpBegin.renderArea = {{0, 0}, {width, height}};
    rpBegin.clearValueCount = 1;
    rpBegin.pClearValues = &clearColor;

    vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

    VkViewport viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{{0, 0}, {width, height}};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    m_device.descriptorAllocator().bind(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS);

    if (m_heapAllocator) {
        m_heapAllocator->pushData(cmd, 0, &m_descriptorHandle.index, sizeof(uint32_t));
    }

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &m_vertexBuffer.buffer, &offset);
    vkCmdDraw(cmd, 6, 1, 0, 0);

    vkCmdEndRenderPass(cmd);

    // The shared offscreen render pass declares this as its finalLayout.
    return kRenderedLayout;
}

}
