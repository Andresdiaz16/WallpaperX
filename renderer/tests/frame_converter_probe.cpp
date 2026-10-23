// SPDX-License-Identifier: GPL-2.0-or-later
//
// The colour conversion pass, and what it forced on the decoder: S13.44-S13.55.
//
// WHY THIS FILE EXISTS. FrameConverter's colour had only ever been compared against libswscale in
// scratch programs its authors did not keep. The reference here is libswscale told the matrix and
// range explicitly, with accurate rounding and no dither -- never the ffmpeg command line's default
// reading, which applied BT.601 to a BT.709 file.
//
// WHERE PIXELS ARE COMPARED. Only where the decoded planes are flat: every luma sample of a 3x3
// neighbourhood equal, and every chroma texel the filter could reach equal. Two correct converters
// may upsample chroma differently across an edge, so comparing everywhere would grade the choice of
// filter rather than the matrix; inside a flat region the colour is the matrix and range alone.
// Each comparison asserts how many pixels it compared, and that references built with the wrong
// matrix and the wrong range differ from the right one there by far more than the tolerance, so a
// fixture that cannot tell them apart fails instead of passing.
//
// MANY ENTRIES, ONE BINARY: each mode needs its own device -- video extensions stripped or not, a
// feature or a format feature hidden -- and one verdict would report unreached states as verified.

#include "renderers/video_renderer.h"
#include "video/frame_converter.h"
#include "video/video_decoder.h"
#include "video/video_pipeline.h"
#include "vulkan/device.h"
#include "vulkan/gpu_fault.h"
#include "vulkan/offscreen.h"
#include "vulkan/shader_module.h"

#include "support/check.h"
#include "support/gpu_readback.h"
#include "support/picture_counter.h"
#include "support/validation_grade.h"
#include "support/video_probe_support.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#ifdef WALLPAPERX_HAVE_SWSCALE
#include <libswscale/swscale.h>
#endif
}

// Off under a sanitizer, whose allocator a replaced operator new would bypass.
#ifndef WALLPAPERX_ALLOCATION_INSTRUMENT
#define WALLPAPERX_ALLOCATION_INSTRUMENT 1
#endif

#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using wallpaperx::DecodedFrame;
using wallpaperx::Device;
using wallpaperx::FrameColour;
using wallpaperx::FrameConverter;
using wallpaperx::OffscreenTarget;
using wallpaperx::OffscreenTargetManager;
using wallpaperx::VideoDecoder;
using wallpaperx::VideoFrame;
using wallpaperx::VideoPipeline;
using wallpaperx::VideoRenderer;
using wallpaperx::WallpaperContentError;
using wallpaperx::testing::check;
using wallpaperx::testing::fail;
using wallpaperx::testing::ScratchDir;

constexpr int kNotVerifiedExitCode = 77;
bool g_notVerified = false;

constexpr const char* kShaderDir = WALLPAPERX_SHADER_DIR;

// ------------------------------------------------------------------------- //
// Injected conditions. Each is inert until a mode turns it on.
// ------------------------------------------------------------------------- //

// Read while a Device is built: strips VK_KHR_video*, so every stream decodes in software.
bool g_hideVideoExtensions = false;
// Read while a Device is built: reports samplerYcbcrConversion as unsupported.
bool g_hideYcbcrFeature = false;
// Read on every format query: R10X6 and R10X6G10X6 lose linear filtering, which is what a device
// whose 10-bit plane formats cannot be filtered reports.
bool g_stripX6LinearFilter = false;
// The next N avcodec_receive_frame() calls report AVERROR_EXTERNAL: the hardware decoder failing
// part way through a stream on a device that still answers.
int g_receiveExternalRemaining = 0;
// The format of the last picture avcodec_receive_frame() handed back: tells where a decoder that
// is not reachable from here, VideoPipeline's, decoded.
AVPixelFormat g_lastReceivedFormat = AV_PIX_FMT_NONE;

// What ffmpeg's hardware setup last proposed, recorded by the avcodec_get_hw_frames_parameters
// interposer: tells a stream the rule refused from one ffmpeg turned away before the rule was asked.
struct HwFramesProposal {
    int calls = 0;
    int result = 0;
    AVPixelFormat swFormat = AV_PIX_FMT_NONE;
    VkFormat imageFormat = VK_FORMAT_UNDEFINED;
    bool singleImage = false;
    bool profileList = false;
};
HwFramesProposal g_hwFrames;

// Every vkQueueSubmit this executable's code makes; ffmpeg's own go through dispatch pointers.
std::atomic<int> g_queueSubmits{0};
std::atomic<int> g_shaderModulesCreated{0};

// operator new calls on a thread that has counting switched on.
thread_local bool t_countAllocations = false;
std::atomic<long> g_allocations{0};

struct CountAllocations {
    CountAllocations() { t_countAllocations = true; }
    ~CountAllocations() { t_countAllocations = false; }
};

struct HiddenVideoExtensions {
    HiddenVideoExtensions() { g_hideVideoExtensions = true; }
    ~HiddenVideoExtensions() { g_hideVideoExtensions = false; }
};

struct StrippedX6LinearFilter {
    StrippedX6LinearFilter() { g_stripX6LinearFilter = true; }
    ~StrippedX6LinearFilter() { g_stripX6LinearFilter = false; }
};

// ------------------------------------------------------------------------- //
// Fixtures
// ------------------------------------------------------------------------- //

std::string asset(const char* name) { return (std::filesystem::path(WALLPAPERX_VIDEO_ASSET_DIR) / name).string(); }

bool haveAsset(const char* name, const std::string& label) {
    if (std::filesystem::exists(asset(name))) return true;
    std::printf("%s: fixture %s was not generated (no ffmpeg command line at configure time?) -- not answerable\n",
                label.c_str(), name);
    g_notVerified = true;
    return false;
}

const char* pathName(VideoDecoder::Path path) {
    switch (path) {
        case VideoDecoder::Path::Undecided: return "Undecided";
        case VideoDecoder::Path::Hardware: return "Hardware";
        case VideoDecoder::Path::SoftwareNoDecoder: return "SoftwareNoDecoder";
        case VideoDecoder::Path::SoftwareCodec: return "SoftwareCodec";
        case VideoDecoder::Path::SoftwareRefused: return "SoftwareRefused";
        case VideoDecoder::Path::SoftwareMissingReference: return "SoftwareMissingReference";
    }
    return "?";
}

const char* orUnknown(const char* name) { return name != nullptr ? name : "?"; }

std::string describeColour(const FrameColour& colour) {
    return std::string("matrix ") + orUnknown(av_color_space_name(colour.matrix)) + ", range " +
           orUnknown(av_color_range_name(colour.range)) + ", primaries " +
           orUnknown(av_color_primaries_name(colour.primaries)) + ", transfer " +
           orUnknown(av_color_transfer_name(colour.transfer)) + ", siting " +
           orUnknown(av_chroma_location_name(colour.chromaLocation));
}

// Writes a YUV4MPEG2 file. The container carries chroma siting in its colour-space tag, and the
// frames are exactly the samples given, so a sharp chroma edge stays sharp.
bool writeY4m(const std::string& path, const char* tag, uint32_t width, uint32_t height, uint32_t chromaWidth,
              uint32_t chromaHeight, int frames, const std::function<uint8_t(int, int, uint32_t, uint32_t)>& sample) {
    std::ofstream out(path, std::ios::binary);
    out << "YUV4MPEG2 W" << width << " H" << height << " F10:1 Ip A1:1 C" << tag << "\n";
    for (int frame = 0; frame < frames; ++frame) {
        out << "FRAME\n";
        for (int plane = 0; plane < 3; ++plane) {
            const uint32_t w = plane == 0 ? width : chromaWidth;
            const uint32_t h = plane == 0 ? height : chromaHeight;
            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = 0; x < w; ++x) out.put(static_cast<char>(sample(plane, frame, x, y)));
            }
        }
    }
    return static_cast<bool>(out);
}

uint32_t ceilShift(uint32_t value, uint32_t shift) { return (value + (1u << shift) - 1) >> shift; }

// ------------------------------------------------------------------------- //
// A decoded picture held in host memory, as the reference reads it
// ------------------------------------------------------------------------- //

struct HostPicture {
    AVPixelFormat format = AV_PIX_FMT_NONE;
    VkExtent2D coded{};
    VkRect2D visible{};
    FrameColour colour;
    uint32_t shiftX = 0;
    uint32_t shiftY = 0;
    uint32_t bytes = 1;
    std::array<std::vector<uint8_t>, 3> planes;
    std::array<int, 3> stride{};

    uint32_t code(int plane, int x, int y) const {
        const uint8_t* p = planes[plane].data() + static_cast<size_t>(y) * stride[plane] + static_cast<size_t>(x) * bytes;
        return bytes == 1 ? p[0] : static_cast<uint32_t>(p[0] | (p[1] << 8));
    }
    // The chroma texels any visible pixel shares, inclusive, derived from the definition rather
    // than from the converter: the first holds the first visible pixel, the last the last one.
    int chromaLeft() const { return visible.offset.x >> shiftX; }
    int chromaRight() const { return static_cast<int>(ceilShift(visible.offset.x + visible.extent.width, shiftX)) - 1; }
    int chromaTop() const { return visible.offset.y >> shiftY; }
    int chromaBottom() const {
        return static_cast<int>(ceilShift(visible.offset.y + visible.extent.height, shiftY)) - 1;
    }
    int chromaColumns() const { return static_cast<int>(ceilShift(coded.width, shiftX)); }
    int chromaRows() const { return static_cast<int>(ceilShift(coded.height, shiftY)); }
};

// A three-plane host frame copied out, or nothing with the reason. Only the layouts a software
// decoder hands back are needed: three planes, codes in the bottom bits, little-endian.
std::optional<HostPicture> copyHostPicture(const DecodedFrame& frame, std::string& why) {
    if (frame.storage() != DecodedFrame::Storage::HostPlanes) {
        why = "the frame is not held in host memory";
        return std::nullopt;
    }
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(frame.pixelFormat());
    if (desc == nullptr || desc->nb_components < 3 || desc->comp[1].plane == desc->comp[2].plane ||
        (desc->flags & AV_PIX_FMT_FLAG_BE) != 0 || desc->comp[0].shift != 0) {
        why = std::string("the software decoder handed back ") + orUnknown(av_get_pix_fmt_name(frame.pixelFormat())) +
              ", which is not a three-plane layout the reference reads";
        return std::nullopt;
    }
    const DecodedFrame::HostPlanes planes = frame.hostPlanes();
    if (planes.count < 3) {
        why = "the host frame has fewer than three planes";
        return std::nullopt;
    }
    HostPicture picture;
    picture.format = frame.pixelFormat();
    picture.coded = frame.extent();
    picture.visible = frame.visible();
    picture.colour = frame.colour();
    picture.shiftX = desc->log2_chroma_w;
    picture.shiftY = desc->log2_chroma_h;
    picture.bytes = desc->comp[0].depth > 8 ? 2 : 1;
    for (int i = 0; i < 3; ++i) {
        if (planes.stride[i] <= 0) {
            why = "a host plane has a stride that is not positive";
            return std::nullopt;
        }
        const size_t rows = i == 0 ? picture.coded.height : ceilShift(picture.coded.height, picture.shiftY);
        picture.stride[i] = planes.stride[i];
        picture.planes[i].assign(planes.data[i], planes.data[i] + rows * static_cast<size_t>(planes.stride[i]));
    }
    return picture;
}

// Pixels of the visible rectangle whose decoded samples are locally constant; see the file header.
std::vector<uint8_t> flatMask(const HostPicture& picture) {
    const int w = static_cast<int>(picture.visible.extent.width);
    const int h = static_cast<int>(picture.visible.extent.height);
    const int x0 = picture.visible.offset.x;
    const int y0 = picture.visible.offset.y;
    const int fx = 1 << picture.shiftX;
    const int fy = 1 << picture.shiftY;
    std::vector<uint8_t> mask(static_cast<size_t>(w) * h, 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int X = x0 + x;
            const int Y = y0 + y;
            bool flat = true;
            const uint32_t luma = picture.code(0, X, Y);
            for (int dy = -1; dy <= 1 && flat; ++dy) {
                for (int dx = -1; dx <= 1 && flat; ++dx) {
                    const int sx = std::clamp(X + dx, x0, x0 + w - 1);
                    const int sy = std::clamp(Y + dy, y0, y0 + h - 1);
                    flat = picture.code(0, sx, sy) == luma;
                }
            }
            if (!flat) continue;
            // Generous: every texel within one texel-and-a-pixel either side, which covers the
            // bilinear footprint under any siting.
            const int cxLo = std::max((X - fx - 1) >> picture.shiftX, picture.chromaLeft());
            const int cxHi = std::min((X + fx + 1) >> picture.shiftX, picture.chromaRight());
            const int cyLo = std::max((Y - fy - 1) >> picture.shiftY, picture.chromaTop());
            const int cyHi = std::min((Y + fy + 1) >> picture.shiftY, picture.chromaBottom());
            const uint32_t cb = picture.code(1, cxLo, cyLo);
            const uint32_t cr = picture.code(2, cxLo, cyLo);
            for (int cy = cyLo; cy <= cyHi && flat; ++cy) {
                for (int cx = cxLo; cx <= cxHi && flat; ++cx) {
                    flat = picture.code(1, cx, cy) == cb && picture.code(2, cx, cy) == cr;
                }
            }
            mask[static_cast<size_t>(y) * w + x] = flat ? 1 : 0;
        }
    }
    return mask;
}

// ------------------------------------------------------------------------- //
// The reference
// ------------------------------------------------------------------------- //

// The "J" formats are ffmpeg's legacy spelling of full range. The reference is handed the range
// explicitly, and libswscale would override it for a J format.
AVPixelFormat withoutLegacyFullRange(AVPixelFormat format) {
    switch (format) {
        case AV_PIX_FMT_YUVJ420P: return AV_PIX_FMT_YUV420P;
        case AV_PIX_FMT_YUVJ422P: return AV_PIX_FMT_YUV422P;
        case AV_PIX_FMT_YUVJ444P: return AV_PIX_FMT_YUV444P;
        case AV_PIX_FMT_YUVJ440P: return AV_PIX_FMT_YUV440P;
        case AV_PIX_FMT_YUVJ411P: return AV_PIX_FMT_YUV411P;
        default: return format;
    }
}

#ifdef WALLPAPERX_HAVE_SWSCALE
constexpr bool kHaveReference = true;

