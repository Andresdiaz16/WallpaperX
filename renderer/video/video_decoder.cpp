// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_decoder.h"

#include "frame_converter.h"

#include "../vulkan/format_features.h"
#include "../vulkan/gpu_fault.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/buffer.h>
#include <libavutil/common.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <iterator>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

namespace wallpaperx {

namespace {

std::string avErrorString(int err) {
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(err, text, sizeof text);
    return text;
}

[[noreturn]] void throwOutOfMemory(const std::string& context) {
    throw std::runtime_error(context + ": out of memory");
}

// An ffmpeg error that is not the GPU's: memory, or the file.
[[noreturn]] void throwAvFailure(const std::string& context, int err) {
    if (err == AVERROR(ENOMEM)) throwOutOfMemory(context);
    throw WallpaperContentError(context + ": " + avErrorString(err));
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

// What VideoDecoder::readVideoPacket() numbered the packet a frame was decoded from; ffmpeg
// carries it in `opaque`, where 0 is no number.
std::optional<uint64_t> packetNumber(const AVFrame& frame) {
    const auto number = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(frame.opaque));
    return number == 0 ? std::nullopt : std::optional<uint64_t>(number);
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
    }
    m_colour = FrameColour{m_frame->colorspace, m_frame->color_range, m_frame->color_primaries, m_frame->color_trc,
                           m_frame->chroma_location};
}

DecodedFrame::~DecodedFrame() { release(); }

DecodedFrame::DecodedFrame(DecodedFrame&& other) noexcept
    : m_device(other.m_device), m_frame(std::exchange(other.m_frame, nullptr)), m_colour(other.m_colour) {}

DecodedFrame& DecodedFrame::operator=(DecodedFrame&& other) noexcept {
    if (this != &other) {
        release();
        m_device = other.m_device;
        m_frame = std::exchange(other.m_frame, nullptr);
        m_colour = other.m_colour;
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
    // paths, and the image pool's own size is the same rounded to the chroma subsampling.
    if (storage() == Storage::DeviceImage) {
        const AVHWFramesContext* frames = framesContext(frame());
        return {static_cast<uint32_t>(frames->width), static_cast<uint32_t>(frames->height)};
    }
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
    const AVVkFrame* vk = vkFrame(frame());
    const auto* frames = static_cast<const AVVulkanFramesContext*>(framesContext(frame())->hwctx);
    return DeviceImage{vk->img[0], frames->format[0], vk->tiling, frames->usage, vk->layout[0]};
}

Device::SubmitTicket DecodedFrame::submit(const std::function<void(VkCommandBuffer)>& record,
                                          VkPipelineStageFlags readStage, Device::SubmitScope scope) {
    if (storage() != Storage::DeviceImage) throw std::logic_error("DecodedFrame: a host frame has nothing to submit");
    AVVkFrame* vk = vkFrame(frame());

    const FrameLock lock(framesContext(frame()), vk);
    const uint64_t current = vk->sem_value[0];
    scope.timelineWait.emplace_back(vk->sem[0], current, readStage);
    scope.timelineSignal.emplace_back(vk->sem[0], current + 1);
    const Device::SubmitTicket ticket = m_device->submit(record, scope);
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

VideoDecoder::VideoDecoder(Device& device, const std::string& file) : m_device(device), m_file(file) {
    m_device.assertGpuThread("VideoDecoder::VideoDecoder");
    try {
        openContainer();
        openDecoder();
    } catch (...) {
        closeDecoder();
        avformat_close_input(&m_format);
        throw;
    }
}

VideoDecoder::~VideoDecoder() {
    closeDecoder();
    avformat_close_input(&m_format);
}

// The one bounded question this renderer can ask of the GPU: an empty batch through the
// Device, which a device that is lost or has stopped fails.
bool VideoDecoder::deviceHealthy() {
    try {
        m_device.immediateSubmit([](VkCommandBuffer) {});
        return true;
    } catch (const GpuFailure&) {
        return false;
    }
}

// Freeing the codec frees its image pool once no frame is out, and ffmpeg waits without bound
// on every image's semaphore first. The codec context is what owns the pool, so it is all that
// is leaked; the scratch objects hold no GPU work.
void VideoDecoder::closeDecoder() noexcept {
    av_frame_free(&m_frame);
    av_packet_free(&m_packet);
    if (m_codec != nullptr && m_codec->hw_frames_ctx != nullptr && !m_device.safeToFree("the video decoder's images")) {
        m_codec = nullptr;
    }
    avcodec_free_context(&m_codec);
}

void VideoDecoder::openContainer() {
    if (const int err = avformat_open_input(&m_format, m_file.c_str(), nullptr, nullptr); err < 0) {
        throwAvFailure("VideoDecoder: cannot open " + m_file, err);
    }
    if (const int err = avformat_find_stream_info(m_format, nullptr); err < 0) {
        throwAvFailure("VideoDecoder: cannot read the streams of " + m_file, err);
    }
    const int stream = av_find_best_stream(m_format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream == AVERROR_STREAM_NOT_FOUND) throw WallpaperContentError("VideoDecoder: " + m_file + " has no video stream");
    if (stream < 0) throwAvFailure("VideoDecoder: cannot pick a video stream in " + m_file, stream);
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
    if (codec == nullptr) throw WallpaperContentError("VideoDecoder: no decoder for the video in " + m_file);

    m_codec = avcodec_alloc_context3(codec);
    if (m_codec == nullptr) throwOutOfMemory("VideoDecoder");
    if (const int err = avcodec_parameters_to_context(m_codec, stream.codecpar); err < 0) {
        throwAvFailure("VideoDecoder: cannot configure the decoder for " + m_file, err);
    }
    m_codec->pkt_timebase = stream.time_base;
    m_codec->opaque = this;
    m_codec->get_format = &VideoDecoder::chooseFormat;
    // Frame threads would decode -- and submit to the decode and graphics queues -- off the GPU
    // thread; the graphics queue is shared with the Device and not free-threaded. Software
    // submits nothing and takes ffmpeg's default.
    m_codec->thread_count = hardware ? 1 : 0;
    // Cropping is reported, not applied: ffmpeg cannot move a device image's origin, so applying
    // it would drop the left and top crop on the hardware path while honouring it in software.
    m_codec->apply_cropping = 0;
    m_codec->flags |= AV_CODEC_FLAG_COPY_OPAQUE;

    if (hardware) attachDevice();

    if (const int err = avcodec_open2(m_codec, codec, nullptr); err < 0) {
        throwAvFailure("VideoDecoder: cannot open the decoder for " + m_file, err);
    }

    m_packet = av_packet_alloc();
    m_frame = av_frame_alloc();
    if (m_packet == nullptr || m_frame == nullptr) throwOutOfMemory("VideoDecoder");
}

void VideoDecoder::reopenInSoftware() {
    m_reopenInSoftware = false;
    m_flushed = false;
    m_awaitingHardwareSetup = false;
    m_parkedFailure = nullptr;
    closeDecoder();

    if (!m_lastTimestamp) {
        // Ending without meeting that packet's picture means software did not decode the same pictures.
        const bool packetMissed = std::holds_alternative<DropThroughPacket>(m_resume);
        rewind();
        if (m_returned == 0) {
            m_resume = std::monostate{};
        } else if (m_lastPacket && !packetMissed) {
            m_resume = DropThroughPacket{};
        } else {
            m_resume = DropCount{m_returned};
        }
    } else {
        // Each retry aims twice as far back, from a second, so a demuxer that lands past its
        // target costs a few keyframe intervals rather than the whole file.
        const Seeked* unlanded = std::get_if<Seeked>(&m_resume);
        const int64_t second =
            std::max<int64_t>(1, av_rescale_q(1, AVRational{1, 1}, m_format->streams[m_stream]->time_base));
        int64_t back = 0;
        if (unlanded != nullptr) back = unlanded->back == 0 ? second : av_sat_add64(unlanded->back, unlanded->back);
        const int64_t target = av_sat_sub64(*m_lastTimestamp, back);
        if (target > streamStart() && av_seek_frame(m_format, m_stream, target, AVSEEK_FLAG_BACKWARD) >= 0) {
            m_packetsRead.reset();
            m_resume = Seeked{back};
        } else {
            rewind();
            m_resume = DropThrough{};
        }
    }
    openDecoder();
}

int64_t VideoDecoder::streamStart() const {
    const int64_t start = m_format->streams[m_stream]->start_time;
    return start == AV_NOPTS_VALUE ? 0 : start;
}

// Back to the first packet on the container already open; a container that cannot seek is
// opened again.
void VideoDecoder::rewind() {
    if (av_seek_frame(m_format, m_stream, streamStart(), AVSEEK_FLAG_BACKWARD) < 0) {
        avformat_close_input(&m_format);
        openContainer();
    }
    m_packetsRead = 0;
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
        if (packet == m_lastPacket) m_resume = std::monostate{};
        return false;
    }
    if (std::holds_alternative<Seeked>(m_resume)) {
        // A picture without a timestamp says nothing of where the seek landed. One past the resume
        // point does: pictures between the two may never have been decoded.
        if (timestamp == AV_NOPTS_VALUE) return false;
        if (timestamp > *m_lastTimestamp) {
            m_reopenInSoftware = true;
        } else {
            m_resume = DropThrough{};
        }
        return false;
    }
    if (std::holds_alternative<DropThrough>(m_resume)) {
        if (timestamp == AV_NOPTS_VALUE || timestamp <= *m_lastTimestamp) return false;
        m_resume = std::monostate{};
    }
    return true;
}

void VideoDecoder::recordReturned(int64_t timestamp, std::optional<uint64_t> packet) {
    const bool increasing =
        timestamp != AV_NOPTS_VALUE && (m_returned == 0 || (m_lastTimestamp && timestamp > *m_lastTimestamp));
    m_lastTimestamp = increasing ? std::optional<int64_t>(timestamp) : std::nullopt;
    m_lastPacket = packet;
    ++m_returned;
}

// Called with each packet read. Only the first after a seek shows where it landed; any later one
// can only show that a picture past the resume point was left to decode.
void VideoDecoder::recordLanding() {
    auto* seeked = std::get_if<Seeked>(&m_resume);
    if (seeked == nullptr || seeked->landing == Landing::Unproven) return;
    const int64_t at = m_packet->pts != AV_NOPTS_VALUE ? m_packet->pts : m_packet->dts;
    const bool atOrBefore = at != AV_NOPTS_VALUE && at <= *m_lastTimestamp;
    const bool keyframe = (m_packet->flags & AV_PKT_FLAG_KEY) != 0;
    const bool first = seeked->landing == Landing::NoPacket;
    seeked->landing = atOrBefore && (keyframe || !first) ? Landing::NothingPast : Landing::Unproven;
}

void VideoDecoder::attachDevice() {
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
    if (const int err = av_hwdevice_ctx_init(ref.ref); err < 0) {
        if (err == AVERROR(ENOMEM)) throwOutOfMemory("VideoDecoder");
        throw GpuFailure("VideoDecoder: ffmpeg cannot decode on " + m_device.info().gpuName + ": " +
                         avErrorString(err));
    }

    m_videoFormatProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceVideoFormatPropertiesKHR>(
        vkGetInstanceProcAddr(m_device.instance(), "vkGetPhysicalDeviceVideoFormatPropertiesKHR"));
    if (m_videoFormatProperties == nullptr) {
        throw GpuFailure("VideoDecoder: " + m_device.info().gpuName +
                         " enables VK_KHR_video_queue but has no vkGetPhysicalDeviceVideoFormatPropertiesKHR");
    }
    m_codec->hw_device_ctx = ref.release();
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
    if (profiles == nullptr || vk->format[0] == VK_FORMAT_UNDEFINED || vk->format[1] != VK_FORMAT_UNDEFINED ||
        !FrameConverter::readsDeviceImage(m_device, context->sw_format, vk->format[0])) {
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
    codec->hw_frames_ctx = frames.release();
    return true;
}

// ffmpeg's format negotiation, called from inside its C frames, which nothing may throw
// through: whatever choose() throws is parked and rethrown once ffmpeg reports the error that
// follows a refusal of every format.
AVPixelFormat VideoDecoder::chooseFormat(AVCodecContext* codec, const AVPixelFormat* offered) {
    auto* self = static_cast<VideoDecoder*>(codec->opaque);
    try {
        return self->choose(codec, offered);
    } catch (const std::bad_alloc&) {
        self->m_parkedFailure = std::make_exception_ptr(std::runtime_error("VideoDecoder: out of memory"));
    } catch (...) {
        self->m_parkedFailure = std::current_exception();
    }
    return AV_PIX_FMT_NONE;
}

// The decision, made from the stream's parsed headers. ffmpeg re-runs this without a format
// whose setup failed, which is how a fault after the device's yes shows up here.
AVPixelFormat VideoDecoder::choose(AVCodecContext* codec, const AVPixelFormat* offered) {
    bool vulkanOffered = false;
    AVPixelFormat software = AV_PIX_FMT_NONE;
    for (const AVPixelFormat* format = offered; *format != AV_PIX_FMT_NONE; ++format) {
        if (*format == AV_PIX_FMT_VULKAN) {
            vulkanOffered = true;
        } else if ((av_pix_fmt_desc_get(*format)->flags & AV_PIX_FMT_FLAG_HWACCEL) == 0) {
            software = *format;
        }
    }

    if (codec->hw_device_ctx == nullptr) {
        if (software == AV_PIX_FMT_NONE) {
            throw WallpaperContentError("VideoDecoder: " + m_file + " decodes to no format software can hold");
        }
        return software;
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
            throw GpuFailure("VideoDecoder: " + m_device.info().gpuName + " accepted " + m_file +
                             " and no longer answers after its hardware decoder failed to initialise");
        }
        m_path = Path::SoftwareRefused;
    } else {
        // The codec offers no Vulkan output for this stream's pixel format.
        m_path = Path::SoftwareRefused;
    }
    // Never software on this context: it was opened for the device -- single-threaded, and for
    // AV1 a decoder with no software output of its own.
    m_reopenInSoftware = true;
    return AV_PIX_FMT_NONE;
}

int VideoDecoder::readVideoPacket() {
    for (;;) {
        const int err = av_read_frame(m_format, m_packet);
        if (err < 0) return err;
        if (m_packet->stream_index == m_stream) {
            if (m_packetsRead) m_packet->opaque = reinterpret_cast<void*>(static_cast<uintptr_t>(++*m_packetsRead));
            return 0;
        }
        av_packet_unref(m_packet);
    }
}

// What an ffmpeg error during decoding means. Returns only when the hardware path failed on a
// device that still answers, having arranged for the stream to be decoded in software instead.
void VideoDecoder::failDecode(const char* what, int err) {
    if (m_parkedFailure) std::rethrow_exception(std::exchange(m_parkedFailure, nullptr));
    const std::string context = std::string("VideoDecoder: ") + what + " on " + m_file;
    const bool hardware = m_awaitingHardwareSetup || m_path == Path::Hardware;
    if (err == AVERROR_EXTERNAL && hardware) {
        if (!deviceHealthy()) throw GpuFailure(context + ": the hardware decoder failed and the device no longer answers");
        m_path = Path::SoftwareRefused;
        m_reopenInSoftware = true;
        return;
    }
    throwAvFailure(context, err);
}

std::optional<DecodedFrame> VideoDecoder::next() {
    m_device.assertGpuThread("VideoDecoder::next");
    for (;;) {
        // What the format callback decided during the last ffmpeg call comes before what that
        // call returned: a refusal of every format reads as bad input or as the end otherwise.
        if (m_reopenInSoftware) reopenInSoftware();
        int err = avcodec_receive_frame(m_codec, m_frame);
        if (m_reopenInSoftware) continue;
        if (m_parkedFailure) failDecode("avcodec_receive_frame", err);
        if (err == 0) {
            m_awaitingHardwareSetup = false;
            if (m_frame->format == AV_PIX_FMT_VULKAN) m_path = Path::Hardware;
            const int64_t timestamp = m_frame->best_effort_timestamp;
            const std::optional<uint64_t> packet = packetNumber(*m_frame);
            if (!admit(timestamp, packet)) {
                av_frame_unref(m_frame);
                continue;
            }
            AVFrame* out = av_frame_alloc();
            if (out == nullptr) throwOutOfMemory("VideoDecoder");
            av_frame_move_ref(out, m_frame);
            DecodedFrame frame(m_device, out);
            recordReturned(timestamp, packet);
            return frame;
        }
        // Nothing more can be sent after the flush, so a decoder that still asks for input has
        // no more pictures to give.
        if (err == AVERROR_EOF || (err == AVERROR(EAGAIN) && m_flushed)) {
            const auto* seeked = std::get_if<Seeked>(&m_resume);
            if ((seeked != nullptr && seeked->landing != Landing::NothingPast) ||
                std::holds_alternative<DropThroughPacket>(m_resume)) {
                m_reopenInSoftware = true;
                continue;
            }
            if (std::holds_alternative<DropCount>(m_resume)) {
                throw WallpaperContentError("VideoDecoder: " + m_file +
                                            " ended before software decoding reached the pictures already returned");
            }
            return std::nullopt;
        }
        if (isDamagedPacket(err)) continue;
        if (err != AVERROR(EAGAIN)) {
            failDecode("avcodec_receive_frame", err);
            continue;
        }

        // A packet ffmpeg asked to see again is still held; otherwise the next one is read. A
        // container that cannot be read is fatal, unlike a packet that cannot be decoded: a
        // demuxer reporting damage is not guaranteed to have moved past it.
        const bool resend = m_packet->data != nullptr;
        if (!resend) {
            err = readVideoPacket();
            if (err == AVERROR_EOF) {
                m_flushed = true;
            } else if (err < 0) {
                throwAvFailure("VideoDecoder: reading " + m_file, err);
            } else {
                recordLanding();
            }
        }
        err = avcodec_send_packet(m_codec, m_flushed ? nullptr : m_packet);
        // EAGAIN is "read the output first, then send this one again", and ffmpeg promises the
        // receive that follows will not ask for input; a second EAGAIN for the same packet is
        // that promise broken, and failing is better than spinning on it.
        if (err == AVERROR(EAGAIN) && !resend) continue;
        av_packet_unref(m_packet);
        if (m_reopenInSoftware) continue;
        if (m_parkedFailure) failDecode("avcodec_send_packet", err);
        if (err == AVERROR(EAGAIN)) {
            throw std::logic_error("VideoDecoder: ffmpeg refused the same packet twice without giving a frame");
        }
        if (err < 0 && !isDamagedPacket(err)) failDecode("avcodec_send_packet", err);
    }
}

}
