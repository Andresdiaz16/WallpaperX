// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "../vulkan/device.h"

extern "C" {
#include <libavutil/pixfmt.h>
}

#include <array>
#include <cstdint>
#include <exception>
#include <functional>
#include <optional>
#include <string>

struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;

namespace wallpaperx {

// What a conversion to RGB needs of a decoded picture beyond its pixels. Read off the frame
// ffmpeg produced, never assumed: the matrix and range decide the conversion, and a wrong
// default is a visibly wrong picture. UNSPECIFIED is common and is the converter's to resolve.
struct FrameColour {
    AVColorSpace matrix = AVCOL_SPC_UNSPECIFIED;
    AVColorRange range = AVCOL_RANGE_UNSPECIFIED;
    AVColorPrimaries primaries = AVCOL_PRI_UNSPECIFIED;
    AVColorTransferCharacteristic transfer = AVCOL_TRC_UNSPECIFIED;
    AVChromaLocation chromaLocation = AVCHROMA_LOC_UNSPECIFIED;
};

// One decoded picture, held until destroyed: an image on the device when the hardware decoded
// it, planes in host memory when software did. Both carry the coded extent -- what the codec
// rounds the picture up to -- and the visible rectangle inside it, so a converter asks the
// same questions of either.
//
// GPU thread only, destruction included: the destructor may have to ask the Device whether the
// GPU has finished reading the image, and that is asked nowhere else. Must be destroyed before
// the Device that decoded it. Destroying the last frame of a decoder that is already gone tears
// the decoder's image pool down, which waits without bound for every batch that read those
// images; the destructor leaks the pool instead when a batch of this renderer's has not
// finished. Using a moved-from frame throws std::logic_error.
class DecodedFrame {
public:
    ~DecodedFrame();
    DecodedFrame(DecodedFrame&& other) noexcept;
    DecodedFrame& operator=(DecodedFrame&& other) noexcept;
    DecodedFrame(const DecodedFrame&) = delete;
    DecodedFrame& operator=(const DecodedFrame&) = delete;

    enum class Storage { DeviceImage, HostPlanes };
    Storage storage() const;

    // The planar Y'CbCr layout of the pixels, as ffmpeg names it, and its significant bits: a
    // 10-bit picture sits in 16-bit samples, so the depth is not readable from the format's
    // sample size.
    AVPixelFormat pixelFormat() const;
    uint32_t bitDepth() const;
    VkExtent2D extent() const;
    VkRect2D visible() const;
    const FrameColour& colour() const { return m_colour; }

    struct DeviceImage {
        VkImage image = VK_NULL_HANDLE;
        // A multi-planar format; the converter copies its planes out through plane aspects.
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL;
        VkImageUsageFlags usage = 0;
        // The layout the decoder left the image in. A batch that reads it must return it there:
        // the decoder's next barrier on this image names this layout as the old one.
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };
    // Throws std::logic_error on a host frame.
    DeviceImage deviceImage() const;

    // Submits a batch that reads the device image, ordered after the decoder's write of it and
    // before the decoder's next write into it. The frame's timeline semaphore is the only
    // ordering the decoder offers, and this is the one place its protocol lives: wait at the
    // frame's value, signal one higher, and record the new value only once the submit has
    // succeeded -- a recorded value nothing signals hangs the decoder's teardown forever.
    //
    // `readStage` is where the batch first reads the image. `scope` is the caller's other
    // ordering, kept intact. Throws what Device::submit throws, and std::logic_error on a host
    // frame. GPU thread only.
    Device::SubmitTicket submit(const std::function<void(VkCommandBuffer)>& record, VkPipelineStageFlags readStage,
                                Device::SubmitScope scope = {});

    struct HostPlanes {
        uint32_t count = 0;
        std::array<const uint8_t*, 4> data{};
        // Bytes per row of each plane; ffmpeg's linesize, which is negative for a bottom-up picture.
        std::array<int, 4> stride{};
    };
    // Valid for as long as this frame lives. Throws std::logic_error on a device frame.
    HostPlanes hostPlanes() const;

private:
    friend class VideoDecoder;
    DecodedFrame(Device& device, AVFrame* frame);
    const AVFrame& frame() const;
    void release() noexcept;