// The visible rectangle as tightly-packed BGRA, converted by libswscale with the matrix and range
// given here and nothing read off the picture.
std::optional<std::vector<uint8_t>> referenceBgra(const HostPicture& picture, int swsMatrix, bool fullRange,
                                                  std::string& why) {
    const int w = static_cast<int>(picture.visible.extent.width);
    const int h = static_cast<int>(picture.visible.extent.height);
    if ((picture.visible.offset.x & ((1 << picture.shiftX) - 1)) != 0 ||
        (picture.visible.offset.y & ((1 << picture.shiftY) - 1)) != 0) {
        why = "the visible rectangle starts part way into a chroma texel, which libswscale cannot be handed";
        return std::nullopt;
    }
    SwsContext* context = sws_alloc_context();
    if (context == nullptr) {
        why = "sws_alloc_context failed";
        return std::nullopt;
    }
    struct Free {
        SwsContext* context;
        ~Free() { sws_freeContext(context); }
    } guard{context};
    context->src_w = w;
    context->src_h = h;
    context->src_format = withoutLegacyFullRange(picture.format);
    context->dst_w = w;
    context->dst_h = h;
    context->dst_format = AV_PIX_FMT_BGRA;
    context->src_range = fullRange ? 1 : 0;
    context->dst_range = 1;
    context->flags = SWS_POINT | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT | SWS_FULL_CHR_H_INP;
    context->dither = SWS_DITHER_NONE;
    if (sws_init_context(context, nullptr, nullptr) < 0) {
        why = "sws_init_context refused the conversion";
        return std::nullopt;
    }
    if (sws_setColorspaceDetails(context, sws_getCoefficients(swsMatrix), fullRange ? 1 : 0,
                                 sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16) < 0) {
        why = "sws_setColorspaceDetails refused the matrix and range";
        return std::nullopt;
    }
    const uint8_t* source[4]{};
    int sourceStride[4]{};
    for (int i = 0; i < 3; ++i) {
        const uint32_t sx = i == 0 ? 0 : picture.shiftX;
        const uint32_t sy = i == 0 ? 0 : picture.shiftY;
        source[i] = picture.planes[i].data() +
                    static_cast<size_t>(picture.visible.offset.y >> sy) * picture.stride[i] +
                    static_cast<size_t>(picture.visible.offset.x >> sx) * picture.bytes;
        sourceStride[i] = picture.stride[i];
    }
    // Rows padded well past four bytes a pixel: libswscale may write a SIMD block past the width.
    const int stride = ((w * 4 + 63) / 64) * 64 + 64;
    std::vector<uint8_t> buffer(static_cast<size_t>(stride) * (h + 1));
    uint8_t* destination[4]{buffer.data()};
    int destinationStride[4]{stride};
    if (sws_scale(context, source, sourceStride, 0, h, destination, destinationStride) != h) {
        why = "sws_scale did not convert every row";
        return std::nullopt;
    }
    std::vector<uint8_t> packed(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        std::memcpy(packed.data() + static_cast<size_t>(y) * w * 4, buffer.data() + static_cast<size_t>(y) * stride,
                    static_cast<size_t>(w) * 4);
    }
    return packed;
}
#else
constexpr bool kHaveReference = false;
constexpr int SWS_CS_ITU709 = 1;
constexpr int SWS_CS_ITU601 = 5;
std::optional<std::vector<uint8_t>> referenceBgra(const HostPicture&, int, bool, std::string& why) {
    why = "this build has no libswscale";
    return std::nullopt;
}
#endif

// The largest difference in B, G or R between two BGRA images, over pixels `select` accepts.
struct Agreement {
    int compared = 0;
    int worst = 0;
    int worstX = -1;
    int worstY = -1;
};

Agreement agreement(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, uint32_t width, uint32_t height,
                    const std::function<bool(uint32_t, uint32_t)>& select) {
    Agreement result;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            if (!select(x, y)) continue;
            ++result.compared;
            const size_t i = (static_cast<size_t>(y) * width + x) * 4;
            for (int c = 0; c < 3; ++c) {
                const int d = std::abs(static_cast<int>(a[i + c]) - static_cast<int>(b[i + c]));
                if (d > result.worst) {
                    result.worst = d;
                    result.worstX = static_cast<int>(x);
                    result.worstY = static_cast<int>(y);
                }
            }
        }
    }
    return result;
}

void waitReporting(Device& device, Device::SubmitTicket ticket, const char* what) {
    try {
        device.wait(ticket);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: waiting for %s did not complete: %s\n", what, e.what());
        fail("a batch this probe submitted never finished");
    }
}

std::vector<uint8_t> convertAndRead(Device& device, FrameConverter& converter, DecodedFrame& frame) {
    waitReporting(device, converter.convert(frame), "a conversion");
    const VideoFrame& out = converter.frame();
    return wallpaperx::testing::readbackImage(device, out.image, out.source.width, out.source.height);
}

// ------------------------------------------------------------------------- //
// Colour against the reference -- S13.44
// ------------------------------------------------------------------------- //

constexpr int kToleranceCodes = 1;
// Misreading the matrix moves saturated bars by 15-30 codes; a fixture whose wrong-matrix and
// wrong-range references sit closer than this to the right one cannot tell the readings apart.
constexpr int kMinDiscriminationCodes = 12;

struct ColourFixture {
    const char* file;
    AVPixelFormat format;
    AVColorSpace matrix;
    AVColorRange range;
    int swsMatrix;
    bool fullRange;
    int swsOtherMatrix;
};

// Whether the frame decoded as the fixture's recipe says; the trap is an encoder that drops a tag,
// which would make every later comparison grade an untagged stream. A harness fault, so it says so.
// CONTROL: conv_709_tv_444.mp4 re-encoded tagged smpte170m: "decoded as yuv444p with matrix
//       smpte170m ... not what its recipe makes" and "not every host-path colour fixture was graded".
bool fixtureDecodedAsRecipe(const std::string& label, const ColourFixture& fixture, const DecodedFrame& frame,
                            bool checkFormat) {
    const bool formatOk =
        !checkFormat || withoutLegacyFullRange(frame.pixelFormat()) == withoutLegacyFullRange(fixture.format);
    if (formatOk && frame.colour().matrix == fixture.matrix && frame.colour().range == fixture.range) return true;
    std::fprintf(stderr, "FAIL: %s: fixture %s decoded as %s with %s, which is not what its recipe makes\n",
                 label.c_str(), fixture.file, orUnknown(av_get_pix_fmt_name(frame.pixelFormat())),
                 describeColour(frame.colour()).c_str());
    fail("a colour fixture does not carry what its recipe tags, so this probe was not given the stream it grades");
    return false;
}

// Grades one converted picture against the reference. Returns the flat mask it compared over, or
// nothing when it could not compare at all (already reported).
std::optional<std::vector<uint8_t>> gradeColour(const std::string& label, const char* fixture, const HostPicture& picture,
                                                const std::vector<uint8_t>& converted, int swsMatrix, bool fullRange,
                                                int swsOtherMatrix) {
    const uint32_t w = picture.visible.extent.width;
    const uint32_t h = picture.visible.extent.height;
    std::string why;
    const auto right = referenceBgra(picture, swsMatrix, fullRange, why);
    const auto wrongMatrix = right ? referenceBgra(picture, swsOtherMatrix, fullRange, why) : std::nullopt;
    const auto wrongRange = wrongMatrix ? referenceBgra(picture, swsMatrix, !fullRange, why) : std::nullopt;
    if (!wrongRange) {
        std::fprintf(stderr, "FAIL: %s: no reference for %s: %s\n", label.c_str(), fixture, why.c_str());
        fail("the libswscale reference could not be built, so nothing was compared");
        return std::nullopt;
    }
    const std::vector<uint8_t> mask = flatMask(picture);
    auto flat = [&](uint32_t x, uint32_t y) { return mask[static_cast<size_t>(y) * w + x] != 0; };

    // COVERAGE-ASSERT: at least a quarter of the picture compared, and the wrong readings far
    //       from the right one where it was -- otherwise "within one code" is a claim about a
    //       handful of pixels, or about a fixture on which every reading agrees. Observed red on
    //       harness inputs: conv_709_tv.mp4 replaced by flat grey ("the wrong matrix is at most 0
    //       codes"), conv_709_tv_422.mp4 by noise ("0 flat pixels of 76800").
    const Agreement matrixGap = agreement(*right, *wrongMatrix, w, h, flat);
    const Agreement rangeGap = agreement(*right, *wrongRange, w, h, flat);
    const Agreement result = agreement(converted, *right, w, h, flat);
    if (result.compared < static_cast<int>(w * h / 4)) {
        std::fprintf(stderr, "FAIL: %s: %s has %d flat pixels of %u, too few to grade its colour\n", label.c_str(),
                     fixture, result.compared, w * h);
        fail("a colour fixture has too few flat pixels to compare");
    }
    if (matrixGap.worst < kMinDiscriminationCodes || rangeGap.worst < kMinDiscriminationCodes) {
        std::fprintf(stderr,
                     "FAIL: %s: on %s the wrong matrix is at most %d codes and the wrong range %d codes from the "
                     "right reading, so a misread tag could pass\n",
                     label.c_str(), fixture, matrixGap.worst, rangeGap.worst);
        fail("a colour fixture cannot tell the right reading of its tags from a wrong one");
    }
    if (result.worst > kToleranceCodes) {
        const size_t i = (static_cast<size_t>(result.worstY) * w + result.worstX) * 4;
        std::fprintf(stderr,
                     "FAIL: %s: %s differs from libswscale by %d codes at (%d,%d): converted BGR %u,%u,%u, reference "
                     "%u,%u,%u\n",
                     label.c_str(), fixture, result.worst, result.worstX, result.worstY, converted[i],
                     converted[i + 1], converted[i + 2], (*right)[i], (*right)[i + 1], (*right)[i + 2]);
        fail("the converted colour is not the reference's within one code");
    }
    int opaque = 0;
    for (size_t i = 3; i < converted.size(); i += 4) opaque += converted[i] == 255 ? 1 : 0;
    check(opaque == static_cast<int>(w * h), "the converted picture is not opaque everywhere");
    std::printf("%s: %s: %d flat pixels within %d code(s) of libswscale; the wrong matrix is %d codes away, the "
                "wrong range %d\n",
                label.c_str(), fixture, result.compared, result.worst, matrixGap.worst, rangeGap.worst);
    return mask;
}

// ------------------------------------------------------------------------- //
// Decoding
// ------------------------------------------------------------------------- //

struct Opened {
    std::unique_ptr<VideoDecoder> decoder;
    std::optional<DecodedFrame> frame;
};

Opened openFirst(Device& device, const std::string& file) {
    Opened opened;
    opened.decoder = std::make_unique<VideoDecoder>(device, file);
    opened.frame = opened.decoder->next();
    return opened;
}

// The file's first frame decoded in software, on a Device whose driver hides its video extensions.
std::optional<HostPicture> softwareFirstFrame(const std::string& file, std::string& why) {
    std::optional<Device> software;
    {
        HiddenVideoExtensions hidden;
        software.emplace(/*enableValidation=*/false);
    }
    VideoDecoder decoder(*software, file);
    std::optional<DecodedFrame> frame = decoder.next();
    if (!frame) {
        why = "the software decode produced no frame";
        return std::nullopt;
    }
    return copyHostPicture(*frame, why);
}

int softwareFrameCount(const std::string& file) {
    std::optional<Device> software;
    {
        HiddenVideoExtensions hidden;
        software.emplace(/*enableValidation=*/false);
    }
    VideoDecoder decoder(*software, file);
    int frames = 0;
    while (decoder.next()) ++frames;
    return frames;
}

bool videoExtensionsHidden(const Device& device, const std::string& label) {
    if (device.info().videoDecodeOps == 0) return true;
    std::fprintf(stderr, "FAIL: %s: the video extensions were not hidden from %s, so this entry would grade the "
                         "hardware path it is not about\n",
                 label.c_str(), device.info().gpuName.c_str());
    fail("the harness did not build the device this entry needs");
    return false;
}

void reportThrow(const std::string& label, const char* what, const std::exception& e) {
    std::fprintf(stderr, "FAIL: %s: %s threw: %s\n", label.c_str(), what, e.what());
    fail("a fixture could not be graded because something threw");
}

// ------------------------------------------------------------------------- //
// S13.44 / S13.45 -- colour, host path
// ------------------------------------------------------------------------- //

const ColourFixture kHostColourFixtures[] = {
    {"conv_709_tv.mp4", AV_PIX_FMT_YUV420P, AVCOL_SPC_BT709, AVCOL_RANGE_MPEG, SWS_CS_ITU709, false, SWS_CS_ITU601},
    {"conv_601_pc.mp4", AV_PIX_FMT_YUV420P, AVCOL_SPC_SMPTE170M, AVCOL_RANGE_JPEG, SWS_CS_ITU601, true, SWS_CS_ITU709},
    {"conv_709_tv_10bit.mp4", AV_PIX_FMT_YUV420P10LE, AVCOL_SPC_BT709, AVCOL_RANGE_MPEG, SWS_CS_ITU709, false,
     SWS_CS_ITU601},
    {"conv_709_tv_422.mp4", AV_PIX_FMT_YUV422P, AVCOL_SPC_BT709, AVCOL_RANGE_MPEG, SWS_CS_ITU709, false,
     SWS_CS_ITU601},
    {"conv_709_tv_444.mp4", AV_PIX_FMT_YUV444P, AVCOL_SPC_BT709, AVCOL_RANGE_MPEG, SWS_CS_ITU709, false,
     SWS_CS_ITU601},
    {"conv_709_as_601.mp4", AV_PIX_FMT_YUV420P, AVCOL_SPC_SMPTE170M, AVCOL_RANGE_MPEG, SWS_CS_ITU601, false,
     SWS_CS_ITU709},
};
constexpr int kHostColourFixtureCount = 6;

