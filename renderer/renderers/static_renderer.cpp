// SPDX-License-Identifier: GPL-2.0-or-later

#include "static_renderer.h"

#include "cover_crop.h"

#include "../vulkan/offscreen.h"

#include "../vulkan/descriptor_buffer.h"
#include "../vulkan/descriptor_classic.h"
#include "../vulkan/descriptor_heap.h"
#include "../vulkan/device.h"
#include "../vulkan/gpu_fault.h"
#include "../vulkan/image_barrier.h"
#include "../vulkan/shader_module.h"
#include "../wallpaper_file.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace wallpaperx {

namespace {
struct Vertex {
    float pos[2];
    float uv[2];
};

// Mirrors the push_constant blocks: the texture region in fullscreen.vert, then the heap index
// fullscreen_heap.frag reads, then whether both fragment shaders repeat the texture. The other
// tiers' fragment shader ignores the index.
struct Push {
    float uvOrigin[2];
    float uvExtent[2];
    uint32_t textureIndex;
    uint32_t tile;
};
static_assert(offsetof(Push, uvOrigin) == 0 && offsetof(Push, uvExtent) == 8 && offsetof(Push, textureIndex) == 16 &&
              offsetof(Push, tile) == 20 && sizeof(Push) <= kPushDataBytes);
}

StaticRenderer::StaticRenderer(Device& device, VkRenderPass renderPass, const std::string& imagePath, const std::string& shaderDir)
    : StaticRenderer(device, renderPass, StillImage::load(device, imagePath), shaderDir) {}

