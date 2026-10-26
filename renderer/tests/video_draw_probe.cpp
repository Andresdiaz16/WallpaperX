// SPDX-License-Identifier: GPL-2.0-or-later
//
// Drawing decoded video, and what it forced on the decoder: S13.44-S13.55 and S13.68, graded
// through YcbcrRenderer and VideoPipeline, the code production draws video with.
//
// WHY THIS FILE EXISTS. The colour of a drawn picture had only ever been compared against
// libswscale in scratch programs nobody kept. The reference here is libswscale told the matrix and
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

#include "video/video_decoder.h"
#include "video/video_pipeline.h"
#include "video/ycbcr_renderer.h"
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
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using wallpaperx::DecodedFrame;
using wallpaperx::Device;
using wallpaperx::FrameColour;
using wallpaperx::OffscreenTarget;
using wallpaperx::OffscreenTargetManager;
using wallpaperx::VideoDecoder;
using wallpaperx::VideoPipeline;
using wallpaperx::WallpaperContentError;
using wallpaperx::YcbcrRenderer;
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

// S15.957's injections, each read by the interposers below. The window opens when a Vulkan frames
// proposal returns from avcodec_get_hw_frames_parameters and closes at the next Vulkan
// av_hwframe_ctx_init or when the test closes it: what is asked inside it is the decoder's own
// decision about that proposal, not ffmpeg's setup query.
std::atomic<bool> g_proposeLinearTiling{false};
std::atomic<bool> g_hideMutableFormat{false};
std::atomic<bool> g_proposalWindow{false};
std::atomic<int> g_linearProposals{0};
std::atomic<int> g_mutableStripped{0};
std::atomic<int> g_queriesInWindow{0};

// What the decoder handed av_hwframe_ctx_init for its decode pool: the first Vulkan init in a window.
struct DecodePoolInit {
    int calls = 0;
    VkImageUsageFlags usage = 0;
    VkImageCreateFlags flags = 0;
};
std::mutex g_poolInitMutex;
DecodePoolInit g_poolInit;

// Each Vulkan picture avcodec_receive_frame handed back, by image: lets a test edit what
// DecodedFrame::deviceImage() reads -- the frame's tiling, its pool's format and usage.
struct ReceivedVkFrame {
    VkImage image = VK_NULL_HANDLE;
    AVVkFrame* frame = nullptr;
    AVHWFramesContext* pool = nullptr;
};
std::mutex g_receivedMutex;
std::vector<ReceivedVkFrame> g_receivedVkFrames;

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

// `frame` drawn one to one by YcbcrRenderer, production's video draw, onto a target the visible
// picture's size, read back as tightly-packed BGRA. A device frame is moved into the renderer.
std::vector<uint8_t> drawAndRead(Device& device, DecodedFrame&& frame) {
    const VkExtent2D size = frame.visible().extent;
    OffscreenTargetManager targets(device);
    const OffscreenTarget& target = targets.create(1, size.width, size.height);
    YcbcrRenderer renderer(device, std::move(frame), kShaderDir);
    targets.render(renderer, target);
    return wallpaperx::testing::reclaimAndReadback(device, targets, target);
}

// Draws every picture `decoder` has left through one renderer onto a small target, rebuilding
// nothing: a picture of another shape throws, as it would in production. Returns how many.
int drawRest(Device& device, VideoDecoder& decoder) {
    OffscreenTargetManager targets(device);
    const OffscreenTarget& target = targets.create(1, 64, 64);
    std::optional<YcbcrRenderer> renderer;
    int drawn = 0;
    while (std::optional<DecodedFrame> frame = decoder.next()) {
        if (!renderer) {
            renderer.emplace(device, std::move(*frame), kShaderDir);
        } else {
            renderer->show(std::move(*frame));
        }
        targets.render(*renderer, target);
        ++drawn;
    }
    device.waitForSubmissions();
    return drawn;
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
            std::vector<uint8_t> converted = drawAndRead(device, std::move(*opened.frame));
            // GOAL: S13.44 -- every flat pixel within one code of libswscale, for each matrix, range,
            //       depth and subsampling in the table. Shape and guards: gradeColour, file header.
            // CONTROL: renderer/video/ycbcr_planes.cpp BT.709 coefficients set to BT.601's, through
            //       control_run.sh: "conv_709_tv.mp4 differs from libswscale by 32 codes at (147,0)", restored green.
            //       renderer/shaders/ycbcr_draw.frag alpha 0.5: "the converted picture is not opaque everywhere".
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
    // CONTROL: renderer/video/ycbcr_planes.cpp BT.709 coefficients set to BT.601's: "convert to pixels at
    //       most" red, restored green. The identical-samples guard is harness-side and path-independent.
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
            //       the others graded. Observed by hand with renderer/video/ycbcr_planes.cpp
            //       deviceFormatFor's 8-bit 4:4:4 case returning UNDEFINED: "conv_hevc444.mp4 took the
            //       SoftwareRefused path", exit 77; restored, passed.
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
            std::vector<uint8_t> converted = drawAndRead(device, std::move(*opened.frame));
            const std::string name = std::string(fixture.file) + " on the device";
            // CONTROL: renderer/video/ycbcr_planes.cpp BT.709 coefficients set to BT.601's: "conv_709_tv.mp4 on
            //       the device differs from libswscale by 32 codes", restored green.
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
// CONTROL: renderer/shaders/ycbcr_draw.frag chroma clamp widened by one texel each way: edges-device
//       "edge_offset_420_hw.mp4 on the device: the first visible row differs from libswscale by 126
//       codes", restored green. That break leaves edges-host green: host planes are uploaded cropped
//       to the visible region, so no padding is there to read. The upload region is the host path's
//       guard: renderer/video/ycbcr_renderer.cpp upload() copying each plane from column 0 instead of
//       the region's left edge, through control_batch.sh: "edge_offset_420.mp4 from host memory: the
//       first visible row differs from libswscale by 153 codes", restored green.
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
            //       not a pass on the others. Observed by hand with renderer/video/ycbcr_planes.cpp
            //       deviceFormatFor's 8-bit 4:4:4 case returning UNDEFINED: "edge_offset_444_hw.mp4
            //       took the SoftwareRefused path", exit 77; restored, passed.
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
            std::vector<uint8_t> converted = drawAndRead(device, std::move(*opened.frame));
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
// CONTROL: renderer/video/ycbcr_planes.cpp chromaSiting()'s default (left) case returning no shift:
//       "4:2:0 sited left, across columns: blend weight measured/expected" red, restored green.
void runSitingChecks(Device& device, const std::string& label) {
    ScratchDir scratch("wallpaperx-video-draw");
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
            const std::vector<uint8_t> converted = drawAndRead(device, std::move(*opened.frame));
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
//       WallpaperContentError from YcbcrRenderer's constructor, each accepted one is drawn.
// WHY-THIS-SHAPE: each refused case changes one tag against an otherwise accepted stream, so a
//       refusal cannot be earned by a different tag. The tags reaching FrameColour are asserted
//       first: libx264 given -color_primaries drops it silently, and a refusal check on an untagged
//       stream would be grading nothing -- which is exactly what the first recipes produced.
// CONTROL: renderer/video/ycbcr_planes.cpp primariesWithoutConversion() also accepting SMPTE432,
//       through control_batch.sh: "a stream tagged with P3 (smpte432) primaries was accepted", restored
//       green. Its refuse() throwing std::runtime_error: "raised something other than
//       WallpaperContentError", restored green. Harness, earlier: tag_p3.mp4 encoded with
//       -color_primaries -> "decoded with ... primaries unknown". Deleting the explicit HDR or
//       BT.2020-matrix refusal alone stays green: the allowlists refuse the same streams.
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
                OffscreenTargetManager targets(device);
                const OffscreenTarget& target = targets.create(1, 64, 64);
                YcbcrRenderer renderer(device, std::move(*opened.frame), kShaderDir);
                if (c.refused) {
                    std::fprintf(stderr, "FAIL: %s: a stream tagged with %s was accepted\n", label.c_str(), c.what);
                    fail("colour this pass cannot render was accepted");
                    continue;
                }
                targets.render(renderer, target);
                device.waitForSubmissions();
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
        check(reached == kRefusedTagCases + kAcceptedTagCases, "not every tag case reached the renderer");
        check(refused == kRefusedTagCases, "not every unsupported tag was refused as content");
        check(accepted == kAcceptedTagCases, "not every supported tag was drawn");
    }
    std::printf("%s: %d tag cases reached the renderer: %d refused as content, %d drawn\n", label.c_str(),
                reached, refused, accepted);
}

// ------------------------------------------------------------------------- //
// S13.51 -- the shader loader refuses a short read
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

// ------------------------------------------------------------------------- //
// S13.50 -- a host frame costs no allocation
// ------------------------------------------------------------------------- //

#if WALLPAPERX_ALLOCATION_INSTRUMENT
constexpr int kAllocationFrames = 12;
constexpr int kAllocationWarmUp = 2;

struct AllocationRun {
    bool ran = false;
    long constructor = 0;
    long showing = 0;
    long drawing = 0;
    int measured = 0;
};

// Builds a renderer for the file's first picture, then shows and draws every picture, counting
// operator new on this thread across the constructor, and across each show() and each render()
// after the warm-up -- never across the decode, the wait or the readback.
AllocationRun measureShows(Device& device, const std::string& file) {
    AllocationRun run;
    VideoDecoder decoder(device, file);
    std::vector<DecodedFrame> frames;
    while (std::optional<DecodedFrame> frame = decoder.next()) frames.push_back(std::move(*frame));
    if (frames.size() != kAllocationFrames || frames[0].storage() != DecodedFrame::Storage::HostPlanes) return run;
    OffscreenTargetManager targets(device);
    const OffscreenTarget& target = targets.create(1, 64, 32);
    std::optional<YcbcrRenderer> renderer;
    {
        g_allocations = 0;
        CountAllocations counting;
        renderer.emplace(device, std::move(frames[0]), kShaderDir);
        run.constructor = g_allocations.load();
    }
    targets.render(*renderer, target);
    // Picture 0 went through the constructor's show(); this brings both upload buffers into use.
    for (int i = 1; i < kAllocationWarmUp; ++i) {
        renderer->show(std::move(frames[i]));
        targets.render(*renderer, target);
    }
    for (int i = kAllocationWarmUp; i < kAllocationFrames; ++i) {
        g_allocations = 0;
        {
            CountAllocations counting;
            renderer->show(std::move(frames[i]));
        }
        run.showing += g_allocations.load();
        g_allocations = 0;
        {
            CountAllocations counting;
            targets.render(*renderer, target);
        }
        run.drawing += g_allocations.load();
        ++run.measured;
        device.waitForSubmissions();
    }
    run.ran = true;
    return run;
}
#endif