void runColourHostChecks(Device& device, const std::string& label) {
    if (!kHaveReference) {
        std::printf("%s: built without libswscale, so there is no independent reference -- not answerable\n",
                    label.c_str());
        g_notVerified = true;
        return;
    }
    if (!videoExtensionsHidden(device, label)) return;

    struct Kept {
        HostPicture picture;
        std::vector<uint8_t> converted;
        std::vector<uint8_t> mask;
    };
    std::optional<Kept> tagged709;
    std::optional<Kept> tagged601;
    int graded = 0;
    int present = 0;
    for (const ColourFixture& fixture : kHostColourFixtures) {
        if (!haveAsset(fixture.file, label)) continue;
        ++present;
        try {
            Opened opened = openFirst(device, asset(fixture.file));
            if (!opened.frame) {
                fail("a colour fixture decoded to no frames");
                continue;
            }
            if (!fixtureDecodedAsRecipe(label, fixture, *opened.frame, /*checkFormat=*/true)) continue;
            std::string why;
            std::optional<HostPicture> picture = copyHostPicture(*opened.frame, why);
            if (!picture) {
                std::fprintf(stderr, "FAIL: %s: %s: %s\n", label.c_str(), fixture.file, why.c_str());
                fail("a host frame could not be copied out for the reference");
                continue;
            }
            FrameConverter converter(device, *opened.frame, kShaderDir);
            std::vector<uint8_t> converted = convertAndRead(device, converter, *opened.frame);
            // GOAL: S13.44 -- every flat pixel within one code of libswscale, for each matrix, range,
            //       depth and subsampling in the table. Shape and guards: gradeColour, file header.
            // CONTROL: renderer/video/frame_converter.cpp, each reverted. BT.709 coefficients set
            //       to BT.601's: the four BT.709-tagged fixtures 32-33 codes off, BT.601 ones green.
            //       Limited-range luma scaled over 255: every limited fixture 36 codes off. The R16
            //       host sample scale 65535 -> 1023: the 10-bit fixture alone, 255 codes off.
            //       renderer/shaders/ycbcr_convert.frag alpha 0.5: "not opaque everywhere", all six.
            std::optional<std::vector<uint8_t>> mask = gradeColour(
                label, fixture.file, *picture, converted, fixture.swsMatrix, fixture.fullRange, fixture.swsOtherMatrix);
            if (!mask) continue;
            ++graded;
            if (std::strcmp(fixture.file, "conv_709_tv.mp4") == 0) tagged709 = Kept{*picture, converted, *mask};
            if (std::strcmp(fixture.file, "conv_709_as_601.mp4") == 0) tagged601 = Kept{*picture, converted, *mask};
        } catch (const std::exception& e) {
            reportThrow(label, fixture.file, e);
        }
    }
    // COVERAGE-ASSERT: against a literal, so dropping a fixture from the table fails here.
    if (present == kHostColourFixtureCount) {
        check(graded == kHostColourFixtureCount, "not every host-path colour fixture was graded");
    }

    // GOAL: S13.45 -- a stream tagged one way does not produce the pixels of the same samples
    //       tagged another way.
    // WHY-THIS-SHAPE: two files whose samples are identical and whose tags differ, compared with
    //       each other. The per-fixture grade already fixes each against its own reference; this
    //       states the goal directly, and asserts the samples really are identical first -- two
    //       encodes that differ would make any difference in output meaningless.
    // CONTROL: renderer/video/frame_converter.cpp, BT.709's coefficients set to BT.601's: "convert
    //       to pixels at most 0 codes apart". Reverted. The identical-samples guard went red when
    //       conv_709_tv.mp4 was swapped for flat grey.
    if (tagged709 && tagged601) {
        const bool samePlanes = tagged709->picture.planes == tagged601->picture.planes &&
                                tagged709->picture.stride == tagged601->picture.stride;
        if (!samePlanes) {
            fail("the BT.709 and BT.601 tagged fixtures do not hold the same samples, so their outputs cannot be "
                 "compared");
        } else {
            constexpr int kMinTagSeparationCodes = 15;
            const uint32_t w = tagged709->picture.visible.extent.width;
            const uint32_t h = tagged709->picture.visible.extent.height;
            const Agreement gap = agreement(tagged709->converted, tagged601->converted, w, h, [&](uint32_t x, uint32_t y) {
                return tagged709->mask[static_cast<size_t>(y) * w + x] != 0;
            });
            if (gap.worst < kMinTagSeparationCodes) {
                std::fprintf(stderr,
                             "FAIL: %s: the same samples tagged BT.709 and BT.601 convert to pixels at most %d codes "
                             "apart\n",
                             label.c_str(), gap.worst);
                fail("the colour matrix tag does not change the converted picture");
            }
            std::printf("%s: the same samples tagged BT.709 and BT.601 convert up to %d codes apart\n", label.c_str(),
                        gap.worst);
        }
    }
}

// ------------------------------------------------------------------------- //
// S13.44 -- colour, device path
// ------------------------------------------------------------------------- //

const ColourFixture kDeviceColourFixtures[] = {
    {"conv_709_tv.mp4", AV_PIX_FMT_YUV420P, AVCOL_SPC_BT709, AVCOL_RANGE_MPEG, SWS_CS_ITU709, false, SWS_CS_ITU601},
    {"conv_601_pc.mp4", AV_PIX_FMT_YUV420P, AVCOL_SPC_SMPTE170M, AVCOL_RANGE_JPEG, SWS_CS_ITU601, true, SWS_CS_ITU709},
    {"conv_hevc10.mp4", AV_PIX_FMT_YUV420P10LE, AVCOL_SPC_BT709, AVCOL_RANGE_MPEG, SWS_CS_ITU709, false,
     SWS_CS_ITU601},
    {"conv_hevc444.mp4", AV_PIX_FMT_YUV444P, AVCOL_SPC_BT709, AVCOL_RANGE_MPEG, SWS_CS_ITU709, false, SWS_CS_ITU601},
    {"conv_hevc444_10.mp4", AV_PIX_FMT_YUV444P10LE, AVCOL_SPC_BT709, AVCOL_RANGE_MPEG, SWS_CS_ITU709, false,
     SWS_CS_ITU601},
};

void runColourDeviceChecks(Device& device, const std::string& label) {
    if (!kHaveReference) {
        std::printf("%s: built without libswscale, so there is no independent reference -- not answerable\n",
                    label.c_str());
        g_notVerified = true;
        return;
    }
    if (device.info().videoDecodeOps == 0) {
        std::printf("%s: %s has no hardware decoder, so no frame reaches the device path -- not answerable\n",
                    label.c_str(), device.info().gpuName.c_str());
        g_notVerified = true;
        return;
    }
    int graded = 0;
    for (const ColourFixture& fixture : kDeviceColourFixtures) {
        if (!haveAsset(fixture.file, label)) continue;
        try {
            // WHY-THIS-SHAPE: the device image's planes cannot be read on the host without
            //       re-implementing the copy under test, so the reference is the same file decoded
            //       in software. H.264 and H.265 decoding is exactly specified; a hardware decoder
            //       that is not bit-exact makes this red, never green.
            std::string why;
            std::optional<HostPicture> reference = softwareFirstFrame(asset(fixture.file), why);
            if (!reference) {
                std::fprintf(stderr, "FAIL: %s: %s: %s\n", label.c_str(), fixture.file, why.c_str());
                fail("the software reference decode failed");
                continue;
            }
            Opened opened = openFirst(device, asset(fixture.file));
            if (!opened.frame) {
                fail("a colour fixture decoded to no frames");
                continue;
            }
            // COVERAGE-ASSERT: a fixture that does not reach the device path makes the entry a skip,
            //       so a layout the decoder stops sending to the device cannot leave it green with
            //       the others graded. Observed with renderer/video/frame_converter.cpp's 8-bit
            //       4:4:4 format refused: "conv_hevc444.mp4 took the SoftwareRefused path", exit 77.
            if (opened.frame->storage() != DecodedFrame::Storage::DeviceImage) {
                std::printf("%s: %s took the %s path on %s, so the device path is not answerable for it\n",
                            label.c_str(), fixture.file, pathName(opened.decoder->path()),
                            device.info().gpuName.c_str());
                g_notVerified = true;
                continue;
            }
            if (!fixtureDecodedAsRecipe(label, fixture, *opened.frame, /*checkFormat=*/false)) continue;
            if (withoutLegacyFullRange(reference->format) != withoutLegacyFullRange(fixture.format) ||
                opened.frame->visible().extent.width != reference->visible.extent.width ||
                opened.frame->visible().extent.height != reference->visible.extent.height) {
                fail("the software reference decode does not describe the picture the hardware decoded");
                continue;
            }
            FrameConverter converter(device, *opened.frame, kShaderDir);
            std::vector<uint8_t> converted = convertAndRead(device, converter, *opened.frame);
            const std::string name = std::string(fixture.file) + " on the device";
            // CONTROL: renderer/video/frame_converter.cpp, each reverted. BT.709 coefficients set
            //       to BT.601's: every BT.709 fixture 32-33 codes off, the BT.601 one green. R10X6
            //       luma sample scale 1023 -> 65535: P010 and 10-bit 4:4:4 alone, 255 codes off. The
            //       device 4:4:4 chroma region computed as if subsampled: both 4:4:4 fixtures alone,
            //       234 and 255 codes off. The shape guard above: renderer/video/video_decoder.cpp
            //       visible() two pixels narrower on device frames, "does not describe the picture
            //       the hardware decoded" for all five.
            if (gradeColour(label, name.c_str(), *reference, converted, fixture.swsMatrix, fixture.fullRange,
                            fixture.swsOtherMatrix)) {
                ++graded;
            }
        } catch (const std::exception& e) {
            reportThrow(label, fixture.file, e);
        }
    }
    if (graded == 0) {
        std::printf("%s: no fixture reached the device path on %s -- not answerable\n", label.c_str(),
                    device.info().gpuName.c_str());
        g_notVerified = true;
    }
}

// ------------------------------------------------------------------------- //
// S13.47 -- the visible rectangle's edges never read the codec's padding
// ------------------------------------------------------------------------- //

struct EdgeFixture {
    const char* file;
    // How many sides of the visible rectangle have coded samples beyond them.
    int paddedSides;
};

const EdgeFixture kHostEdgeFixtures[] = {
    {"edge_offset_420.mp4", 4},
    {"edge_corner_420.mp4", 2},
    {"edge_offset_422.mp4", 4},
    {"edge_offset_444.mp4", 4},
};
const EdgeFixture kDeviceEdgeFixtures[] = {
    {"edge_offset_420_hw.mp4", 4},
    {"edge_corner_420_hw.mp4", 2},
    {"edge_offset_444_hw.mp4", 4},
    {"edge_offset_444_10_hw.mp4", 4},
};

constexpr int kMinEdgePixels = 16;
constexpr int kMinPaddingContrast = 30;

// GOAL: S13.47 -- the first and last visible row and column match the reference, on a stream whose
//       coded samples beyond each cropped side differ sharply from the side's own.
// WHY-THIS-SHAPE: the fixtures pad the picture in red and crop the red away, so reading one texel
//       past the rectangle moves an edge pixel by tens of codes. A plain cropped stream is no test:
//       padding that repeats the edge changes nothing when read. So the padding's difference from
//       the edge is asserted per side first, as is the count of sides that have any.
// CONTROL: renderer/video/frame_converter.cpp, the constructor's chroma plane region, each reverted.
//       One texel wider and taller: every edge fixture red on all four edges, host and device. One
//       texel earlier on a padded side: the offset fixtures red, the corner ones green, both paths.
//       The device 4:4:4 region computed as if subsampled, or one texel into the padding: both
//       4:4:4 device fixtures red on every edge, host memory green.
//       Harness: padding smeared from the edge -> "differs by at most 0 luma and 0 Cr codes"; the
//       corner fixture swapped for a four-side crop -> "4 sides ... not 2"; a 4:4:4 recipe that
//       padded before converting misplaced its border -> "0 flat pixels, too few to grade".
// COVERAGE-ASSERT: each edge compares at least kMinEdgePixels flat pixels, and the number of padded
//       sides found must be the fixture's literal.
void gradeEdges(const std::string& label, const std::string& name, const HostPicture& picture,
                const std::vector<uint8_t>& converted, int paddedSides) {
    const uint32_t w = picture.visible.extent.width;
    const uint32_t h = picture.visible.extent.height;
    std::string why;
    const std::optional<std::vector<uint8_t>> reference = referenceBgra(picture, SWS_CS_ITU709, false, why);
    if (!reference) {
        std::fprintf(stderr, "FAIL: %s: no reference for %s: %s\n", label.c_str(), name.c_str(), why.c_str());
        fail("the libswscale reference could not be built, so nothing was compared");
        return;
    }
    const std::vector<uint8_t> mask = flatMask(picture);

    struct Edge {
        const char* name;
        bool column;
        uint32_t index;
    };
    const Edge edges[] = {{"first visible row", false, 0},
                          {"last visible row", false, h - 1},
                          {"first visible column", true, 0},
                          {"last visible column", true, w - 1}};
    for (const Edge& edge : edges) {
        const Agreement result = agreement(converted, *reference, w, h, [&](uint32_t x, uint32_t y) {
            return (edge.column ? x == edge.index : y == edge.index) && mask[static_cast<size_t>(y) * w + x] != 0;
        });
        if (result.compared < kMinEdgePixels) {
            std::fprintf(stderr, "FAIL: %s: %s: the %s has %d flat pixels, too few to grade\n", label.c_str(),
                         name.c_str(), edge.name, result.compared);
            fail("an edge fixture's edge has too few flat pixels to grade");
            continue;
        }
        if (result.worst > kToleranceCodes) {
            const size_t i = (static_cast<size_t>(result.worstY) * w + result.worstX) * 4;
            std::fprintf(stderr,
                         "FAIL: %s: %s: the %s differs from libswscale by %d codes at (%d,%d): converted BGR "
                         "%u,%u,%u, reference %u,%u,%u\n",
                         label.c_str(), name.c_str(), edge.name, result.worst, result.worstX, result.worstY,
                         converted[i], converted[i + 1], converted[i + 2], (*reference)[i], (*reference)[i + 1],
                         (*reference)[i + 2]);
            fail("an edge of the visible rectangle is tinted by samples outside it");
            continue;
        }
        std::printf("%s: %s: %s, %d flat pixels within %d code(s)\n", label.c_str(), name.c_str(), edge.name,
                    result.compared, result.worst);
    }

    // The largest step, in luma and in Cr, between a side's own samples and the coded ones past it.
    struct Side {
        const char* name;
        bool present;
        int lumaGap;
        int chromaGap;
    };
    const int x0 = picture.visible.offset.x;
    const int y0 = picture.visible.offset.y;
    const int x1 = x0 + static_cast<int>(w) - 1;
    const int y1 = y0 + static_cast<int>(h) - 1;
    auto lumaStep = [&](bool column, int inside, int outside) {
        int gap = 0;
        const int from = column ? y0 : x0;
        const int to = column ? y1 : x1;
        for (int t = from; t <= to; ++t) {
            const int a = static_cast<int>(column ? picture.code(0, inside, t) : picture.code(0, t, inside));
            const int b = static_cast<int>(column ? picture.code(0, outside, t) : picture.code(0, t, outside));
            gap = std::max(gap, std::abs(a - b));
        }
        return gap;
    };
    auto chromaStep = [&](bool column, int inside, int outside) {
        int gap = 0;
        const int from = column ? picture.chromaTop() : picture.chromaLeft();
        const int to = column ? picture.chromaBottom() : picture.chromaRight();
        for (int t = from; t <= to; ++t) {
            const int a = static_cast<int>(column ? picture.code(2, inside, t) : picture.code(2, t, inside));
            const int b = static_cast<int>(column ? picture.code(2, outside, t) : picture.code(2, t, outside));
            gap = std::max(gap, std::abs(a - b));
        }
        return gap;
    };
    std::vector<Side> sides;
    if (x0 > 0) {
        sides.push_back({"left", true, lumaStep(true, x0, x0 - 1),
                         picture.chromaLeft() > 0 ? chromaStep(true, picture.chromaLeft(), picture.chromaLeft() - 1)
                                                  : 0});
    }
    if (x1 + 1 < static_cast<int>(picture.coded.width)) {
        sides.push_back({"right", true, lumaStep(true, x1, x1 + 1),
                         picture.chromaRight() + 1 < picture.chromaColumns()
                             ? chromaStep(true, picture.chromaRight(), picture.chromaRight() + 1)
                             : 0});
    }
    if (y0 > 0) {
        sides.push_back({"top", true, lumaStep(false, y0, y0 - 1),
                         picture.chromaTop() > 0 ? chromaStep(false, picture.chromaTop(), picture.chromaTop() - 1)
                                                 : 0});
    }
    if (y1 + 1 < static_cast<int>(picture.coded.height)) {
        sides.push_back({"bottom", true, lumaStep(false, y1, y1 + 1),
                         picture.chromaBottom() + 1 < picture.chromaRows()
                             ? chromaStep(false, picture.chromaBottom(), picture.chromaBottom() + 1)
                             : 0});
    }
    if (static_cast<int>(sides.size()) != paddedSides) {
        std::fprintf(stderr, "FAIL: %s: %s has coded samples beyond %zu sides of its visible rectangle, not %d\n",
                     label.c_str(), name.c_str(), sides.size(), paddedSides);
        fail("an edge fixture is not cropped where its recipe crops it");
    }
    for (const Side& side : sides) {
        if (side.lumaGap < kMinPaddingContrast || side.chromaGap < kMinPaddingContrast) {
            std::fprintf(stderr,
                         "FAIL: %s: %s: the padding past the %s side differs from it by at most %d luma and %d Cr "
                         "codes\n",
                         label.c_str(), name.c_str(), side.name, side.lumaGap, side.chromaGap);
            fail("an edge fixture's padding does not differ from its edge, so reading it would change nothing");
        }
    }
}

