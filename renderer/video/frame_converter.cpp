// SPDX-License-Identifier: GPL-2.0-or-later

#include "frame_converter.h"

#include "../vulkan/format_features.h"
#include "../vulkan/gpu_fault.h"
#include "../vulkan/image_barrier.h"
#include "../vulkan/shader_module.h"

extern "C" {
#include <libavutil/pixdesc.h>
}

#include <cstddef>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <variant>

namespace wallpaperx {

// Mirrors the push_constant block in ycbcr_convert.frag, offset for offset. Columns of the
// matrix are padded to vec4 as std430 lays a mat3 out.
struct ConvertPush {
    float matrix[3][4];
    float offset[4];
    float lumaScale[2];
    float chromaScale[2];
    float chromaShift[2];
};
static_assert(offsetof(ConvertPush, offset) == 48);
static_assert(offsetof(ConvertPush, lumaScale) == 64);
static_assert(offsetof(ConvertPush, chromaScale) == 72);
static_assert(offsetof(ConvertPush, chromaShift) == 80);
static_assert(sizeof(ConvertPush) == 88);

namespace {

constexpr VkFormat kOutputFormat = VK_FORMAT_B8G8R8A8_UNORM;
constexpr VkImageUsageFlags kOutputUsage =
    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
constexpr VkImageLayout kOutputLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
constexpr VkImageSubresourceRange kWholeImage{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
constexpr VkImageSubresourceLayers kBaseLayer{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
constexpr VkImageAspectFlags kPlaneAspects[3] = {VK_IMAGE_ASPECT_PLANE_0_BIT, VK_IMAGE_ASPECT_PLANE_1_BIT,
                                                 VK_IMAGE_ASPECT_PLANE_2_BIT};
// One for the decoder's image plus one per plane.
constexpr size_t kMaxBarriers = 4;

// A planar Y'CbCr layout as ffmpeg describes it: Y' on its own plane, Cb and Cr either
// interleaved on a second plane or on a plane each, every sample the same depth.
struct Layout {
    uint32_t planeCount;
    uint32_t bitDepth;
    // Whether a code sits in the top bits of its 16-bit sample, as P010 and every Vulkan X6
    // format store it, rather than the bottom bits as ffmpeg's yuv420p10 does.
    bool codeInTopBits;
    uint32_t chromaShiftX;
    uint32_t chromaShiftY;
    // The plane and the component within it that hold Cb and Cr.
    uint32_t cbPlane, crPlane;
    VkComponentSwizzle cbComponent, crComponent;
};

std::optional<Layout> layoutFor(AVPixelFormat format) {
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(format);
    if (desc == nullptr || desc->nb_components < 3) return std::nullopt;
    constexpr uint64_t unreadable = AV_PIX_FMT_FLAG_BE | AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_BITSTREAM |
                                    AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_BAYER |
                                    AV_PIX_FMT_FLAG_FLOAT | AV_PIX_FMT_FLAG_XYZ;
    if ((desc->flags & unreadable) != 0 || (desc->flags & AV_PIX_FMT_FLAG_PLANAR) == 0) return std::nullopt;

    const AVComponentDescriptor& y = desc->comp[0];
    const AVComponentDescriptor& cb = desc->comp[1];
    const AVComponentDescriptor& cr = desc->comp[2];
    const int bytes = y.depth > 8 ? 2 : 1;
    const bool sameSamples = cb.depth == y.depth && cr.depth == y.depth && cb.shift == y.shift && cr.shift == y.shift;
    if (!sameSamples || y.depth < 8 || y.depth > 16 || y.step != bytes || y.plane == cb.plane) return std::nullopt;

    Layout layout{};
    layout.bitDepth = static_cast<uint32_t>(y.depth);
    layout.codeInTopBits = y.shift != 0;
    layout.chromaShiftX = desc->log2_chroma_w;
    layout.chromaShiftY = desc->log2_chroma_h;
    layout.cbPlane = static_cast<uint32_t>(cb.plane);
    layout.crPlane = static_cast<uint32_t>(cr.plane);
    if (cb.plane == cr.plane) {
        const bool interleaved = cb.step == 2 * bytes && cr.step == 2 * bytes &&
                                 ((cb.offset == 0 && cr.offset == bytes) || (cb.offset == bytes && cr.offset == 0));
        if (!interleaved || cb.plane != 1) return std::nullopt;
        layout.planeCount = 2;
        layout.cbComponent = cb.offset == 0 ? VK_COMPONENT_SWIZZLE_R : VK_COMPONENT_SWIZZLE_G;
        layout.crComponent = cr.offset == 0 ? VK_COMPONENT_SWIZZLE_R : VK_COMPONENT_SWIZZLE_G;
    } else {
        if (cb.step != bytes || cr.step != bytes || cb.plane > 2 || cr.plane > 2) return std::nullopt;
        layout.planeCount = 3;
        layout.cbComponent = VK_COMPONENT_SWIZZLE_R;
        layout.crComponent = VK_COMPONENT_SWIZZLE_R;
    }
    return layout;
}

// The single-plane format a plane is held in, and what a sample of it multiplies to for the
// code value. A plane copied out of a device image must take the plane's own compatible
// format, which above 8 bits is an X6 or X4 format whose samples are normalised over the code's
// bits; a plane uploaded from host memory takes whichever format its bit placement matches.
struct PlaneFormat {
    VkFormat format;
    uint32_t texelBytes;
    float sampleToCode;
};

std::optional<PlaneFormat> planeFormat(const Layout& layout, uint32_t components) {
    const bool two = components == 2;
    if (layout.bitDepth == 8) {
        return two ? PlaneFormat{VK_FORMAT_R8G8_UNORM, 2, 255.0f} : PlaneFormat{VK_FORMAT_R8_UNORM, 1, 255.0f};
    }
    if (!layout.codeInTopBits) {
        return two ? PlaneFormat{VK_FORMAT_R16G16_UNORM, 4, 65535.0f} : PlaneFormat{VK_FORMAT_R16_UNORM, 2, 65535.0f};
    }
    if (layout.bitDepth == 10) {
        return two ? PlaneFormat{VK_FORMAT_R10X6G10X6_UNORM_2PACK16, 4, 1023.0f}
                   : PlaneFormat{VK_FORMAT_R10X6_UNORM_PACK16, 2, 1023.0f};
    }
    if (layout.bitDepth == 12) {
        return two ? PlaneFormat{VK_FORMAT_R12X4G12X4_UNORM_2PACK16, 4, 4095.0f}
                   : PlaneFormat{VK_FORMAT_R12X4_UNORM_PACK16, 2, 4095.0f};
    }
    return std::nullopt;
}

uint32_t ceilShift(uint32_t value, uint32_t shift) { return (value + (1u << shift) - 1) >> shift; }

// The texels of a plane subsampled by the given shifts that any pixel of `visible` shares, and no
// others. `visible` must lie inside the coded picture.
VkRect2D planeRegion(VkRect2D visible, uint32_t shiftX, uint32_t shiftY) {
    const auto x = static_cast<uint32_t>(visible.offset.x);
    const auto y = static_cast<uint32_t>(visible.offset.y);
    const uint32_t left = x >> shiftX;
    const uint32_t top = y >> shiftY;
    return {{static_cast<int32_t>(left), static_cast<int32_t>(top)},
            {ceilShift(x + visible.extent.width, shiftX) - left, ceilShift(y + visible.extent.height, shiftY) - top}};
}

// The one multi-planar VkFormat a decoder's image of this layout is held in, as Vulkan names
// them, or UNDEFINED for one never read from a decoder's image. Vulkan's formats hold Cb before
// Cr. 4:4:4 names only the formats a hardware decoder has been seen producing; 4:2:2 has none.
VkFormat deviceFormatFor(const Layout& layout) {
    const bool two = layout.planeCount == 2;
    const bool cbFirst =
        layout.cbPlane == 1 && (two ? layout.cbComponent == VK_COMPONENT_SWIZZLE_R : layout.crPlane == 2);
    if (!cbFirst) return VK_FORMAT_UNDEFINED;
    if (layout.chromaShiftX == 0 && layout.chromaShiftY == 0) {
        if (!two) return VK_FORMAT_UNDEFINED;
        switch (layout.bitDepth) {
            case 8: return VK_FORMAT_G8_B8R8_2PLANE_444_UNORM;
            case 10: return VK_FORMAT_G10X6_B10X6R10X6_2PLANE_444_UNORM_3PACK16;
            default: return VK_FORMAT_UNDEFINED;
        }
    }
    if (layout.chromaShiftX != 1 || layout.chromaShiftY != 1) return VK_FORMAT_UNDEFINED;
    switch (layout.bitDepth) {
        case 8: return two ? VK_FORMAT_G8_B8R8_2PLANE_420_UNORM : VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM;
        case 10:
            return two ? VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16
                       : VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16;
        case 12:
            return two ? VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16
                       : VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16;
        case 16: return two ? VK_FORMAT_G16_B16R16_2PLANE_420_UNORM : VK_FORMAT_G16_B16_R16_3PLANE_420_UNORM;
        default: return VK_FORMAT_UNDEFINED;
    }
}

struct Coefficients {
    double kr, kb;
};

// The luma coefficients of every non-constant-luminance matrix ffmpeg names that this pass can
// render, and nothing for the rest. An untagged stream gets the guess every player makes:
// standard definition is BT.601, anything larger is BT.709.
std::optional<Coefficients> coefficientsFor(AVColorSpace matrix, VkExtent2D coded) {
    switch (matrix) {
        case AVCOL_SPC_BT709: return Coefficients{0.2126, 0.0722};
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M: return Coefficients{0.299, 0.114};
        case AVCOL_SPC_SMPTE240M: return Coefficients{0.212, 0.087};
        case AVCOL_SPC_FCC: return Coefficients{0.30, 0.11};
        case AVCOL_SPC_UNSPECIFIED:
            return (coded.width >= 1280 || coded.height > 576) ? Coefficients{0.2126, 0.0722}
                                                                : Coefficients{0.299, 0.114};
        default: return std::nullopt;
    }
}

// The primaries this pass writes out as they are, with no gamut conversion.
bool primariesWithoutConversion(AVColorPrimaries primaries) {
    switch (primaries) {
        case AVCOL_PRI_BT709:
        case AVCOL_PRI_UNSPECIFIED:
        case AVCOL_PRI_BT470BG:
        case AVCOL_PRI_SMPTE170M:
        case AVCOL_PRI_SMPTE240M: return true;
        default: return false;
    }
}

// The SDR gamma transfers this pass writes out as they are. BT2020_10 and BT2020_12 are BT.709's
// curve; their primaries are refused separately.
bool transferWithoutConversion(AVColorTransferCharacteristic transfer) {
    switch (transfer) {
        case AVCOL_TRC_BT709:
        case AVCOL_TRC_UNSPECIFIED:
        case AVCOL_TRC_SMPTE170M:
        case AVCOL_TRC_GAMMA22:
        case AVCOL_TRC_GAMMA28:
        case AVCOL_TRC_SMPTE240M:
        case AVCOL_TRC_IEC61966_2_1:
        case AVCOL_TRC_BT2020_10:
        case AVCOL_TRC_BT2020_12: return true;
        default: return false;
    }
}

std::string describe(const char* name) { return name != nullptr ? name : "unknown"; }

// The "J" formats are ffmpeg's legacy spelling of full range; a frame in one that carries no
// range tag is full range.
bool fullRangeByName(AVPixelFormat format) {
    const char* name = av_get_pix_fmt_name(format);
    return name != nullptr && std::strncmp(name, "yuvj", 4) == 0;
}

// Where a chroma sample sits relative to the luma sample it shares a texel with, as a shift of
// the chroma lookup in the chroma plane's normalised coordinates: a sample sited at the first of
// the `factor` luma pixels its texel spans is (factor - 1) / 2 luma pixels from the texel's
// centre, which is none on an axis that is not subsampled. Untagged is left, which is what H.264
// and H.265 infer.
void chromaShift(AVChromaLocation location, const Layout& layout, const float chromaScale[2], float out[2]) {
    const float x = 0.5f * static_cast<float>((1u << layout.chromaShiftX) - 1) * chromaScale[0];
    const float y = 0.5f * static_cast<float>((1u << layout.chromaShiftY) - 1) * chromaScale[1];
    switch (location) {
        case AVCHROMA_LOC_CENTER: out[0] = 0.0f; out[1] = 0.0f; break;
        case AVCHROMA_LOC_TOPLEFT: out[0] = x; out[1] = y; break;
        case AVCHROMA_LOC_TOP: out[0] = 0.0f; out[1] = y; break;
        case AVCHROMA_LOC_BOTTOMLEFT: out[0] = x; out[1] = -y; break;
        case AVCHROMA_LOC_BOTTOM: out[0] = 0.0f; out[1] = -y; break;
        default: out[0] = x; out[1] = 0.0f; break;
    }
}

// Everything the pass needs of one frame's tags, folded into one affine map from the sampled
// plane values to RGB. Throws WallpaperContentError unless primaries, transfer and matrix are
// each one this pass accepts by name; anything else needs a conversion this pass does not do.
// The BT.2020 matrix is refused even beside accepted primaries: it is never used with any other
// gamut, and content tagged with one but not the other is more often mistagged than genuinely
// BT.709.
ConvertPush pushConstantsFor(const DecodedFrame& frame, const Layout& layout, float sampleToCode) {
    const FrameColour& colour = frame.colour();
    const VkExtent2D coded = frame.extent();
    if (colour.transfer == AVCOL_TRC_SMPTE2084 || colour.transfer == AVCOL_TRC_ARIB_STD_B67) {
        throw WallpaperContentError("FrameConverter: the stream is HDR (" +
                                    describe(av_color_transfer_name(colour.transfer)) +
                                    "), and this pass does no tone mapping");
    }
    if (!transferWithoutConversion(colour.transfer)) {
        throw WallpaperContentError("FrameConverter: the stream's transfer " +
                                    describe(av_color_transfer_name(colour.transfer)) +
                                    " is not an SDR gamma curve, and this pass does no transfer conversion");
    }
    if (!primariesWithoutConversion(colour.primaries)) {
        throw WallpaperContentError("FrameConverter: the stream's colour primaries " +
                                    describe(av_color_primaries_name(colour.primaries)) +
                                    " are not BT.709-class, and this pass does no gamut conversion");
    }
    if (colour.matrix == AVCOL_SPC_BT2020_NCL || colour.matrix == AVCOL_SPC_BT2020_CL) {
        throw WallpaperContentError("FrameConverter: the stream is BT.2020, and this pass does no gamut conversion");
    }
    const std::optional<Coefficients> k = coefficientsFor(colour.matrix, coded);
    if (!k) {
        throw WallpaperContentError("FrameConverter: the stream's colour matrix " +
                                    describe(av_color_space_name(colour.matrix)) +
                                    " is not a plain Y'CbCr matrix, and this pass applies nothing else");
    }

    const bool full = colour.range == AVCOL_RANGE_JPEG ||
                      (colour.range == AVCOL_RANGE_UNSPECIFIED && fullRangeByName(frame.pixelFormat()));
    const double unit = static_cast<double>(1u << (layout.bitDepth - 8));
    const double codeMax = static_cast<double>((1u << layout.bitDepth) - 1);
    const double lumaOffset = full ? 0.0 : 16.0 * unit;
    const double lumaScale = full ? codeMax : 219.0 * unit;
    const double chromaOffset = 128.0 * unit;
    const double chromaScale = full ? codeMax : 224.0 * unit;

    const double kr = k->kr;
    const double kb = k->kb;
    const double kg = 1.0 - kr - kb;
    // Rows R, G, B over columns Y', Cb, Cr, for Y' in [0, 1] and chroma in [-0.5, 0.5].
    const double m[3][3] = {
        {1.0, 0.0, 2.0 * (1.0 - kr)},
        {1.0, -2.0 * kb * (1.0 - kb) / kg, -2.0 * kr * (1.0 - kr) / kg},
        {1.0, 2.0 * (1.0 - kb), 0.0},
    };
    const double scale[3] = {1.0 / lumaScale, 1.0 / chromaScale, 1.0 / chromaScale};
    const double offset[3] = {lumaOffset, chromaOffset, chromaOffset};

    ConvertPush push{};
    for (int row = 0; row < 3; ++row) {
        double bias = 0.0;
        for (int column = 0; column < 3; ++column) {
            const double coefficient = m[row][column] * scale[column];
            push.matrix[column][row] = static_cast<float>(coefficient * sampleToCode);
            bias -= coefficient * offset[column];
        }
        push.offset[row] = static_cast<float>(bias);
    }

    // In the plane images' own coordinates, whose luma texels are the visible rectangle's pixels.
    const VkRect2D visible = frame.visible();
    const VkRect2D chroma = planeRegion(visible, layout.chromaShiftX, layout.chromaShiftY);
    push.lumaScale[0] = 1.0f / static_cast<float>(visible.extent.width);
    push.lumaScale[1] = 1.0f / static_cast<float>(visible.extent.height);
    push.chromaScale[0] = 1.0f / static_cast<float>(chroma.extent.width << layout.chromaShiftX);
    push.chromaScale[1] = 1.0f / static_cast<float>(chroma.extent.height << layout.chromaShiftY);
    chromaShift(colour.chromaLocation, layout, push.chromaScale, push.chromaShift);
    // A visible rectangle starting part way into a chroma texel starts that far into the image.
    const int32_t phaseX = visible.offset.x - (chroma.offset.x << layout.chromaShiftX);
    const int32_t phaseY = visible.offset.y - (chroma.offset.y << layout.chromaShiftY);
    push.chromaShift[0] += static_cast<float>(phaseX) * push.chromaScale[0];
    push.chromaShift[1] += static_cast<float>(phaseY) * push.chromaScale[1];
    return push;
}

bool supports(const Device& device, VkFormat format, VkFormatFeatureFlags2 needed) {
    return (formatFeatures(device.physicalDevice(), format, VK_IMAGE_TILING_OPTIMAL) & needed) == needed;
}

struct Planes {
    Layout layout;
    PlaneFormat luma;
    PlaneFormat chroma;
};

// How this pass holds the planes of a picture in `format` from `storage` on this device, or why
// it cannot. The one place that decides which pictures are read from a device image.
std::variant<Planes, std::string> planesFor(const Device& device, AVPixelFormat format,
                                            DecodedFrame::Storage storage) {
    const std::string name = describe(av_get_pix_fmt_name(format));
    const std::optional<Layout> layout = layoutFor(format);
    if (!layout) return name + " is not a planar Y'CbCr layout this pass reads";
    const bool onDevice = storage == DecodedFrame::Storage::DeviceImage;
    if (onDevice && deviceFormatFor(*layout) == VK_FORMAT_UNDEFINED) {
        return name + " is read from host memory only, not from a decoder's image";
    }

    // A device image's planes are always X6-style, code in the top bits, whatever ffmpeg calls
    // the format; a host plane is however ffmpeg stores it.
    Layout stored = *layout;
    stored.codeInTopBits = onDevice || layout->codeInTopBits;
    const std::optional<PlaneFormat> luma = planeFormat(stored, 1);
    const std::optional<PlaneFormat> chroma = planeFormat(stored, layout->planeCount == 2 ? 2 : 1);
    if (!luma || !chroma) return name + " stores its samples in a way no single-plane format holds";

    constexpr VkFormatFeatureFlags2 planeFeatures = VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT |
                                                    VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT |
                                                    VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if (!supports(device, luma->format, planeFeatures)) {
        return device.info().gpuName + " cannot use " + formatName(luma->format) + " as a sampled luma plane";
    }
    if (!supports(device, chroma->format, planeFeatures)) {
        return device.info().gpuName + " cannot use " + formatName(chroma->format) + " as a sampled chroma plane";
    }
    return Planes{*layout, *luma, *chroma};
}

}  // namespace

bool FrameConverter::readsDeviceImage(const Device& device, AVPixelFormat swFormat, VkFormat imageFormat) {
    const std::variant<Planes, std::string> planes = planesFor(device, swFormat, DecodedFrame::Storage::DeviceImage);
    const Planes* accepted = std::get_if<Planes>(&planes);
    return accepted != nullptr && deviceFormatFor(accepted->layout) == imageFormat;
}

FrameConverter::Picture FrameConverter::pictureOf(const DecodedFrame& frame) {
    return Picture{frame.storage(), frame.pixelFormat(), frame.extent(), frame.visible()};
}

FrameConverter::FrameConverter(Device& device, const DecodedFrame& first, const std::string& shaderDir)
    : m_device(device), m_picture(pictureOf(first)) {
    m_device.assertGpuThread("FrameConverter::FrameConverter");

    const bool onDevice = m_picture.storage == DecodedFrame::Storage::DeviceImage;
    const std::variant<Planes, std::string> planesOrRefusal = planesFor(m_device, m_picture.format, m_picture.storage);
    const Planes* planes = std::get_if<Planes>(&planesOrRefusal);
    if (planes == nullptr) {
        const std::string message = "FrameConverter: " + std::get<std::string>(planesOrRefusal);
        // The decoder asks readsDeviceImage() before decoding on the device, so a device image
        // refused here is the decoder's defect. A host picture refused is content this device
        // cannot show, not a fault of the device.
        if (onDevice) throw std::logic_error(message);
        throw WallpaperContentError(message);
    }
    const Layout& layout = planes->layout;
    const VkExtent2D coded = m_picture.extent;
    const VkRect2D visible = m_picture.visible;
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(m_device.physicalDevice(), &properties);
    const uint32_t limit = properties.limits.maxImageDimension2D;
    if (coded.width == 0 || coded.height == 0 || coded.width > limit || coded.height > limit) {
        throw WallpaperContentError("FrameConverter: a " + std::to_string(coded.width) + "x" +
                                    std::to_string(coded.height) + " picture cannot be held as an image on " +
                                    m_device.info().gpuName);
    }
    if (visible.offset.x < 0 || visible.offset.y < 0 || visible.extent.width == 0 || visible.extent.height == 0 ||
        static_cast<uint64_t>(visible.offset.x) + visible.extent.width > coded.width ||
        static_cast<uint64_t>(visible.offset.y) + visible.extent.height > coded.height) {
        throw WallpaperContentError("FrameConverter: the stream's visible rectangle lies outside its coded picture");
    }

    if (onDevice) {
        const DecodedFrame::DeviceImage image = first.deviceImage();
        if (image.format != deviceFormatFor(layout)) {
            throw std::logic_error("FrameConverter: the decoder's image format does not match its pixel format");
        }
        if ((image.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0) {
            throw std::logic_error("FrameConverter: the decoder's image cannot be copied from");
        }
    }

    m_planeCount = layout.planeCount;
    m_sampleToCode = planes->luma.sampleToCode;
    m_planes[0] = Plane{{}, visible, planes->luma.format, planes->luma.texelBytes};
    const VkRect2D chromaRegion = planeRegion(visible, layout.chromaShiftX, layout.chromaShiftY);
    for (uint32_t i = 1; i < m_planeCount; ++i) {
        m_planes[i] = Plane{{}, chromaRegion, planes->chroma.format, planes->chroma.texelBytes};
    }
    m_bindings[0] = Binding{0, VK_COMPONENT_SWIZZLE_IDENTITY};
    m_bindings[1] = Binding{layout.cbPlane, layout.cbComponent};
    m_bindings[2] = Binding{layout.crPlane, layout.crComponent};

    // Refused before anything is created, so a stream this pass cannot show costs nothing.
    (void)pushFor(first);

    if (!supports(m_device, kOutputFormat, VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT)) {
        throw WallpaperContentError("FrameConverter: " + m_device.info().gpuName + " cannot use " +
                                    formatName(kOutputFormat) + " as a colour attachment");
    }

    // Nothing below submits until clearOutput(), and that waits for its batch before returning;
    // so a throw before it leaves the GPU with nothing of ours, and only a throw from it can
    // leave a batch reading these objects.
    bool clearing = false;
    try {
        createPlanes();
        createOutput();
        createSampling();
        createPipeline(shaderDir);
        if (!onDevice) {
            VkDeviceSize bytes = 0;
            for (uint32_t i = 0; i < m_planeCount; ++i) {
                bytes += static_cast<VkDeviceSize>(m_planes[i].region.extent.width) *
                         m_planes[i].region.extent.height * m_planes[i].texelBytes;
            }
            for (Staging& staging : m_staging) {
                staging.buffer = m_device.memory().createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                                HostAccess::Write, "frame-converter-upload");
            }
        }
        clearing = true;
        clearOutput();
    } catch (...) {
        if (!clearing || m_device.safeToFree("a video converter's images and pipeline, built part way")) {
            releaseObjects();
        }
        throw;
    }

    m_frame.source = VideoSource{kOutputFormat, VK_IMAGE_TILING_OPTIMAL, visible.extent.width, visible.extent.height,
                                 kOutputUsage};
    m_frame.image = m_output.image;
    m_frame.layout = kOutputLayout;
}

FrameConverter::~FrameConverter() {
    // Every batch this converter submitted reads these, and so does every render that blitted
    // from the image; nothing here records which was last, so any pending batch may be.
    if (m_device.safeToFree("a video converter's plane images, output image, views, sampler, descriptor pool, "
                            "render pass, framebuffer, pipeline and upload buffers")) {
        releaseObjects();
    }
}

void FrameConverter::releaseObjects() noexcept {
    const VkDevice vk = m_device.device();
    if (m_pipeline) vkDestroyPipeline(vk, m_pipeline, nullptr);
    if (m_pipelineLayout) vkDestroyPipelineLayout(vk, m_pipelineLayout, nullptr);
    if (m_framebuffer) vkDestroyFramebuffer(vk, m_framebuffer, nullptr);
    if (m_renderPass) vkDestroyRenderPass(vk, m_renderPass, nullptr);
    if (m_outputView) vkDestroyImageView(vk, m_outputView, nullptr);
    if (m_descriptorPool) vkDestroyDescriptorPool(vk, m_descriptorPool, nullptr);
    if (m_setLayout) vkDestroyDescriptorSetLayout(vk, m_setLayout, nullptr);
    if (m_sampler) vkDestroySampler(vk, m_sampler, nullptr);
    for (Binding& binding : m_bindings) {
        if (binding.view) vkDestroyImageView(vk, binding.view, nullptr);
        binding.view = VK_NULL_HANDLE;
    }
    m_pipeline = VK_NULL_HANDLE;
    m_pipelineLayout = VK_NULL_HANDLE;
    m_framebuffer = VK_NULL_HANDLE;
    m_renderPass = VK_NULL_HANDLE;
    m_outputView = VK_NULL_HANDLE;
    m_descriptorPool = VK_NULL_HANDLE;
    m_setLayout = VK_NULL_HANDLE;
    m_sampler = VK_NULL_HANDLE;
    m_device.memory().destroyImage(m_output);
    for (Plane& plane : m_planes) m_device.memory().destroyImage(plane.image);
    for (Staging& staging : m_staging) m_device.memory().destroyBuffer(staging.buffer);
}

void FrameConverter::createPlanes() {
    for (uint32_t i = 0; i < m_planeCount; ++i) {
        Plane& plane = m_planes[i];
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = plane.format;
        info.extent = {plane.region.extent.width, plane.region.extent.height, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        plane.image = m_device.memory().createImage(info, "frame-converter-plane");
    }
    for (Binding& binding : m_bindings) {
        const Plane& plane = m_planes[binding.plane];
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = plane.image.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = plane.format;
        view.components.r = binding.r;
        view.subresourceRange = kWholeImage;
        if (const VkResult r = vkCreateImageView(m_device.device(), &view, nullptr, &binding.view); r != VK_SUCCESS) {
            throwVkFailure("FrameConverter: vkCreateImageView (plane)", r);
        }
    }
}

void FrameConverter::createOutput() {
    const VkExtent2D extent = m_picture.visible.extent;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = kOutputFormat;
    info.extent = {extent.width, extent.height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = kOutputUsage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    m_output = m_device.memory().createImage(info, "frame-converter-output");

    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = m_output.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = kOutputFormat;
    view.subresourceRange = kWholeImage;
    if (const VkResult r = vkCreateImageView(m_device.device(), &view, nullptr, &m_outputView); r != VK_SUCCESS) {
        throwVkFailure("FrameConverter: vkCreateImageView (output)", r);
    }

    // Whatever the image held is discarded, and it is handed on in the layout a blit reads.
    VkAttachmentDescription attachment{};
    attachment.format = kOutputFormat;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = kOutputLayout;

    VkAttachmentReference colour{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colour;

    // In: after the blits that read the last picture. Out: before the blits that read this one.
    VkSubpassDependency dependencies[2]{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

    VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    pass.attachmentCount = 1;
    pass.pAttachments = &attachment;
    pass.subpassCount = 1;
    pass.pSubpasses = &subpass;
    pass.dependencyCount = 2;
    pass.pDependencies = dependencies;
    if (const VkResult r = vkCreateRenderPass(m_device.device(), &pass, nullptr, &m_renderPass); r != VK_SUCCESS) {
        throwVkFailure("FrameConverter: vkCreateRenderPass", r);
    }

    VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    framebuffer.renderPass = m_renderPass;
    framebuffer.attachmentCount = 1;
    framebuffer.pAttachments = &m_outputView;
    framebuffer.width = extent.width;
    framebuffer.height = extent.height;
    framebuffer.layers = 1;
    if (const VkResult r = vkCreateFramebuffer(m_device.device(), &framebuffer, nullptr, &m_framebuffer);
        r != VK_SUCCESS) {
        throwVkFailure("FrameConverter: vkCreateFramebuffer", r);
    }
}

// Black, and in the layout frame() declares, before the first conversion has run.
void FrameConverter::clearOutput() {
    m_device.immediateSubmit([&](VkCommandBuffer cmd) {
        const VkImageMemoryBarrier toClear =
            imageLayoutBarrier(m_output.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                               VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &toClear);
        const VkClearColorValue black{{0.0f, 0.0f, 0.0f, 1.0f}};
        vkCmdClearColorImage(cmd, m_output.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &kWholeImage);
        const VkImageMemoryBarrier toSource =
            imageLayoutBarrier(m_output.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kOutputLayout,
                               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &toSource);
    });
}

void FrameConverter::createSampling() {
    const VkDevice vk = m_device.device();

    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (const VkResult r = vkCreateSampler(vk, &sampler, nullptr, &m_sampler); r != VK_SUCCESS) {
        throwVkFailure("FrameConverter: vkCreateSampler", r);
    }

    // A set of this pass's own, written once: the Device's descriptor tiers hand out one
    // sampled image on two of the three and wait for the GPU when it is rewritten, and this
    // pass samples three.
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
        throwVkFailure("FrameConverter: vkCreateDescriptorSetLayout", r);
    }

    const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 1;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &poolSize;
    if (const VkResult r = vkCreateDescriptorPool(vk, &pool, nullptr, &m_descriptorPool); r != VK_SUCCESS) {
        throwVkFailure("FrameConverter: vkCreateDescriptorPool", r);
    }

    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = m_descriptorPool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &m_setLayout;
    if (const VkResult r = vkAllocateDescriptorSets(vk, &allocate, &m_set); r != VK_SUCCESS) {
        throwVkFailure("FrameConverter: vkAllocateDescriptorSets", r);
    }

    VkDescriptorImageInfo images[3]{};
    VkWriteDescriptorSet writes[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        images[i].imageView = m_bindings[i].view;
        images[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = m_set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(vk, 3, writes, 0, nullptr);
}

void FrameConverter::createPipeline(const std::string& shaderDir) {
    const VkDevice vk = m_device.device();

    VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ConvertPush)};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &m_setLayout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    if (const VkResult r = vkCreatePipelineLayout(vk, &layout, nullptr, &m_pipelineLayout); r != VK_SUCCESS) {
        throwVkFailure("FrameConverter: vkCreatePipelineLayout", r);
    }

    const ShaderModule vertModule(vk, shaderDir + "/ycbcr_convert.vert.spv", "FrameConverter");
    const ShaderModule fragModule(vk, shaderDir + "/ycbcr_convert.frag.spv", "FrameConverter");

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule.get();
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule.get();
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    const VkExtent2D extent = m_picture.visible.extent;
    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height),
                              0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, extent};
    VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo rasterizer{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState attachment{};
    attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                                VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &attachment;

    VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeline.stageCount = 2;
    pipeline.pStages = stages;
    pipeline.pVertexInputState = &vertexInput;
    pipeline.pInputAssemblyState = &inputAssembly;
    pipeline.pViewportState = &viewportState;
    pipeline.pRasterizationState = &rasterizer;
    pipeline.pMultisampleState = &multisample;
    pipeline.pColorBlendState = &blend;
    pipeline.layout = m_pipelineLayout;
    pipeline.renderPass = m_renderPass;
    pipeline.subpass = 0;

    const VkResult result = vkCreateGraphicsPipelines(vk, VK_NULL_HANDLE, 1, &pipeline, nullptr, &m_pipeline);
    if (result != VK_SUCCESS) {
        // Unclassified, as the shader modules are: a rejection may be the bundle's fault.
        throw std::runtime_error("FrameConverter: vkCreateGraphicsPipelines failed with " + vkResultName(result));
    }
}

ConvertPush FrameConverter::pushFor(const DecodedFrame& frame) const {
    return pushConstantsFor(frame, *layoutFor(m_picture.format), m_sampleToCode);
}

Device::SubmitTicket FrameConverter::convert(DecodedFrame& frame) {
    m_device.assertGpuThread("FrameConverter::convert");
    if (pictureOf(frame) != m_picture) {
        throw std::invalid_argument("FrameConverter: the frame's picture differs from the one this converter was "
                                    "built for; build a new converter for it");
    }

    // Everything that can refuse the frame happens before the queue is touched. The record
    // callbacks capture one pointer besides `this`, which keeps std::function from allocating.
    if (m_picture.storage == DecodedFrame::Storage::DeviceImage) {
        struct Job {
            DecodedFrame::DeviceImage source;
            ConvertPush push;
        };
        const Job job{frame.deviceImage(), pushFor(frame)};
        return frame.submit([this, &job](VkCommandBuffer cmd) { recordDevice(cmd, job.source, job.push); },
                            VK_PIPELINE_STAGE_TRANSFER_BIT);
    }

    struct Job {
        const Staging* staging;
        ConvertPush push;
    };
    Staging& staging = m_staging[m_nextStaging];
    const Job job{&staging, pushFor(frame)};
    // The upload from two frames ago has almost always run by now; wait() returns at once then.
    m_device.wait(staging.lastUpload);
    fillStaging(frame, staging);
    const Device::SubmitTicket ticket =
        m_device.submit([this, &job](VkCommandBuffer cmd) { recordHost(cmd, *job.staging, job.push); });
    staging.lastUpload = ticket;
    m_nextStaging = (m_nextStaging + 1) % static_cast<uint32_t>(m_staging.size());
    return ticket;
}

void FrameConverter::recordDevice(VkCommandBuffer cmd, const DecodedFrame::DeviceImage& source,
                                  const ConvertPush& push) {
    // The decoder's image to a copy source, this pass's planes to copy targets. The decoder's
    // writes are made visible from wherever they were made.
    VkImageMemoryBarrier before[kMaxBarriers];
    before[0] = imageLayoutBarrier(source.image, source.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    planeBarriers(PlaneTransition::ToCopyTarget, before + 1);
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1 + m_planeCount, before);

    for (uint32_t i = 0; i < m_planeCount; ++i) {
        const VkRect2D& region = m_planes[i].region;
        VkImageCopy copy{};
        copy.srcSubresource = {kPlaneAspects[i], 0, 0, 1};
        copy.srcOffset = {region.offset.x, region.offset.y, 0};
        copy.dstSubresource = kBaseLayer;
        copy.extent = {region.extent.width, region.extent.height, 1};
        vkCmdCopyImage(cmd, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_planes[i].image.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    }

    // The decoder's image back exactly as found, ordered before whatever it submits next.
    VkImageMemoryBarrier after[kMaxBarriers];
    after[0] = imageLayoutBarrier(source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, source.layout, 0,
                                  VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    planeBarriers(PlaneTransition::ToSampled, after + 1);
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1 + m_planeCount, after);

    recordPass(cmd, push);
}

// Each plane's region, rows packed tightly, plane after plane, whatever ffmpeg's stride and its sign.
void FrameConverter::fillStaging(const DecodedFrame& frame, Staging& staging) const {
    const DecodedFrame::HostPlanes planes = frame.hostPlanes();
    if (planes.count < m_planeCount) {
        throw std::logic_error("FrameConverter: a host frame has fewer planes than its pixel format");
    }
    auto* out = static_cast<uint8_t*>(staging.buffer.mapped);
    VkDeviceSize offset = 0;
    for (uint32_t i = 0; i < m_planeCount; ++i) {
        const Plane& plane = m_planes[i];
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
            const uint8_t* row = first + stride * y;
            std::memcpy(out + offset, row, rowBytes);
            offset += rowBytes;
        }
    }
    m_device.memory().flushForHostWrite(staging.buffer);
}

void FrameConverter::planeBarriers(PlaneTransition transition, VkImageMemoryBarrier* out) const {
    for (uint32_t i = 0; i < m_planeCount; ++i) {
        const VkImage image = m_planes[i].image.image;
        // What a plane held was read by the last pass and is discarded.
        out[i] = transition == PlaneTransition::ToCopyTarget
                     ? imageLayoutBarrier(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                                          VK_ACCESS_TRANSFER_WRITE_BIT)
                     : imageLayoutBarrier(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                                          VK_ACCESS_SHADER_READ_BIT);
    }
}

void FrameConverter::recordHost(VkCommandBuffer cmd, const Staging& staging, const ConvertPush& push) {
    VkImageMemoryBarrier before[kMaxBarriers];
    planeBarriers(PlaneTransition::ToCopyTarget, before);
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, m_planeCount, before);

    VkDeviceSize offset = 0;
    for (uint32_t i = 0; i < m_planeCount; ++i) {
        const VkExtent2D extent = m_planes[i].region.extent;
        VkBufferImageCopy copy{};
        copy.bufferOffset = offset;
        copy.imageSubresource = kBaseLayer;
        copy.imageExtent = {extent.width, extent.height, 1};
        vkCmdCopyBufferToImage(cmd, staging.buffer.buffer, m_planes[i].image.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        offset += static_cast<VkDeviceSize>(extent.width) * extent.height * m_planes[i].texelBytes;
    }

    VkImageMemoryBarrier after[kMaxBarriers];
    planeBarriers(PlaneTransition::ToSampled, after);
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, m_planeCount, after);

    recordPass(cmd, push);
}

void FrameConverter::recordPass(VkCommandBuffer cmd, const ConvertPush& push) {
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = m_renderPass;
    begin.framebuffer = m_framebuffer;
    begin.renderArea = {{0, 0}, m_picture.visible.extent};
    vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &m_set, 0, nullptr);
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ConvertPush), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
}

}