void runAllocationChecks(Device& device, const std::string& label) {
#if WALLPAPERX_ALLOCATION_INSTRUMENT
    ScratchDir scratch("wallpaperx-video-draw");
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

    // The same pictures under the validation layer, so the layer grades them; not counted.
    if (!measureShows(device, file).ran) {
        fail("the allocation fixture did not decode to its twelve host frames");
        return;
    }

    // GOAL: S13.50 -- YcbcrRenderer::show() on a host picture makes no operator new call once both
    //       upload buffers have been used: uploading a software-decoded picture costs no allocation.
    // WHY-THIS-SHAPE: counted through a replaced global operator new, on a second Device built
    //       without the validation layer. The layer is C++ and its own bookkeeping reaches the
    //       replaced operator new from inside vkQueueSubmit (147 calls per picture, measured, with it
    //       loaded; 0 without). Counting only callers inside this executable would miss allocations
    //       libstdc++ makes out of line on the renderer's behalf. render() is counted apart and
    //       printed, not asserted: it is the draw every renderer shares, outside this goal, and today
    //       makes one allocation per draw (OffscreenTargetManager::render's lambda outgrows
    //       std::function's inline storage).
    // CONTROL: renderer/video/ycbcr_renderer.cpp upload()'s barrier array made a std::vector, through
    //       control_batch.sh: "10 host pictures shown made 10 operator new calls in show()", restored
    //       green; re-run 2026-10-26 (G6) after Device::submit stopped allocating, the same line. Harness:
    //       CountAllocations never switching counting on -> "saw nothing while the renderer was built".
    // COVERAGE-ASSERT: the constructor, measured the same way, must count allocations -- otherwise
    //       a counter that sees nothing on this thread would report zero here as well.
    AllocationRun counted;
    {
        Device unlayered(/*enableValidation=*/false);
        if (unlayered.validationLayerActive()) {
            std::printf("%s: a validation layer is loaded into a device built without one (VK_INSTANCE_LAYERS?), "
                        "and its allocations cannot be told from the renderer's -- not answerable\n",
                        label.c_str());
            g_notVerified = true;
            return;
        }
        counted = measureShows(unlayered, file);
    }
    if (!counted.ran) {
        fail("the allocation fixture did not decode to its twelve host frames on the unlayered device");
        return;
    }
    check(counted.constructor > 0, "the allocation counter saw nothing while the renderer was built");
    if (counted.showing != 0) {
        std::fprintf(stderr, "FAIL: %s: %d host pictures shown made %ld operator new calls in show()\n", label.c_str(),
                     counted.measured, counted.showing);
        fail("showing a host picture allocates host memory");
    }
    std::printf("%s: %d host pictures, %ld allocations showing and %ld drawing (the constructor made %ld)\n",
                label.c_str(), counted.measured, counted.showing, counted.drawing, counted.constructor);
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
    //       Asked of YcbcrRenderer::readsDeviceImage, the function video_decoder.cpp calls.
    // CONTROL: renderer/video/ycbcr_planes.cpp deviceFormatFor(), each through control_batch.sh and
    //       restored green. The 8-bit 4:4:4 case returning UNDEFINED: "refuses an 8-bit 4:4:4 decoder
    //       image (nv24)". The Cb-before-Cr test made true: "accepts a 4:4:4 image with Cr before Cb".
    check(YcbcrRenderer::readsDeviceImage(device, AV_PIX_FMT_NV24, VK_FORMAT_G8_B8R8_2PLANE_444_UNORM),
          "readsDeviceImage refuses an 8-bit 4:4:4 decoder image (nv24)");
    check(YcbcrRenderer::readsDeviceImage(device, AV_PIX_FMT_P410LE,
                                           VK_FORMAT_G10X6_B10X6R10X6_2PLANE_444_UNORM_3PACK16),
          "readsDeviceImage refuses a 10-bit 4:4:4 decoder image (p410le)");
    check(!YcbcrRenderer::readsDeviceImage(device, AV_PIX_FMT_NV42, VK_FORMAT_G8_B8R8_2PLANE_444_UNORM),
          "readsDeviceImage accepts a 4:4:4 image with Cr before Cb (nv42) in a format that holds Cb first");

    // GOAL: S13.52 -- the decoder asks the rule before decoding on the device: an H.265 4:4:4
    //       12-bit stream, whose hardware setup succeeds here with an image the rule refuses, takes
    //       SoftwareRefused and converts from host memory within one code.
    // WHY-THIS-SHAPE: 12-bit 4:4:4 is the one stream measured here that reaches the rule and is
    //       refused by it: 8- and 10-bit 4:4:4 are accepted, and 4:2:2, H.264 4:4:4 and AV1 4:4:4
    //       are turned away inside avcodec_get_hw_frames_parameters, which also ends in
    //       SoftwareRefused and would pass with the decoder never asking. So the interposed setup is
    //       asserted to have proposed one image with a video profile first; otherwise not answerable.
    // CONTROL: renderer/video/video_decoder.cpp without its YcbcrRenderer::readsDeviceImage() call,
    //       through control_batch.sh: "the rule refuses did not take the software path", so this GPU
    //       decodes the stream and the fallback is the rule's doing; restored green. Harness, earlier:
    //       the interposer recording a failed setup -> "never asked the rule -- not answerable", exit 77.
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
                check(!YcbcrRenderer::readsDeviceImage(device, proposal.swFormat, proposal.imageFormat),
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
                    const std::vector<uint8_t> converted = drawAndRead(device, std::move(*frame));
                    if (kHaveReference) {
                        (void)gradeColour(label, fixture.file, *picture, converted, fixture.swsMatrix,
                                          fixture.fullRange, fixture.swsOtherMatrix);
                    } else {
                        std::printf("%s: built without libswscale, so the fallback's colour is not graded -- not "
                                    "answerable\n",
                                    label.c_str());
                        g_notVerified = true;
                    }
                    const int more = drawRest(device, decoder);
                    std::printf("%s: H.265 4:4:4 12-bit -> ffmpeg proposed %s, the rule refused it, %s, %d frames "
                                "drawn\n",
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
    //       The constructor is given a second picture of the stream: the first is moved into the
    //       renderer that accepts it.
    // CONTROL: renderer/video/ycbcr_renderer.cpp, each through control_batch.sh and restored green.
    //       readsDeviceImage answering from the layout without asking the device: "still accepts P010
    //       on a device that cannot filter" and "the decoder did not take software", the decoder
    //       asking the same function. The constructor's device refusal thrown as
    //       WallpaperContentError: "refused the device frame as content".
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
        std::optional<DecodedFrame> second = decoder.next();
        if (!second || second->storage() != DecodedFrame::Storage::DeviceImage) {
            fail("the 10-bit fixture did not decode a second picture on the device");
            return;
        }
        const DecodedFrame::DeviceImage image = frame->deviceImage();
        check(YcbcrRenderer::readsDeviceImage(device, frame->pixelFormat(), image.format),
              "readsDeviceImage refuses a device frame the decoder chose to decode on the device");
        (void)drawAndRead(device, std::move(*second));
        device.waitForSubmissions();

        StrippedX6LinearFilter stripped;
        check(!YcbcrRenderer::readsDeviceImage(device, frame->pixelFormat(), image.format),
              "readsDeviceImage still accepts P010 on a device that cannot filter its plane format");
        bool constructorRefused = false;
        try {
            YcbcrRenderer refused(device, std::move(*frame), kShaderDir);
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
        if (host->storage() == DecodedFrame::Storage::HostPlanes) (void)drawAndRead(device, std::move(*host));
        std::printf("%s: H.265 Main10 -> Hardware; with its plane format unfilterable -> %s\n", label.c_str(),
                    pathName(fallback.path()));
    } catch (const std::exception& e) {
        reportThrow(label, "the H.265 Main10 fixture", e);
    }
}

// ------------------------------------------------------------------------- //
// S15.957 -- what the decoder asks of its decode pool, and what the renderer refuses
// ------------------------------------------------------------------------- //

std::optional<ReceivedVkFrame> receivedFor(VkImage image) {
    std::lock_guard lock(g_receivedMutex);
    for (const ReceivedVkFrame& received : g_receivedVkFrames) {
        if (received.image == image) return received;
    }
    return std::nullopt;
}

void openProposalWindow() {
    {
        std::lock_guard lock(g_poolInitMutex);
        g_poolInit = DecodePoolInit{};
    }
    g_linearProposals = 0;
    g_mutableStripped = 0;
    g_queriesInWindow = 0;
    g_proposalWindow = false;
}

DecodePoolInit poolInit() {
    std::lock_guard lock(g_poolInitMutex);
    return g_poolInit;
}

// The constructor given `frame`, one field of whose image description the caller has made false.
// Graded on checkDecoderImage's own message, so a refusal from anywhere else does not count.
void gradeContractRefusal(Device& device, DecodedFrame&& frame, const std::string& label, const char* what,
                          const char* message) {
    bool refused = false;
    try {
        YcbcrRenderer accepted(device, std::move(frame), kShaderDir);
        std::fprintf(stderr, "FAIL: %s: the constructor accepted a decoder image %s\n", label.c_str(), what);
    } catch (const std::logic_error& e) {
        refused = std::strstr(e.what(), message) != nullptr;
        if (!refused) std::fprintf(stderr, "FAIL: %s: an image %s was refused as: %s\n", label.c_str(), what, e.what());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s: an image %s raised: %s\n", label.c_str(), what, e.what());
    }
    check(refused, std::string("YcbcrRenderer refuses a decoder image ") + what);
}

void runDecodePoolChecks(Device& device, const std::string& label) {
    if (device.info().videoDecodeOps == 0 || !haveAsset("conv_hevc10.mp4", label)) return;  // said by the gate
    const std::string file = asset("conv_hevc10.mp4");

    // GOAL: S15.957 -- the decode pool the decoder hands ffmpeg asks for SAMPLED usage, which the
    //       renderer's plane views read, and for MUTABLE_FORMAT, which a plane view in its own format
    //       needs. False if either is missing from what av_hwframe_ctx_init is given.
    // WHY-THIS-SHAPE: read at av_hwframe_ctx_init's entry, not from the finished pool: ffmpeg adds
    //       SAMPLED itself wherever the format can be sampled, so the finished pool carries it whether
    //       or not the decoder asked. ffmpeg's own DPB pool is also built with this call; only the
    //       first Vulkan init after the stream's proposal is the decoder's.
    // CONTROL: renderer/video/video_decoder.cpp attachHardwareFrames(), each through control_batch.sh.
    //       `| renderer` dropped from `asked` -> "FAIL: the decoder's decode pool asks for SAMPLED usage".
    //       img_flags given only EXTENDED_USAGE -> "FAIL: the decoder's decode pool asks for MUTABLE_FORMAT".
    //       Both held (red, then green restored).
    std::optional<VideoDecoder> decoder;
    std::optional<DecodedFrame> frames[4];
    try {
        openProposalWindow();
        decoder.emplace(device, file);
        for (std::optional<DecodedFrame>& frame : frames) frame = decoder->next();
        g_proposalWindow = false;
    } catch (const std::exception& e) {
        g_proposalWindow = false;
        reportThrow(label, "the decode-pool fixture", e);
        return;
    }
    for (const std::optional<DecodedFrame>& frame : frames) {
        if (!frame || frame->storage() != DecodedFrame::Storage::DeviceImage) {
            std::printf("%s: H.265 Main10 did not decode four pictures on the device here, so the decode pool "
                        "cannot be read -- not answerable\n",
                        label.c_str());
            g_notVerified = true;
            return;
        }
    }
    const DecodePoolInit init = poolInit();
    if (init.calls == 0) {
        fail("the stream decoded on the device, yet no av_hwframe_ctx_init followed its proposal");
    } else {
        check((init.usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0, "the decoder's decode pool asks for SAMPLED usage");
        check((init.flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) != 0,
              "the decoder's decode pool asks for MUTABLE_FORMAT");
        std::printf("%s: decode pool asked usage 0x%x, flags 0x%x\n", label.c_str(), init.usage, init.flags);
    }

    // GOAL: S15.957 -- YcbcrRenderer refuses a decoder image of the wrong format, one without SAMPLED
    //       usage, and one not optimally tiled, each with checkDecoderImage's std::logic_error.
    // WHY-THIS-SHAPE: a real decoded picture whose description is edited where deviceImage() reads it
    //       -- the pool's format and usage, the frame's tiling -- and restored at once. The image
    //       itself stays valid, so a renderer that skips the check builds views on it and is seen
    //       accepting it, rather than crashing. A hand-made DecodedFrame cannot exist.
    // CONTROL: renderer/video/ycbcr_renderer.cpp checkDecoderImage(), each `if` made `if (false)`,
    //       through control_batch.sh, each held: "FAIL: YcbcrRenderer refuses a decoder image of
    //       another format", "... without SAMPLED usage", "... not optimally tiled".
    struct Lie {
        const char* what;
        const char* message;
    };
    const Lie lies[3] = {{"of another format", "format does not match"},
                         {"without SAMPLED usage", "cannot be sampled"},
                         {"not optimally tiled", "not optimally tiled"}};
    for (int i = 0; i < 3; ++i) {
        DecodedFrame& frame = *frames[i + 1];
        const std::optional<ReceivedVkFrame> received = receivedFor(frame.deviceImage().image);
        if (!received) {
            fail("a device picture was handed back without avcodec_receive_frame seeing it");
            continue;
        }
        auto* pool = static_cast<AVVulkanFramesContext*>(received->pool->hwctx);
        const VkFormat format = pool->format[0];
        const VkImageUsageFlagBits usage = pool->usage;
        const VkImageTiling tiling = received->frame->tiling;
        if (i == 0) {
            pool->format[0] = format == VK_FORMAT_G8_B8R8_2PLANE_444_UNORM ? VK_FORMAT_G8_B8R8_2PLANE_420_UNORM
                                                                          : VK_FORMAT_G8_B8R8_2PLANE_444_UNORM;
        }
        if (i == 1) pool->usage = static_cast<VkImageUsageFlagBits>(usage & ~VK_IMAGE_USAGE_SAMPLED_BIT);
        if (i == 2) received->frame->tiling = VK_IMAGE_TILING_LINEAR;
        gradeContractRefusal(device, std::move(frame), label, lies[i].what, lies[i].message);
        device.waitForSubmissions();
        pool->format[0] = format;
        pool->usage = usage;
        received->frame->tiling = tiling;
    }
    for (std::optional<DecodedFrame>& frame : frames) frame.reset();
    decoder.reset();

    // GOAL: S15.957 -- a proposal for a decode pool that is not optimally tiled is sent to software
    //       at once: the decoder asks the device nothing about it, creates no pool, and decodes on the
    //       processor. False if a linear pool goes on to be asked about or built.
    // WHY-THIS-SHAPE: the proposal is made linear as it leaves avcodec_get_hw_frames_parameters, and
    //       the device queries after it are counted. Grading the path alone cannot fail: on this card
    //       a linear decode pool is refused further on anyway, so a decoder without the tiling test
    //       still ends on SoftwareRefused, having asked the device about the linear pool first.
    // CONTROL: renderer/video/video_decoder.cpp attachHardwareFrames(), `vk->tiling !=
    //       VK_IMAGE_TILING_OPTIMAL ||` deleted from the refusal, through control_batch.sh: "FAIL: a linear
    //       decode-pool proposal is refused before the device is asked about it". Held.
    // COVERAGE-ASSERT: the injection is asserted to have made a proposal linear.
    try {
        openProposalWindow();
        g_proposeLinearTiling = true;
        VideoDecoder linear(device, file);
        std::optional<DecodedFrame> frame = linear.next();
        g_proposeLinearTiling = false;
        g_proposalWindow = false;
        const int queries = g_queriesInWindow.load();
        check(g_linearProposals.load() > 0, "the linear-tiling injection reached a decode-pool proposal");
        if (queries != 0) {
            std::fprintf(stderr, "FAIL: %s: %d device queries followed a linear decode-pool proposal\n",
                         label.c_str(), queries);
        }
        check(queries == 0, "a linear decode-pool proposal is refused before the device is asked about it");
        check(poolInit().calls == 0, "a linear decode-pool proposal created no decode pool");
        check(linear.path() == VideoDecoder::Path::SoftwareRefused,
              "a linear decode-pool proposal sends the stream to software");
        check(frame && frame->storage() == DecodedFrame::Storage::HostPlanes,
              "a stream whose decode pool would be linear decodes into host memory");
        std::printf("%s: linear proposal -> %s, %d queries after it\n", label.c_str(), pathName(linear.path()),
                    queries);
    } catch (const std::exception& e) {
        g_proposeLinearTiling = false;
        g_proposalWindow = false;
        reportThrow(label, "the linear-tiling proposal", e);
    }

    // GOAL: S15.957 -- a device whose video-format answer for the stream offers no MUTABLE_FORMAT sends
    //       the stream to software, creating no decode pool. False if the decoder decodes on the device
    //       into images its plane views cannot be made on.
    // WHY-THIS-SHAPE: the flag is stripped only from the decoder's own query -- the one inside the
    //       window after ffmpeg's proposal -- so ffmpeg's setup query still succeeds and the stream
    //       reaches the requirement rather than being turned away before it.
    // CONTROL: renderer/video/video_decoder.cpp attachHardwareFrames(), the MUTABLE_FORMAT refusal made
    //       `if (false)`, through control_batch.sh: "FAIL: a device offering no MUTABLE_FORMAT for the
    //       stream sends it to software". Held.
    // COVERAGE-ASSERT: the injection is asserted to have stripped the flag from an answer.
    try {
        openProposalWindow();
        g_hideMutableFormat = true;
        VideoDecoder immutable(device, file);
        std::optional<DecodedFrame> frame = immutable.next();
        g_hideMutableFormat = false;
        g_proposalWindow = false;
        check(g_mutableStripped.load() > 0, "the injection stripped MUTABLE_FORMAT from the decoder's format query");
        check(immutable.path() == VideoDecoder::Path::SoftwareRefused,
              "a device offering no MUTABLE_FORMAT for the stream sends it to software");
        check(poolInit().calls == 0, "a device offering no MUTABLE_FORMAT gets no decode pool built");
        check(frame && frame->storage() == DecodedFrame::Storage::HostPlanes,
              "a stream refused for MUTABLE_FORMAT decodes into host memory");
        std::printf("%s: no MUTABLE_FORMAT offered -> %s\n", label.c_str(), pathName(immutable.path()));
    } catch (const std::exception& e) {
        g_hideMutableFormat = false;
        g_proposalWindow = false;
        reportThrow(label, "the MUTABLE_FORMAT refusal", e);
    }
}

// ------------------------------------------------------------------------- //
// S15.958 -- a submission's scope holds four of each kind without allocating
// ------------------------------------------------------------------------- //

VkSemaphore fakeSemaphore(uintptr_t n) { return reinterpret_cast<VkSemaphore>(0x1000 + n * 16); }

bool holdsFour(const Device::SubmitScope& scope) {
    if (scope.wait.size() != 4 || scope.signal.size() != 4 || scope.timelineWait.size() != 4 ||
        scope.timelineSignal.size() != 4) {
        return false;
    }
    for (uintptr_t i = 0; i < 4; ++i) {
        const auto& wait = scope.wait.begin()[i];
        const auto& timelineWait = scope.timelineWait.begin()[i];
        const auto& timelineSignal = scope.timelineSignal.begin()[i];
        if (wait.semaphore != fakeSemaphore(i) || wait.stage != VK_PIPELINE_STAGE_ALL_COMMANDS_BIT ||
            scope.signal.begin()[i] != fakeSemaphore(10 + i) || timelineWait.semaphore != fakeSemaphore(20 + i) ||
            timelineWait.value != 100 + i || timelineSignal.semaphore != fakeSemaphore(30 + i) ||
            timelineSignal.value != 200 + i) {
            return false;
        }
    }
    return true;
}

void runSubmitScopeChecks(Device& device, const std::string& label) {
    using Scope = Device::SubmitScope;
    constexpr VkPipelineStageFlags kStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

    // GOAL: S15.958 -- a SubmitScope holds four entries of each kind, built by emplace_back, push_back
    //       and an initializer list, kept intact through a copy and an assignment, with no operator new.
    // WHY-THIS-SHAPE: the entries are compared by value after the copy and the assignment, since a
    //       copy is how DecodedFrame::submit takes its scope; a size check alone would pass a copy that
    //       lost its contents. Fake handles: nothing here reaches Vulkan.
    // CONTROL: renderer/vulkan/device.h, each through control_batch.sh, each held. kCapacity 4 -> 3:
    //       "FAIL: Device::SubmitScope holds four entries of each kind, intact through a copy". The
    //       copy constructor staging through a std::vector: "FAIL: building, copying and assigning a
    //       full SubmitScope allocates nothing".
    // COVERAGE-ASSERT: one deliberate allocation inside the counted region must be counted.
#if WALLPAPERX_ALLOCATION_INSTRUMENT
    long allocations = -1;
    long probe = 0;
    bool held = false;
    std::string threw;
    {
        const CountAllocations counting;
        const long before = g_allocations.load();
        try {
            Scope scope;
            for (uintptr_t i = 0; i < 4; ++i) {
                scope.wait.emplace_back(fakeSemaphore(i), kStage);
                scope.signal.push_back(fakeSemaphore(10 + i));
                scope.timelineWait.emplace_back(fakeSemaphore(20 + i), 100 + i, kStage);
            }
            scope.timelineSignal = {Scope::TimelineSignal(fakeSemaphore(30), 200), Scope::TimelineSignal(fakeSemaphore(31), 201),
                                    Scope::TimelineSignal(fakeSemaphore(32), 202), Scope::TimelineSignal(fakeSemaphore(33), 203)};
            const Scope copied(scope);
            Scope assigned;
            assigned = copied;
            allocations = g_allocations.load() - before;
            held = holdsFour(scope) && holdsFour(copied) && holdsFour(assigned);
        } catch (const std::exception& e) {
            threw = e.what();
        }
        const long beforeProbe = g_allocations.load();
        int* volatile deliberate = new int(1);  // volatile: a plain new/delete pair may be elided
        delete deliberate;
        probe = g_allocations.load() - beforeProbe;
    }
    if (!threw.empty()) std::fprintf(stderr, "FAIL: %s: a full SubmitScope threw: %s\n", label.c_str(), threw.c_str());
    check(held, "Device::SubmitScope holds four entries of each kind, intact through a copy");
    check(probe == 1, "the allocation counter saw the deliberate allocation in the SubmitScope check");
    // A scope that threw before the count was taken has no count; the check above already failed.
    if (threw.empty()) {
        if (allocations != 0) {
            std::fprintf(stderr, "FAIL: %s: a full SubmitScope made %ld operator new calls\n", label.c_str(),
                         allocations);
        }
        check(allocations == 0, "building, copying and assigning a full SubmitScope allocates nothing");
    }
#else
    std::printf("%s: built with a sanitizer, whose allocator the counter cannot replace -- SubmitScope's "
                "allocations not answerable\n",
                label.c_str());
    g_notVerified = true;
#endif

    // GOAL: S15.958 -- a fifth entry of any kind throws std::length_error and leaves the four in place,
    //       and a scope over-filled while it is built for Device::submit submits nothing.
    // WHY-THIS-SHAPE: the over-filled scope is built in the argument to a real submit, with real
    //       semaphores, so a list that dropped the fifth quietly would be seen submitting. The other
    //       three kinds are only filled, with fake handles that must never reach a submit.
    // CONTROL: renderer/vulkan/device.h emplace_back, the full case returning the last entry instead of
    //       throwing, through control_batch.sh: "FAIL: a fifth timeline signal in a scope built for
    //       Device::submit throws std::length_error and submits nothing". Held.
    auto fifth = [&](auto& list, auto entry, const char* kind) {
        bool lengthError = false;
        try {
            list.push_back(entry);
        } catch (const std::length_error&) {
            lengthError = true;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "FAIL: %s: a fifth %s raised: %s\n", label.c_str(), kind, e.what());
        }
        check(lengthError && list.size() == 4,
              std::string("a fifth ") + kind + " throws std::length_error and leaves four entries");
    };
    {
        Scope scope;
        for (uintptr_t i = 0; i < 4; ++i) {
            scope.wait.emplace_back(fakeSemaphore(i), kStage);
            scope.signal.push_back(fakeSemaphore(10 + i));
            scope.timelineWait.emplace_back(fakeSemaphore(20 + i), 100 + i, kStage);
        }
        fifth(scope.wait, Scope::Wait(fakeSemaphore(4), kStage), "wait");
        fifth(scope.signal, fakeSemaphore(14), "signal");
        fifth(scope.timelineWait, Scope::TimelineWait(fakeSemaphore(24), 104, kStage), "timeline wait");
    }

    // GOAL: S15.958 -- Device::submit carries all four entries of each kind to the queue: every one of
    //       four timeline signals is reached, after waits on four binary semaphores and four timelines.
    // WHY-THIS-SHAPE: the counters are read after the batch completed, so a dropped signal reads as a
    //       value short, not as a hang. Timeline waits are on value 0, which never blocks: a dropped
    //       signal must not wedge the queue the rest of the probe runs on.
    // CONTROL: renderer/vulkan/device.cpp submit(), the flattening of timeline signals skipping the
    //       last entry, through control_batch.sh: "FAIL: a submission with four timeline signals
    //       signals all four". Held.
    VkDevice vk = device.device();
    std::array<VkSemaphore, 4> binary{};
    std::array<VkSemaphore, 4> timeline{};
    auto destroy = [&] {
        try {
            device.waitForSubmissions();
        } catch (const std::exception& e) {
            reportThrow(label, "waiting before the SubmitScope semaphores are destroyed", e);
        }
        for (VkSemaphore s : binary) if (s) vkDestroySemaphore(vk, s, nullptr);
        for (VkSemaphore s : timeline) if (s) vkDestroySemaphore(vk, s, nullptr);
    };
    try {
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo timelineInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
        const VkSemaphoreCreateInfo binaryInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (size_t i = 0; i < 4; ++i) {
            if (vkCreateSemaphore(vk, &binaryInfo, nullptr, &binary[i]) != VK_SUCCESS ||
                vkCreateSemaphore(vk, &timelineInfo, nullptr, &timeline[i]) != VK_SUCCESS) {
                throw std::runtime_error("vkCreateSemaphore failed");
            }
        }
        const auto nothing = [](VkCommandBuffer) {};

        Scope full;
        for (uintptr_t i = 0; i < 4; ++i) full.timelineSignal.emplace_back(timeline[i], 1 + i);
        const int before = g_queueSubmits.load();
        bool lengthError = false;
        try {
            device.submit(nothing, [&] {
                Scope over = full;
                over.timelineSignal.emplace_back(timeline[0], 9);
                return over;
            }());
        } catch (const std::length_error&) {
            lengthError = true;
        }
        check(lengthError && g_queueSubmits.load() == before,
              "a fifth timeline signal in a scope built for Device::submit throws std::length_error and submits "
              "nothing");

        Scope signalling;
        for (VkSemaphore s : binary) signalling.signal.push_back(s);
        device.submit(nothing, signalling);
        Scope waiting = full;
        for (size_t i = 0; i < 4; ++i) {
            waiting.wait.emplace_back(binary[i], kStage);
            waiting.timelineWait.emplace_back(timeline[i], 0, kStage);
        }
        device.wait(device.submit(nothing, waiting));
        bool all = true;
        for (size_t i = 0; i < 4; ++i) {
            uint64_t value = 0;
            if (vkGetSemaphoreCounterValue(vk, timeline[i], &value) != VK_SUCCESS) throw std::runtime_error("counter");
            if (value != 1 + i) {
                std::fprintf(stderr, "FAIL: %s: timeline signal %zu reads %llu, expected %zu\n", label.c_str(), i,
                             static_cast<unsigned long long>(value), 1 + i);
                all = false;
            }
        }
        check(all, "a submission with four timeline signals signals all four");
    } catch (const std::exception& e) {
        reportThrow(label, "the SubmitScope submissions", e);
    }
    destroy();
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
    //       does on the same GPU with the feature: every frame decodes and is drawn by YcbcrRenderer,
    //       on the same path.
    // WHY-THIS-SHAPE: compared with the same GPU built with the feature, in this run, rather than
    //       asserting Hardware: whether ffmpeg needs the feature is the open question, and the
    //       property worth holding is that its absence changes nothing -- not that a fallback
    //       exists. Frame counts are compared so a stream that stops early cannot pass as playing.
    // CONTROL: through control_batch.sh, each restored green. renderer/video/video_decoder.cpp
    //       openDecoder() refusing to SoftwareRefused whenever the feature is absent: "10 frames on
    //       SoftwareRefused; with it: 10 frames on Hardware". renderer/video/ycbcr_renderer.cpp's
    //       constructor throwing WallpaperContentError without the feature: "conv_709_tv.mp4 threw".
    //       Harness, earlier: feature not hidden -> "was not hidden, so a device without it was never built".
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
            const int frames = drawRest(device, decoder);
            if (frames != baselineFrames || decoder.path() != baselinePath) {
                std::fprintf(stderr,
                             "FAIL: %s: %s without samplerYcbcrConversion: %d frames on %s; with it: %d frames on %s\n",
                             label.c_str(), name, frames, pathName(decoder.path()), baselineFrames,
                             pathName(baselinePath));
                fail("a device without samplerYcbcrConversion does not play the stream as one with it does");
            }
            std::printf("%s: %s without samplerYcbcrConversion: %d frames drawn on %s (with it: %d on %s)\n",
                        label.c_str(), name, frames, pathName(decoder.path()), baselineFrames, pathName(baselinePath));
        } catch (const std::exception& e) {
            reportThrow(label, name, e);
        }
    }
}