void runEdgeChecks(Device& device, const std::string& label, bool onDevice) {
    if (!kHaveReference) {
        std::printf("%s: built without libswscale, so there is no independent reference -- not answerable\n",
                    label.c_str());
        g_notVerified = true;
        return;
    }
    if (onDevice && device.info().videoDecodeOps == 0) {
        std::printf("%s: %s has no hardware decoder, so no frame reaches the device path -- not answerable\n",
                    label.c_str(), device.info().gpuName.c_str());
        g_notVerified = true;
        return;
    }
    if (!onDevice && !videoExtensionsHidden(device, label)) return;

    const ColourFixture tags{"", AV_PIX_FMT_NONE, AVCOL_SPC_BT709, AVCOL_RANGE_MPEG, SWS_CS_ITU709, false,
                             SWS_CS_ITU601};
    int graded = 0;
    const auto run = [&](const EdgeFixture& fixture) {
        if (!haveAsset(fixture.file, label)) return;
        try {
            std::string why;
            std::optional<HostPicture> picture;
            if (onDevice) picture = softwareFirstFrame(asset(fixture.file), why);
            Opened opened = openFirst(device, asset(fixture.file));
            if (!opened.frame) {
                fail("an edge fixture decoded to no frames");
                return;
            }
            const bool deviceFrame = opened.frame->storage() == DecodedFrame::Storage::DeviceImage;
            // COVERAGE-ASSERT: a device fixture that falls back to software makes the entry a skip,
            //       not a pass on the others. Observed with renderer/video/frame_converter.cpp's 8-bit
            //       4:4:4 format refused: "edge_offset_444_hw.mp4 took the SoftwareRefused path", exit 77.
            if (onDevice && !deviceFrame) {
                std::printf("%s: %s took the %s path, so the device path is not answerable for it\n", label.c_str(),
                            fixture.file, pathName(opened.decoder->path()));
                g_notVerified = true;
                return;
            }
            ColourFixture named = tags;
            named.file = fixture.file;
            if (!fixtureDecodedAsRecipe(label, named, *opened.frame, /*checkFormat=*/false)) return;
            if (!onDevice) picture = copyHostPicture(*opened.frame, why);
            if (!picture) {
                std::fprintf(stderr, "FAIL: %s: %s: %s\n", label.c_str(), fixture.file, why.c_str());
                fail("an edge fixture's samples could not be copied out for the reference");
                return;
            }
            const VkRect2D visible = opened.frame->visible();
            if (visible.offset.x != picture->visible.offset.x || visible.offset.y != picture->visible.offset.y ||
                visible.extent.width != picture->visible.extent.width ||
                visible.extent.height != picture->visible.extent.height) {
                fail("the reference decode and the graded decode disagree on the visible rectangle");
                return;
            }
            FrameConverter converter(device, *opened.frame, kShaderDir);
            std::vector<uint8_t> converted = convertAndRead(device, converter, *opened.frame);
            gradeEdges(label, std::string(fixture.file) + (deviceFrame ? " on the device" : " from host memory"),
                       *picture, converted, fixture.paddedSides);
            ++graded;
        } catch (const std::exception& e) {
            reportThrow(label, fixture.file, e);
        }
    };
    if (onDevice) {
        for (const EdgeFixture& fixture : kDeviceEdgeFixtures) run(fixture);
    } else {
        for (const EdgeFixture& fixture : kHostEdgeFixtures) run(fixture);
    }
    if (onDevice && graded == 0) {
        std::printf("%s: no edge fixture reached the device path -- not answerable\n", label.c_str());
        g_notVerified = true;
    }
}

// ------------------------------------------------------------------------- //
// S13.46 -- chroma siting follows the tag and the subsampling factor
// ------------------------------------------------------------------------- //

struct SitingCase {
    const char* what;
    const char* y4mTag;
    AVPixelFormat format;
    uint32_t shiftX;
    uint32_t shiftY;
    bool acrossRows;
    AVChromaLocation tag;
    // Luma pixels from the first pixel a chroma texel spans to where its sample sits.
    double samplePhase;
};

constexpr SitingCase kSitingCases[] = {
    {"4:2:0 sited left, across columns", "420mpeg2", AV_PIX_FMT_YUV420P, 1, 1, false, AVCHROMA_LOC_LEFT, 0.0},
    {"4:2:0 sited centre, across columns", "420jpeg", AV_PIX_FMT_YUV420P, 1, 1, false, AVCHROMA_LOC_CENTER, 0.5},
    {"4:1:1 untagged and so sited left, across columns", "411", AV_PIX_FMT_YUV411P, 2, 0, false,
     AVCHROMA_LOC_UNSPECIFIED, 0.0},
    {"4:2:0 sited top-left, across rows", "420paldv", AV_PIX_FMT_YUV420P, 1, 1, true, AVCHROMA_LOC_TOPLEFT, 0.0},
    {"4:2:0 sited left and so centred vertically, across rows", "420mpeg2", AV_PIX_FMT_YUV420P, 1, 1, true,
     AVCHROMA_LOC_LEFT, 0.5},
};
constexpr int kSitingCaseCount = 5;

// GOAL: S13.46 -- a sharp chroma edge blends across exactly the luma pixels its siting and
//       subsampling factor put it at: a sample sited at a texel's first pixel is co-sited with it,
//       one sited centrally sits (factor - 1) / 2 pixels in, and linear interpolation between two
//       neighbouring samples gives each pixel between them a weight from its distance.
// WHY-THIS-SHAPE: the expected weights come from where the samples sit, not from the converter's
//       shift formula, so a formula that is wrong for one factor cannot agree with itself here.
//       4:1:1 is in the table because a half-pixel shift is right for 4:2:0 and wrong for 4:1:1 --
//       the defect this goal exists for. Written as YUV4MPEG2 at runtime: the container carries the
//       siting tag and the samples verbatim, where any lossy codec would soften the edge being
//       measured. The tag reaching the frame is asserted before anything is measured.
// CONTROL: renderer/video/frame_converter.cpp chromaShift(), each reverted. x hard-coded to half a
//       pixel: 4:1:1 alone red, its ramp one pixel late. CENTER given the left shift: the centre
//       case red. TOPLEFT's y negated: the top-left case red. Harness: the centre case written as
//       420paldv -> "does not carry the siting"; Cr 120/136 -> "too faint to locate".
void runSitingChecks(Device& device, const std::string& label) {
    ScratchDir scratch("wallpaperx-converter");
    if (!scratch.ok()) {
        fail("could not make a scratch directory, so chroma siting was not exercised");
        return;
    }
    constexpr uint32_t kEdgeTexel = 8;
    constexpr uint32_t kAlong = 64;
    constexpr uint32_t kAcross = 8;
    constexpr double kTolerance = 0.04;
    int graded = 0;
    for (size_t n = 0; n < std::size(kSitingCases); ++n) {
        const SitingCase& c = kSitingCases[n];
        const uint32_t width = c.acrossRows ? kAcross : kAlong;
        const uint32_t height = c.acrossRows ? kAlong : kAcross;
        const uint32_t factor = 1u << (c.acrossRows ? c.shiftY : c.shiftX);
        const std::string file = scratch.file(("siting" + std::to_string(n) + ".y4m").c_str());
        const bool written = writeY4m(file, c.y4mTag, width, height, ceilShift(width, c.shiftX),
                                      ceilShift(height, c.shiftY), 1, [&](int plane, int, uint32_t x, uint32_t y) {
                                          if (plane != 2) return uint8_t{128};
                                          return (c.acrossRows ? y : x) < kEdgeTexel ? uint8_t{68} : uint8_t{188};
                                      });
        if (!written) {
            fail("could not write a siting fixture");
            continue;
        }
        try {
            Opened opened = openFirst(device, file);
            if (!opened.frame) {
                fail("a siting fixture decoded to no frames");
                continue;
            }
            if (opened.frame->pixelFormat() != c.format || opened.frame->colour().chromaLocation != c.tag) {
                std::fprintf(stderr, "FAIL: %s: the %s fixture decoded as %s with %s\n", label.c_str(), c.what,
                             orUnknown(av_get_pix_fmt_name(opened.frame->pixelFormat())),
                             describeColour(opened.frame->colour()).c_str());
                fail("a siting fixture does not carry the siting its container tags");
                continue;
            }
            FrameConverter converter(device, *opened.frame, kShaderDir);
            const std::vector<uint8_t> converted = convertAndRead(device, converter, *opened.frame);
            const auto red = [&](uint32_t p) {
                const uint32_t x = c.acrossRows ? width / 2 : p;
                const uint32_t y = c.acrossRows ? p : height / 2;
                return static_cast<double>(converted[(static_cast<size_t>(y) * width + x) * 4 + 2]);
            };
            const double low = red(0);
            const double high = red(kAlong - 1);
            if (high - low < 100.0) {
                fail("a siting fixture's chroma edge is too faint in the output to locate");
                continue;
            }
            const double lastLowSample = (kEdgeTexel - 1) * factor + c.samplePhase;
            double worst = 0.0;
            std::string ramp;
            for (uint32_t p = (kEdgeTexel - 2) * factor; p <= (kEdgeTexel + 1) * factor; ++p) {
                const double expected = std::clamp((p - lastLowSample) / factor, 0.0, 1.0);
                const double measured = (red(p) - low) / (high - low);
                worst = std::max(worst, std::abs(measured - expected));
                char entry[48];
                std::snprintf(entry, sizeof entry, " %u:%.2f/%.2f", p, measured, expected);
                ramp += entry;
            }
            if (worst > kTolerance) {
                std::fprintf(stderr, "FAIL: %s: %s: blend weight measured/expected by pixel:%s\n", label.c_str(), c.what,
                             ramp.c_str());
                fail("a sharp chroma edge does not blend where its siting and subsampling put it");
                continue;
            }
            ++graded;
            std::printf("%s: %s: blend within %.3f of where its samples sit\n", label.c_str(), c.what, worst);
        } catch (const std::exception& e) {
            reportThrow(label, c.what, e);
        }
    }
    check(graded == kSitingCaseCount, "not every chroma siting case was graded");
}

// ------------------------------------------------------------------------- //
// S13.48 -- unsupported colour is refused, not shown wrong
// ------------------------------------------------------------------------- //

struct TagCase {
    const char* file;
    AVColorPrimaries primaries;
    AVColorTransferCharacteristic transfer;
    AVColorSpace matrix;
    bool refused;
    const char* what;
};

constexpr TagCase kTagCases[] = {
    {"tag_p3.mp4", AVCOL_PRI_SMPTE432, AVCOL_TRC_BT709, AVCOL_SPC_BT709, true, "P3 (smpte432) primaries"},
    {"tag_bt470m.mp4", AVCOL_PRI_BT470M, AVCOL_TRC_BT709, AVCOL_SPC_BT709, true, "bt470m primaries"},
    {"tag_linear.mp4", AVCOL_PRI_BT709, AVCOL_TRC_LINEAR, AVCOL_SPC_BT709, true, "a linear transfer"},
    {"tag_pq.mp4", AVCOL_PRI_BT709, AVCOL_TRC_SMPTE2084, AVCOL_SPC_BT709, true, "the PQ transfer"},
    {"tag_hlg.mp4", AVCOL_PRI_BT709, AVCOL_TRC_ARIB_STD_B67, AVCOL_SPC_BT709, true, "the HLG transfer"},
    {"tag_bt2020.mp4", AVCOL_PRI_BT2020, AVCOL_TRC_BT2020_10, AVCOL_SPC_BT2020_NCL, true, "BT.2020"},
    {"tag_bt2020_matrix.mp4", AVCOL_PRI_BT709, AVCOL_TRC_BT709, AVCOL_SPC_BT2020_NCL, true,
     "the BT.2020 matrix beside BT.709 primaries"},
    {"tag_bt709.mp4", AVCOL_PRI_BT709, AVCOL_TRC_BT709, AVCOL_SPC_BT709, false, "BT.709"},
    {"tag_srgb.mp4", AVCOL_PRI_BT709, AVCOL_TRC_IEC61966_2_1, AVCOL_SPC_BT709, false, "the sRGB transfer"},
    {"tag_smpte170m.mp4", AVCOL_PRI_SMPTE170M, AVCOL_TRC_SMPTE170M, AVCOL_SPC_SMPTE170M, false, "SMPTE 170M"},
    {"tag_bt2020_10_transfer.mp4", AVCOL_PRI_BT709, AVCOL_TRC_BT2020_10, AVCOL_SPC_BT709, false,
     "the bt2020-10 transfer beside BT.709 primaries"},
};
constexpr int kRefusedTagCases = 7;
constexpr int kAcceptedTagCases = 4;

