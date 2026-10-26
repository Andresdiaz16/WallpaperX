// SPDX-License-Identifier: GPL-2.0-or-later

#include "ycbcr_planes.h"

#include "../vulkan/format_features.h"
#include "../vulkan/gpu_fault.h"

extern "C" {
#include <libavutil/pixdesc.h>
}

#include <cstring>

namespace wallpaperx::ycbcr {

namespace {

uint32_t ceilShift(uint32_t value, uint32_t shift) { return (value + (1u << shift) - 1) >> shift; }

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

struct Coefficients {
    double kr, kb;
};

// The luma coefficients of every non-constant-luminance matrix ffmpeg names that can be rendered
// here, and nothing for the rest. An untagged stream gets the guess every player makes:
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

// The primaries written out as they are, with no gamut conversion.
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

// The SDR gamma transfers written out as they are. BT2020_10 and BT2020_12 are BT.709's curve;
// their primaries are refused separately.
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

// The message is built only here, so a picture that is accepted costs no allocation.
[[noreturn]] void refuse(const char* component, const std::string& why) {
    throw WallpaperContentError(std::string(component) + ": " + why);
}

bool supports(const Device& device, VkFormat format, VkFormatFeatureFlags2 needed) {
    return (formatFeatures(device.physicalDevice(), format, VK_IMAGE_TILING_OPTIMAL) & needed) == needed;
}

}  // namespace

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

VkRect2D planeRegion(VkRect2D visible, uint32_t shiftX, uint32_t shiftY) {
    const auto x = static_cast<uint32_t>(visible.offset.x);
    const auto y = static_cast<uint32_t>(visible.offset.y);
    const uint32_t left = x >> shiftX;
    const uint32_t top = y >> shiftY;
    return {{static_cast<int32_t>(left), static_cast<int32_t>(top)},
            {ceilShift(x + visible.extent.width, shiftX) - left, ceilShift(y + visible.extent.height, shiftY) - top}};
}

// Vulkan's formats hold Cb before Cr. 4:4:4 names only the formats a hardware decoder has been
// seen producing; 4:2:2 has none.
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

std::variant<Planes, std::string> planesFor(const Device& device, AVPixelFormat format, DecodedFrame::Storage storage,
                                            VkFormatFeatureFlags2 planeFeatures) {
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

    if (!supports(device, luma->format, planeFeatures)) {
        return device.info().gpuName + " cannot use " + formatName(luma->format) + " as a sampled luma plane";
    }
    if (!supports(device, chroma->format, planeFeatures)) {
        return device.info().gpuName + " cannot use " + formatName(chroma->format) + " as a sampled chroma plane";
    }
    return Planes{*layout, *luma, *chroma};
}

// The BT.2020 matrix is refused even beside accepted primaries: it is never used with any other
// gamut, and content tagged with one but not the other is more often mistagged than genuinely
// BT.709.
ColourMap colourMapFor(const DecodedFrame& frame, const Layout& layout, float sampleToCode, const char* component) {
    const FrameColour& colour = frame.colour();
    if (colour.transfer == AVCOL_TRC_SMPTE2084 || colour.transfer == AVCOL_TRC_ARIB_STD_B67) {
        refuse(component, "the stream is HDR (" + describe(av_color_transfer_name(colour.transfer)) +
               "), and this pass does no tone mapping");
    }
    if (!transferWithoutConversion(colour.transfer)) {
        refuse(component, "the stream's transfer " + describe(av_color_transfer_name(colour.transfer)) +
               " is not an SDR gamma curve, and this pass does no transfer conversion");
    }
    if (!primariesWithoutConversion(colour.primaries)) {
        refuse(component, "the stream's colour primaries " + describe(av_color_primaries_name(colour.primaries)) +
               " are not BT.709-class, and this pass does no gamut conversion");
    }
    if (colour.matrix == AVCOL_SPC_BT2020_NCL || colour.matrix == AVCOL_SPC_BT2020_CL) {
        refuse(component, "the stream is BT.2020, and this pass does no gamut conversion");
    }
    const std::optional<Coefficients> k = coefficientsFor(colour.matrix, frame.extent());
    if (!k) {
        refuse(component, "the stream's colour matrix " + describe(av_color_space_name(colour.matrix)) +
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

    ColourMap map{};
    for (int row = 0; row < 3; ++row) {
        double bias = 0.0;
        for (int column = 0; column < 3; ++column) {
            const double coefficient = m[row][column] * scale[column];
            map.matrix[column][row] = static_cast<float>(coefficient * sampleToCode);
            bias -= coefficient * offset[column];
        }
        map.offset[row] = static_cast<float>(bias);
    }
    return map;
}

// A sample sited at the first of the `factor` luma pixels its texel spans is (factor - 1) / 2 luma
// pixels from the texel's centre, which is none on an axis that is not subsampled.
std::array<float, 2> chromaSiting(AVChromaLocation location, const Layout& layout) {
    const float x = 0.5f * static_cast<float>((1u << layout.chromaShiftX) - 1);
    const float y = 0.5f * static_cast<float>((1u << layout.chromaShiftY) - 1);
    switch (location) {
        case AVCHROMA_LOC_CENTER: return {0.0f, 0.0f};
        case AVCHROMA_LOC_TOPLEFT: return {x, y};
        case AVCHROMA_LOC_TOP: return {0.0f, y};
        case AVCHROMA_LOC_BOTTOMLEFT: return {x, -y};
        case AVCHROMA_LOC_BOTTOM: return {0.0f, -y};
        default: return {x, 0.0f};
    }
}

}
