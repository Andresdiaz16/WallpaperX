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
#include <string>

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

// The peak host memory this renderer will spend to decode one image, in bytes.
//
// This bounds the PEAK, not the finished buffer, and the distinction is the whole point.
// An earlier version compared `w*h*4` -- stb's final RGBA output -- against 256 MiB, which
// measured one allocation out of the several that are live simultaneously. A 16-bit RGBA
// PNG at 8192x8192 computes to exactly 268435456, was therefore *not* over the old cap,
// and then peaked near 1.5 GB. It compresses to a few megabytes. That is the same
// OOM-kill the cap was added to prevent, from a file the cap accepted.
//
// The motivating case, measured: a 995 KB PNG of 16000x16000 decodes to 976 MiB of RGBA,
// the daemon peaked at +2.0 GB RSS, and it was OOM-killed under a 1.2 GB limit. Nothing
// else stopped it. stb's own limits are not a substitute -- read out of stb_image.h, not
// assumed:
//   - PNG has a product bound in the IHDR parse, `(1<<30)/w/img_n < h` (`/4` when
//     paletted), so the ceiling moves with the channel count: ~16384x16384 for RGBA,
//     ~18918x18918 for RGB. It does run at header time, inside the stbi_info() below.
//   - JPEG has nothing at header time. `stbi__mad3sizes_valid` sits after
//     `if (scan != STBI__SCAN_load) return 1;` (stb_image.h:3296-3298), so stbi_info
//     returns before reaching it. Two things do bound a JPEG, and neither helps here: the
//     SOF marker stores each axis as 16-bit big-endian, so 65535 is a hard format ceiling
//     (:3270-3271) -- STBI_MAX_DIMENSIONS of 1<<24 is checked but can never bind -- and at
//     *load* time mad3sizes_valid caps `x*y*img_n` at INT_MAX, about 26755x26755 for a
//     3-component image. Both are far above anything this daemon should accept, and the
//     second runs only after the decode has been committed to.
// So for JPEG, which is what most wallpapers are, this is the only threshold that acts
// before memory is spent.
//
// 768 MiB admits every real display geometry -- 8K at 7680x4320 estimates to ~532 MiB,
// five 4K panels side by side at 19200x2160 to ~664 MiB -- and puts the square ceiling
// near 7094x7094. It is deliberately below the 1.2 GB limit the original kill happened
// under. Note this is slightly tighter than the old nominal 8192x8192: that figure was
// only ever reachable because the old model under-counted, so nothing that actually
// worked is lost.
constexpr uint64_t kMaxPeakDecodeBytes = 768ull * 1024 * 1024;

// Multiples of the final RGBA buffer that are live at once, worst case. Counted out of
// stb_image.h rather than estimated, because a factor invented to look safe is how the
// previous version of this check ended up not binding:
//   - 8-bit: the zlib-inflated scanline data (`img_width_bytes+1` per row, up to ~1x for
//     4-channel) is alive while `a->out` (1x) is filled, and an interlaced PNG holds
//     `final` at full size plus the largest Adam7 pass, which is half the image
//     (:4870-4899). Progressive JPEG is the other shape: per-component coefficient buffers
//     at w*h*2 bytes each. 3x covers all of them, and this function's own staging buffer
//     adds the 4th.
//   - 16-bit: `stbi__load_and_postprocess_8bit` materialises the 16-bit image (2x) and
//     then allocates the 8-bit target (1x) before freeing it (:1260-1272), and interlacing
//     applies on top of that. 5x plus staging.
// Interlacing is not observable through any stb API -- there is no stbi_is_interlaced --
// so it cannot be probed and has to be carried in the factor.
constexpr uint64_t kPeakFactor8Bit = 4;
constexpr uint64_t kPeakFactor16Bit = 6;