// GOAL: S13.48 -- primaries and transfer are allowlists: each refused tag raises
//       WallpaperContentError from the constructor, each accepted one converts.
// WHY-THIS-SHAPE: each refused case changes one tag against an otherwise accepted stream, so a
//       refusal cannot be earned by a different tag. The tags reaching FrameColour are asserted
//       first: libx264 given -color_primaries drops it silently, and a refusal check on an untagged
//       stream would be grading nothing -- which is exactly what the first recipes produced.
// CONTROL: renderer/video/frame_converter.cpp, reverted. P3 and bt470m allowed, LINEAR allowed,
//       SMPTE170M primaries and the sRGB and bt2020-10 transfers removed: six named reds. PQ, HLG,
//       BT.2020 primaries and a BT.2020 matrix allowed: four more. The primaries refusal thrown as
//       std::runtime_error: "raised something other than WallpaperContentError". Harness: tag_p3.mp4
//       encoded with -color_primaries -> "decoded with ... primaries unknown". Deleting the explicit
//       HDR or BT.2020-matrix refusal alone stays green: the allowlists refuse the same streams.
// COVERAGE-ASSERT: literal counts of cases reached, refused and accepted.
void runRefusalChecks(Device& device, const std::string& label) {
    int reached = 0;
    int refused = 0;
    int accepted = 0;
    int present = 0;
    for (const TagCase& c : kTagCases) {
        if (!haveAsset(c.file, label)) continue;
        ++present;
        try {
            Opened opened = openFirst(device, asset(c.file));
            if (!opened.frame) {
                fail("a tag fixture decoded to no frames");
                continue;
            }
            const FrameColour& colour = opened.frame->colour();
            if (colour.primaries != c.primaries || colour.transfer != c.transfer || colour.matrix != c.matrix) {
                std::fprintf(stderr, "FAIL: %s: the fixture for %s decoded with %s\n", label.c_str(), c.what,
                             describeColour(colour).c_str());
                fail("a tag fixture does not carry the tags its recipe sets");
                continue;
            }
            ++reached;
            try {
                FrameConverter converter(device, *opened.frame, kShaderDir);
                if (c.refused) {
                    std::fprintf(stderr, "FAIL: %s: a stream tagged with %s was accepted\n", label.c_str(), c.what);
                    fail("colour this pass cannot render was accepted");
                    continue;
                }
                waitReporting(device, converter.convert(*opened.frame), c.what);
                ++accepted;
            } catch (const WallpaperContentError& e) {
                if (c.refused) {
                    ++refused;
                } else {
                    std::fprintf(stderr, "FAIL: %s: a stream tagged with %s was refused: %s\n", label.c_str(), c.what,
                                 e.what());
                    fail("colour this pass renders was refused");
                }
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "FAIL: %s: the stream tagged with %s raised something other than "
                                 "WallpaperContentError: %s\n",
                         label.c_str(), c.what, e.what());
            fail("a tag was refused with the wrong exception, or the fixture could not be decoded");
        }
    }
    if (present == static_cast<int>(std::size(kTagCases))) {
        check(reached == kRefusedTagCases + kAcceptedTagCases, "not every tag case reached the converter");
        check(refused == kRefusedTagCases, "not every unsupported tag was refused as content");
        check(accepted == kAcceptedTagCases, "not every supported tag converted");
    }
    std::printf("%s: %d tag cases reached the converter: %d refused as content, %d converted\n", label.c_str(),
                reached, refused, accepted);
}

// ------------------------------------------------------------------------- //
// S13.49 -- the output is what VideoRenderer takes, from construction
// S13.51 -- the shader loader refuses a short read (the module half)
// ------------------------------------------------------------------------- //

// A file that reports more bytes than a read returns, which is what a shader file truncated between
// sizing and reading looks like. sysfs attributes report a page and hold a line.
constexpr const char* kShortReadFile = "/sys/devices/system/cpu/online";

bool shortReadAvailable(const std::string& label) {
    const int fd = ::open(kShortReadFile, O_RDONLY);
    if (fd < 0) {
        std::printf("%s: %s cannot be opened, so no short read can be produced -- not answerable\n", label.c_str(),
                    kShortReadFile);
        return false;
    }
    const off_t reported = ::lseek(fd, 0, SEEK_END);
    ::lseek(fd, 0, SEEK_SET);
    char buffer[8192];
    ssize_t total = 0;
    for (;;) {
        const ssize_t got = ::read(fd, buffer, sizeof buffer);
        if (got <= 0) break;
        total += got;
    }
    ::close(fd);
    if (reported > 0 && reported % 4 == 0 && total < reported) return true;
    std::printf("%s: %s reports %lld bytes and reads %zd, which is not a short read of a word-sized file -- not "
                "answerable\n",
                label.c_str(), kShortReadFile, static_cast<long long>(reported), total);
    return false;
}