StaticRenderer::StaticRenderer(Device& device, VkRenderPass renderPass, std::shared_ptr<const StillImage> image,
                               const std::string& shaderDir)
    : m_device(device), m_image(std::move(image)), m_renderPass(renderPass) {
    if (!m_image) throw std::invalid_argument("StaticRenderer: no image to draw");
    m_heapAllocator = dynamic_cast<DescriptorHeapAllocator*>(&m_device.descriptorAllocator());

    try {
        createSampler();
        m_descriptorHandle = m_device.descriptorAllocator().allocate(m_image->view(), m_sampler);
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

    // The render this was built for does not wait, and it reads every Vulkan object below.
    // Freeing them while the GPU still has them is corruption, and this runs from a destructor
    // where the alternative to freeing cannot be an exception -- so a GPU that has not finished
    // within the bound gets them left behind instead, and the next submit is refused, which is
    // how a wedge reaches the daemon's health report from here. No batch to name: nothing
    // records which render last drew with this pipeline, so any pending one may still be.
    const bool idle = m_device.safeToFree("a wallpaper's sampler, its pipeline, and its vertex buffer");
    if (idle) {
        if (m_pipeline) vkDestroyPipeline(device, m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
        m_device.memory().destroyBuffer(m_vertexBuffer);
    }

    // Host bookkeeping, so it runs even when the GPU objects were left behind: the slot is not
    // a Vulkan object, and the classic and descriptor-buffer tiers have one of them -- holding
    // it back would fail every later wallpaper as "all 1 descriptor slots are in use" long
    // after the GPU recovered. Handing it out again is separately guarded: the pool records
    // this batch as its last reader and will not reissue it until that batch completes.
    //
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

    if (m_heapAllocator) m_heapAllocator->unregisterSampler(m_sampler);

    if (!idle) return;

    if (m_sampler) vkDestroySampler(device, m_sampler, nullptr);
    m_sampler = VK_NULL_HANDLE;
}

StillImage::~StillImage() {
    cleanup();
}

void StillImage::cleanup() {
    // Host bookkeeping, so it runs even when the view itself is left behind.
    if (m_heapAllocator) m_heapAllocator->unregisterImageView(m_view);

    // Any pending render may have drawn from it: the image outlives the renderers that recorded those.
    if (!m_device.safeToFree("a wallpaper's texture and that texture's view")) return;

    if (m_view) vkDestroyImageView(m_device.device(), m_view, nullptr);
    m_view = VK_NULL_HANDLE;
    m_device.memory().destroyImage(m_texture);
}

bool StillImage::FileIdentity::operator==(const FileIdentity& other) const {
    const auto same = [](const timespec& a, const timespec& b) { return a.tv_sec == b.tv_sec && a.tv_nsec == b.tv_nsec; };
    return device == other.device && inode == other.inode && size == other.size && same(modified, other.modified) &&
           same(changed, other.changed);
}

// Peak host memory allowed for one decode -- the PEAK, not the finished buffer. Several
// allocations are live at once, so bounding only `w*h*4` lets a 16-bit 8192x8192 PNG sit
// exactly on a 256 MiB cap and then reach ~1.5 GB.
//
// This is the ONLY size gate a JPEG meets before memory is spent: stb's
// `stbi__mad3sizes_valid` sits after `if (scan != STBI__SCAN_load) return 1;`
// (stb_image.h:3296-3298), so stbi_info never reaches it. PNG has an IHDR bound that does
// run at header time, but most wallpapers are JPEG.
//
// 768 MiB admits every real geometry (8K ~532 MiB, five 4K panels ~664 MiB), putting the
// square ceiling near 7094x7094, and stays under the 1.2 GB limit of the original kill.
constexpr uint64_t kMaxPeakDecodeBytes = 768ull * 1024 * 1024;

// Multiples of the final RGBA buffer live at once, worst case, counted out of stb_image.h:
// inflated scanline data alongside `a->out`, Adam7's `final` plus its largest pass, and for
// 16-bit the 2x source buffer plus its 8-bit conversion target. Plus our staging copy.
//
// UNVERIFIED as upper bounds -- see GOALS.md S4.10. Interlacing is not probeable through
// any stb API, so it has to be carried in the factor rather than detected.
constexpr uint64_t kPeakFactor8Bit = 4;
constexpr uint64_t kPeakFactor16Bit = 6;

namespace {

// stbi_failure_reason() is NULL until some path calls stbi__err (stb_image.h:970), and
// concatenating a NULL into std::string is undefined.
std::string stbFailureReason() {
    const char* reason = stbi_failure_reason();
    return reason != nullptr ? std::string(reason) : std::string("no reason reported");
}

constexpr const char* kComponent = "StaticRenderer";

struct FileCloser {
    void operator()(FILE* f) const noexcept { std::fclose(f); }
};
using FilePtr = std::unique_ptr<FILE, FileCloser>;

struct StbFreer {
    void operator()(stbi_uc* p) const noexcept { stbi_image_free(p); }
};
using PixelsPtr = std::unique_ptr<stbi_uc, StbFreer>;

// A local buffer is unreachable from cleanup(), so nothing else would ever free it.
//
// The check is not redundant with the wait immediateSubmit already did: the path that reaches
// here by that wait FAILING leaves the upload batch running, still reading this buffer, and
// freeing it then is the corruption the bound exists to avoid. When the GPU has not finished,
// the buffer is left behind instead -- Device then leaks itself rather than letting VMA abort
// the process over the orphan.
//
// The upload is the only batch that reads this buffer and the newest one submitted while the
// guard is alive, so lastSubmission() names it. A batch older than the upload cannot read the
// buffer, so naming one on the path where nothing was submitted at all is conservative only.
struct BufferGuard {
    Device& device;
    AllocatedBuffer& buffer;
    ~BufferGuard() {
        if (device.safeToFree("a wallpaper texture's staging buffer", device.lastSubmission())) {
            device.memory().destroyBuffer(buffer);
        }
    }
};

}  // namespace

std::shared_ptr<const StillImage> StillImage::load(Device& device, const std::string& imagePath,
                                                   std::shared_ptr<const StillImage> kept) {
    // Probes, decode and the identity check share one open: reopening by path lets a swapped file
    // be charged the wrong peak factor, or be taken for the one already decoded. Safe without a
    // rewind because both _from_file probes ftell/fseek around their work (stb_image.h:7701, :7722).
    //
    // Through openWallpaperFile() rather than fopen: a render happens whenever a screen reports a
    // new size, long after the path was checked, and opening what is a named pipe by then parks
    // this thread -- the one GPU thread -- for good.
    const int fd = openWallpaperFile(imagePath, kComponent);
    if (fd < 0) {
        const int why = errno;
        throw WallpaperContentError(
            describeRejection(kComponent, std::string("cannot open image (") + std::strerror(why) + ")", imagePath));
    }
    // fdopen fails only for want of memory to build the stream, which is not the image's doing.
    FilePtr file(::fdopen(fd, "rb"));
    if (!file) {
        ::close(fd);
        throw ResourceExhausted(describeRejection(kComponent, "out of memory opening the image", imagePath));
    }

    // openWallpaperFile() has already asked this descriptor once, so only a shortage fails here.
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        const int why = errno;
        const std::string reason = std::string("cannot read the image's attributes (") + std::strerror(why) + ")";
        if (why == ENOMEM) throw ResourceExhausted(describeRejection(kComponent, reason, imagePath));
        throw WallpaperContentError(describeRejection(kComponent, reason, imagePath));
    }
    // Read before the decode, so a write landing during it is a change the next load sees.
    const FileIdentity identity{st.st_dev, st.st_ino, st.st_size, st.st_mtim, st.st_ctim};
    if (kept && kept->m_file == identity) return kept;
    kept.reset();

    return std::shared_ptr<const StillImage>(new StillImage(device, file.get(), imagePath, identity));
}

StillImage::StillImage(Device& device, FILE* file, const std::string& imagePath, const FileIdentity& identity)
    : m_device(device),
      m_heapAllocator(dynamic_cast<DescriptorHeapAllocator*>(&device.descriptorAllocator())),
      m_file(identity) {
    try {
        upload(file, imagePath);
    } catch (...) {
        cleanup();
        throw;
    }
}

void StillImage::upload(FILE* probe, const std::string& imagePath) {
    int probeWidth = 0;
    int probeHeight = 0;
    int probeChannels = 0;
    const bool headerRead = stbi_info_from_file(probe, &probeWidth, &probeHeight, &probeChannels) != 0;
    const bool sixteenBit = headerRead && stbi_is_16_bit_from_file(probe) != 0;

    if (!headerRead) {
        throw WallpaperContentError(
            describeRejection(kComponent, "not a decodable image (" + stbFailureReason() + ")", imagePath));
    }
    if (probeWidth <= 0 || probeHeight <= 0) {
        throw WallpaperContentError(describeRejection(
            kComponent,
            "image reports non-positive dimensions (" + std::to_string(probeWidth) + "x" +
                std::to_string(probeHeight) + ")",
            imagePath));
    }

    // probeChannels is reported but deliberately not part of the arithmetic: what drives
    // the peak is bit depth and interlacing, not how many channels the source had, and
    // stb converts to 4 channels regardless of it.
    const uint64_t outputBytes =
        static_cast<uint64_t>(probeWidth) * static_cast<uint64_t>(probeHeight) * 4;
    const uint64_t peakBytes = outputBytes * (sixteenBit ? kPeakFactor16Bit : kPeakFactor8Bit);
    if (peakBytes > kMaxPeakDecodeBytes) {
        throw WallpaperContentError(describeRejection(
            kComponent,
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
    PixelsPtr pixels(stbi_load_from_file(probe, &imageWidth, &imageHeight, &channels, STBI_rgb_alpha));
    if (!pixels) {
        throw WallpaperContentError(
            describeRejection(kComponent, "failed to load image (" + stbFailureReason() + ")", imagePath));
    }

    // Guards stb's two parsers disagreeing with each other, not a swap -- the shared open
    // above rules that out.
    if (imageWidth != probeWidth || imageHeight != probeHeight) {
        throw WallpaperContentError(describeRejection(
            kComponent,
            "decoded to " + std::to_string(imageWidth) + "x" + std::to_string(imageHeight) +
                " but its header reported " + std::to_string(probeWidth) + "x" +
                std::to_string(probeHeight) + " -- header and decoder disagree",
            imagePath));
    }

    const VkDeviceSize imageSize = static_cast<VkDeviceSize>(imageWidth) * imageHeight * 4;

    AllocatedBuffer staging = m_device.memory().createBuffer(
        imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, HostAccess::Write, "static-renderer-staging");
    BufferGuard stagingGuard{m_device, staging};
    std::memcpy(staging.mapped, pixels.get(), static_cast<size_t>(imageSize));
    m_device.memory().flushForHostWrite(staging);
    // Released before the GPU work, not at scope exit: it is the other half of the peak the
    // decode budget bounds.
    pixels.reset();

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
    m_extent = {imageInfo.extent.width, imageInfo.extent.height};

    m_device.immediateSubmit([&](VkCommandBuffer cmd) {
        const VkImageMemoryBarrier toTransferDst =
            imageLayoutBarrier(m_texture.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                               VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              0, 0, nullptr, 0, nullptr, 1, &toTransferDst);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {static_cast<uint32_t>(imageWidth), static_cast<uint32_t>(imageHeight), 1};

        vkCmdCopyBufferToImage(cmd, staging.buffer, m_texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        const VkImageMemoryBarrier toShaderRead =
            imageLayoutBarrier(m_texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                               VK_ACCESS_SHADER_READ_BIT);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                              0, 0, nullptr, 0, nullptr, 1, &toShaderRead);
    });

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = m_texture.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    if (const VkResult r = vkCreateImageView(m_device.device(), &viewInfo, nullptr, &m_view); r != VK_SUCCESS) {
        throwVkFailure("StaticRenderer: vkCreateImageView", r);
    }

    if (m_heapAllocator) {
        m_heapAllocator->registerImageView(m_view, viewInfo);
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

    if (const VkResult r = vkCreateSampler(m_device.device(), &samplerInfo, nullptr, &m_sampler); r != VK_SUCCESS) {
        throwVkFailure("StaticRenderer: vkCreateSampler", r);
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

void StaticRenderer::createPipeline(VkRenderPass renderPass, const std::string& shaderDir) {
    const bool isHeapTier = m_heapAllocator != nullptr;

    const ShaderModule vertModule(m_device.device(), shaderDir + "/fullscreen.vert.spv", "StaticRenderer");
    const ShaderModule fragModule(m_device.device(),
                                  shaderDir + (isHeapTier ? "/fullscreen_heap.frag.spv" : "/fullscreen_set.frag.spv"),
                                  "StaticRenderer");

    VkPipelineShaderStageCreateInfo vertStage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertStage.module = vertModule.get();
    vertStage.pName = "main";

    VkPipelineShaderStageCreateInfo fragStage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    fragStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragStage.module = fragModule.get();
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
    } else if (auto* classic = dynamic_cast<ClassicDescriptorAllocator*>(&m_device.descriptorAllocator())) {
        pipelineInfo.layout = classic->pipelineLayout();
    } else if (auto* buffer = dynamic_cast<DescriptorBufferAllocator*>(&m_device.descriptorAllocator())) {
        pipelineInfo.layout = buffer->pipelineLayout();
        pipelineInfo.flags = VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
    }

    VkResult result = vkCreateGraphicsPipelines(m_device.device(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline);
    if (result != VK_SUCCESS) {
        // Unclassified for the same reason as a rejected shader module: the shader modules are
        // compiled here, so a rejection may be the shader bundle rather than the driver.
        throw std::runtime_error("StaticRenderer: vkCreateGraphicsPipelines failed with " + vkResultName(result));
    }
}

void StaticRenderer::place(const Placement& placement) {
    if (!placementAllowed(placement)) {
        throw std::invalid_argument("StaticRenderer: a zoom or offset outside what Free mode allows");
    }
    m_placement = placement;
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

    // Per record, not per renderer: one renderer may draw monitors of different shapes. The clear
    // above is the black wherever the image does not reach.
    const std::optional<Region> region =
        placedRegion(m_placement, m_image->extent().width, m_image->extent().height, width, height);
    if (region) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

        const bool tile = m_placement.mode == PositionMode::Tile;
        // A tiled draw covers the target and repeats the texture from where the centred copy starts.
        const Crop drawn = tile ? Crop{0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)} : region->target;
        // Sampled from the exact edges, not the whole-texel crop, so a cut edge moves smoothly.
        const Area& exact = region->exact;
        const Area shown = tile ? Area{exact.x0 - region->target.x0, exact.y0 - region->target.y0,
                                       exact.x0 - region->target.x0 + width, exact.y0 - region->target.y0 + height}
                                : exact;

        const VkViewport viewport{static_cast<float>(drawn.x0), static_cast<float>(drawn.y0),
                                  static_cast<float>(drawn.x1 - drawn.x0), static_cast<float>(drawn.y1 - drawn.y0),
                                  0.0f, 1.0f};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        const VkRect2D scissor{{drawn.x0, drawn.y0},
                               {static_cast<uint32_t>(drawn.x1 - drawn.x0), static_cast<uint32_t>(drawn.y1 - drawn.y0)}};
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        m_device.descriptorAllocator().bind(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS);

        const double textureWidth = m_image->extent().width;
        const double textureHeight = m_image->extent().height;
        const Push push{
            {static_cast<float>(shown.x0 / textureWidth), static_cast<float>(shown.y0 / textureHeight)},
            {static_cast<float>((shown.x1 - shown.x0) / textureWidth),
             static_cast<float>((shown.y1 - shown.y0) / textureHeight)},
            m_descriptorHandle.index,
            tile ? 1u : 0u,
        };
        m_device.descriptorAllocator().pushData(cmd, 0, &push, sizeof(push));

        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &m_vertexBuffer.buffer, &offset);
        vkCmdDraw(cmd, 6, 1, 0, 0);
    }

    vkCmdEndRenderPass(cmd);

    // The shared offscreen render pass declares this as its finalLayout.
    return kRenderedLayout;
}

}
