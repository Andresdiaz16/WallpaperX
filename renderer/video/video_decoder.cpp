// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_decoder.h"

#include "ffmpeg_file.h"
#include "frame_converter.h"
#include "reset_point.h"

#include "../vulkan/format_features.h"
#include "../vulkan/gpu_fault.h"
#include "../wallpaper_file.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avstring.h>
#include <libavutil/buffer.h>
#include <libavutil/common.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
}

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <iterator>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

namespace wallpaperx {

namespace {

using ffmpeg_file::avErrorString;
using ffmpeg_file::throwOutOfMemory;

constexpr const char* kComponent = "VideoDecoder";

// The most pictures a start decodes and drops to reach its picture from the keyframe it decodes from.
// The catch-up holds the GPU lock; 250 is x264's and x265's default keyframe interval, so a default
// closed-GOP file always reaches its picture.
constexpr int64_t kCatchUpPictures = 250;

[[noreturn]] void throwFileFailure(const std::string& reason, int err, const std::string& file) {
    ffmpeg_file::throwFileFailure(kComponent, reason, err, file);
}

struct BufferRef {
    AVBufferRef* ref = nullptr;
    ~BufferRef() { av_buffer_unref(&ref); }
    AVBufferRef* release() { return std::exchange(ref, nullptr); }
};

// The codec-operation bit a codec needs of a decode queue, as Device::info().videoDecodeOps
// names them; NONE for a codec no queue here decodes.
VkVideoCodecOperationFlagBitsKHR decodeOperation(AVCodecID id) {
    switch (id) {
        case AV_CODEC_ID_H264: return VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
        case AV_CODEC_ID_HEVC: return VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR;
        case AV_CODEC_ID_AV1: return VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR;
        default: return VK_VIDEO_CODEC_OPERATION_NONE_KHR;
    }
}

// The queue families ffmpeg may use, most preferred first. It takes compute and transfer from
// the first family offering each, so the graphics family leads: ffmpeg prepares every new
// image there. It dereferences the compute family unconditionally, which is why the graphics
// family must be listed even on a device whose decode family has transfer of its own.
void listQueueFamilies(const Device& device, AVVulkanDeviceContext& hw) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device.physicalDevice(), &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device.physicalDevice(), &count, families.data());

    auto add = [&](uint32_t family, VkVideoCodecOperationFlagsKHR ops) {
        for (int i = 0; i < hw.nb_qf; ++i) {
            if (static_cast<uint32_t>(hw.qf[i].idx) != family) continue;
            hw.qf[i].video_caps = static_cast<VkVideoCodecOperationFlagBitsKHR>(hw.qf[i].video_caps | ops);
            return;
        }
        if (static_cast<size_t>(hw.nb_qf) == std::size(hw.qf)) {
            throw std::logic_error("VideoDecoder: more queue families than ffmpeg can be told about");
        }
        AVVulkanDeviceQueueFamily& entry = hw.qf[hw.nb_qf++];
        entry.idx = static_cast<int>(family);
        entry.num = 1;  // one queue per family is what the Device created
        entry.flags = static_cast<VkQueueFlagBits>(families[family].queueFlags);
        entry.video_caps = static_cast<VkVideoCodecOperationFlagBitsKHR>(ops);
    };
    add(device.graphicsQueueFamily(), 0);
    for (const Device::VideoDecodeFamily& decode : device.videoDecodeFamilies()) add(decode.family, decode.ops);
}