void runOutputChecks(Device& device, const std::string& label) {
    ScratchDir scratch("wallpaperx-converter");
    if (!scratch.ok()) {
        fail("could not make a scratch directory, so the output was not exercised");
        return;
    }
    constexpr uint32_t kWidth = 64;
    constexpr uint32_t kHeight = 32;
    const std::string file = scratch.file("output.y4m");
    if (!writeY4m(file, "420mpeg2", kWidth, kHeight, kWidth / 2, kHeight / 2, 1,
                  [](int plane, int, uint32_t x, uint32_t y) {
                      if (plane == 0) return static_cast<uint8_t>(40 + 2 * x + y);
                      if (plane == 1) return static_cast<uint8_t>(90 + 3 * y);
                      return static_cast<uint8_t>(80 + 4 * x);
                  })) {
        fail("could not write the output fixture");
        return;
    }

    Opened opened = openFirst(device, file);
    if (!opened.frame) {
        fail("the output fixture decoded to no frames");
        return;
    }
    FrameConverter converter(device, *opened.frame, kShaderDir);
    const VideoFrame& out = converter.frame();
    const uint32_t w = out.source.width;
    const uint32_t h = out.source.height;

    // GOAL: S13.49 -- before any convert(), the image already is what frame() declares: that
    //       declaration, in TRANSFER_SRC_OPTIMAL, and cleared to opaque black.
    // WHY-THIS-SHAPE: read back before the first conversion, because every later state is written
    //       by the pass and would hide a constructor that never cleared or transitioned. The copy
    //       out names TRANSFER_SRC_OPTIMAL, so an image left in another layout is reported by the
    //       validation layer rather than by the pixels, which a zero-filled allocation can fake.
    // CONTROL: renderer/video/frame_converter.cpp, each reverted. Clear colour 0.2 red: "2048 pixels
    //       ... not opaque black". clearOutput() not called: the same, plus VUID-vkCmdDraw-None-09600.
    //       Output layout SHADER_READ_ONLY, format R8G8B8A8, TRANSFER_SRC dropped, width and height
    //       swapped: each reddens its own named check. The layout also draws validation errors; the
    //       other three break the render or the readback after it.
    check(out.layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, "the converter's frame is not declared TRANSFER_SRC");
    check(out.source.format == VK_FORMAT_B8G8R8A8_UNORM && out.source.tiling == VK_IMAGE_TILING_OPTIMAL,
          "the converter's frame is not B8G8R8A8_UNORM optimal");
    check(w == kWidth && h == kHeight, "the converter's frame is not the visible rectangle's size");
    check((out.source.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0, "the converter's frame cannot be blitted from");
    const std::vector<uint8_t> initial = wallpaperx::testing::readbackImage(device, out.image, w, h);
    int notBlack = 0;
    for (size_t i = 0; i < initial.size(); i += 4) {
        notBlack += (initial[i] | initial[i + 1] | initial[i + 2]) != 0 || initial[i + 3] != 255 ? 1 : 0;
    }
    if (notBlack != 0) {
        std::fprintf(stderr, "FAIL: %s: %d pixels of the unconverted output are not opaque black\n", label.c_str(),
                     notBlack);
        fail("the converter's image is not cleared before its first conversion");
    }

    // GOAL: S13.49 -- VideoRenderer accepts that frame as it stands, before and after a conversion,
    //       and a render shows exactly what the conversion wrote.
    // WHY-THIS-SHAPE: the target is the frame's own size, so the blit is one to one and the shown
    //       picture can be compared with the converted one directly. The converted readback is
    //       asserted to hold something other than black first, so "shows black before" is not
    //       satisfied by an output nothing ever writes.
    // CONTROL: renderer/renderers/video_renderer.cpp blit destination flipped vertically: "differs
    //       from the converted frame by 127 codes". renderer/video/frame_converter.cpp recordHost()
    //       without its pass: "the conversion left the output mostly black". Both reverted.
    OffscreenTargetManager targets(device);
    const OffscreenTarget& target = targets.create(/*monitorId=*/1, w, h);
    std::optional<VideoRenderer> renderer;
    try {
        renderer.emplace(device, out);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s: VideoRenderer refused the unconverted frame: %s\n", label.c_str(), e.what());
        fail("VideoRenderer does not accept what the converter hands out before its first conversion");
        return;
    }
    targets.render(*renderer, target);
    const std::vector<uint8_t> shownBefore = wallpaperx::testing::reclaimAndReadback(device, targets, target);
    int shownNotBlack = 0;
    for (size_t i = 0; i < shownBefore.size(); i += 4) {
        shownNotBlack += (shownBefore[i] | shownBefore[i + 1] | shownBefore[i + 2]) != 0 ? 1 : 0;
    }
    check(shownNotBlack == 0, "a render of the unconverted frame did not show black");

    waitReporting(device, converter.convert(*opened.frame), "the output fixture's conversion");
    const std::vector<uint8_t> converted = wallpaperx::testing::readbackImage(device, out.image, w, h);
    int convertedNotBlack = 0;
    for (size_t i = 0; i < converted.size(); i += 4) {
        convertedNotBlack += (converted[i] | converted[i + 1] | converted[i + 2]) != 0 ? 1 : 0;
    }
    check(convertedNotBlack > static_cast<int>(w * h / 2), "the conversion left the output mostly black");
    try {
        renderer->show(converter.frame());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s: VideoRenderer refused the converted frame: %s\n", label.c_str(), e.what());
        fail("VideoRenderer does not accept what the converter hands out after a conversion");
        return;
    }
    targets.render(*renderer, target);
    const std::vector<uint8_t> shownAfter = wallpaperx::testing::reclaimAndReadback(device, targets, target);
    const Agreement shown = agreement(shownAfter, converted, w, h, [](uint32_t, uint32_t) { return true; });
    if (shown.worst > kToleranceCodes) {
        std::fprintf(stderr, "FAIL: %s: the rendered target differs from the converted frame by %d codes at (%d,%d)\n",
                     label.c_str(), shown.worst, shown.worstX, shown.worstY);
        fail("a render does not show the converted picture");
    }
    device.waitForSubmissions();

    // GOAL: S13.51 -- a ShaderModule built from a file that reads short throws before the driver
    //       is handed anything.
    // CONTROL: renderer/vulkan/shader_module.cpp, readSpirv's short-read test replaced by `false`:
    //       both lines red, and VUID-VkShaderModuleCreateInfo-pCode-08738 from the layer. Reverted.
    if (shortReadAvailable(label)) {
        const int before = g_shaderModulesCreated.load();
        bool refused = false;
        try {
            wallpaperx::ShaderModule module(device.device(), kShortReadFile, "frame_converter_probe");
        } catch (const std::runtime_error&) {
            refused = true;
        }
        check(refused, "a shader module was built from a file that read short");
        check(g_shaderModulesCreated.load() == before, "a file that read short was handed to vkCreateShaderModule");
    } else {
        g_notVerified = true;
    }
    std::printf("%s: %ux%u output declared and cleared before conversion; rendered within %d code(s) after\n",
                label.c_str(), w, h, shown.worst);
}

// ------------------------------------------------------------------------- //
// S13.50 -- a host frame costs no allocation
// ------------------------------------------------------------------------- //

#if WALLPAPERX_ALLOCATION_INSTRUMENT
constexpr int kAllocationFrames = 12;
constexpr int kAllocationWarmUp = 2;

struct AllocationRun {
    bool ran = false;
    long constructor = 0;
    long converting = 0;
    int measured = 0;
};

// Builds a converter for the file's first frame and converts every frame, counting operator new on
// this thread across the constructor and across each convert() after the warm-up -- never across
// the decode, the wait or the readback, about which the header promises nothing.
AllocationRun measureConversions(Device& device, const std::string& file) {
    AllocationRun run;
    VideoDecoder decoder(device, file);
    std::vector<DecodedFrame> frames;
    while (std::optional<DecodedFrame> frame = decoder.next()) frames.push_back(std::move(*frame));
    if (frames.size() != kAllocationFrames || frames[0].storage() != DecodedFrame::Storage::HostPlanes) return run;
    std::optional<FrameConverter> converter;
    {
        g_allocations = 0;
        CountAllocations counting;
        converter.emplace(device, frames[0], kShaderDir);
        run.constructor = g_allocations.load();
    }
    for (int i = 0; i < kAllocationWarmUp; ++i) {
        waitReporting(device, converter->convert(frames[i]), "a warm-up conversion");
    }
    g_allocations = 0;
    for (int i = kAllocationWarmUp; i < kAllocationFrames; ++i) {
        Device::SubmitTicket ticket;
        {
            CountAllocations counting;
            ticket = converter->convert(frames[i]);
        }
        ++run.measured;
        waitReporting(device, ticket, "a measured conversion");
    }
    run.converting = g_allocations.load();
    device.waitForSubmissions();
    run.ran = true;
    return run;
}
#endif

void runAllocationChecks(Device& device, const std::string& label) {
#if WALLPAPERX_ALLOCATION_INSTRUMENT
    ScratchDir scratch("wallpaperx-converter");
    if (!scratch.ok()) {
        fail("could not make a scratch directory, so allocation was not measured");
        return;
    }
    constexpr uint32_t kWidth = 64;
    constexpr uint32_t kHeight = 32;
    const std::string file = scratch.file("allocation.y4m");
    if (!writeY4m(file, "420mpeg2", kWidth, kHeight, kWidth / 2, kHeight / 2, kAllocationFrames,
                  [](int plane, int frame, uint32_t x, uint32_t y) {
                      return static_cast<uint8_t>(60 + plane * 20 + frame * 7 + x + y);
                  })) {
        fail("could not write the allocation fixture");
        return;
    }

    // The same conversions under the validation layer, so the layer grades them; not counted.
    if (!measureConversions(device, file).ran) {
        fail("the allocation fixture did not decode to its twelve host frames");
        return;
    }

    // GOAL: S13.50 -- convert() on a host frame makes no operator new call, once the converter has
    //       been through both of its upload buffers.
    // WHY-THIS-SHAPE: counted through a replaced global operator new, on a second Device built
    //       without the validation layer. The layer is C++ and its own bookkeeping reaches the
    //       replaced operator new from inside vkQueueSubmit: measured at 147 calls per conversion
    //       with the layer loaded and 0 without, on both drivers. Counting only callers inside this
    //       executable would miss allocations libstdc++ makes out of line on the converter's behalf.
    // CONTROL: renderer/video/frame_converter.cpp convert() building a 40-character std::string on
    //       the host path: "10 host-frame conversions made 10 operator new calls". Reverted. Harness:
    //       CountAllocations never switching counting on -> "saw nothing while the converter was built".
    // COVERAGE-ASSERT: the constructor, measured the same way, must count allocations -- otherwise
    //       a counter that sees nothing on this thread would report zero here as well.
    AllocationRun counted;
    {
        Device unlayered(/*enableValidation=*/false);
        if (unlayered.validationLayerActive()) {
            std::printf("%s: a validation layer is loaded into a device built without one (VK_INSTANCE_LAYERS?), "
                        "and its allocations cannot be told from the converter's -- not answerable\n",
                        label.c_str());
            g_notVerified = true;
            return;
        }
        counted = measureConversions(unlayered, file);
    }
    if (!counted.ran) {
        fail("the allocation fixture did not decode to its twelve host frames on the unlayered device");
        return;
    }
    check(counted.constructor > 0, "the allocation counter saw nothing while the converter was built");
    if (counted.converting != 0) {
        std::fprintf(stderr, "FAIL: %s: %d host-frame conversions made %ld operator new calls\n", label.c_str(),
                     counted.measured, counted.converting);
        fail("converting a host frame allocates host memory");
    }
    std::printf("%s: %d host-frame conversions, %ld allocations (the constructor made %ld)\n", label.c_str(),
                counted.measured, counted.converting, counted.constructor);
#else
    (void)device;
    std::printf("%s: built with a sanitizer, whose allocator the counter cannot replace -- not answerable\n",
                label.c_str());
    g_notVerified = true;
#endif
}

// ------------------------------------------------------------------------- //
// S13.52 -- one rule decides what a hardware image may be
// ------------------------------------------------------------------------- //

void runGateChecks(Device& device, const std::string& label) {
    if (device.info().videoDecodeOps == 0) {
        std::printf("%s: %s has no hardware decoder, so nothing asks the rule -- not answerable\n", label.c_str(),
                    device.info().gpuName.c_str());
        g_notVerified = true;
        return;
    }
    // GOAL: S13.52 -- the rule accepts the two-plane 4:4:4 images a hardware decoder hands back,
    //       Cb before Cr, at 8 and 10 bits, and refuses the same image described with Cr first.
    // WHY-THIS-SHAPE: asked with nv24, p410le and nv42, the names ffmpeg gives a decoder's image.
    //       yuv444p is three-plane, which no decoder produces and the rule refused both before and
    //       after 4:4:4 was admitted, so asking about it grades nothing. nv42 is asked about the
    //       VkFormat nv24 is accepted in, so only the plane order differs.
    // CONTROL: renderer/video/frame_converter.cpp deviceFormatFor(), each reverted. The 8-bit 4:4:4
    //       case returning UNDEFINED: "refuses an 8-bit 4:4:4 decoder image (nv24)". The 10-bit case
    //       the same: "(p410le)". The Cb-before-Cr test removed: "accepts ... Cr before Cb (nv42)".
    check(FrameConverter::readsDeviceImage(device, AV_PIX_FMT_NV24, VK_FORMAT_G8_B8R8_2PLANE_444_UNORM),
          "readsDeviceImage refuses an 8-bit 4:4:4 decoder image (nv24)");
    check(FrameConverter::readsDeviceImage(device, AV_PIX_FMT_P410LE,
                                           VK_FORMAT_G10X6_B10X6R10X6_2PLANE_444_UNORM_3PACK16),
          "readsDeviceImage refuses a 10-bit 4:4:4 decoder image (p410le)");
    check(!FrameConverter::readsDeviceImage(device, AV_PIX_FMT_NV42, VK_FORMAT_G8_B8R8_2PLANE_444_UNORM),
          "readsDeviceImage accepts a 4:4:4 image with Cr before Cb (nv42) in a format that holds Cb first");

    // GOAL: S13.52 -- the decoder asks the rule before decoding on the device: an H.265 4:4:4
    //       12-bit stream, whose hardware setup succeeds here with an image the rule refuses, takes
    //       SoftwareRefused and converts from host memory within one code.
    // WHY-THIS-SHAPE: 12-bit 4:4:4 is the one stream measured here that reaches the rule and is
    //       refused by it: 8- and 10-bit 4:4:4 are accepted, and 4:2:2, H.264 4:4:4 and AV1 4:4:4
    //       are turned away inside avcodec_get_hw_frames_parameters, which also ends in
    //       SoftwareRefused and would pass with the decoder never asking. So the interposed setup is
    //       asserted to have proposed one image with a video profile first; otherwise not answerable.
    // CONTROL: renderer/video/video_decoder.cpp without its readsDeviceImage() call: "did not take the
    //       software path" and "was decoded into a device image", so this GPU decodes the stream and
    //       the fallback is the rule's doing. Reverted. Harness: the interposer recording a failed
    //       setup -> "never asked the rule -- not answerable", exit 77.
    if (haveAsset("conv_hevc444_12.mp4", label)) {
        try {
            g_hwFrames = HwFramesProposal{};
            VideoDecoder decoder(device, asset("conv_hevc444_12.mp4"));
            std::optional<DecodedFrame> frame = decoder.next();
            const HwFramesProposal proposal = g_hwFrames;
            if (!frame) {
                fail("the H.265 4:4:4 12-bit fixture decoded to no frames");
            } else if (proposal.calls == 0 || proposal.result < 0 || !proposal.singleImage || !proposal.profileList) {
                std::printf("%s: ffmpeg's hardware setup did not propose one image with a video profile for H.265 "
                            "4:4:4 12-bit on %s (%d call(s), last result %d), so the decoder never asked the rule "
                            "-- not answerable\n",
                            label.c_str(), device.info().gpuName.c_str(), proposal.calls, proposal.result);
                g_notVerified = true;
            } else {
                check(!FrameConverter::readsDeviceImage(device, proposal.swFormat, proposal.imageFormat),
                      "readsDeviceImage accepts the 12-bit 4:4:4 image ffmpeg proposed, so this stream no longer "
                      "shows the decoder obeying a refusal");
                check(decoder.path() == VideoDecoder::Path::SoftwareRefused,
                      "an H.265 4:4:4 12-bit stream the rule refuses did not take the software path");
                check(frame->storage() == DecodedFrame::Storage::HostPlanes,
                      "an H.265 4:4:4 12-bit stream the rule refuses was decoded into a device image");
                const ColourFixture fixture{"conv_hevc444_12.mp4", AV_PIX_FMT_YUV444P12LE, AVCOL_SPC_BT709,
                                            AVCOL_RANGE_MPEG, SWS_CS_ITU709, false, SWS_CS_ITU601};
                std::string why;
                std::optional<HostPicture> picture;
                if (frame->storage() == DecodedFrame::Storage::HostPlanes &&
                    fixtureDecodedAsRecipe(label, fixture, *frame, /*checkFormat=*/true)) {
                    picture = copyHostPicture(*frame, why);
                    if (!picture) {
                        std::fprintf(stderr, "FAIL: %s: %s: %s\n", label.c_str(), fixture.file, why.c_str());
                        fail("a host frame could not be copied out for the reference");
                    }
                }
                if (picture) {
                    FrameConverter converter(device, *frame, kShaderDir);
                    const std::vector<uint8_t> converted = convertAndRead(device, converter, *frame);
                    if (kHaveReference) {
                        (void)gradeColour(label, fixture.file, *picture, converted, fixture.swsMatrix,
                                          fixture.fullRange, fixture.swsOtherMatrix);
                    } else {
                        std::printf("%s: built without libswscale, so the fallback's colour is not graded -- not "
                                    "answerable\n",
                                    label.c_str());
                        g_notVerified = true;
                    }
                    int more = 0;
                    while (std::optional<DecodedFrame> next = decoder.next()) {
                        waitReporting(device, converter.convert(*next), "a later H.265 4:4:4 12-bit frame");
                        ++more;
                    }
                    std::printf("%s: H.265 4:4:4 12-bit -> ffmpeg proposed %s, the rule refused it, %s, %d frames "
                                "converted\n",
                                label.c_str(), orUnknown(av_get_pix_fmt_name(proposal.swFormat)),
                                pathName(decoder.path()), more + 1);
                }
            }
        } catch (const std::exception& e) {
            reportThrow(label, "the H.265 4:4:4 12-bit fixture", e);
        }
    }
    if (!haveAsset("conv_hevc10.mp4", label)) return;

    // GOAL: S13.52 -- the decoder and the constructor ask the same rule. The same device frame is
    //       accepted by both while the device can filter its plane format, and refused by both --
    //       readsDeviceImage false, the constructor std::logic_error, a fresh decode SoftwareRefused
    //       -- once the device reports it cannot.
    // WHY-THIS-SHAPE: the device's own format query is interposed so the one stream flips between
    //       the two outcomes in one run. A decoder that asked a different rule, or a constructor
    //       that did, leaves one of the three answers unchanged. Stripping a 10-bit plane format
    //       leaves the host path's R16 formats alone, so the software fallback still converts.
    // CONTROL: renderer/video/frame_converter.cpp, each reverted. readsDeviceImage answering from the
    //       layout without asking the device: "still accepts P010", "did not take software", "decoded
    //       on the device". The constructor's device refusal thrown as WallpaperContentError: "refused
    //       the device frame as content". The rule negated with the decoder not asking it: "refuses a
    //       device frame the decoder chose".
    try {
        VideoDecoder decoder(device, asset("conv_hevc10.mp4"));
        std::optional<DecodedFrame> frame = decoder.next();
        if (!frame || frame->storage() != DecodedFrame::Storage::DeviceImage) {
            std::printf("%s: H.265 Main10 did not decode on the device here, so the two askers cannot be compared "
                        "-- not answerable\n",
                        label.c_str());
            g_notVerified = true;
            return;
        }
        const DecodedFrame::DeviceImage image = frame->deviceImage();
        check(FrameConverter::readsDeviceImage(device, frame->pixelFormat(), image.format),
              "readsDeviceImage refuses a device frame the decoder chose to decode on the device");
        {
            FrameConverter accepted(device, *frame, kShaderDir);
            waitReporting(device, accepted.convert(*frame), "the accepted 10-bit device frame");
        }
        device.waitForSubmissions();

        StrippedX6LinearFilter stripped;
        check(!FrameConverter::readsDeviceImage(device, frame->pixelFormat(), image.format),
              "readsDeviceImage still accepts P010 on a device that cannot filter its plane format");
        bool constructorRefused = false;
        try {
            FrameConverter refused(device, *frame, kShaderDir);
        } catch (const WallpaperContentError& e) {
            std::fprintf(stderr, "FAIL: %s: the constructor refused the device frame as content: %s\n", label.c_str(),
                         e.what());
        } catch (const std::logic_error&) {
            constructorRefused = true;
        }
        check(constructorRefused, "the constructor did not refuse a device frame readsDeviceImage refuses");

        VideoDecoder fallback(device, asset("conv_hevc10.mp4"));
        std::optional<DecodedFrame> host = fallback.next();
        if (!host) {
            fail("the 10-bit fixture decoded to no frames with its plane format stripped");
            return;
        }
        check(fallback.path() == VideoDecoder::Path::SoftwareRefused,
              "the decoder did not take software for a stream the rule refuses");
        check(host->storage() == DecodedFrame::Storage::HostPlanes,
              "the decoder decoded on the device a stream the rule refuses");
        if (host->storage() == DecodedFrame::Storage::HostPlanes) {
            FrameConverter converter(device, *host, kShaderDir);
            waitReporting(device, converter.convert(*host), "the software-decoded 10-bit frame");
        }
        std::printf("%s: H.265 Main10 -> Hardware; with its plane format unfilterable -> %s\n", label.c_str(),
                    pathName(fallback.path()));
    } catch (const std::exception& e) {
        reportThrow(label, "the H.265 Main10 fixture", e);
    }
}

// ------------------------------------------------------------------------- //
// S13.53 -- a device without the Y'CbCr sampler feature still plays
// ------------------------------------------------------------------------- //

void runNoYcbcrChecks(Device& device, const std::string& label) {
    if (device.samplerYcbcrConversion()) {
        fail("the samplerYcbcrConversion feature was not hidden, so a device without it was never built");
        return;
    }
    if (device.info().videoDecodeOps == 0) {
        std::printf("%s: %s has no hardware decoder, so nothing could need the feature -- not answerable\n",
                    label.c_str(), device.info().gpuName.c_str());
        g_notVerified = true;
        return;
    }
    // GOAL: S13.53 -- on a device that lacks samplerYcbcrConversion, a stream plays exactly as it
    //       does on the same GPU with the feature: every frame decodes and converts, on the same path.
    // WHY-THIS-SHAPE: compared with the same GPU built with the feature, in this run, rather than
    //       asserting Hardware: whether ffmpeg needs the feature is the open question, and the
    //       property worth holding is that its absence changes nothing -- not that a fallback
    //       exists. Frame counts are compared so a stream that stops early cannot pass as playing.
    // CONTROL: renderer/video/video_decoder.cpp openDecoder() choosing SoftwareRefused whenever the
    //       feature is absent -- the removed check, restored: "10 frames on SoftwareRefused; with it:
    //       10 frames on Hardware" for both streams. Reverted. Harness: feature not hidden -> "was not
    //       hidden, so a device without it was never built".
    for (const char* name : {"conv_709_tv.mp4", "conv_hevc10.mp4"}) {
        if (!haveAsset(name, label)) continue;
        try {
            int baselineFrames = 0;
            VideoDecoder::Path baselinePath = VideoDecoder::Path::Undecided;
            {
                Device withFeature(/*enableValidation=*/false);
                if (!withFeature.samplerYcbcrConversion()) {
                    std::printf("%s: %s lacks the feature even unhidden -- not answerable\n", label.c_str(),
                                withFeature.info().gpuName.c_str());
                    g_notVerified = true;
                    return;
                }
                VideoDecoder decoder(withFeature, asset(name));
                while (decoder.next()) ++baselineFrames;
                baselinePath = decoder.path();
            }
            VideoDecoder decoder(device, asset(name));
            std::optional<FrameConverter> converter;
            int frames = 0;
            while (std::optional<DecodedFrame> frame = decoder.next()) {
                if (!converter) converter.emplace(device, *frame, kShaderDir);
                waitReporting(device, converter->convert(*frame), "a frame on the device without the feature");
                ++frames;
            }
            if (frames != baselineFrames || decoder.path() != baselinePath) {
                std::fprintf(stderr,
                             "FAIL: %s: %s without samplerYcbcrConversion: %d frames on %s; with it: %d frames on %s\n",
                             label.c_str(), name, frames, pathName(decoder.path()), baselineFrames,
                             pathName(baselinePath));
                fail("a device without samplerYcbcrConversion does not play the stream as one with it does");
            }
            std::printf("%s: %s without samplerYcbcrConversion: %d frames converted on %s (with it: %d on %s)\n",
                        label.c_str(), name, frames, pathName(decoder.path()), baselineFrames, pathName(baselinePath));
        } catch (const std::exception& e) {
            reportThrow(label, name, e);
        }
    }
}

// ------------------------------------------------------------------------- //
// S13.54 -- frames that change shape or colour mid-stream
// ------------------------------------------------------------------------- //

// Expects convert() to throw exactly `Expected` and to submit nothing.
// ------------------------------------------------------------------------- //
// S13.68 -- VideoPipeline's target against the reference
// ------------------------------------------------------------------------- //

void runPipelineColourChecks(Device& device, const std::string& label, bool onDevice) {
    if (!kHaveReference) {
        std::printf("%s: built without libswscale, so there is no independent reference -- not answerable\n",
                    label.c_str());
        g_notVerified = true;
        return;
    }
    if (onDevice && device.info().videoDecodeOps == 0) {
        std::printf("%s: %s has no hardware decoder, so no picture reaches the device path -- not answerable\n",
                    label.c_str(), device.info().gpuName.c_str());
        g_notVerified = true;
        return;
    }
    if (!onDevice && !videoExtensionsHidden(device, label)) return;

    // GOAL: S13.68 -- what VideoPipeline leaves on a target the size of the picture is within one code
    //       of libswscale told the matrix and range explicitly, from host planes and from device images.
    // WHY-THIS-SHAPE: colour-host and colour-device grade FrameConverter's image; this grades the
    //       target after decode, convert and blit together, which they cannot: a blit ahead of its
    //       conversion, or a stage showing another image, leaves the converter's image right. The
    //       target is the visible size, so the blit is one to one and each pixel is the picture's.
    //       The device fixtures are the converter's own; one that decodes in software here is a skip.
    // CONTROL: each reverted. renderer/video/frame_converter.cpp BT.709 coefficients set to BT.601's:
    //       every BT.709 fixture 32-33 codes off on both paths, the two BT.601 ones green.
    //       renderer/video/video_pipeline.cpp renderNext() rendering before converting: every fixture
    //       "differs from libswscale by 255 codes", the target black.
    // COVERAGE-ASSERT: gradeColour's own -- a quarter of the picture compared, wrong readings at least
    //       12 codes away -- plus each fixture's tags: red with a BT.601-tagged file under the BT.709
    //       name, "decoded with matrix smpte170m".
    int graded = 0;
    for (const ColourFixture& fixture : onDevice ? std::vector<ColourFixture>(std::begin(kDeviceColourFixtures),
                                                                              std::end(kDeviceColourFixtures))
                                                 : std::vector<ColourFixture>(std::begin(kHostColourFixtures),
                                                                              std::end(kHostColourFixtures))) {
        if (!haveAsset(fixture.file, label)) continue;
        try {
            std::string why;
            std::optional<HostPicture> reference = softwareFirstFrame(asset(fixture.file), why);
            if (!reference) {
                std::fprintf(stderr, "FAIL: %s: %s: %s\n", label.c_str(), fixture.file, why.c_str());
                fail("the software reference decode failed");
                continue;
            }
            if (reference->colour.matrix != fixture.matrix || reference->colour.range != fixture.range) {
                std::fprintf(stderr, "FAIL: %s: %s decoded with %s\n", label.c_str(), fixture.file,
                             describeColour(reference->colour).c_str());
                fail("a colour fixture does not carry what its recipe tags, so this probe was not given the stream "
                     "it grades");
                continue;
            }
            std::vector<unsigned char> shown;
            bool deviceFrame = false;
            {
                OffscreenTargetManager targets(device);
                const OffscreenTarget& target =
                    targets.create(1, reference->visible.extent.width, reference->visible.extent.height);
                VideoPipeline pipeline(device, asset(fixture.file), kShaderDir);
                g_lastReceivedFormat = AV_PIX_FMT_NONE;
                if (!pipeline.renderNext(targets, target)) {
                    fail("a colour fixture played no picture through the pipeline");
                    continue;
                }
                deviceFrame = g_lastReceivedFormat == AV_PIX_FMT_VULKAN;
                shown = wallpaperx::testing::reclaimAndReadback(device, targets, target);
            }
            if (onDevice && !deviceFrame) {
                std::printf("%s: %s did not decode on the device -- not answerable for it\n", label.c_str(),
                            fixture.file);
                g_notVerified = true;
                continue;
            }
            if (!onDevice && deviceFrame) {
                fail("a software-only device decoded a picture on the device");
                continue;
            }
            const std::string name = std::string(fixture.file) + " through the pipeline";
            if (gradeColour(label, name.c_str(), *reference, shown, fixture.swsMatrix, fixture.fullRange,
                            fixture.swsOtherMatrix)) {
                ++graded;
            }
        } catch (const std::exception& e) {
            reportThrow(label, fixture.file, e);
        }
    }
    if (graded == 0 && wallpaperx::testing::failureCount() == 0) {
        std::printf("%s: no fixture was graded -- not answerable\n", label.c_str());
        g_notVerified = true;
    }
}

template <class Expected>
bool refusedWithoutSubmitting(FrameConverter& converter, DecodedFrame& frame, const std::string& label,
                              const char* what) {
    const int before = g_queueSubmits.load();
    bool threw = false;
    try {
        (void)converter.convert(frame);
    } catch (const Expected&) {
        threw = true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s: %s raised the wrong exception: %s\n", label.c_str(), what, e.what());
    }
    const bool submitted = g_queueSubmits.load() != before;
    if (!threw) {
        std::fprintf(stderr, "FAIL: %s: %s was not refused\n", label.c_str(), what);
        fail("convert() did not refuse a frame its contract refuses");
    }
    if (submitted) {
        std::fprintf(stderr, "FAIL: %s: %s submitted a batch before throwing\n", label.c_str(), what);
        fail("convert() submitted work for a frame it then refused");
    }
    return threw && !submitted;
}

void runShapeResizeChecks(Device& device, const std::string& label) {
    if (!videoExtensionsHidden(device, label)) return;
    if (!haveAsset("shape_resize.h264", label)) return;

    // GOAL: S13.54 -- a frame whose size differs from the converter's picture is refused with
    //       std::invalid_argument before anything is submitted, and a converter built for the new
    //       picture converts it: a resolution change needs a new converter, and gets a working one.
    // WHY-THIS-SHAPE: a real stream whose second half is a different size, decoded by the real
    //       decoder, so what reaches convert() is what a mid-stream change produces rather than a
    //       frame this probe could not build on its own. Submissions are counted at vkQueueSubmit
    //       so "nothing is submitted on a throw" is measured, not inferred from the exception.
    //       Decoded in software: the refusal is the converter's and does not depend on the path, and
    //       a size change on the hardware path is graded by video_pipeline_probe's shape-device entry.
    // CONTROL: renderer/video/frame_converter.cpp convert(), each reverted. An empty submit ahead of
    //       the picture check: "submitted a batch before throwing". The refusal as std::runtime_error:
    //       "raised the wrong exception". Harness: a single-size stream -> both coverage lines; video
    //       extensions left visible -> "were not hidden".
    // COVERAGE-ASSERT: the stream must yield two sizes and at least one refusal, so a fixture that
    //       stopped changing size fails instead of passing with nothing refused.
    try {
        VideoDecoder decoder(device, asset("shape_resize.h264"));
        std::optional<FrameConverter> converter;
        int converted = 0;
        int refused = 0;
        std::vector<uint32_t> widths;
        while (std::optional<DecodedFrame> frame = decoder.next()) {
            if (std::find(widths.begin(), widths.end(), frame->visible().extent.width) == widths.end()) {
                widths.push_back(frame->visible().extent.width);
            }
            if (!converter) converter.emplace(device, *frame, kShaderDir);
            if (FrameConverter::pictureOf(*frame) != converter->picture()) {
                if (refusedWithoutSubmitting<std::invalid_argument>(*converter, *frame, label,
                                                                    "a frame of a different size")) {
                    ++refused;
                }
                device.waitForSubmissions();
                converter.reset();
                converter.emplace(device, *frame, kShaderDir);
            }
            waitReporting(device, converter->convert(*frame), "a frame of the resized stream");
            ++converted;
        }
        check(widths.size() == 2, "the resize fixture did not decode to two picture sizes");
        check(refused >= 1, "no frame of the resized stream was refused by the converter built for the first size");
        std::printf("%s: resized stream -> %zu sizes on %s, %d frames converted, %d refused before a rebuild\n",
                    label.c_str(), widths.size(), pathName(decoder.path()), converted, refused);
    } catch (const std::exception& e) {
        reportThrow(label, "the resized stream", e);
    }
}

void runShapeTagChecks(Device& device, const std::string& label) {
    if (!haveAsset("shape_tags.h264", label)) return;

    // GOAL: S13.54 -- a frame whose colour tags change to ones this pass cannot render is refused by
    //       convert() with WallpaperContentError, before anything is submitted, while the frames
    //       before the change convert.
    // CONTROL: renderer/video/frame_converter.cpp convert() with an empty submit ahead of its checks,
    //       run on this GPU's device path: "a frame retagged as PQ submitted a batch before throwing"
    //       for each of the five. Reverted.
    try {
        VideoDecoder decoder(device, asset("shape_tags.h264"));
        std::optional<FrameConverter> converter;
        int converted = 0;
        int refused = 0;
        while (std::optional<DecodedFrame> frame = decoder.next()) {
            if (!converter) converter.emplace(device, *frame, kShaderDir);
            if (frame->colour().transfer == AVCOL_TRC_SMPTE2084) {
                if (refusedWithoutSubmitting<WallpaperContentError>(*converter, *frame, label,
                                                                    "a frame retagged as PQ")) {
                    ++refused;
                }
                continue;
            }
            waitReporting(device, converter->convert(*frame), "a frame before the retag");
            ++converted;
        }
        check(refused >= 1, "the retagged stream produced no frame tagged PQ");
        std::printf("%s: retagged stream -> %d converted, %d refused as PQ\n", label.c_str(), converted, refused);
    } catch (const std::exception& e) {
        reportThrow(label, "the retagged stream", e);
    }
}

void runShapeFallbackChecks(Device& device, const std::string& label) {
    if (device.info().videoDecodeOps == 0) {
        std::printf("%s: %s has no hardware decoder, so nothing can fall back mid-stream -- not answerable\n",
                    label.c_str(), device.info().gpuName.c_str());
        g_notVerified = true;
        return;
    }
    const char* const file = "counter_short.mp4";
    if (!haveAsset(file, label)) return;

    // GOAL: S13.55 -- when the hardware decoder fails part way through a stream on a device that
    //       still answers, later frames arrive from host memory, the converter built for the device
    //       frames refuses the first with std::invalid_argument before submitting, the path is
    //       SoftwareRefused, and every picture of the stream is converted once, in order.
    // WHY-THIS-SHAPE: the failure is injected at avcodec_receive_frame after three device frames,
    //       because no file makes a working hardware decoder fail mid-stream. Each conversion is read
    //       back and identified by the frame number the fixture draws into it, against libavcodec
    //       decoding the file directly. A frame count cannot tell a repeat and a skip that cancel, and
    //       the decoder's timestamps are what it decides from.
    // CONTROL: each reverted. renderer/video/video_decoder.cpp admit()'s DropThrough test `<=` made `<`:
    //       "21 pictures converted against 20 ... 1 converted more than once". DropThrough also dropping
    //       the first picture past the resume point: "19 ... 1 never converted".
    //       renderer/video/frame_converter.cpp's picture refusal as std::runtime_error: "raised the wrong
    //       exception".
    // COVERAGE-ASSERT: the injection consumed, three device frames seen first, and the independent
    //       decode naming every picture once -- red with the colour-bars fixture substituted, "does not
    //       name each picture once".
    constexpr int kDeviceFramesFirst = 3;
    try {
        std::string why;
        const std::optional<wallpaperx::testing::CounterReference> reference =
            wallpaperx::testing::decodeCounterReference(asset(file), why);
        if (!reference || !reference->distinctAndReadable()) {
            std::fprintf(stderr, "FAIL: %s: %s: %s\n", label.c_str(), file,
                         reference ? "does not name each picture once" : why.c_str());
            fail("the counter fixture gives no sequence to grade against");
            return;
        }
        VideoDecoder decoder(device, asset(file));
        std::optional<FrameConverter> converter;
        std::vector<int> converted;
        int deviceFrames = 0;
        int hostFrames = 0;
        bool refusedTheSwitch = false;
        auto identify = [&](DecodedFrame& frame) {
            const std::vector<uint8_t> pixels = convertAndRead(device, *converter, frame);
            converted.push_back(wallpaperx::testing::counterFromBgra(pixels, converter->frame().source.width,
                                                                     converter->frame().source.height));
        };
        while (std::optional<DecodedFrame> frame = decoder.next()) {
            if (frame->storage() == DecodedFrame::Storage::DeviceImage) {
                if (!converter) converter.emplace(device, *frame, kShaderDir);
                identify(*frame);
                if (++deviceFrames == kDeviceFramesFirst) g_receiveExternalRemaining = 1;
                continue;
            }
            if (hostFrames++ == 0 && converter) {
                refusedTheSwitch = refusedWithoutSubmitting<std::invalid_argument>(
                    *converter, *frame, label, "the first host frame after the fallback");
                device.waitForSubmissions();
                converter.reset();
            }
            if (!converter) converter.emplace(device, *frame, kShaderDir);
            identify(*frame);
            if (converted.size() > reference->ids.size() + 8) {
                fail("the decoder went on returning pictures past the end of the stream");
                break;
            }
        }
        const int unconsumed = g_receiveExternalRemaining;
        g_receiveExternalRemaining = 0;
        if (deviceFrames < kDeviceFramesFirst) {
            std::printf("%s: only %d device frames before the stream ended -- not answerable\n", label.c_str(),
                        deviceFrames);
            g_notVerified = true;
            return;
        }
        check(unconsumed == 0, "the injected hardware failure was never consumed");
        check(decoder.path() == VideoDecoder::Path::SoftwareRefused,
              "a hardware failure mid-stream did not leave the decoder on SoftwareRefused");
        check(refusedTheSwitch, "the converter built for device frames did not refuse the first host frame");
        if (converted != reference->ids) {
            std::vector<int> times(1 << wallpaperx::testing::kCounterBits, 0);
            for (int id : converted) {
                if (id >= 0) ++times[static_cast<size_t>(id)];
            }
            int repeated = 0;
            for (int count : times) repeated += count > 1 ? 1 : 0;
            int missing = 0;
            for (int id : reference->ids) missing += times[static_cast<size_t>(id)] == 0 ? 1 : 0;
            std::fprintf(stderr,
                         "FAIL: %s: %zu pictures converted against %zu in an independent decode: %d converted more "
                         "than once, %d never converted\n",
                         label.c_str(), converted.size(), reference->ids.size(), repeated, missing);
            fail("the pictures converted across a mid-stream fallback are not the stream's, each once, in order");
        }
        std::printf("%s: %d device frames, then the hardware failed; %d host frames followed, %zu pictures converted "
                    "against %zu in the stream\n",
                    label.c_str(), deviceFrames, hostFrames, converted.size(), reference->ids.size());
    } catch (const std::exception& e) {
        g_receiveExternalRemaining = 0;
        reportThrow(label, "the mid-stream fallback", e);
    }
}

// ------------------------------------------------------------------------- //
// S13.51 -- the shader loader refuses a short read (opens no driver)
// ------------------------------------------------------------------------- //

void runShaderReadChecks(const std::string& label) {
    // GOAL: S13.51 -- readSpirv throws for a file that reports a word-sized length and reads
    //       short, instead of returning the words with a zero-filled tail.
    // WHY-THIS-SHAPE: a sysfs attribute is a real file that reports a page and reads a line, so the
    //       short read comes from the kernel, not from an interposed read(). A readSpirv that throws
    //       for everything would pass that alone, so a real compiled shader must read in full first.
    // CONTROL: renderer/vulkan/shader_module.cpp, each reverted. The short-read test replaced by
    //       `false`: "read as 1024 words". The size test inverted: "refuses a valid shader". The first
    //       word skipped and one fewer returned: "did not read to its full length" and "magic number".
    // COVERAGE-ASSERT: the valid shader's word count and SPIR-V magic number.
    const std::string shader = std::string(kShaderDir) + "/ycbcr_convert.frag.spv";
    std::error_code error;
    const auto size = std::filesystem::file_size(shader, error);
    if (error || size == 0) {
        fail("the compiled ycbcr_convert.frag.spv is missing, so a successful read cannot be shown");
        return;
    }
    try {
        const std::vector<uint32_t> words = wallpaperx::readSpirv(shader, "frame_converter_probe");
        check(words.size() * 4 == size, "a valid shader did not read to its full length");
        check(!words.empty() && words[0] == 0x07230203, "a valid shader did not read back its SPIR-V magic number");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s: a valid shader was refused: %s\n", label.c_str(), e.what());
        fail("readSpirv refuses a valid shader");
    }
    if (!shortReadAvailable(label)) {
        g_notVerified = true;
        return;
    }
    bool refused = false;
    try {
        const std::vector<uint32_t> words = wallpaperx::readSpirv(kShortReadFile, "frame_converter_probe");
        std::fprintf(stderr, "FAIL: %s: %s read as %zu words\n", label.c_str(), kShortReadFile, words.size());
    } catch (const std::runtime_error&) {
        refused = true;
    }
    check(refused, "readSpirv accepted a file that read short");
    std::printf("%s: a valid shader read in full; a file that reads short was refused\n", label.c_str());
}

}  // namespace