// ------------------------------------------------------------------------- //
// S13.54 -- frames that change shape or colour mid-stream
// ------------------------------------------------------------------------- //

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
    // WHY-THIS-SHAPE: colour-host and colour-device draw each picture through a YcbcrRenderer of
    //       their own; this grades what the pipeline's decode, renderer choice and draw leave
    //       together. The target is the visible size, so the draw is one to one and each pixel is
    //       the picture's. The device fixtures are colour-device's; one that decodes in software here
    //       is a skip.
    // CONTROL: renderer/video/ycbcr_planes.cpp BT.709 coefficients set to BT.601's, through
    //       control_batch.sh: "conv_709_tv.mp4 through the pipeline differs from libswscale by 32
    //       codes" on the host entry and "conv_709_tv.mp4 through the pipeline differs" on the device
    //       entry, restored green.
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

// Expects show() to throw exactly `Expected` and to submit nothing; `frame` is left as it was.
template <class Expected>
bool refusedWithoutSubmitting(YcbcrRenderer& renderer, DecodedFrame& frame, const std::string& label,
                              const char* what) {
    const int before = g_queueSubmits.load();
    bool threw = false;
    try {
        renderer.show(std::move(frame));
    } catch (const Expected&) {
        threw = true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s: %s raised the wrong exception: %s\n", label.c_str(), what, e.what());
    }
    const bool submitted = g_queueSubmits.load() != before;
    if (!threw) {
        std::fprintf(stderr, "FAIL: %s: %s was not refused\n", label.c_str(), what);
        fail("show() did not refuse a frame its contract refuses");
    }
    if (submitted) {
        std::fprintf(stderr, "FAIL: %s: %s submitted a batch before throwing\n", label.c_str(), what);
        fail("show() submitted work for a frame it then refused");
    }
    return threw && !submitted;
}