// The decoder that can hand its work to a Vulkan device, or null when ffmpeg has none for this
// codec. Not always ffmpeg's default: it registers libdav1d ahead of its own AV1 decoder, and
// only the latter has a Vulkan hook.
const AVCodec* vulkanDecoder(AVCodecID id) {
    void* iterator = nullptr;
    while (const AVCodec* codec = av_codec_iterate(&iterator)) {
        if (codec->id != id || !av_codec_is_decoder(codec)) continue;
        for (int i = 0;; ++i) {
            const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
            if (config == nullptr) break;
            if (config->device_type == AV_HWDEVICE_TYPE_VULKAN &&
                (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0) {
                return codec;
            }
        }
    }
    return nullptr;
}

const void* findInChain(const void* chain, VkStructureType type) {
    for (const auto* node = static_cast<const VkBaseInStructure*>(chain); node != nullptr; node = node->pNext) {
        if (node->sType == type) return node;
    }
    return nullptr;
}

// The usage ffmpeg will create the images with, given what it is asked for: it adds sampled,
// storage and both transfer usages whenever the format's own features allow them, whatever the
// video profile allows. Predicted here so the profile is asked about the image that will exist.
VkImageUsageFlags usageFfmpegCreates(VkImageUsageFlags asked, VkFormatFeatureFlags2 features) {
    VkImageUsageFlags usage = asked;
    if (features & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (features & VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (features & VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (features & VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT) usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return usage;
}

// Whether `result` is this device declining a stream -- what vkGetPhysicalDeviceVideo* return
// for a profile, format or usage they do not support -- as opposed to a driver failing.
bool isVideoRefusal(VkResult result) {
    switch (result) {
        case VK_ERROR_IMAGE_USAGE_NOT_SUPPORTED_KHR:
        case VK_ERROR_VIDEO_PICTURE_LAYOUT_NOT_SUPPORTED_KHR:
        case VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR:
        case VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR:
        case VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR:
        case VK_ERROR_VIDEO_STD_VERSION_NOT_SUPPORTED_KHR:
            return true;
        default:
            return false;
    }
}

// The smallest coded picture the device decodes under `profile`, which the video session ffmpeg
// creates from it holds every decode to.
VkResult minCodedExtent(PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR query, VkPhysicalDevice gpu,
                        const VkVideoProfileInfoKHR& profile, VkExtent2D& out) {
    VkVideoDecodeH264CapabilitiesKHR h264{VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR};
    VkVideoDecodeH265CapabilitiesKHR h265{VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_CAPABILITIES_KHR};
    VkVideoDecodeAV1CapabilitiesKHR av1{VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_CAPABILITIES_KHR};
    VkVideoDecodeCapabilitiesKHR decode{VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR};
    switch (profile.videoCodecOperation) {
        case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR: decode.pNext = &h264; break;
        case VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR: decode.pNext = &h265; break;
        case VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR: decode.pNext = &av1; break;
        default: return VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR;
    }
    VkVideoCapabilitiesKHR caps{VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR};
    caps.pNext = &decode;
    const VkResult result = query(gpu, &profile, &caps);
    out = caps.minCodedExtent;
    return result;
}

AVHWFramesContext* framesContext(const AVFrame& frame) {
    return reinterpret_cast<AVHWFramesContext*>(frame.hw_frames_ctx->data);
}

AVVkFrame* vkFrame(const AVFrame& frame) { return reinterpret_cast<AVVkFrame*>(frame.data[0]); }

// Holds ffmpeg's per-frame lock for a scope, so no throw can leave it taken.
class FrameLock {
public:
    FrameLock(AVHWFramesContext* frames, AVVkFrame* frame)
        : m_frames(frames), m_vk(static_cast<AVVulkanFramesContext*>(frames->hwctx)), m_frame(frame) {
        m_vk->lock_frame(m_frames, m_frame);
    }
    ~FrameLock() { m_vk->unlock_frame(m_frames, m_frame); }
    FrameLock(const FrameLock&) = delete;
    FrameLock& operator=(const FrameLock&) = delete;

private:
    AVHWFramesContext* m_frames;
    AVVulkanFramesContext* m_vk;
    AVVkFrame* m_frame;
};

// A packet the decoder cannot parse is skipped, as every player does: ffmpeg has already
// dropped it and conceals the pictures that depended on it. Whichever of send and receive ran
// the decode reports it, and neither is retried -- the packet is gone, so a retry would spin.
bool isDamagedPacket(int err) { return err == AVERROR_INVALIDDATA; }

AVDiscard discardFor(PictureSkip skip) {
    switch (skip) {
        case PictureSkip::Nothing: return AVDISCARD_DEFAULT;
        case PictureSkip::Unreferenced: return AVDISCARD_NONREF;
        case PictureSkip::NonIntra: return AVDISCARD_NONINTRA;
        case PictureSkip::NonKey: return AVDISCARD_NONKEY;
    }
    return AVDISCARD_DEFAULT;
}

// Whether going from `from` to `to` starts decoding pictures that can reference ones `from` left out.
// Nothing references an unreferenced picture, and an intra one references nothing.
bool lowersOntoSkipped(PictureSkip from, PictureSkip to) {
    return from > PictureSkip::Unreferenced && to <= PictureSkip::Unreferenced;
}

// `value` in `base` units -- pkt_timebase, which is the stream's -- or nothing for an unset one.
std::optional<std::chrono::nanoseconds> inNanoseconds(int64_t value, AVRational base) {
    if (value == AV_NOPTS_VALUE || base.num <= 0 || base.den <= 0) return std::nullopt;
    return std::chrono::nanoseconds(av_rescale_q(value, base, AVRational{1, 1'000'000'000}));
}

// One frame at the rate the container declares, else the rate ffmpeg guessed while probing.
std::optional<std::chrono::nanoseconds> nominalInterval(const AVStream& stream) {
    for (const AVRational rate : {stream.avg_frame_rate, stream.r_frame_rate}) {
        if (rate.num > 0 && rate.den > 0) return inNanoseconds(1, av_inv_q(rate));
    }
    return std::nullopt;
}

// A packet's `opaque`, which ffmpeg copies to the frames decoded from it: the number
// VideoDecoder::readVideoPacket() gave it above kLevelBits, 0 for none, and below them the
// PictureSkip it was sent at.
constexpr unsigned kLevelBits = 3;
constexpr uintptr_t kLevelMask = (uintptr_t{1} << kLevelBits) - 1;
static_assert(static_cast<uintptr_t>(PictureSkip::NonKey) <= kLevelMask);

std::optional<uint64_t> packetNumber(const AVFrame& frame) {
    const auto number = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(frame.opaque) >> kLevelBits);
    return number == 0 ? std::nullopt : std::optional<uint64_t>(number);
}

PictureSkip sentSkipping(const AVFrame& frame) {
    return static_cast<PictureSkip>(reinterpret_cast<uintptr_t>(frame.opaque) & kLevelMask);
}

void tagSkipping(AVPacket& packet, PictureSkip skip) {
    const uintptr_t number = reinterpret_cast<uintptr_t>(packet.opaque) & ~kLevelMask;
    packet.opaque = reinterpret_cast<void*>(number | static_cast<uintptr_t>(skip));
}

}  // namespace

// --------------------------------------------------------------------------- DecodedFrame

DecodedFrame::DecodedFrame(Device& device, AVFrame* frame) : m_device(&device), m_frame(frame) {
    if (storage() == Storage::DeviceImage) {
        // What attachHardwareFrames() accepted: one multi-planar image per picture, with one
        // semaphore, shared with or owned by the graphics family. Anything else is ffmpeg
        // departing from the frames context it was given.
        const AVVkFrame* vk = vkFrame(*m_frame);
        if (m_frame->hw_frames_ctx == nullptr || vk == nullptr || vk->img[0] == VK_NULL_HANDLE ||
            vk->img[1] != VK_NULL_HANDLE || vk->sem[0] == VK_NULL_HANDLE) {
            release();
            throw std::logic_error("DecodedFrame: ffmpeg produced a frame that is not one image with one semaphore");
        }
        const uint32_t graphics = m_device->graphicsQueueFamily();
        if (vk->queue_family[0] != VK_QUEUE_FAMILY_IGNORED && vk->queue_family[0] != graphics) {
            release();
            throw std::logic_error("DecodedFrame: ffmpeg made the image exclusive to queue family " +
                                   std::to_string(vk->queue_family[0]) + ", which is not the graphics family");
        }
        const AVHWFramesContext* frames = framesContext(*m_frame);
        if (m_frame->width > frames->width || m_frame->height > frames->height) {
            release();
            throw std::logic_error("DecodedFrame: ffmpeg decoded a picture larger than the image it is in");
        }
    }
    m_colour = FrameColour{m_frame->colorspace, m_frame->color_range, m_frame->color_primaries, m_frame->color_trc,
                           m_frame->chroma_location};
}

DecodedFrame::~DecodedFrame() { release(); }

DecodedFrame::DecodedFrame(DecodedFrame&& other) noexcept
    : m_device(other.m_device),
      m_frame(std::exchange(other.m_frame, nullptr)),
      m_colour(other.m_colour),
      m_presentation(other.m_presentation),
      m_duration(other.m_duration),
      m_position(other.m_position),
      m_skip(other.m_skip),
      m_readSince(other.m_readSince) {}

DecodedFrame& DecodedFrame::operator=(DecodedFrame&& other) noexcept {
    if (this != &other) {
        release();
        m_device = other.m_device;
        m_frame = std::exchange(other.m_frame, nullptr);
        m_colour = other.m_colour;
        m_presentation = other.m_presentation;
        m_duration = other.m_duration;
        m_position = other.m_position;
        m_skip = other.m_skip;
        m_readSince = other.m_readSince;
    }
    return *this;
}

void DecodedFrame::release() noexcept {
    if (m_frame == nullptr) return;
    // The last reference to the pool frees it, and ffmpeg waits without bound on every image's
    // semaphore first -- which a batch on a GPU that has stopped will never signal.
    if (m_frame->hw_frames_ctx != nullptr && av_buffer_get_ref_count(m_frame->hw_frames_ctx) == 1 &&
        !m_device->safeToFree("the video decoder's images")) {
        m_frame = nullptr;
        return;
    }
    av_frame_free(&m_frame);
}

const AVFrame& DecodedFrame::frame() const {
    if (m_frame == nullptr) throw std::logic_error("DecodedFrame: used after being moved from");
    return *m_frame;
}

PictureSkip DecodedFrame::keptThrough() const {
    const AVFrame& f = frame();
    if ((f.flags & AV_FRAME_FLAG_KEY) != 0) return PictureSkip::NonKey;
    if (f.pict_type == AV_PICTURE_TYPE_I) return PictureSkip::NonIntra;
    if (f.pict_type != AV_PICTURE_TYPE_B) return PictureSkip::Unreferenced;
    return PictureSkip::Nothing;
}

DecodedFrame::Storage DecodedFrame::storage() const {
    return frame().format == AV_PIX_FMT_VULKAN ? Storage::DeviceImage : Storage::HostPlanes;
}

AVPixelFormat DecodedFrame::pixelFormat() const {
    return storage() == Storage::DeviceImage ? framesContext(frame())->sw_format
                                             : static_cast<AVPixelFormat>(frame().format);
}

uint32_t DecodedFrame::bitDepth() const {
    return static_cast<uint32_t>(av_pix_fmt_desc_get(pixelFormat())->comp[0].depth);
}

VkExtent2D DecodedFrame::extent() const {
    // With cropping left to the caller, ffmpeg's width and height are the coded size on both
    // paths, never the image pool's size.
    return {static_cast<uint32_t>(frame().width), static_cast<uint32_t>(frame().height)};
}

VkRect2D DecodedFrame::visible() const {
    const AVFrame& f = frame();
    const size_t width = f.width - f.crop_left - f.crop_right;
    const size_t height = f.height - f.crop_top - f.crop_bottom;
    return {{static_cast<int32_t>(f.crop_left), static_cast<int32_t>(f.crop_top)},
            {static_cast<uint32_t>(width), static_cast<uint32_t>(height)}};
}

DecodedFrame::DeviceImage DecodedFrame::deviceImage() const {
    if (storage() != Storage::DeviceImage) throw std::logic_error("DecodedFrame: a host frame has no device image");
    const FrameLock lock(framesContext(frame()), vkFrame(frame()));
    return deviceImageLocked();
}

DecodedFrame::DeviceImage DecodedFrame::deviceImageLocked() const {
    const AVVkFrame* vk = vkFrame(frame());
    const auto* frames = static_cast<const AVVulkanFramesContext*>(framesContext(frame())->hwctx);
    return DeviceImage{vk->img[0], frames->format[0], vk->tiling, frames->usage, vk->layout[0]};
}

Device::SubmitTicket DecodedFrame::submit(const std::function<void(VkCommandBuffer, const DeviceImage&)>& record,
                                          VkPipelineStageFlags readStage, Device::SubmitScope scope) {
    if (storage() != Storage::DeviceImage) throw std::logic_error("DecodedFrame: a host frame has nothing to submit");
    AVVkFrame* vk = vkFrame(frame());

    const FrameLock lock(framesContext(frame()), vk);
    const DeviceImage image = deviceImageLocked();
    const uint64_t current = vk->sem_value[0];
    scope.timelineWait.emplace_back(vk->sem[0], current, readStage);
    scope.timelineSignal.emplace_back(vk->sem[0], current + 1);
    const Device::SubmitTicket ticket = m_device->submit([&](VkCommandBuffer cmd) { record(cmd, image); }, scope);
    vk->sem_value[0] = current + 1;
    return ticket;
}

DecodedFrame::HostPlanes DecodedFrame::hostPlanes() const {
    if (storage() != Storage::HostPlanes) throw std::logic_error("DecodedFrame: a device frame has no host planes");
    HostPlanes planes;
    const int count = av_pix_fmt_count_planes(pixelFormat());
    planes.count = static_cast<uint32_t>(std::clamp(count, 0, static_cast<int>(planes.data.size())));
    for (uint32_t i = 0; i < planes.count; ++i) {
        planes.data[i] = frame().data[i];
        planes.stride[i] = frame().linesize[i];
    }
    return planes;
}

// --------------------------------------------------------------------------- VideoDecoder

VideoDecoder::VideoDecoder(Device& device, const std::string& file, std::optional<StreamPosition> position)
    : m_device(device), m_file(file) {
    m_device.assertGpuThread("VideoDecoder::VideoDecoder");
    try {
        m_packet = av_packet_alloc();
        if (m_packet == nullptr) throwOutOfMemory("VideoDecoder");
        openContainer();
        // The decoder first: whether it is the device's decides how the start seeks.
        openDecoder();
        if (position) startAt(*position);
    } catch (...) {
        closeAll();
        throw;
    }
}

VideoDecoder::~VideoDecoder() { closeAll(); }

// The container before the file it reads through.
void VideoDecoder::closeAll() noexcept {
    closeDecoder();
    av_packet_free(&m_packet);
    avformat_close_input(&m_format);
    m_reader.close();
}

// The one bounded question this renderer can ask of the GPU.
bool VideoDecoder::deviceAnswers(Device& device) {
    try {
        device.immediateSubmit([](VkCommandBuffer) {});
        return true;
    } catch (const GpuFailure&) {
        return false;
    }
}

bool VideoDecoder::deviceHealthy() { return m_askDevice ? m_askDevice() : deviceAnswers(m_device); }

// The same question as deviceHealthy(), from a destructor, which is on the GPU thread. Anything
// the probe throws is an unhealthy answer: the alternative is entering ffmpeg's unbounded wait
// from a noexcept path.
bool VideoDecoder::decodeWorkCanDrain() noexcept {
    try {
        return deviceAnswers(m_device);
    } catch (...) {
        return false;
    }
}

// Freeing the codec and the frames contexts frees their image pools once no frame is out, and
// ffmpeg waits without bound on every image's semaphore first, then on its own decode work. Those
// are what own the pools, so they are all that is leaked; the scratch objects and a software codec
// hold no GPU work. Nor does a hardware codec with no context held: ffmpeg's Vulkan decoder
// references its frames context.
//
// A bounded question in front of each wait, and either answering no leaks instead: this runs on
// the daemon's one GPU thread with its lock held, so a hang here stops every monitor's frames.
void VideoDecoder::closeDecoder() noexcept {
    av_frame_free(&m_frame);
    // ffmpeg's Vulkan decoder references the frames context it decodes into, so a hardware codec
    // with none held has no decode work to wait on and must not be made to pay the probe: a
    // decoder refused before it decoded anything tears down touching the GPU not at all.
    const bool decodeWorkHeld =
        !m_framesContexts.empty() &&
        ((m_codec != nullptr && m_codec->hw_device_ctx != nullptr) || m_retiredCodec.has_value());
    const bool imagesFree = m_framesContexts.empty() || m_device.safeToFree("the video decoder's images");
    if (!imagesFree || (decodeWorkHeld && !decodeWorkCanDrain())) {
        // A false safeToFree() has told the Device already; an unanswered probe has not.
        if (imagesFree) m_device.recordLeak("the video decoder, whose decode work the device did not answer for,");
        if (m_codec != nullptr && m_codec->hw_device_ctx != nullptr) m_codec = nullptr;
        m_retiredCodec.reset();
        m_framesContexts.clear();
    }
    if (m_retiredCodec) avcodec_free_context(&m_retiredCodec->codec);
    m_retiredCodec.reset();
    avcodec_free_context(&m_codec);
    for (HeldFrames& held : m_framesContexts) av_buffer_unref(&held.ref);
    m_framesContexts.clear();
}

// Playback's close, which waits on none of this renderer's reads. A hardware codec is kept for
// releaseRetiredCodec(), and the frames contexts for releaseIdleFramesContexts(). The path never
// returns to hardware, so no codec is retired while another is.
void VideoDecoder::retireDecoder() noexcept {
    av_frame_free(&m_frame);
    if (m_codec == nullptr || m_codec->hw_device_ctx == nullptr) {
        avcodec_free_context(&m_codec);
        return;
    }
    m_retiredCodec = RetiredCodec{std::exchange(m_codec, nullptr), m_device.lastSubmission()};
}

// Before releaseIdleFramesContexts(), so the contexts this codec held are released in the same call.
// The free waits without bound on ffmpeg's own outstanding decode work.
void VideoDecoder::releaseRetiredCodec() noexcept {
    if (!m_retiredCodec || !m_device.hasCompleted(m_retiredCodec->lastReader)) return;
    avcodec_free_context(&m_retiredCodec->codec);
    m_retiredCodec.reset();
}

// A context only this decoder references has no picture left, in ffmpeg or handed out, and no
// decode work: ffmpeg waits out its decode batches before the codec drops its reference. Only
// batches submitted before then can still read the images, and those are polled, never waited on.
void VideoDecoder::releaseIdleFramesContexts() noexcept {
    std::erase_if(m_framesContexts, [this](HeldFrames& held) {
        if (!held.lastReader) {
            if (av_buffer_get_ref_count(held.ref) != 1) return false;
            held.lastReader = m_device.lastSubmission();
        }
        if (!m_device.hasCompleted(*held.lastReader)) return false;
        av_buffer_unref(&held.ref);
        return true;
    });
}

// The first call opens the file; a later one reads it again from its start.
void VideoDecoder::openContainer() {
    if (m_reader.io() == nullptr) {
        m_reader.open(m_file, kComponent);
    } else if (const int64_t err = avio_seek(m_reader.io(), 0, SEEK_SET); err < 0) {
        throwFileFailure("cannot return to the start of the wallpaper", static_cast<int>(err), m_file);
    }
    ffmpeg_file::openContainer(m_format, m_reader.io(), m_file, kComponent);
    const int stream = av_find_best_stream(m_format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream == AVERROR_STREAM_NOT_FOUND) {
        throw WallpaperContentError(describeRejection(kComponent, "the wallpaper has no video stream", m_file));
    }
    if (stream < 0) throwFileFailure("cannot pick a video stream", stream, m_file);
    m_stream = stream;
}

void VideoDecoder::openDecoder() {
    const AVStream& stream = *m_format->streams[m_stream];
    const AVCodecID id = stream.codecpar->codec_id;

    // Software is settled here whenever the device's own report settles it; the stream's
    // profile and size are the decoder's question, asked in choose().
    if (m_device.info().videoDecodeOps == 0) {
        m_path = Path::SoftwareNoDecoder;
    } else if (m_path == Path::Undecided && (m_device.info().videoDecodeOps & decodeOperation(id)) == 0) {
        m_path = Path::SoftwareCodec;
    }
    const AVCodec* codec = m_path == Path::Undecided ? vulkanDecoder(id) : nullptr;
    if (m_path == Path::Undecided && codec == nullptr) m_path = Path::SoftwareCodec;
    const bool hardware = m_path == Path::Undecided;
    if (!hardware) codec = avcodec_find_decoder(id);
    if (codec == nullptr) throw WallpaperContentError(describeRejection(kComponent, "no decoder for the video", m_file));

    m_codec = avcodec_alloc_context3(codec);
    if (m_codec == nullptr) throwOutOfMemory("VideoDecoder");
    if (const int err = avcodec_parameters_to_context(m_codec, stream.codecpar); err < 0) {
        throwFileFailure("cannot configure the decoder", err, m_file);
    }
    m_codec->pkt_timebase = stream.time_base;
    m_softwareFormatRefused.store(false, std::memory_order_relaxed);
    m_codec->opaque = this;
    m_codec->get_format = &VideoDecoder::chooseFormat;
    // Frame threads would decode -- and submit to the decode and graphics queues -- off the thread
    // that calls next(), which is the GPU thread on a device whose queues are not internally
    // synchronised; and the format and picture callbacks assume one decoding thread. Software
    // submits nothing and takes ffmpeg's default.
    m_codec->thread_count = hardware ? 1 : 0;
    // Cropping is reported, not applied: ffmpeg cannot move a device image's origin, so applying
    // it would drop the left and top crop on the hardware path while honouring it in software.
    m_codec->apply_cropping = 0;
    m_codec->flags |= AV_CODEC_FLAG_COPY_OPAQUE;

    if (hardware) {
        if (!attachDevice()) {
            // Never software on this context: it was opened for the device -- single-threaded, and
            // for AV1 a decoder with no software output of its own. attachDevice() settled the
            // path, so this opens again on ffmpeg's default decoder.
            avcodec_free_context(&m_codec);
            openDecoder();
            return;
        }
        m_codec->get_buffer2 = &VideoDecoder::allocatePicture;
    }

    if (const int err = avcodec_open2(m_codec, codec, nullptr); err < 0) {
        throwFileFailure("cannot open the decoder", err, m_file);
    }

    m_frame = av_frame_alloc();
    if (m_frame == nullptr) throwOutOfMemory("VideoDecoder");
}

void VideoDecoder::reopenInSoftware() {
    m_pending = Pending::Nothing;
    m_flushed = false;
    m_skipping = PictureSkip::Nothing;
    m_awaitingHardwareSetup = false;
    m_parkedFailure = nullptr;
    retireDecoder();
    av_packet_unref(m_packet);
    seekToResume();
    openDecoder();
}

void VideoDecoder::seekAgain() {
    // A start on the device is positioned at a reset point it has read, so it never retries.
    if (decodesOnDevice()) throw std::logic_error("VideoDecoder: a seek retry on the hardware decoder");
    m_pending = Pending::Nothing;
    avcodec_flush_buffers(m_codec);
    m_flushed = false;
    m_skipping = PictureSkip::Nothing;
    av_packet_unref(m_packet);
    seekToResume();
}

// Nothing is marked returned, so a hardware failure during the drop resumes at the same picture.
void VideoDecoder::startAt(StreamPosition position) {
    const int64_t before = av_sat_sub64(position.m_timestamp, 1);
    if (before <= streamStart()) return;
    if (decodesOnDevice()) {
        startAtResetPoint(before);
        return;
    }
    m_returned.lastTimestamp = before;
    m_startedAfter = before;
    seekToResume();
}

void VideoDecoder::startAtFirstPicture() {
    rewind();
    m_resume = std::monostate{};
    m_returned = {};
    m_startedAfter.reset();
}

bool VideoDecoder::withinCatchUp(int64_t distance) const {
    const AVStream& stream = *m_format->streams[m_stream];
    const std::optional<std::chrono::nanoseconds> interval = nominalInterval(stream);
    const std::optional<std::chrono::nanoseconds> span = inNanoseconds(distance, stream.time_base);
    return interval && span && *span <= kCatchUpPictures * *interval;
}

int64_t VideoDecoder::nextSeekBack(int64_t back) const {
    if (back != 0) return av_sat_add64(back, back);
    return std::max<int64_t>(1, av_rescale_q(1, AVRational{1, 1}, m_format->streams[m_stream]->time_base));
}

// Each window aims twice as far back as the last and is read up to where the last one began. The first
// window's first reset point is taken as it is met, which is where a keyframe start always decoded
// from; in a later one the last is taken, being the nearest, and sought again.
void VideoDecoder::startAtResetPoint(int64_t resume) {
    std::optional<PacketMark> newer;
    for (int64_t back = 0;; back = nextSeekBack(back)) {
        const int64_t target = av_sat_sub64(resume, back);
        bool fromStart = target <= streamStart();
        if (!fromStart && ffmpeg_file::seekFrame(*m_format, m_stream, target, AVSEEK_FLAG_BACKWARD) < 0) {
            // Where a failed seek left the container is not known, and reading from the first packet
            // is bounded only when the position itself is within the budget; seekToResume() decides alike.
            if (!withinCatchUp(resume - streamStart())) {
                rewind();
                return;
            }
            fromStart = true;
        }
        if (fromStart) {
            rewind();
        } else {
            m_packetsRead.reset();
        }
        const Window window = scanWindow(resume, newer, fromStart, back == 0);
        if (!window.last) {
            // From the start, the first packet is not timestamped at or before the position.
            // Past the budget, a reset point further back could only be shown where it stands, and
            // reading back to it holds the GPU lock for as long as the position is from it.
            if (fromStart || !withinCatchUp(back)) {
                rewind();
                return;
            }
            if (window.first) newer = window.first;
            continue;
        }
        const ResetPoint& reset = *window.last;
        if (back != 0 && !seekToPacket(reset.mark, fromStart ? std::nullopt : std::optional<int64_t>(target))) {
            // Only a file changed since the scan gets here.
            rewind();
            return;
        }
        const bool withinBudget = withinCatchUp(resume - reset.time);
        if (!withinBudget && (reset.fileStart || reset.time <= streamStart())) {
            rewind();
            return;
        }
        m_returned.lastTimestamp = withinBudget ? resume : av_sat_sub64(reset.time, 1);
        m_resume = DropThrough{};
        m_startedAfter = m_returned.lastTimestamp;
        return;
    }
}

// Reads packets without decoding them, from where the container stands -- a packet already held
// first -- until one decoded after the position or the first packet of the window read before.
// Nothing before a reset point in decode order is shown after it, so none past that one is nearer.
// With `stopAtFirst` the first reset point found is left held for the decoder.
VideoDecoder::Window VideoDecoder::scanWindow(int64_t resume, const std::optional<PacketMark>& newer, bool fromStart,
                                              bool stopAtFirst) {
    Window window;
    for (;;) {
        if (m_packet->data == nullptr) {
            const int err = readVideoPacket();
            if (err == AVERROR_EOF) return window;
            if (err < 0) throwFileFailure("cannot read the wallpaper", err, m_file);
        }
        const bool fileStart = fromStart && !window.first;
        if (!window.first) window.first = PacketMark::of(*m_packet);
        const int64_t time = m_packet->pts != AV_NOPTS_VALUE ? m_packet->pts : m_packet->dts;
        if (time != AV_NOPTS_VALUE && time <= resume &&
            (fileStart || resetsDecoder(*m_format->streams[m_stream]->codecpar, *m_packet))) {
            window.last = ResetPoint{PacketMark::of(*m_packet), time, fileStart};
            if (stopAtFirst) return window;
        }
        const int64_t order = m_packet->dts != AV_NOPTS_VALUE ? m_packet->dts : m_packet->pts;
        const bool past = (newer && newer->matches(*m_packet)) || (order != AV_NOPTS_VALUE && order > resume);
        av_packet_unref(m_packet);
        if (past) return window;
    }
}

// Leaves the packet `mark` names read and held, as rewind() leaves packet 1: found after a seek to
// `target`, else by reading from the first packet. A packet held from before the seek is dropped: it
// could be that same one, which would then be sent twice.
bool VideoDecoder::seekToPacket(const PacketMark& mark, std::optional<int64_t> target) {
    av_packet_unref(m_packet);
    if (target && ffmpeg_file::seekFrame(*m_format, m_stream, *target, AVSEEK_FLAG_BACKWARD) >= 0) {
        m_packetsRead.reset();
        if (skipToPacket(mark)) return true;
    }
    rewind();
    return skipToPacket(mark);
}

// Stops at a packet decoded after the one sought: `mark` is a reset point, so every packet before it
// in decode order is also presented before it.
bool VideoDecoder::skipToPacket(const PacketMark& mark) {
    const int64_t sought = mark.dts != AV_NOPTS_VALUE ? mark.dts : mark.pts;
    for (;;) {
        if (m_packet->data == nullptr && readVideoPacket() < 0) return false;
        if (mark.matches(*m_packet)) return true;
        const int64_t at = m_packet->dts != AV_NOPTS_VALUE ? m_packet->dts : m_packet->pts;
        av_packet_unref(m_packet);
        if (sought != AV_NOPTS_VALUE && at != AV_NOPTS_VALUE && at > sought) return false;
    }
}

bool VideoDecoder::decodesOnDevice() const { return m_codec != nullptr && m_codec->hw_device_ctx != nullptr; }

PictureSkip VideoDecoder::deepestSkip() const {
    switch (m_codec != nullptr ? m_codec->codec_id : AV_CODEC_ID_NONE) {
        case AV_CODEC_ID_H264:
        case AV_CODEC_ID_HEVC: return decodesOnDevice() ? PictureSkip::Unreferenced : PictureSkip::NonKey;
        case AV_CODEC_ID_MPEG1VIDEO:
        case AV_CODEC_ID_MPEG2VIDEO:
        case AV_CODEC_ID_MPEG4: return PictureSkip::Unreferenced;
        default: return PictureSkip::Nothing;
    }
}

void VideoDecoder::seekToResume() {
    // Packets read before a seek lie nowhere near the picture the resume returns. Not in restart(): the
    // packets read past a pass's last picture are part of its distance to the next pass's first.
    m_readSinceReturned = 0;
    // Ending without meeting that packet's picture means software did not decode the same pictures.
    const bool byPacket = m_returned.lastPacket && !std::holds_alternative<DropThroughPacket>(m_resume);
    if (!m_returned.lastTimestamp && !byPacket && m_startedAfter) {
        // A count of pictures since a start part way in says nothing measured from the first packet, so
        // this resumes by timestamp, where the count is never a number to drop. It is kept: it is what
        // says pictures have been shown.
        m_returned.lastTimestamp = m_startedAfter;
        m_returned.lastPacket.reset();
        m_resume = std::monostate{};
    }
    if (!m_returned.lastTimestamp) {
        // Nothing left says which picture was returned last, which loses the place, not the file.
        if (m_returned.count != 0 && !byPacket && m_returned.skipped) {
            startAtFirstPicture();
            return;
        }
        rewind();
        if (m_returned.count == 0) {
            m_resume = std::monostate{};
        } else if (byPacket) {
            m_resume = DropThroughPacket{};
        } else {
            m_resume = DropCount{m_returned.count};
        }
        return;
    }
    // Each retry aims further back, so a demuxer that lands past its target costs a few keyframe
    // intervals rather than the whole file. A start has shown nothing, so it has no continuity to keep
    // past the catch-up budget and starts at the stream's first picture instead.
    const Seeked* unlanded = std::get_if<Seeked>(&m_resume);
    const int64_t back = unlanded != nullptr ? nextSeekBack(unlanded->back) : 0;
    const bool starting = m_returned.count == 0;
    if (starting && back != 0 && !withinCatchUp(back)) {
        startAtFirstPicture();
        return;
    }
    const int64_t target = av_sat_sub64(*m_returned.lastTimestamp, back);
    if (target > streamStart() && ffmpeg_file::seekFrame(*m_format, m_stream, target, AVSEEK_FLAG_BACKWARD) >= 0) {
        m_packetsRead.reset();
        m_resume = Seeked{back};
    } else if (starting && !withinCatchUp(*m_returned.lastTimestamp - streamStart())) {
        startAtFirstPicture();
    } else {
        rewind();
        m_resume = DropThrough{};
    }
}

int64_t VideoDecoder::streamStart() const {
    const int64_t start = m_format->streams[m_stream]->start_time;
    return start == AV_NOPTS_VALUE ? 0 : start;
}

// Back to the first video packet. A seek shown to reach packet 1 leaves it read and numbered;
// otherwise the container is opened again.
void VideoDecoder::rewind() {
    av_packet_unref(m_packet);
    m_packetsRead.reset();
    if (m_firstPacket && seekToFirstPacket()) {
        m_packetsRead = 0;
        numberPacket();
        return;
    }
    avformat_close_input(&m_format);
    openContainer();
    m_packetsRead = 0;
}

// No one target reaches packet 1 in every container. Its decode time does in a transport stream,
// where the stream's start time lands past a packet 1 that decodes before it; its presentation time
// does in Matroska, which gives packet 1 no decode time; its byte position does in a raw bitstream,
// which cannot seek by time.
bool VideoDecoder::seekToFirstPacket() {
    const PacketMark& first = *m_firstPacket;
    const int64_t presented = first.pts != AV_NOPTS_VALUE ? first.pts : streamStart();
    return (first.dts != AV_NOPTS_VALUE && seekLandsOnFirstPacket(first.dts, AVSEEK_FLAG_BACKWARD)) ||
           (presented != first.dts && seekLandsOnFirstPacket(presented, AVSEEK_FLAG_BACKWARD)) ||
           (first.position && seekLandsOnFirstPacket(*first.position, AVSEEK_FLAG_BYTE));
}

bool VideoDecoder::seekLandsOnFirstPacket(int64_t target, int flags) {
    const int stream = (flags & AVSEEK_FLAG_BYTE) != 0 ? -1 : m_stream;
    if (ffmpeg_file::seekFrame(*m_format, stream, target, flags) < 0) return false;
    if (readVideoPacket() >= 0 && m_firstPacket->matches(*m_packet)) return true;
    av_packet_unref(m_packet);
    return false;
}

VideoDecoder::PacketMark VideoDecoder::PacketMark::of(const AVPacket& packet) {
    return PacketMark{packet.pos >= 0 ? std::optional<int64_t>(packet.pos) : std::nullopt, packet.dts, packet.pts,
                      packet.size};
}

// By byte position where the mark has one. Otherwise by timestamps and size, which another packet
// could share, and a false match of packet 1 misnumbers every packet after it for a resume by packet
// number; so that match is only made between packets without a position, and needs a timestamp.
bool VideoDecoder::PacketMark::matches(const AVPacket& packet) const {
    if (position) return packet.pos == *position;
    return packet.pos < 0 && (dts != AV_NOPTS_VALUE || pts != AV_NOPTS_VALUE) && packet.dts == dts &&
           packet.pts == pts && packet.size == size;
}

void VideoDecoder::assertMayDecodeHere(const char* what) const {
    if (!m_device.queuesInternallySynchronized()) m_device.assertGpuThread(what);
}

void VideoDecoder::restart() {
    assertMayDecodeHere("VideoDecoder::restart");
    avcodec_flush_buffers(m_codec);
    m_flushed = false;
    m_skipping = PictureSkip::Nothing;
    m_returned = {};
    m_resume = std::monostate{};
    m_startedAfter.reset();
    rewind();
}

// Whether a decoded picture is one next() has not yet returned. Pictures before the resume point
// are decoded and dropped here rather than skipped as packets: later pictures reference them.
bool VideoDecoder::admit(int64_t timestamp, std::optional<uint64_t> packet) {
    if (auto* count = std::get_if<DropCount>(&m_resume)) {
        if (--count->remaining == 0) m_resume = std::monostate{};
        return false;
    }
    if (std::holds_alternative<DropThroughPacket>(m_resume)) {
        // Equal, never ordered: with B-frames, pictures come out in another order than their packets.
        if (packet == m_returned.lastPacket) m_resume = std::monostate{};
        return false;
    }
    if (std::holds_alternative<Seeked>(m_resume)) {
        // A picture without a timestamp says nothing of where the seek landed. One past the resume
        // point does: pictures between the two may never have been decoded.
        if (timestamp == AV_NOPTS_VALUE) return false;
        if (timestamp > *m_returned.lastTimestamp) {
            require(Pending::SeekAgain);
        } else {
            m_resume = DropThrough{};
        }
        return false;
    }
    if (std::holds_alternative<DropThrough>(m_resume)) {
        if (timestamp == AV_NOPTS_VALUE || timestamp <= *m_returned.lastTimestamp) return false;
        m_resume = std::monostate{};
    }
    return true;
}

void VideoDecoder::recordReturned(int64_t timestamp, std::optional<uint64_t> packet) {
    const bool increasing = timestamp != AV_NOPTS_VALUE &&
                            (m_returned.count == 0 ||
                             (m_returned.lastTimestamp && timestamp > *m_returned.lastTimestamp));
    m_returned.lastTimestamp = increasing ? std::optional<int64_t>(timestamp) : std::nullopt;
    m_returned.lastPacket = packet;
    ++m_returned.count;
}

// A seek can land on a picture that is not a keyframe: a transport stream's does, having no index.
// Its references were never decoded, and recordLanding() proves nothing from it. Software only, as
// every Seeked state is.
bool VideoDecoder::beforeLandingKeyframe() const {
    const auto* seeked = std::get_if<Seeked>(&m_resume);
    return seeked != nullptr && seeked->landing == Landing::NoPacket && (m_packet->flags & AV_PKT_FLAG_KEY) == 0;
}

// Called with each packet decoded. The first after a seek is a keyframe and shows where it landed;
// any later one can only show that a picture past the resume point was left to decode.
void VideoDecoder::recordLanding() {
    auto* seeked = std::get_if<Seeked>(&m_resume);
    if (seeked == nullptr || seeked->landing == Landing::Unproven) return;
    const int64_t at = m_packet->pts != AV_NOPTS_VALUE ? m_packet->pts : m_packet->dts;
    const bool atOrBefore = at != AV_NOPTS_VALUE && at <= *m_returned.lastTimestamp;
    if (seeked->landing == Landing::NoPacket && atOrBefore && m_returned.count == 0 &&
        !withinCatchUp(*m_returned.lastTimestamp - at)) {
        // A start past the budget from its keyframe shows the keyframe, as startAtResetPoint() does. No
        // rewind: this packet is about to be sent.
        if (at <= streamStart()) {
            m_returned = {};
            m_startedAfter.reset();
            m_resume = std::monostate{};
        } else {
            m_returned.lastTimestamp = av_sat_sub64(at, 1);
            m_startedAfter = m_returned.lastTimestamp;
            m_resume = DropThrough{};
        }
        return;
    }
    seeked->landing = atOrBefore ? Landing::NothingPast : Landing::Unproven;
}

bool VideoDecoder::attachDevice() {
    BufferRef ref{av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN)};
    if (ref.ref == nullptr) throwOutOfMemory("VideoDecoder");
    auto* context = reinterpret_cast<AVHWDeviceContext*>(ref.ref->data);
    auto* hw = static_cast<AVVulkanDeviceContext*>(context->hwctx);

    // ffmpeg loads libvulkan itself only for a device it creates; on this path an unset loader
    // is called through as a null pointer.
    hw->get_proc_addr = vkGetInstanceProcAddr;
    hw->inst = m_device.instance();
    hw->phys_dev = m_device.physicalDevice();
    hw->act_dev = m_device.device();
    hw->device_features = m_device.enabledFeatures();
    hw->enabled_inst_extensions = m_device.enabledInstanceExtensions().data();
    hw->nb_enabled_inst_extensions = static_cast<int>(m_device.enabledInstanceExtensions().size());
    hw->enabled_dev_extensions = m_device.enabledDeviceExtensions().data();
    hw->nb_enabled_dev_extensions = static_cast<int>(m_device.enabledDeviceExtensions().size());
    listQueueFamilies(m_device, *hw);

    // ffmpeg never destroys a device it was given, so the Device outlives this context whatever
    // order they are freed in.
    // Each refusal below is this ffmpeg and this driver declining, not the GPU failing, so the
    // device is asked which it is -- the same question every refusal during decoding asks.
    if (const int err = av_hwdevice_ctx_init(ref.ref); err < 0) {
        if (err == AVERROR(ENOMEM)) throwOutOfMemory("VideoDecoder");
        if (!deviceHealthy()) {
            throw GpuFailure("VideoDecoder: ffmpeg cannot decode on " + m_device.info().gpuName + " (" +
                             avErrorString(err) + ") and the device no longer answers");
        }
        m_path = Path::SoftwareRefused;
        return false;
    }

    m_videoFormatProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceVideoFormatPropertiesKHR>(
        vkGetInstanceProcAddr(m_device.instance(), "vkGetPhysicalDeviceVideoFormatPropertiesKHR"));
    if (m_videoFormatProperties == nullptr) {
        if (!deviceHealthy()) {
            throw GpuFailure("VideoDecoder: " + m_device.info().gpuName +
                             " enables VK_KHR_video_queue but has no "
                             "vkGetPhysicalDeviceVideoFormatPropertiesKHR, and no longer answers");
        }
        m_path = Path::SoftwareRefused;
        return false;
    }
    m_videoCapabilities = reinterpret_cast<PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR>(
        vkGetInstanceProcAddr(m_device.instance(), "vkGetPhysicalDeviceVideoCapabilitiesKHR"));
    if (m_videoCapabilities == nullptr) {
        if (!deviceHealthy()) {
            throw GpuFailure("VideoDecoder: " + m_device.info().gpuName +
                             " enables VK_KHR_video_queue but has no vkGetPhysicalDeviceVideoCapabilitiesKHR, "
                             "and no longer answers");
        }
        m_path = Path::SoftwareRefused;
        return false;
    }
    m_codec->hw_device_ctx = ref.release();
    return true;
}

// Asks the device whether it decodes this stream, through the query ffmpeg's own setup makes --
// profile, bit depth and coded size against the decoder's capabilities -- and on a yes builds
// the frames ffmpeg will decode into. Built here rather than left to ffmpeg because its
// defaults ask more of the images than a video profile need allow: STORAGE usage and an ALIAS
// flag. The image that will exist -- what is asked for plus what ffmpeg adds from the format's
// features -- is what the profile is asked about, so a no lands on the software path.
//
// True means decode on the device. False means the path was decided as software. Only a fault
// is thrown: a query that fails as a driver call, or images that cannot be created after the
// device's yes on a device that no longer answers.
bool VideoDecoder::attachHardwareFrames(AVCodecContext* codec) {
    BufferRef frames;
    const int err = avcodec_get_hw_frames_parameters(codec, codec->hw_device_ctx, AV_PIX_FMT_VULKAN, &frames.ref);
    if (err == AVERROR(ENOMEM)) throwOutOfMemory("VideoDecoder");
    if (err == AVERROR(ENOENT) || err == AVERROR(ENOSYS)) {
        m_path = Path::SoftwareCodec;
        return false;
    }
    if (err < 0) {
        // ffmpeg folds every capability answer it does not name -- picture layout, codec, Std
        // header version -- into the same code as a failed call; nothing has been asked of the
        // device but questions, so all of it is the device declining.
        m_path = Path::SoftwareRefused;
        return false;
    }

    auto* context = reinterpret_cast<AVHWFramesContext*>(frames.ref->data);
    auto* vk = static_cast<AVVulkanFramesContext*>(context->hwctx);
    const auto* profiles = static_cast<const VkVideoProfileListInfoKHR*>(
        findInChain(vk->create_pnext, VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR));
    // One multi-planar image per picture, of a layout and format the converter reads, is what
    // submit() and the conversion after it handle. Anything else goes to software, never to a
    // converter that would refuse it as bad content.
    if (profiles == nullptr || profiles->profileCount == 0 || vk->format[0] == VK_FORMAT_UNDEFINED ||
        vk->format[1] != VK_FORMAT_UNDEFINED || !FrameConverter::readsDeviceImage(m_device, context->sw_format, vk->format[0])) {
        m_path = Path::SoftwareRefused;
        return false;
    }

    // The decoder's own usage plus a copy source for the conversion that follows, which copies
    // the planes out rather than sampling them; the query is made with what ffmpeg will add.
    const VkImageUsageFlags renderer = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    const VkImageUsageFlags asked =
        (vk->usage & (VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR)) | renderer;
    const VkFormatFeatureFlags2 features = formatFeatures(m_device.physicalDevice(), vk->format[0], vk->tiling);
    if ((usageFfmpegCreates(0, features) & renderer) != renderer) {
        m_path = Path::SoftwareRefused;
        return false;
    }
    const VkImageUsageFlags usage = usageFfmpegCreates(asked, features);

    VkPhysicalDeviceVideoFormatInfoKHR query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR};
    query.pNext = profiles;
    query.imageUsage = usage;
    uint32_t count = 0;
    VkResult result = m_videoFormatProperties(m_device.physicalDevice(), &query, &count, nullptr);
    std::vector<VkVideoFormatPropertiesKHR> formats(count, VkVideoFormatPropertiesKHR{VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR});
    if ((result == VK_SUCCESS || result == VK_INCOMPLETE) && count != 0) {
        result = m_videoFormatProperties(m_device.physicalDevice(), &query, &count, formats.data());
        formats.resize(count);
    }
    if (isVideoRefusal(result)) {
        m_path = Path::SoftwareRefused;
        return false;
    }
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        throwVkFailure("VideoDecoder: vkGetPhysicalDeviceVideoFormatPropertiesKHR", result);
    }
    const VkVideoFormatPropertiesKHR* match = nullptr;
    for (const VkVideoFormatPropertiesKHR& candidate : formats) {
        if (candidate.format == vk->format[0] && candidate.imageTiling == vk->tiling) match = &candidate;
    }
    // ffmpeg reads zero flags as unset and fills in its own, ALIAS among them, so a profile that
    // allows neither of the two it would otherwise get cannot be given a valid image.
    const VkImageCreateFlags flags =
        match == nullptr ? 0
                         : match->imageCreateFlags &
                               (VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT);
    if (flags == 0) {
        m_path = Path::SoftwareRefused;
        return false;
    }
    VkExtent2D minCoded{};
    result = minCodedExtent(m_videoCapabilities, m_device.physicalDevice(), profiles->pProfiles[0], minCoded);
    if (result != VK_SUCCESS) throwVkFailure("VideoDecoder: vkGetPhysicalDeviceVideoCapabilitiesKHR", result);
    vk->usage = static_cast<VkImageUsageFlagBits>(asked);
    vk->img_flags = flags;

    // Past the device's yes. ffmpeg still has reasons of its own to fail here, so the device is
    // asked whether it is the one at fault.
    if (const int initError = av_hwframe_ctx_init(frames.ref); initError < 0) {
        if (initError == AVERROR(ENOMEM)) throwOutOfMemory("VideoDecoder");
        if (!deviceHealthy()) {
            throw GpuFailure("VideoDecoder: creating decode images on " + m_device.info().gpuName + " failed (" +
                             avErrorString(initError) + ") and the device no longer answers");
        }
        m_path = Path::SoftwareRefused;
        return false;
    }
    m_framesContexts.reserve(m_framesContexts.size() + 1);
    AVBufferRef* held = av_buffer_ref(frames.ref);
    if (held == nullptr) throwOutOfMemory("VideoDecoder");
    m_framesContexts.push_back({held, minCoded, std::nullopt});
    codec->hw_frames_ctx = frames.release();
    return true;
}

// ffmpeg's format negotiation, called from inside its C frames, which nothing may throw
// through: whatever choose() throws is parked and rethrown once ffmpeg reports the error that
// follows a refusal of every format.
AVPixelFormat VideoDecoder::chooseFormat(AVCodecContext* codec, const AVPixelFormat* offered) {
    auto* self = static_cast<VideoDecoder*>(codec->opaque);
    // Software takes the last plain format offered. A refusal is flagged rather than left to ffmpeg,
    // whose slice decoders skip a slice they get no format for and go on reading.
    if (codec->hw_device_ctx == nullptr) {
        AVPixelFormat software = AV_PIX_FMT_NONE;
        for (const AVPixelFormat* format = offered; *format != AV_PIX_FMT_NONE; ++format) {
            if ((av_pix_fmt_desc_get(*format)->flags & AV_PIX_FMT_FLAG_HWACCEL) == 0) software = *format;
        }
        if (software == AV_PIX_FMT_NONE) self->m_softwareFormatRefused.store(true, std::memory_order_relaxed);
        return software;
    }
    try {
        return self->choose(codec, offered);
    } catch (const std::bad_alloc&) {
        self->m_parkedFailure = std::make_exception_ptr(ResourceExhausted("VideoDecoder: out of memory"));
    } catch (...) {
        self->m_parkedFailure = std::current_exception();
    }
    return AV_PIX_FMT_NONE;
}

// The decision, made from the stream's parsed headers. ffmpeg re-runs this without a format
// whose setup failed, which is how a fault after the device's yes shows up here. A codec opened for
// the device only.
AVPixelFormat VideoDecoder::choose(AVCodecContext* codec, const AVPixelFormat* offered) {
    bool vulkanOffered = false;
    for (const AVPixelFormat* format = offered; *format != AV_PIX_FMT_NONE; ++format) {
        if (*format == AV_PIX_FMT_VULKAN) vulkanOffered = true;
    }

    if (vulkanOffered) {
        if (attachHardwareFrames(codec)) {
            m_awaitingHardwareSetup = true;
            return AV_PIX_FMT_VULKAN;
        }
    } else if (m_awaitingHardwareSetup) {
        // ffmpeg's own decoder setup failed after the device's yes, and it says nothing of why.
        m_awaitingHardwareSetup = false;
        if (!deviceHealthy()) {
            throw GpuFailure(describeRejection(kComponent,
                                               m_device.info().gpuName + " accepted the wallpaper and no longer answers "
                                                                         "after its hardware decoder failed to initialise",
                                               m_file));
        }
        m_path = Path::SoftwareRefused;
    } else {
        // The codec offers no Vulkan output for this stream's pixel format.
        m_path = Path::SoftwareRefused;
    }
    // Never software on this context: it was opened for the device -- single-threaded, and for
    // AV1 a decoder with no software output of its own.
    require(Pending::ReopenInSoftware);
    return AV_PIX_FMT_NONE;
}

// ffmpeg's request for the image a picture decodes into, made before any decode work for it is
// recorded. AV1 keeps its images across a size change within one pixel format, so a picture can
// arrive that they cannot hold or that the device cannot decode; either is the device declining it.
int VideoDecoder::allocatePicture(AVCodecContext* codec, AVFrame* frame, int flags) {
    auto* self = static_cast<VideoDecoder*>(codec->opaque);
    if (codec->hw_frames_ctx != nullptr && !self->holds(*codec->hw_frames_ctx, frame->width, frame->height)) {
        self->m_path = Path::SoftwareRefused;
        self->require(Pending::ReopenInSoftware);
        return AVERROR(ENOSYS);
    }
    return avcodec_default_get_buffer2(codec, frame, flags);
}

bool VideoDecoder::holds(const AVBufferRef& frames, int width, int height) const {
    const auto held = std::find_if(m_framesContexts.begin(), m_framesContexts.end(),
                                   [&](const HeldFrames& h) { return h.ref->data == frames.data; });
    if (held == m_framesContexts.end()) return false;
    const auto* context = reinterpret_cast<const AVHWFramesContext*>(frames.data);
    return width <= context->width && height <= context->height && width >= static_cast<int>(held->minCoded.width) &&
           height >= static_cast<int>(held->minCoded.height);
}

int VideoDecoder::readVideoPacket() {
    for (;;) {
        const int err = ffmpeg_file::readPacket(*m_format, m_packet);
        if (err < 0) return err;
        if (m_packet->stream_index == m_stream) {
            if (m_packetsRead) numberPacket();
            // One the container marks as decoded only for reference, as outside an edit list, is
            // never shown, so it neither ends a pass nor puts distance between two pictures.
            if ((m_packet->flags & AV_PKT_FLAG_DISCARD) != 0) return 0;
            ++m_readSinceReturned;
            const AVStream& stream = *m_format->streams[m_stream];
            const auto start = inNanoseconds(m_packet->pts, stream.time_base);
            const auto length = m_packet->duration > 0 ? inNanoseconds(m_packet->duration, stream.time_base)
                                                       : nominalInterval(stream);
            if (start && length && (!m_readThrough || *start + *length > *m_readThrough)) m_readThrough = *start + *length;
            return 0;
        }
        av_packet_unref(m_packet);
    }
}

void VideoDecoder::numberPacket() {
    m_packet->opaque = reinterpret_cast<void*>(static_cast<uintptr_t>(++*m_packetsRead) << kLevelBits);
    if (*m_packetsRead == 1) m_firstPacket = PacketMark::of(*m_packet);
}

// What an ffmpeg error during decoding means. Returns only when the hardware path failed on a
// device that still answers, having arranged for the stream to be decoded in software instead.
void VideoDecoder::failDecode(const char* what, int err) {
    if (m_parkedFailure) std::rethrow_exception(std::exchange(m_parkedFailure, nullptr));
    if (m_softwareFormatRefused.load(std::memory_order_relaxed)) {
        throw WallpaperContentError(
            describeRejection(kComponent, "the wallpaper decodes to no format software can hold", m_file));
    }
    const std::string failed = std::string(what) + " failed";
    const bool hardware = m_awaitingHardwareSetup || m_path == Path::Hardware;
    if (err == AVERROR_EXTERNAL && hardware) {
        if (!deviceHealthy()) {
            throw GpuFailure(describeRejection(
                kComponent, failed + ": the hardware decoder failed and the device no longer answers", m_file));
        }
        m_path = Path::SoftwareRefused;
        require(Pending::ReopenInSoftware);
        return;
    }
    throwFileFailure(failed, err, m_file);
}

std::optional<DecodedFrame> VideoDecoder::next(PictureSkip skip) {
    assertMayDecodeHere("VideoDecoder::next");
    return std::get<std::optional<DecodedFrame>>(decode(std::nullopt, skip));
}

std::variant<std::optional<DecodedFrame>, VideoDecoder::Dropping> VideoDecoder::next(uint32_t dropLimit, PictureSkip skip) {
    assertMayDecodeHere("VideoDecoder::next");
    return decode(dropLimit, skip);
}

// Every state a drop can stop in is one the loop's head already resumes from: a dropped picture is
// gone, and what admit() decided about it is in m_resume and m_pending.
std::variant<std::optional<DecodedFrame>, VideoDecoder::Dropping> VideoDecoder::decode(
    std::optional<uint32_t> dropLimit, PictureSkip skip) {
    releaseRetiredCodec();
    releaseIdleFramesContexts();
    uint32_t dropped = 0;
    for (;;) {
        // What the format callback decided during the last ffmpeg call comes before what that
        // call returned: a refusal of every format reads as bad input or as the end otherwise.
        if (m_pending == Pending::ReopenInSoftware) reopenInSoftware();
        if (m_pending == Pending::SeekAgain) seekAgain();
        int err = avcodec_receive_frame(m_codec, m_frame);
        if (m_pending != Pending::Nothing) continue;
        if (m_parkedFailure || m_softwareFormatRefused.load(std::memory_order_relaxed)) {
            failDecode("avcodec_receive_frame", err);
        }
        if (err == 0) {
            m_awaitingHardwareSetup = false;
            if (m_frame->format == AV_PIX_FMT_VULKAN) m_path = Path::Hardware;
            const int64_t timestamp = m_frame->best_effort_timestamp;
            const std::optional<uint64_t> packet = packetNumber(*m_frame);
            if (!admit(timestamp, packet)) {
                // The next picture returned is counted on from here, where the last one returned was.
                m_readSinceReturned = 0;
                av_frame_unref(m_frame);
                if (dropLimit && ++dropped >= *dropLimit) return Dropping{};
                continue;
            }
            AVFrame* out = av_frame_alloc();
            if (out == nullptr) throwOutOfMemory("VideoDecoder");
            av_frame_move_ref(out, m_frame);
            DecodedFrame frame(m_device, out);
            const AVStream& stream = *m_format->streams[m_stream];
            frame.m_presentation = inNanoseconds(timestamp, stream.time_base);
            frame.m_duration = out->duration > 0 ? inNanoseconds(out->duration, stream.time_base)
                                                 : nominalInterval(stream);
            frame.m_skip = sentSkipping(*out);
            frame.m_readSince = std::max(std::exchange(m_readSinceReturned, 0), 1U);
            recordReturned(timestamp, packet);
            if (m_returned.lastTimestamp) frame.m_position = StreamPosition(*m_returned.lastTimestamp);
            return frame;
        }
        // Nothing more can be sent after the flush, so a decoder that still asks for input has
        // no more pictures to give.
        if (err == AVERROR_EOF || (err == AVERROR(EAGAIN) && m_flushed)) {
            const auto* seeked = std::get_if<Seeked>(&m_resume);
            if ((seeked != nullptr && seeked->landing != Landing::NothingPast) ||
                std::holds_alternative<DropThroughPacket>(m_resume)) {
                require(Pending::SeekAgain);
                continue;
            }
            if (std::holds_alternative<DropCount>(m_resume)) {
                throw WallpaperContentError(describeRejection(
                    kComponent, "the wallpaper ended before software decoding reached the pictures already returned",
                    m_file));
            }
            // A start past the last picture, which a position from another file can be. Not again:
            // restart() makes this an ordinary pass.
            if (m_startedAfter && m_returned.count == 0) {
                restart();
                continue;
            }
            // A play-once caller may not call again, which would hold a decoder retired near the end until close.
            releaseRetiredCodec();
            releaseIdleFramesContexts();
            return std::nullopt;
        }
        if (isDamagedPacket(err)) continue;
        if (err != AVERROR(EAGAIN)) {
            failDecode("avcodec_receive_frame", err);
            continue;
        }

        // A packet ffmpeg asked to see again, or one a rewind read, is still held; otherwise the
        // next one is read. A container that cannot be read is fatal, unlike a packet that cannot
        // be decoded: a demuxer reporting damage is not guaranteed to have moved past it.
        const bool resend = m_packet->data != nullptr;
        if (!resend) {
            err = readVideoPacket();
            while (err >= 0 && beforeLandingKeyframe()) {
                av_packet_unref(m_packet);
                err = readVideoPacket();
            }
            if (err == AVERROR_EOF) {
                m_flushed = true;
            } else if (err < 0) {
                throwFileFailure("cannot read the wallpaper", err, m_file);
            } else {
                recordLanding();
            }
        }
        // Read by ffmpeg per packet. A level goes up at once, and down onto pictures that can reference
        // skipped ones only at a packet that resets the decoder. A container's key flag is not enough: it
        // also marks open-GOP pictures, whose leading pictures reference ones before them.
        const PictureSkip wanted =
            std::holds_alternative<std::monostate>(m_resume) ? std::min(skip, deepestSkip()) : PictureSkip::Nothing;
        if (!lowersOntoSkipped(m_skipping, wanted) ||
            (!m_flushed && resetsDecoder(*m_format->streams[m_stream]->codecpar, *m_packet))) {
            m_skipping = wanted;
        }
        m_codec->skip_frame = discardFor(m_skipping);
        if (m_skipping != PictureSkip::Nothing) m_returned.skipped = true;
        if (!m_flushed) tagSkipping(*m_packet, m_skipping);
        err = avcodec_send_packet(m_codec, m_flushed ? nullptr : m_packet);
        // EAGAIN is "read the output first, then send this one again", and ffmpeg promises the
        // receive that follows will not ask for input; a second EAGAIN for the same packet is
        // that promise broken, and failing is better than spinning on it.
        if (err == AVERROR(EAGAIN) && !resend) continue;
        av_packet_unref(m_packet);
        if (m_pending != Pending::Nothing) continue;
        if (m_parkedFailure || m_softwareFormatRefused.load(std::memory_order_relaxed)) {
            failDecode("avcodec_send_packet", err);
        }
        if (err == AVERROR(EAGAIN)) {
            throw std::logic_error("VideoDecoder: ffmpeg refused the same packet twice without giving a frame");
        }
        if (err < 0 && !isDamagedPacket(err)) failDecode("avcodec_send_packet", err);
    }
}

}