// ------------------------------------------------------------------------- //
// Interposers. The executable's definitions preempt libvulkan's and ffmpeg's for every call site
// linked into it, wallpaperx-renderer.a included; the real entry point is reached through RTLD_NEXT.
// ------------------------------------------------------------------------- //

namespace {
template <class Fn>
Fn realSymbol(const char* name) {
    const Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, name));
    if (real == nullptr) {
        std::fprintf(stderr, "frame_converter_probe: cannot resolve the real %s\n", name);
        std::abort();
    }
    return real;
}
}  // namespace

extern "C" VkResult vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice, const char* layerName,
                                                         uint32_t* count, VkExtensionProperties* properties) {
    static const auto real =
        realSymbol<wallpaperx::testing::EnumerateDeviceExtensionsFn>("vkEnumerateDeviceExtensionProperties");
    if (!g_hideVideoExtensions) return real(physicalDevice, layerName, count, properties);
    return wallpaperx::testing::enumerateWithoutVideoExtensions(real, physicalDevice, layerName, count, properties);
}

extern "C" void vkGetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2* features) {
    using Fn = void (*)(VkPhysicalDevice, VkPhysicalDeviceFeatures2*);
    static const Fn real = realSymbol<Fn>("vkGetPhysicalDeviceFeatures2");
    real(physicalDevice, features);
    if (!g_hideYcbcrFeature) return;
    for (auto* node = static_cast<VkBaseOutStructure*>(features->pNext); node != nullptr; node = node->pNext) {
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES) {
            reinterpret_cast<VkPhysicalDeviceVulkan11Features*>(node)->samplerYcbcrConversion = VK_FALSE;
        } else if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES) {
            reinterpret_cast<VkPhysicalDeviceSamplerYcbcrConversionFeatures*>(node)->samplerYcbcrConversion = VK_FALSE;
        }
    }
}