namespace {

// stbi__g_failure_reason is a file-scope `const char *` with no initialiser
// (stb_image.h:970), so it is NULL until some path calls stbi__err. Concatenating that
// into a std::string is undefined, and the failure paths are exactly where nobody looks
// until it matters.
std::string stbFailureReason() {
    const char* reason = stbi_failure_reason();
    return reason != nullptr ? std::string(reason) : std::string("no reason reported");
}

// Why every message below puts its diagnosis first and the path last.
//
// These strings become MonitorState::lastError, which is truncated to 1024 bytes by
// sanitizeForLog -- and that keeps the HEAD. With the path first, a long but perfectly
// legal path pushed stb's reason past the cut, so `last_error` arrived saying a file could
// not be decoded without a word about why. Since SetWallpaper replies before rendering is
// attempted, this is the only channel a render failure has. The path is the part the
// client already knows and therefore the right part to lose.
std::string describeRejection(const std::string& what, const std::string& imagePath) {
    return "StaticRenderer: " + what + ": " + imagePath;
}

}  // namespace

void StaticRenderer::loadAndUploadTexture(const std::string& imagePath) {
    // Checked from the header alone, before decoding, because a limit applied after
    // stbi_load has already paid the whole cost it exists to prevent. stbi_info reads
    // dimensions without allocating the pixels.
    int probeWidth = 0;
    int probeHeight = 0;
    int probeChannels = 0;
    if (!stbi_info(imagePath.c_str(), &probeWidth, &probeHeight, &probeChannels)) {
        throw std::runtime_error(
            describeRejection("not a decodable image (" + stbFailureReason() + ")", imagePath));
    }
    if (probeWidth <= 0 || probeHeight <= 0) {
        throw std::runtime_error(describeRejection(
            "image reports non-positive dimensions (" + std::to_string(probeWidth) + "x" +
                std::to_string(probeHeight) + ")",
            imagePath));
    }

    // probeChannels is reported but deliberately not part of the arithmetic: what drives
    // the peak is bit depth and interlacing, not how many channels the source had, and
    // stb converts to 4 channels regardless of it.
    const uint64_t outputBytes =
        static_cast<uint64_t>(probeWidth) * static_cast<uint64_t>(probeHeight) * 4;
    const bool sixteenBit = stbi_is_16_bit(imagePath.c_str()) != 0;
    const uint64_t peakBytes = outputBytes * (sixteenBit ? kPeakFactor16Bit : kPeakFactor8Bit);
    if (peakBytes > kMaxPeakDecodeBytes) {
        throw std::runtime_error(describeRejection(
            "estimated peak " + std::to_string(peakBytes / (1024 * 1024)) + " MiB to decode " +
                std::to_string(probeWidth) + "x" + std::to_string(probeHeight) + " (" +
                std::to_string(probeChannels) + " channels, " + (sixteenBit ? "16" : "8") +
                "-bit), over the " + std::to_string(kMaxPeakDecodeBytes / (1024 * 1024)) +
                " MiB limit",
            imagePath));
    }

    int imageWidth = 0;
    int imageHeight = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load(imagePath.c_str(), &imageWidth, &imageHeight, &channels, STBI_rgb_alpha);
    if (!pixels) {
        throw std::runtime_error(
            describeRejection("failed to load image (" + stbFailureReason() + ")", imagePath));
    }

    // Re-checked against what stbi_load ACTUALLY returned, because the check above ran on a
    // different open of the file. stbi_info and stbi_load are two independent fopen()s, so
    // anything with write access to the wallpaper directory -- a slideshow tool, a
    // downloader, the user -- can replace a 1920x1080 PNG with a 16000x16000 JPEG between
    // them, and every allocation below would then be sized from dimensions nothing had
    // vetted. The decode itself is already paid for by this point; what this still buys is
    // that the staging buffer and the texture cannot be sized by an unvetted number.
    //
    // Uses the 8-bit factor unconditionally: the decode is done, so the only cost left is
    // this function's own. That also makes this gate strictly looser than the header one,
    // so a file that was not swapped can never be rejected here after passing there.
    const uint64_t loadedBytes =
        static_cast<uint64_t>(imageWidth) * static_cast<uint64_t>(imageHeight) * 4;
    if (loadedBytes * kPeakFactor8Bit > kMaxPeakDecodeBytes) {
        stbi_image_free(pixels);
        throw std::runtime_error(describeRejection(
            "decoded to " + std::to_string(imageWidth) + "x" + std::to_string(imageHeight) +
                ", larger than the " + std::to_string(probeWidth) + "x" +
                std::to_string(probeHeight) + " its header reported -- file changed under us",
            imagePath));
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