void runShapeResizeChecks(Device& device, const std::string& label) {
    if (!videoExtensionsHidden(device, label)) return;
    if (!haveAsset("shape_resize.h264", label)) return;

    // GOAL: S13.54 -- a frame whose size differs from the renderer's shape is refused by show() with
    //       std::invalid_argument before anything is submitted, and a renderer built for the new
    //       picture draws it: a resolution change needs a new renderer, and gets a working one.
    // WHY-THIS-SHAPE: a real stream whose second half is a different size, decoded by the real
    //       decoder, so what reaches show() is what a mid-stream change produces rather than a
    //       frame this probe could not build on its own. Submissions are counted at vkQueueSubmit
    //       so "nothing is submitted on a throw" is measured, not inferred from the exception.
    //       Decoded in software: the refusal is the renderer's and does not depend on the path, and
    //       a size change on the hardware path is graded by video_pipeline_probe's shape-device entry.
    // CONTROL: renderer/video/ycbcr_renderer.cpp show(), each through control_batch.sh and restored
    //       green. An empty submit ahead of the shape check: "a frame of a different size submitted a
    //       batch before throwing". The refusal as std::runtime_error: "a frame of a different size
    //       raised the wrong exception". Harness, earlier: a single-size stream -> both coverage
    //       lines; video extensions left visible -> "were not hidden".
    // COVERAGE-ASSERT: the stream must yield two sizes and at least one refusal, so a fixture that
    //       stopped changing size fails instead of passing with nothing refused.
    try {
        VideoDecoder decoder(device, asset("shape_resize.h264"));
        OffscreenTargetManager targets(device);
        const OffscreenTarget& target = targets.create(1, 64, 64);
        std::optional<YcbcrRenderer> renderer;
        int drawn = 0;
        int refused = 0;
        std::vector<uint32_t> widths;
        while (std::optional<DecodedFrame> frame = decoder.next()) {
            if (std::find(widths.begin(), widths.end(), frame->visible().extent.width) == widths.end()) {
                widths.push_back(frame->visible().extent.width);
            }
            if (!renderer) {
                renderer.emplace(device, std::move(*frame), kShaderDir);
            } else if (YcbcrRenderer::shapeOf(*frame) != renderer->shape()) {
                if (refusedWithoutSubmitting<std::invalid_argument>(*renderer, *frame, label,
                                                                    "a frame of a different size")) {
                    ++refused;
                }
                device.waitForSubmissions();
                renderer.reset();
                renderer.emplace(device, std::move(*frame), kShaderDir);
            } else {
                renderer->show(std::move(*frame));
            }
            targets.render(*renderer, target);
            ++drawn;
        }
        device.waitForSubmissions();
        check(widths.size() == 2, "the resize fixture did not decode to two picture sizes");
        check(refused >= 1, "no frame of the resized stream was refused by the renderer built for the first size");
        std::printf("%s: resized stream -> %zu sizes on %s, %d frames drawn, %d refused before a rebuild\n",
                    label.c_str(), widths.size(), pathName(decoder.path()), drawn, refused);
    } catch (const std::exception& e) {
        reportThrow(label, "the resized stream", e);
    }
}