    Device* m_device = nullptr;
    AVFrame* m_frame = nullptr;
    FrameColour m_colour;
};

// Demuxes one file and decodes its video stream on this GPU's hardware decoder when the device
// can, and in software when it cannot -- which is a property of the device and the stream, not
// a failure. Every entry point is GPU thread only: ffmpeg submits to the decode queue and,
// preparing images, to the graphics queue too, and neither is free-threaded.
//
// A refusal and a fault are told apart deliberately rather than by ffmpeg's own fallback,
// which retries in software after any failure. The device is asked whether it accepts the
// stream -- codec, profile, bit depth, coded size, the image it would decode into -- and a no
// selects software. When ffmpeg's hardware path fails after that yes, which it can for reasons
// of its own, the device is asked whether it still answers: one that does not is a GpuFailure,
// one that does means the stream is decoded in software from its first packet.
//
// The decoder ffmpeg is asked for is the one that can use this device, which for AV1 is a
// hardware-only shim, and it is opened single-threaded so ffmpeg never touches a queue off
// the GPU thread. Neither suits software, so a refusal reopens the stream from its first
// packet on ffmpeg's default decoder with its default threads.
//
// Frames handed out may outlive this decoder but not the Device. Destroying this decoder frees
// its image pool once no frame is out, and ffmpeg waits without bound there twice over: on each
// image's semaphore, which covers this renderer's reads of it, and on its own decode work. The
// Device's bounded wait guards only the first -- a read batch of this renderer's that has not
// finished makes the destructor leak the codec context rather than wait. The second has no
// guard here: a device that is declared lost returns from ffmpeg's waits with an error, but one
// that merely stops does not, and that destructor hangs inside ffmpeg.
class VideoDecoder {
public:
    // Where frames come from. The software states say why hardware was not used.
    enum class Path {
        Undecided,          // no frame decoded yet, and hardware is still possible
        Hardware,
        SoftwareNoDecoder,  // the device has no video decode queue at all (Lavapipe)
        SoftwareCodec,      // the device, or ffmpeg, decodes but not this codec
        // The device decodes this codec but not this stream -- profile, depth, size -- or not
        // into one multi-planar image FrameConverter::readsDeviceImage() accepts and can copy
        // from under this profile; or ffmpeg's hardware path failed on a device that still answers.
        SoftwareRefused,
    };

    // Opens `file` and readies the decoder; the path is decided when the first frame is decoded,
    // since the stream's own headers are what the device is asked about. Throws
    // WallpaperContentError for a file that cannot be opened or has no decodable video stream,
    // GpuFailure when ffmpeg cannot adopt this device, and std::runtime_error when out of memory.
    VideoDecoder(Device& device, const std::string& file);
    ~VideoDecoder();

    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    // The next picture in decode order, or nothing once the stream is exhausted. A packet the
    // decoder cannot parse is skipped, not reported. Throws GpuFailure for a hardware fault,
    // WallpaperContentError for a container that cannot be read or a stream that cannot be
    // decoded, and std::runtime_error when out of memory. After a throw the decoder is not
    // usable.
    std::optional<DecodedFrame> next();

    Path path() const { return m_path; }

private:
    static AVPixelFormat chooseFormat(AVCodecContext* codec, const AVPixelFormat* offered);
    AVPixelFormat choose(AVCodecContext* codec, const AVPixelFormat* offered);
    bool attachHardwareFrames(AVCodecContext* codec);
    void openContainer();
    void openDecoder();
    void closeDecoder() noexcept;
    void reopenInSoftware();
    void attachDevice();
    bool deviceHealthy();
    int readVideoPacket();
    void failDecode(const char* what, int err);

    Device& m_device;
    std::string m_file;
    AVFormatContext* m_format = nullptr;
    AVCodecContext* m_codec = nullptr;
    AVPacket* m_packet = nullptr;
    AVFrame* m_frame = nullptr;
    PFN_vkGetPhysicalDeviceVideoFormatPropertiesKHR m_videoFormatProperties = nullptr;
    int m_stream = -1;
    bool m_flushed = false;
    Path m_path = Path::Undecided;
    // Set when the device accepted the stream and ffmpeg was told to decode on it, cleared when a
    // frame arrives. A format negotiation that comes back without Vulkan while this is set is
    // ffmpeg retreating from a hardware setup that failed after the device's yes.
    bool m_awaitingHardwareSetup = false;
    // The path was decided as software on a decoder opened for hardware.
    bool m_reopenInSoftware = false;
    // A failure decided inside the format callback, which cannot throw through ffmpeg's frames;
    // rethrown when ffmpeg reports the error that follows.
    std::exception_ptr m_parkedFailure;
};

}