extern "C" void vkGetPhysicalDeviceFormatProperties2(VkPhysicalDevice physicalDevice, VkFormat format,
                                                     VkFormatProperties2* properties) {
    using Fn = void (*)(VkPhysicalDevice, VkFormat, VkFormatProperties2*);
    static const Fn real = realSymbol<Fn>("vkGetPhysicalDeviceFormatProperties2");
    real(physicalDevice, format, properties);
    if (!g_stripX6LinearFilter) return;
    if (format != VK_FORMAT_R10X6_UNORM_PACK16 && format != VK_FORMAT_R10X6G10X6_UNORM_2PACK16) return;
    properties->formatProperties.optimalTilingFeatures &= ~VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    for (auto* node = static_cast<VkBaseOutStructure*>(properties->pNext); node != nullptr; node = node->pNext) {
        if (node->sType == VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3) {
            reinterpret_cast<VkFormatProperties3*>(node)->optimalTilingFeatures &=
                ~VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        }
    }
}

extern "C" VkResult vkQueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
    using Fn = VkResult (*)(VkQueue, uint32_t, const VkSubmitInfo*, VkFence);
    static const Fn real = realSymbol<Fn>("vkQueueSubmit");
    g_queueSubmits.fetch_add(1, std::memory_order_relaxed);
    return real(queue, count, submits, fence);
}

extern "C" VkResult vkCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo* info,
                                         const VkAllocationCallbacks* allocator, VkShaderModule* module) {
    using Fn = VkResult (*)(VkDevice, const VkShaderModuleCreateInfo*, const VkAllocationCallbacks*, VkShaderModule*);
    static const Fn real = realSymbol<Fn>("vkCreateShaderModule");
    g_shaderModulesCreated.fetch_add(1, std::memory_order_relaxed);
    return real(device, info, allocator, module);
}

extern "C" int avcodec_get_hw_frames_parameters(AVCodecContext* context, AVBufferRef* deviceRef,
                                                enum AVPixelFormat hwFormat, AVBufferRef** out) {
    using Fn = int (*)(AVCodecContext*, AVBufferRef*, enum AVPixelFormat, AVBufferRef**);
    static const Fn real = realSymbol<Fn>("avcodec_get_hw_frames_parameters");
    const int result = real(context, deviceRef, hwFormat, out);
    HwFramesProposal proposal;
    proposal.calls = g_hwFrames.calls + 1;
    proposal.result = result;
    if (result >= 0 && hwFormat == AV_PIX_FMT_VULKAN && out != nullptr && *out != nullptr) {
        const auto* frames = reinterpret_cast<AVHWFramesContext*>((*out)->data);
        const auto* vk = static_cast<AVVulkanFramesContext*>(frames->hwctx);
        proposal.swFormat = frames->sw_format;
        proposal.imageFormat = vk->format[0];
        proposal.singleImage = vk->format[0] != VK_FORMAT_UNDEFINED && vk->format[1] == VK_FORMAT_UNDEFINED;
        for (auto* node = static_cast<const VkBaseInStructure*>(vk->create_pnext); node != nullptr;
             node = node->pNext) {
            if (node->sType == VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR) proposal.profileList = true;
        }
    }
    g_hwFrames = proposal;
    return result;
}

extern "C" int avcodec_receive_frame(AVCodecContext* context, AVFrame* frame) {
    using Fn = int (*)(AVCodecContext*, AVFrame*);
    static const Fn real = realSymbol<Fn>("avcodec_receive_frame");
    if (g_receiveExternalRemaining > 0) {
        --g_receiveExternalRemaining;
        return AVERROR_EXTERNAL;
    }
    const int err = real(context, frame);
    if (err == 0) g_lastReceivedFormat = static_cast<AVPixelFormat>(frame->format);
    return err;
}

#if WALLPAPERX_ALLOCATION_INSTRUMENT
// The replaceable global allocation functions, counting on a thread that asked. Plain malloc and
// free underneath, so every variant pairs with every delete.
void* operator new(std::size_t size) {
    if (t_countAllocations) g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size == 0 ? 1 : size)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    if (t_countAllocations) g_allocations.fetch_add(1, std::memory_order_relaxed);
    return std::malloc(size == 0 ? 1 : size);
}
void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept { return ::operator new(size, tag); }
void* operator new(std::size_t size, std::align_val_t alignment) {
    if (t_countAllocations) g_allocations.fetch_add(1, std::memory_order_relaxed);
    void* p = nullptr;
    if (posix_memalign(&p, std::max(sizeof(void*), static_cast<std::size_t>(alignment)), size == 0 ? 1 : size) == 0) {
        return p;
    }
    throw std::bad_alloc();
}
void* operator new[](std::size_t size, std::align_val_t alignment) { return ::operator new(size, alignment); }
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size, alignment);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t& tag) noexcept {
    return ::operator new(size, alignment, tag);
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
#endif

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    const std::vector<std::string> modes = {"colour-host", "colour-device", "edges-host", "edges-device",
                                            "siting",      "refusal",       "output",     "allocation",
                                            "gate",        "no-ycbcr",      "shape-resize", "shape-tags",
                                            "shape-fallback", "shader-read", "pipeline-colour-host",
                                            "pipeline-colour-device"};
    if (std::find(modes.begin(), modes.end(), mode) == modes.end()) {
        std::fprintf(stderr, "usage: %s <mode>, one of:", argv[0]);
        for (const std::string& m : modes) std::fprintf(stderr, " %s", m.c_str());
        std::fprintf(stderr, "\n");
        return 1;
    }
    const std::string label = "frame_converter_probe (" + mode + ")";

    bool graded = true;
    if (mode == "shader-read") {
        // Opens no Vulkan driver: the entry is labelled novulkan.
        runShaderReadChecks(label);
    } else {
        graded = wallpaperx::testing::gradedRun(label, [&](wallpaperx::testing::ValidationGrade& grade) {
            wallpaperx::testing::DecoderValidationOracle oracle;
            {
                std::optional<Device> device;
                try {
                    // Read while the Device is built, and off again before anything else builds one.
                    g_hideVideoExtensions =
                        mode == "colour-host" || mode == "edges-host" || mode == "shape-resize" ||
                        mode == "pipeline-colour-host";
                    g_hideYcbcrFeature = mode == "no-ycbcr";
                    try {
                        device.emplace(/*enableValidation=*/true);
                    } catch (...) {
                        g_hideVideoExtensions = false;
                        g_hideYcbcrFeature = false;
                        throw;
                    }
                    g_hideVideoExtensions = false;
                    g_hideYcbcrFeature = false;
                    oracle.attach(*device, grade);
                    std::printf("%s: on %s\n", label.c_str(), device->info().gpuName.c_str());

                    if (mode == "colour-host") runColourHostChecks(*device, label);
                    if (mode == "colour-device") runColourDeviceChecks(*device, label);
                    if (mode == "edges-host") runEdgeChecks(*device, label, /*onDevice=*/false);
                    if (mode == "edges-device") runEdgeChecks(*device, label, /*onDevice=*/true);
                    if (mode == "siting") runSitingChecks(*device, label);
                    if (mode == "refusal") runRefusalChecks(*device, label);
                    if (mode == "output") runOutputChecks(*device, label);
                    if (mode == "allocation") runAllocationChecks(*device, label);
                    if (mode == "gate") runGateChecks(*device, label);
                    if (mode == "no-ycbcr") runNoYcbcrChecks(*device, label);
                    if (mode == "shape-resize") runShapeResizeChecks(*device, label);
                    if (mode == "shape-tags") runShapeTagChecks(*device, label);
                    if (mode == "shape-fallback") runShapeFallbackChecks(*device, label);
                    if (mode == "pipeline-colour-host") runPipelineColourChecks(*device, label, /*onDevice=*/false);
                    if (mode == "pipeline-colour-device") runPipelineColourChecks(*device, label, /*onDevice=*/true);
                    device->waitForSubmissions();
                } catch (const std::exception& e) {
                    // Reported, not rethrown: an escaping exception would skip the reconciliation
                    // and gradedRun's own verdict.
                    fail(label + " threw: " + e.what());
                }
                g_stripX6LinearFilter = false;
                g_receiveExternalRemaining = 0;
                oracle.detach();
            }
            oracle.reconcile(label.c_str());
            return wallpaperx::testing::failureCount() == 0;
        });
    }

    const int failures = wallpaperx::testing::reportFailures("frame_converter_probe");
    if (!graded || failures > 0) return 1;
    if (g_notVerified) {
        std::printf("%s: NOT VERIFIED here -- reported to ctest as a skip, not a pass\n", label.c_str());
        return kNotVerifiedExitCode;
    }
    std::printf("%s: all checks passed, no unexpected validation messages\n", label.c_str());
    return 0;
}