void runShapeTagChecks(Device& device, const std::string& label) {
    if (!haveAsset("shape_tags.h264", label)) return;

    // GOAL: S13.54 -- a frame whose colour tags change to ones this renderer cannot draw is refused
    //       by show() with WallpaperContentError, before anything is submitted, while the frames
    //       before the change are drawn.
    // CONTROL: renderer/video/ycbcr_renderer.cpp show(), each through control_batch.sh and restored
    //       green. An empty submit ahead of its checks: "a frame retagged as PQ submitted a batch
    //       before throwing". show() reusing the shown picture's colour instead of asking colourMapFor:
    //       "a frame retagged as PQ was not refused".
    try {
        VideoDecoder decoder(device, asset("shape_tags.h264"));
        OffscreenTargetManager targets(device);
        const OffscreenTarget& target = targets.create(1, 64, 64);
        std::optional<YcbcrRenderer> renderer;
        int drawn = 0;
        int refused = 0;
        while (std::optional<DecodedFrame> frame = decoder.next()) {
            if (frame->colour().transfer == AVCOL_TRC_SMPTE2084) {
                if (!renderer) {
                    fail("the retagged stream's first picture is already tagged PQ, so no renderer was built");
                    break;
                }
                if (refusedWithoutSubmitting<WallpaperContentError>(*renderer, *frame, label,
                                                                    "a frame retagged as PQ")) {
                    ++refused;
                }
                continue;
            }
            if (!renderer) {
                renderer.emplace(device, std::move(*frame), kShaderDir);
            } else {
                renderer->show(std::move(*frame));
            }
            targets.render(*renderer, target);
            ++drawn;
        }
        device.waitForSubmissions();
        check(refused >= 1, "the retagged stream produced no frame tagged PQ");
        std::printf("%s: retagged stream -> %d drawn, %d refused as PQ\n", label.c_str(), drawn, refused);
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
    //       still answers, later frames arrive from host memory, the YcbcrRenderer built for the
    //       device frames refuses the first with std::invalid_argument before submitting, the path is
    //       SoftwareRefused, and every picture of the stream is drawn once, in order.
    // WHY-THIS-SHAPE: the failure is injected at avcodec_receive_frame after three device frames,
    //       because no file makes a working hardware decoder fail mid-stream. Each draw is read
    //       back and identified by the frame number the fixture draws into it, against libavcodec
    //       decoding the file directly. A frame count cannot tell a repeat and a skip that cancel, and
    //       the decoder's timestamps are what it decides from.
    // CONTROL: through control_batch.sh, each restored green. renderer/video/video_decoder.cpp
    //       admit()'s DropThrough test `<=` made `<`: "pictures drawn against ... 1 drawn more than
    //       once". renderer/video/ycbcr_renderer.cpp show()'s shape refusal as std::runtime_error: "the
    //       first host frame after the fallback raised the wrong exception".
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
        OffscreenTargetManager targets(device);
        const OffscreenTarget* target = nullptr;
        std::optional<YcbcrRenderer> renderer;
        std::vector<int> converted;
        int deviceFrames = 0;
        int hostFrames = 0;
        bool refusedTheSwitch = false;
        // Shows `frame` -- through a new renderer when there is none -- draws it one to one and
        // reads which picture it is.
        auto identify = [&](DecodedFrame&& frame) {
            const VkExtent2D size = frame.visible().extent;
            if (target == nullptr) target = &targets.create(1, size.width, size.height);
            if (!renderer) {
                renderer.emplace(device, std::move(frame), kShaderDir);
            } else {
                renderer->show(std::move(frame));
            }
            targets.render(*renderer, *target);
            const std::vector<uint8_t> pixels = wallpaperx::testing::reclaimAndReadback(device, targets, *target);
            converted.push_back(wallpaperx::testing::counterFromBgra(pixels, target->width, target->height));
        };
        while (std::optional<DecodedFrame> frame = decoder.next()) {
            if (frame->storage() == DecodedFrame::Storage::DeviceImage) {
                identify(std::move(*frame));
                if (++deviceFrames == kDeviceFramesFirst) g_receiveExternalRemaining = 1;
                continue;
            }
            if (hostFrames++ == 0 && renderer) {
                refusedTheSwitch = refusedWithoutSubmitting<std::invalid_argument>(
                    *renderer, *frame, label, "the first host frame after the fallback");
                device.waitForSubmissions();
                renderer.reset();
            }
            identify(std::move(*frame));
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
        check(refusedTheSwitch, "the renderer built for device frames did not refuse the first host frame");
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
                         "FAIL: %s: %zu pictures drawn against %zu in an independent decode: %d drawn more "
                         "than once, %d never drawn\n",
                         label.c_str(), converted.size(), reference->ids.size(), repeated, missing);
            fail("the pictures drawn across a mid-stream fallback are not the stream's, each once, in order");
        }
        std::printf("%s: %d device frames, then the hardware failed; %d host frames followed, %zu pictures drawn "
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
        const std::vector<uint32_t> words = wallpaperx::readSpirv(shader, "video_draw_probe");
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
        const std::vector<uint32_t> words = wallpaperx::readSpirv(kShortReadFile, "video_draw_probe");
        std::fprintf(stderr, "FAIL: %s: %s read as %zu words\n", label.c_str(), kShortReadFile, words.size());
    } catch (const std::runtime_error&) {
        refused = true;
    }
    check(refused, "readSpirv accepted a file that read short");

    // GOAL: S13.51 -- a ShaderModule built from a file that reads short throws before the driver
    //       is handed anything.
    // WHY-THIS-SHAPE: built on a null VkDevice, so this entry still opens no driver; the
    //       interposed vkCreateShaderModule counts the call and refuses a null device instead of
    //       forwarding it, so a module that reaches the driver is reported, not a crash.
    // CONTROL: renderer/vulkan/shader_module.cpp readSpirv's short-read test replaced by `false`,
    //       through control_batch.sh: "a file that read short was handed to vkCreateShaderModule",
    //       restored green.
    const int before = g_shaderModulesCreated.load();
    bool moduleRefused = false;
    try {
        wallpaperx::ShaderModule module(VK_NULL_HANDLE, kShortReadFile, "video_draw_probe");
    } catch (const std::runtime_error&) {
        moduleRefused = true;
    }
    check(moduleRefused, "a shader module was built from a file that read short");
    check(g_shaderModulesCreated.load() == before, "a file that read short was handed to vkCreateShaderModule");
    std::printf("%s: a valid shader read in full; a file that reads short was refused, by readSpirv and by "
                "ShaderModule\n",
                label.c_str());
}

// ------------------------------------------------------------------------- //
// Placement -- where the picture lands on a target, and what a refusal leaves there
// ------------------------------------------------------------------------- //
//
// The ramp fixtures (renderer/CMakeLists.txt, ramp_*) name their own positions: red rises with x,
// green with y, blue is constant. A target pixel's red and green name the picture position it was
// sampled from, so the region shown is recovered from the drawn frame rather than compared with a
// stored image, which would make one driver's filter output the contract.

constexpr int kRampBlue = 128;

struct RecordedBarrier {
    VkImage image = VK_NULL_HANDLE;
    VkImageLayout oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout newLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t srcFamily = VK_QUEUE_FAMILY_IGNORED;
    uint32_t dstFamily = VK_QUEUE_FAMILY_IGNORED;
};
// Filled by the vkCmdPipelineBarrier interposer while g_recordingBarriers is on.
std::mutex g_barrierMutex;
std::vector<RecordedBarrier> g_barriers;
bool g_recordingBarriers = false;

void recordBarriers(bool on) {
    std::lock_guard lock(g_barrierMutex);
    if (on) g_barriers.clear();
    g_recordingBarriers = on;
}

std::vector<RecordedBarrier> barriersOn(VkImage image) {
    std::lock_guard lock(g_barrierMutex);
    std::vector<RecordedBarrier> matching;
    for (const RecordedBarrier& barrier : g_barriers) {
        if (barrier.image == image) matching.push_back(barrier);
    }
    return matching;
}

std::vector<RecordedBarrier> ownershipTransfersOf(VkImage image) {
    std::lock_guard lock(g_barrierMutex);
    std::vector<RecordedBarrier> matching;
    for (const RecordedBarrier& barrier : g_barriers) {
        if (barrier.image == image && barrier.srcFamily != barrier.dstFamily) matching.push_back(barrier);
    }
    return matching;
}

std::string layoutName(VkImageLayout layout) {
    switch (layout) {
        case VK_IMAGE_LAYOUT_GENERAL: return "GENERAL";
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return "SHADER_READ_ONLY_OPTIMAL";
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return "TRANSFER_DST_OPTIMAL";
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return "COLOR_ATTACHMENT_OPTIMAL";
        case VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR: return "VIDEO_DECODE_DST_KHR";
        case VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR: return "VIDEO_DECODE_DPB_KHR";
        default: return "VkImageLayout(" + std::to_string(static_cast<int>(layout)) + ")";
    }
}

// "Scaled to cover, cropped centrally" as the crop it takes from the picture, worked out here and
// never through placedRegion(), which is under test. Exact edges, however thin: YcbcrRenderer
// samples the exact crop, not one rounded to whole texels.
struct CoverCrop {
    double x0, y0, width, height;
};
CoverCrop exactCover(VkExtent2D picture, uint32_t targetWidth, uint32_t targetHeight) {
    const double scale = std::max(static_cast<double>(targetWidth) / picture.width,
                                  static_cast<double>(targetHeight) / picture.height);
    const double width = targetWidth / scale;
    const double height = targetHeight / scale;
    return {(picture.width - width) / 2, (picture.height - height) / 2, width, height};
}

constexpr int kCoverAxisPoints = 5;
constexpr int kCoverSamples = kCoverAxisPoints * kCoverAxisPoints;

struct CoverReport {
    int compared = 0, matchedX = 0, matchedY = 0, fromPicture = 0;
    double worstX = 0, worstY = 0, toleranceX = 0, toleranceY = 0;
};

// For a 5x5 grid of target pixels, the picture position each was sampled from against the cover
// rule. The tolerance is one target pixel's worth of picture (what a linear filter averages over),
// half a code of the 8-bit target's ramp, and 1.5 pixels for the filter's taps; everything this
// must catch -- a squeeze, an anchored crop, a quarter-size draw -- misses by tens of pixels.
CoverReport sampleCover(const std::vector<uint8_t>& pixels, VkExtent2D picture, uint32_t targetWidth,
                        uint32_t targetHeight) {
    const CoverCrop crop = exactCover(picture, targetWidth, targetHeight);
    CoverReport report;
    report.toleranceX = 1.5 + crop.width / targetWidth + 0.5 * (picture.width - 1) / 255.0;
    report.toleranceY = 1.5 + crop.height / targetHeight + 0.5 * (picture.height - 1) / 255.0;
    constexpr double kFractions[kCoverAxisPoints] = {0.0, 0.25, 0.5, 0.75, 1.0};
    for (const double fy : kFractions) {
        for (const double fx : kFractions) {
            const auto x = std::min(static_cast<uint32_t>(fx * (targetWidth - 1) + 0.5), targetWidth - 1);
            const auto y = std::min(static_cast<uint32_t>(fy * (targetHeight - 1) + 0.5), targetHeight - 1);
            const size_t offset = (static_cast<size_t>(y) * targetWidth + x) * 4;
            if (offset + 4 > pixels.size()) return report;  // compared < kCoverSamples: the caller fails
            ++report.compared;
            // The picture position this pixel's centre maps to, as the index a linear filter reads.
            const double u = crop.x0 + (x + 0.5) * crop.width / targetWidth;
            const double v = crop.y0 + (y + 0.5) * crop.height / targetHeight;
            const double expectedX = std::clamp(u - 0.5, 0.0, picture.width - 1.0);
            const double expectedY = std::clamp(v - 0.5, 0.0, picture.height - 1.0);
            const double gotX = pixels[offset + 2] * (picture.width - 1) / 255.0;
            const double gotY = pixels[offset + 1] * (picture.height - 1) / 255.0;
            report.worstX = std::max(report.worstX, std::abs(gotX - expectedX));
            report.worstY = std::max(report.worstY, std::abs(gotY - expectedY));
            if (std::abs(gotX - expectedX) <= report.toleranceX) ++report.matchedX;
            if (std::abs(gotY - expectedY) <= report.toleranceY) ++report.matchedY;
            // A cleared or letterboxed pixel did not come from the picture, and its blue says so.
            if (std::abs(pixels[offset] - kRampBlue) <= 4 && pixels[offset + 3] >= 250) ++report.fromPicture;
        }
    }
    return report;
}

// A ramp fixture's first picture, shown by a renderer of its own. Declared after the decoder, so
// the renderer, which may hold one of its images, goes first.
struct Ramp {
    Opened opened;
    VkExtent2D size{};
    // The decoder's image as the decoder left it, for a picture on the device path.
    std::optional<DecodedFrame::DeviceImage> deviceImage;
    std::optional<YcbcrRenderer> renderer;
};

const char* const kRampFiles[] = {"ramp_512x256.mp4", "ramp_256x512.mp4", "ramp_256x256.mp4", "ramp_512x2.mp4",
                                  "ramp_mirror_pq_512x256.mp4"};

std::unique_ptr<Ramp> openRamp(Device& device, const std::string& label, const char* file) {
    auto ramp = std::make_unique<Ramp>();
    ramp->opened = openFirst(device, asset(file));
    if (!ramp->opened.frame) {
        fail(std::string(file) + " decoded to no frames");
        return nullptr;
    }
    ramp->size = ramp->opened.frame->visible().extent;
    if (ramp->opened.frame->storage() == DecodedFrame::Storage::DeviceImage) {
        ramp->deviceImage = ramp->opened.frame->deviceImage();
    }
    std::printf("%s: %s, %ux%u, decoded on the %s path\n", label.c_str(), file, ramp->size.width,
                ramp->size.height, pathName(ramp->opened.decoder->path()));
    ramp->renderer.emplace(device, std::move(*ramp->opened.frame), kShaderDir);
    return ramp;
}

int g_coverCasesGraded = 0;

void gradeCover(Device& device, OffscreenTargetManager& targets, const Ramp& ramp, int monitorId,
                uint32_t targetWidth, uint32_t targetHeight, const char* what) {
    const OffscreenTarget& target = targets.create(monitorId, targetWidth, targetHeight);
    targets.render(const_cast<YcbcrRenderer&>(*ramp.renderer), target);
    const std::vector<uint8_t> pixels = wallpaperx::testing::reclaimAndReadback(device, targets, target);
    targets.destroy(monitorId);
    const CoverReport report = sampleCover(pixels, ramp.size, targetWidth, targetHeight);
    ++g_coverCasesGraded;

    check(report.compared == kCoverSamples, std::string("every sample point was read for: ") + what);
    const bool held = report.matchedX == report.compared && report.matchedY == report.compared;
    check(held, what);
    if (!held) {
        const CoverCrop crop = exactCover(ramp.size, targetWidth, targetHeight);
        std::fprintf(stderr,
                     "  %ux%u picture on a %ux%u target: expected x=[%.3f,%.3f) y=[%.3f,%.3f); %d of %d samples from "
                     "the right column (worst miss %.1f, tolerance %.1f), %d of %d from the right row (worst miss "
                     "%.1f, tolerance %.1f)\n",
                     ramp.size.width, ramp.size.height, targetWidth, targetHeight, crop.x0, crop.x0 + crop.width,
                     crop.y0, crop.y0 + crop.height, report.matchedX, report.compared, report.worstX,
                     report.toleranceX, report.matchedY, report.compared, report.worstY, report.toleranceY);
    }
    check(report.fromPicture == report.compared,
          std::string("every sampled pixel came from the picture rather than from a border, for: ") + what);
}

void runPlacementGeometry(Device& device, OffscreenTargetManager& targets, const std::string& label) {
    // GOAL: S13.20, S13.22 -- a picture whose aspect differs from the target's is scaled to cover
    //       and cropped centrally (never squeezed, letterboxed or cropped from a corner), and one
    //       whose crop is a quarter of a texel wide still shows the picture's middle column.
    // WHY-THIS-SHAPE: compared with the position each pixel names, not a stored image, which would
    //       fix one driver's filter as the contract and could not say which region was read. Four
    //       shapes: wider and taller take opposite arms of cover's scale, an equal aspect neither,
    //       and 512x2 on 8x64 crops 0.25 texel wide. The expectation is the exact crop (exactCover);
    //       a crop clamped to one whole texel was the blit's, which this renderer does not take.
    // CONTROL: through control_batch.sh, card and Lavapipe, each restored green.
    //       renderer/renderers/cover_crop.h placedRegion's cover scale `std::max(widthRatio,
    //       heightRatio)` -> `widthRatio`: "FAIL: a picture wider than the target is cropped left
    //       and right, not squeezed" (3 of 25 from the right column, worst miss 381.5) and the
    //       512x2 case, also wider; the other three green. placedRegion's cover `left`/`top` -> 0,
    //       an anchored crop: four cases red. ycbcr_renderer.cpp record()'s viewport halved: all
    //       five "came from the picture rather than from a border" lines red. placeSpan's exact1
    //       floored at exact0 + 64: the 512x2 case alone, "0 of 25 samples from the right column
    //       (worst miss 61.0, tolerance 2.5)".
    // COVERAGE-ASSERT: each case asserts all 25 samples were read, and g_coverCasesGraded below
    //       that all five cases ran; the latter observed red by hand with one gradeCover call
    //       removed. The 25-sample assert has no control: sampleCover has no input that stops
    //       it short of a readback smaller than the target.
    {
        std::unique_ptr<Ramp> wide = openRamp(device, label, "ramp_512x256.mp4");
        if (!wide) return;
        gradeCover(device, targets, *wide, 1, 64, 64, "a picture wider than the target is cropped left and right, "
                                                      "not squeezed");
        // GOAL: S13.21 -- the crop is decided per target: one renderer showing one picture crops a
        //       target of another shape on the other axis.
        // WHY-THIS-SHAPE: the renderer above, reused: a second renderer would pass for one that
        //       worked the crop out once, in show(), and kept it. A 2:1 picture is taller than a 4:1
        //       target, so this takes the other arm of the branch through the same renderer.
        // CONTROL: the anchored-crop break above reddens this case (on rows) while the case above
        //       goes red on columns; the squeeze break leaves this case green, so it is not a copy
        //       of the one above.
        gradeCover(device, targets, *wide, 2, 128, 32, "the same picture on a wider target is cropped top and bottom "
                                                       "instead");
    }
    {
        std::unique_ptr<Ramp> tall = openRamp(device, label, "ramp_256x512.mp4");
        if (!tall) return;
        gradeCover(device, targets, *tall, 3, 64, 64, "a picture taller than the target is cropped top and bottom, "
                                                      "not squeezed");
    }
    {
        std::unique_ptr<Ramp> square = openRamp(device, label, "ramp_256x256.mp4");
        if (!square) return;
        gradeCover(device, targets, *square, 4, 64, 64, "a picture with the target's own aspect is shown whole");
    }
    {
        // The x half carries this case: every pixel must come from columns 255.875 to 256.125. Its
        // two rows are fewer than the tolerance, so its y half is graded by the cases above.
        std::unique_ptr<Ramp> sliver = openRamp(device, label, "ramp_512x2.mp4");
        if (!sliver) return;
        gradeCover(device, targets, *sliver, 5, 8, 64,
                   "a crop a quarter of a texel wide still shows the picture's middle column");
    }
    check(g_coverCasesGraded == 5, "all five placement cases were drawn and read back");
}

void runPlacementRepeat(Device& device, OffscreenTargetManager& targets, const std::string& label) {
    std::unique_ptr<Ramp> ramp = openRamp(device, label, "ramp_256x256.mp4");
    if (!ramp) return;
    YcbcrRenderer& renderer = *ramp->renderer;

    // GOAL: S13.24 -- drawing the shown picture again is legal and gives the same picture: the
    //       renderer carries no state from one draw to the next.
    // WHY-THIS-SHAPE: two targets, because reading a target back reclaims it. Compared row for
    //       row with no tolerance: one picture, one placement, one pipeline, so any difference is
    //       state carried between draws.
    // CONTROL: renderer/video/ycbcr_renderer.cpp record() given `static int draws = 0; if
    //       (draws++ % 2 == 1) push.srcOffset[0] += 8.0f;`, each draw shifted by the one before it,
    //       through control_batch.sh: "FAIL: the shown picture drawn twice gives the same picture",
    //       restored green. (Shifting every draw after the first does not redden it: by this phase
    //       both draws are shifted alike.)
    const OffscreenTarget& first = targets.create(10, 64, 64);
    targets.render(renderer, first);
    const OffscreenTarget& second = targets.create(11, 64, 64);
    targets.render(renderer, second);
    const std::vector<uint8_t> firstPixels = wallpaperx::testing::reclaimAndReadback(device, targets, first);
    const std::vector<uint8_t> secondPixels = wallpaperx::testing::reclaimAndReadback(device, targets, second);
    const size_t differing = wallpaperx::testing::countDifferingRows(firstPixels, secondPixels, 64, 64);
    check(!firstPixels.empty() && differing == 0, "the shown picture drawn twice gives the same picture");
    if (differing != 0) std::fprintf(stderr, "  %zu of 64 rows differ between the two draws\n", differing);
    targets.destroy(10);
    targets.destroy(11);

    // GOAL: S13.25, restated for a draw -- a video draw leaves its target in kRenderedLayout, the
    //       layout the shared render pass ends in, and says so: the target records that layout as
    //       the one it was released from, and the next draw's acquire names the same layout.
    // WHY-THIS-SHAPE: record()'s return value only matters through the pair it makes with the
    //       release, so the release and the next acquire are read off the recorded barriers and
    //       compared to each other and to kRenderedLayout. No validation layer checks this:
    //       ownership transfers are not tracked.
    // CONTROL: through control_batch.sh, each restored green. renderer/video/ycbcr_renderer.cpp
    //       record() returning VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL instead of kRenderedLayout:
    //       "FAIL: a video draw leaves the target in the layout the render pass ends in".
    //       renderer/vulkan/offscreen.cpp recordAcquire given VK_IMAGE_LAYOUT_GENERAL as its old
    //       layout instead of target.releasedFromLayout: "FAIL: the next acquire names the layout
    //       the release named"; re-run 2026-10-26 (G6) after render() changed, the same line.
    // COVERAGE-ASSERT: exactly three ownership transfers (release, acquire, release), so a run
    //       whose second draw never happened cannot pass on the first release alone.
    const OffscreenTarget& target = targets.create(20, 64, 64);
    recordBarriers(true);
    targets.render(renderer, target);
    const VkImageLayout afterFirst = target.releasedFromLayout;
    targets.render(renderer, target);
    recordBarriers(false);
    const std::vector<RecordedBarrier> transfers = ownershipTransfersOf(target.image.image);
    check(transfers.size() == 3, "two draws on one target recorded a release, an acquire and a release");
    if (transfers.size() != 3) std::fprintf(stderr, "  %zu ownership transfer(s), not 3\n", transfers.size());
    check(afterFirst == wallpaperx::kRenderedLayout,
          "a video draw leaves the target in the layout the render pass ends in");
    if (afterFirst != wallpaperx::kRenderedLayout) {
        std::fprintf(stderr, "  the target recorded %s as its release layout\n", layoutName(afterFirst).c_str());
    }
    if (transfers.size() == 3) {
        check(transfers[0].oldLayout == wallpaperx::kRenderedLayout,
              "the release hands the target over from the layout the draw left it in");
        check(transfers[1].oldLayout == transfers[0].oldLayout, "the next acquire names the layout the release named");
        if (transfers[1].oldLayout != transfers[0].oldLayout) {
            std::fprintf(stderr, "  released from %s but acquired from %s\n", layoutName(transfers[0].oldLayout).c_str(),
                         layoutName(transfers[1].oldLayout).c_str());
        }
    }
    targets.destroy(20);

    // GOAL: S13.228 -- a draw of a hardware-decoded picture hands the decoder's image back in the
    //       layout the decoder left it in: exactly two barriers on that image, that layout to
    //       SHADER_READ_ONLY_OPTIMAL and back, since the decoder's next barrier names it as old.
    // WHY-THIS-SHAPE: read off the recorded barriers, because the validation layer cannot see this:
    //       the image is the decoder's and its next use is recorded by ffmpeg in a later batch. The
    //       decoder's layout is asserted to differ from the sampled one, or a renderer that never
    //       returned the image would pass. Card only: Lavapipe has no hardware decoder.
    // CONTROL: renderer/video/ycbcr_renderer.cpp record()'s return barrier given
    //       VK_IMAGE_LAYOUT_GENERAL as its new layout instead of source->layout, through
    //       control_batch.sh on the card: "FAIL: a draw of a hardware-decoded picture returns the
    //       decoder's image to the layout the decoder left it in", the only failure; restored green.
    if (!ramp->deviceImage) {
        // A card with a hardware decoder must reach this check; one without cannot answer it.
        check(device.info().videoDecodeOps == 0,
              "a card with a hardware decoder decoded ramp_256x256.mp4 on the device path, so the return of "
              "the decoder's image is graded");
        std::printf("%s: ramp_256x256.mp4 was decoded on the %s path on %s, so the return of a decoder's image is "
                    "not answerable here\n",
                    label.c_str(), pathName(ramp->opened.decoder->path()), device.info().gpuName.c_str());
        return;
    }
    const DecodedFrame::DeviceImage decoded = *ramp->deviceImage;
    const OffscreenTarget& sampledFrom = targets.create(21, 64, 64);
    recordBarriers(true);
    targets.render(renderer, sampledFrom);
    recordBarriers(false);
    targets.destroy(21);
    const std::vector<RecordedBarrier> onImage = barriersOn(decoded.image);
    const bool distinguishable = decoded.layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    check(distinguishable, "the decoder leaves its image in a layout other than the sampled one, so a return to "
                           "it can be told from no return");
    const bool returned = onImage.size() == 2 && onImage[0].oldLayout == decoded.layout &&
                          onImage[0].newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
                          onImage[1].oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
                          onImage[1].newLayout == decoded.layout;
    check(returned, "a draw of a hardware-decoded picture returns the decoder's image to the layout the decoder "
                    "left it in");
    std::printf("%s: the decoder left its image in %s; the draw recorded %zu barrier(s) on it:", label.c_str(),
                layoutName(decoded.layout).c_str(), onImage.size());
    for (const RecordedBarrier& barrier : onImage) {
        std::printf(" %s -> %s;", layoutName(barrier.oldLayout).c_str(), layoutName(barrier.newLayout).c_str());
    }
    std::printf("\n");
}

enum class Refusal { ContentError, InvalidArgument };

// Whether `attempt` was refused with the type that names whose problem it is: the daemon routes
// WallpaperContentError (the file) and std::invalid_argument (the caller) differently.
void refusedAs(const std::string& what, Refusal expected, const std::function<void()>& attempt) {
    try {
        attempt();
    } catch (const WallpaperContentError& e) {
        check(expected == Refusal::ContentError, what);
        if (expected != Refusal::ContentError) std::fprintf(stderr, "  refused as content: %s\n", e.what());
        return;
    } catch (const std::invalid_argument& e) {
        check(expected == Refusal::InvalidArgument, what);
        if (expected != Refusal::InvalidArgument) std::fprintf(stderr, "  refused as invalid: %s\n", e.what());
        return;
    } catch (const std::exception& e) {
        check(false, what);
        std::fprintf(stderr, "  refused with another exception: %s\n", e.what());
        return;
    }
    check(false, what);
    std::fprintf(stderr, "  it was accepted\n");
}

void runPlacementRefusals(Device& device, OffscreenTargetManager& targets, const std::string& label) {
    std::unique_ptr<Ramp> ramp = openRamp(device, label, "ramp_512x256.mp4");
    if (!ramp) return;
    YcbcrRenderer& renderer = *ramp->renderer;
    const OffscreenTarget& target = targets.create(30, 64, 64);
    targets.render(renderer, target);
    const std::vector<uint8_t> reference = wallpaperx::testing::reclaimAndReadback(device, targets, target);
    // Without this the comparisons below could be between two readbacks of a target nothing drew.
    check(wallpaperx::testing::variesInBothAxes(reference, 64, 64),
          "the picture drawn before the refusals varies in both axes, so a later draw of anything else differs");

    int examined = 0, stillShown = 0;
    auto drawsAsBefore = [&](const std::string& after) {
        ++examined;
        targets.render(renderer, target);
        const std::vector<uint8_t> pixels = wallpaperx::testing::reclaimAndReadback(device, targets, target);
        const size_t differing = wallpaperx::testing::countDifferingRows(pixels, reference, 64, 64);
        if (differing == 0) {
            ++stillShown;
        } else {
            std::fprintf(stderr, "  after %s, %zu of 64 rows differ from what was drawn before\n", after.c_str(),
                         differing);
        }
    };

    // GOAL: S15.705 -- the video renderer refuses tile with std::invalid_argument and keeps drawing
    //       the layout it had.
    // WHY-THIS-SHAPE: the draw after the refusal is compared with the one before, so a place() that
    //       stored the mode before throwing fails too, not only one that accepted it.
    // CONTROL: renderer/video/ycbcr_renderer.cpp place()'s positionModeShowsVideo test made `if
    //       (false)`, through control_batch.sh: "FAIL: a video renderer refuses tile", and every
    //       draw after it differs in 64 of 64 rows, since the tile layout stays stored; restored
    //       green.
    refusedAs("a video renderer refuses tile", Refusal::InvalidArgument,
              [&] { renderer.place({wallpaperx::PositionMode::Tile}); });
    drawsAsBefore("refusing tile");

    // GOAL: S13.27 -- a refused picture leaves the one shown before it on the target: nothing of
    //       the refused picture is adopted.
    // WHY-THIS-SHAPE: the refused same-shape picture is the ramp mirrored and tagged PQ, so taking
    //       in its pixels before refusing its colour shows on the target; an identical picture
    //       would hide that leak. A picture of another shape is refused too and graded the same
    //       way, but no break is safe for it: drawing a 256x512 picture through planes made for
    //       512x256 reads outside its rows.
    // CONTROL: renderer/video/ycbcr_renderer.cpp show()'s colourMapFor call moved below the
    //       storage branch, so the picture is uploaded or held before its colour is refused,
    //       through control_batch.sh: "FAIL: a refused picture leaves the one shown before it on
    //       the target", restored green.
    // COVERAGE-ASSERT: the number of draws compared is asserted, so a refusal case that stopped
    //       being drawn after cannot pass by grading less.
    {
        Opened other = openFirst(device, asset("ramp_256x512.mp4"));
        if (!other.frame) {
            fail("ramp_256x512.mp4 decoded to no frames");
            return;
        }
        refusedAs("a picture of another shape is refused", Refusal::InvalidArgument,
                  [&] { renderer.show(std::move(*other.frame)); });
        drawsAsBefore("refusing a picture of another shape");
    }
    {
        Opened pq = openFirst(device, asset("ramp_mirror_pq_512x256.mp4"));
        if (!pq.frame) {
            fail("ramp_mirror_pq_512x256.mp4 decoded to no frames");
            return;
        }
        refusedAs("a same-shape picture tagged PQ is refused as content", Refusal::ContentError,
                  [&] { renderer.show(std::move(*pq.frame)); });
        drawsAsBefore("refusing a picture tagged PQ");
    }
    check(examined == 3, "every refusal was followed by a draw");
    check(stillShown == examined, "a refused picture leaves the one shown before it on the target");
    targets.destroy(30);
}

void runPlacementChecks(Device& device, const std::string& label) {
    for (const char* file : kRampFiles) {
        if (!haveAsset(file, label)) return;
    }
    OffscreenTargetManager targets(device);
    runPlacementGeometry(device, targets, label);
    runPlacementRepeat(device, targets, label);
    runPlacementRefusals(device, targets, label);
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
        std::fprintf(stderr, "video_draw_probe: cannot resolve the real %s\n", name);
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
    if (g_proposalWindow.load()) g_queriesInWindow.fetch_add(1);
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

extern "C" void vkCmdPipelineBarrier(VkCommandBuffer cmd, VkPipelineStageFlags srcStages, VkPipelineStageFlags dstStages,
                                     VkDependencyFlags flags, uint32_t memoryCount, const VkMemoryBarrier* memory,
                                     uint32_t bufferCount, const VkBufferMemoryBarrier* buffers, uint32_t imageCount,
                                     const VkImageMemoryBarrier* images) {
    {
        std::lock_guard lock(g_barrierMutex);
        if (g_recordingBarriers) {
            for (uint32_t i = 0; i < imageCount; ++i) {
                g_barriers.push_back({images[i].image, images[i].oldLayout, images[i].newLayout,
                                      images[i].srcQueueFamilyIndex, images[i].dstQueueFamilyIndex});
            }
        }
    }
    using Fn = void (*)(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags, uint32_t,
                        const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*, uint32_t,
                        const VkImageMemoryBarrier*);
    static const Fn real = realSymbol<Fn>("vkCmdPipelineBarrier");
    real(cmd, srcStages, dstStages, flags, memoryCount, memory, bufferCount, buffers, imageCount, images);
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
    // shader-read builds a module on no device at all, to stay driver-free.
    if (device == VK_NULL_HANDLE) return VK_ERROR_INITIALIZATION_FAILED;
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
        auto* vk = static_cast<AVVulkanFramesContext*>(frames->hwctx);
        if (g_proposeLinearTiling.load()) {
            vk->tiling = VK_IMAGE_TILING_LINEAR;
            g_linearProposals.fetch_add(1);
        }
        g_proposalWindow = true;
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
    if (err == 0 && frame->format == AV_PIX_FMT_VULKAN && frame->hw_frames_ctx != nullptr) {
        auto* vk = reinterpret_cast<AVVkFrame*>(frame->data[0]);
        auto* pool = reinterpret_cast<AVHWFramesContext*>(frame->hw_frames_ctx->data);
        std::lock_guard lock(g_receivedMutex);
        bool known = false;
        for (ReceivedVkFrame& received : g_receivedVkFrames) {
            if (received.image == vk->img[0]) {
                received = {vk->img[0], vk, pool};
                known = true;
            }
        }
        if (!known) g_receivedVkFrames.push_back({vk->img[0], vk, pool});
    }
    return err;
}

extern "C" int av_hwframe_ctx_init(AVBufferRef* ref) {
    using Fn = int (*)(AVBufferRef*);
    static const Fn real = realSymbol<Fn>("av_hwframe_ctx_init");
    if (ref != nullptr && g_proposalWindow.exchange(false)) {
        const auto* frames = reinterpret_cast<AVHWFramesContext*>(ref->data);
        if (frames->device_ctx->type == AV_HWDEVICE_TYPE_VULKAN) {
            const auto* vk = static_cast<const AVVulkanFramesContext*>(frames->hwctx);
            std::lock_guard lock(g_poolInitMutex);
            g_poolInit.calls += 1;
            g_poolInit.usage = vk->usage;
            g_poolInit.flags = vk->img_flags;
        }
    }
    return real(ref);
}

// The decoder loads its video-format query through vkGetInstanceProcAddr; this hands it a wrapper.
namespace {
std::atomic<PFN_vkGetPhysicalDeviceVideoFormatPropertiesKHR> g_realVideoFormatProperties{nullptr};

VKAPI_ATTR VkResult VKAPI_CALL videoFormatProperties(VkPhysicalDevice physicalDevice,
                                                     const VkPhysicalDeviceVideoFormatInfoKHR* info, uint32_t* count,
                                                     VkVideoFormatPropertiesKHR* properties) {
    const VkResult result = g_realVideoFormatProperties.load()(physicalDevice, info, count, properties);
    if (!g_proposalWindow.load()) return result;
    g_queriesInWindow.fetch_add(1);
    if (g_hideMutableFormat.load() && properties != nullptr && (result == VK_SUCCESS || result == VK_INCOMPLETE)) {
        for (uint32_t i = 0; i < *count; ++i) {
            if (properties[i].imageCreateFlags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) {
                properties[i].imageCreateFlags &= ~VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
                g_mutableStripped.fetch_add(1);
            }
        }
    }
    return result;
}
}  // namespace

extern "C" PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance instance, const char* name) {
    static const auto real = realSymbol<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
    const PFN_vkVoidFunction found = real(instance, name);
    if (found == nullptr || name == nullptr || std::strcmp(name, "vkGetPhysicalDeviceVideoFormatPropertiesKHR") != 0) {
        return found;
    }
    g_realVideoFormatProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceVideoFormatPropertiesKHR>(found);
    return reinterpret_cast<PFN_vkVoidFunction>(&videoFormatProperties);
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
                                            "siting",      "refusal",       "allocation",
                                            "gate",        "no-ycbcr",      "shape-resize", "shape-tags",
                                            "shape-fallback", "shader-read", "pipeline-colour-host",
                                            "pipeline-colour-device", "placement"};
    if (std::find(modes.begin(), modes.end(), mode) == modes.end()) {
        std::fprintf(stderr, "usage: %s <mode>, one of:", argv[0]);
        for (const std::string& m : modes) std::fprintf(stderr, " %s", m.c_str());
        std::fprintf(stderr, "\n");
        return 1;
    }
    const std::string label = "video_draw_probe (" + mode + ")";

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
                    if (mode == "allocation") {
                        runSubmitScopeChecks(*device, label);
                        runAllocationChecks(*device, label);
                    }
                    if (mode == "gate") {
                        runGateChecks(*device, label);
                        runDecodePoolChecks(*device, label);
                    }
                    if (mode == "no-ycbcr") runNoYcbcrChecks(*device, label);
                    if (mode == "shape-resize") runShapeResizeChecks(*device, label);
                    if (mode == "shape-tags") runShapeTagChecks(*device, label);
                    if (mode == "shape-fallback") runShapeFallbackChecks(*device, label);
                    if (mode == "pipeline-colour-host") runPipelineColourChecks(*device, label, /*onDevice=*/false);
                    if (mode == "pipeline-colour-device") runPipelineColourChecks(*device, label, /*onDevice=*/true);
                    if (mode == "placement") runPlacementChecks(*device, label);
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

    const int failures = wallpaperx::testing::reportFailures("video_draw_probe");
    if (!graded || failures > 0) return 1;
    if (g_notVerified) {
        std::printf("%s: NOT VERIFIED here -- reported to ctest as a skip, not a pass\n", label.c_str());
        return kNotVerifiedExitCode;
    }
    std::printf("%s: all checks passed, no unexpected validation messages\n", label.c_str());
    return 0;
}
