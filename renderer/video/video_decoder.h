// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "../vulkan/device.h"
#include "ffmpeg_file.h"
#include "picture_skip.h"
#include "stream_position.h"

extern "C" {
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

struct AVBufferRef;
struct AVCodecContext;
struct AVDictionary;
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
// same questions of either. A device picture sits at its image's origin, and the image can be
// larger: AV1 keeps its images across a change to a smaller size.
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

    // When this picture is shown, on the stream's own timeline, whose origin is arbitrary: only
    // differences between pictures of one stream mean anything. Empty when the stream carries no
    // timestamp for it.
    std::optional<std::chrono::nanoseconds> presentationTime() const { return m_presentation; }
    // How long it is shown: its own duration, else the stream's nominal frame interval. Empty
    // when neither is known.
    std::optional<std::chrono::nanoseconds> duration() const { return m_duration; }
    // Where a decoder opened on the same file can start at this picture. Empty when it could not be
    // found again: it has no timestamp, or the pictures returned before it did not rise in time.
    std::optional<StreamPosition> position() const { return m_position; }
    // What the decoder was leaving out when this picture's packet was sent. With frame threads that
    // can be a level asked for several pictures before this one was returned.
    PictureSkip skip() const { return m_skip; }
    // The highest level that would still decode this picture, judged by its type alone. A B picture
    // is taken as one no other picture references, which in a B pyramid it need not be.
    PictureSkip keptThrough() const;
    // Video packets read since the picture before it was returned, at least 1: about how far on from
    // that picture it lies in the stream, skipped pictures included.
    uint32_t readSince() const { return m_readSince; }

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
    std::optional<std::chrono::nanoseconds> m_presentation;
    std::optional<std::chrono::nanoseconds> m_duration;
    std::optional<StreamPosition> m_position;
    PictureSkip m_skip = PictureSkip::Nothing;
    uint32_t m_readSince = 1;
};

// Demuxes one file and decodes its video stream on this GPU's hardware decoder when the device
// can, and in software when it cannot -- which is a property of the device and the stream, not
// a failure. Every entry point is GPU thread only: ffmpeg submits to the decode queue and,
// preparing images, to the graphics queue too, and neither is free-threaded.
//
// A refusal and a fault are told apart deliberately rather than by ffmpeg's own fallback,
// which retries in software after any failure. The device is asked whether it accepts the
// stream -- codec, profile, bit depth, coded size, the image it would decode into -- and a no
// selects software. So does a picture its images cannot hold or smaller than the device decodes,
// checked before it is decoded; DecodedFrame says why the two can differ. When
// ffmpeg's hardware path fails after the device's yes, which it can for reasons of its own, the
// device is asked whether it still answers: one that does not is a GpuFailure, one that does
// means decoding continues in software, as next() describes.
//
// The decoder ffmpeg is asked for is the one that can use this device, which for AV1 is a
// hardware-only shim, and it is opened single-threaded so ffmpeg never touches a queue off
// the GPU thread. Neither suits software, so a refusal reopens the stream on ffmpeg's default
// decoder with its default threads.
//
// Frames handed out may outlive this decoder but not the Device. A size or format change
// mid-stream leaves an image pool no picture uses, and a switch to software leaves the hardware
// decoder it replaces; next() frees each once this renderer's last read of it has finished, and
// never waits for those reads. That free still enters ffmpeg's unbounded wait on its own decode
// work, which a device that stalls without being declared lost never returns from -- unguarded,
// because the only bounded question here is itself a wait and the playback path may make none.
//
// Destroying this decoder frees the rest once no frame is out, where ffmpeg waits without bound
// twice over: on each image's semaphore, which covers this renderer's reads of it, and on its own
// decode work. Neither is entered blind -- the Device answers the first, and an empty batch
// through it answers the second before a hardware codec holding decode images is freed -- and
// either answering no makes the destructor leak the codec context and every pool rather than
// wait. What that buys is a device already gone when nothing of this renderer's was left pending,
// which is where the first question answers yes without asking the GPU anything. It does not
// cover a decode queue wedged while the rest of the device still answers: the probe goes through
// the graphics queue, so it comes back yes and ffmpeg's wait is entered anyway.
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
    // GpuFailure when ffmpeg cannot adopt this device and the device has stopped answering, and
    // ResourceExhausted when out of memory or file descriptors. A device that still answers
    // selects software instead: ffmpeg failing to adopt it is a property of that pairing, not a
    // fault.
    //
    // With `startAt`, a position a decoder on this same file handed out, the first picture next()
    // returns is the one there: it seeks to a keyframe at or before it and drops the pictures up to
    // it, checking where the seek landed exactly as a software restart does. A position at the
    // stream's first picture starts there. A position from another file is not detected and lands
    // wherever its timestamp falls in this one; one past its last picture starts at its first.
    //
    // The drop is bounded by a catch-up budget of pictures. From a keyframe more than that before the
    // position, the first picture is the keyframe's instead, and a keyframe at the stream's start
    // makes it a plain first pass: a file with keyframes far apart resumes up to one keyframe
    // interval early. A container whose seek fails is read from its first packet when the position
    // is within the budget of the stream's start, and starts at the first picture otherwise.
    //
    // On the hardware decoder that keyframe is the nearest reset point, as resetsDecoder() defines
    // one, found by scanning packets back from the position without decoding them. The file's first
    // packet counts as one. The scan stops about one window past the budget, so a reset point further
    // back is never found and the start is the stream's first picture. In software, a seek that has
    // to retry further back than the budget starts at the stream's first picture too.
    VideoDecoder(Device& device, const std::string& file, std::optional<StreamPosition> startAt = std::nullopt);
    ~VideoDecoder();

    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    // The next picture in output order, or nothing once the stream is exhausted. A packet the
    // decoder cannot parse is skipped, not reported. Pictures `skip` names are left out, except
    // while a start or resume drops its way to a picture, which it matches against pictures
    // returned unskipped.
    //
    // When hardware decoding fails part way through, software continues from the picture after
    // the last one returned; none is returned twice. While every picture returned has carried a
    // timestamp above the one before, that timestamp is what software resumes after: it decodes
    // from a keyframe at or before it and drops the pictures up to it, so the call that meets the
    // failure costs about a keyframe interval wherever in the file it falls. A seek that cannot
    // be shown to have landed at or before that picture -- a transport stream's can land past it --
    // is retried further back. A container that cannot seek is decoded from its first packet,
    // exactly but at a cost that grows with the position.
    //
    // Pictures without timestamps -- a raw bitstream -- or whose timestamps did not increase are
    // matched by the packet each was decoded from instead: software decodes from the first packet
    // and drops pictures through the one decoded from the packet the last returned picture came
    // from. That does not bound the call. If software never decodes that packet into a picture, it
    // decodes from the first packet again and drops as many pictures as were returned, which is not
    // exact when the two decoders emit different pictures; a stream that ends before that many
    // throws WallpaperContentError. A count kept while pictures were skipped says nothing, so software
    // then starts again at the stream's first picture. After a start part way in, that count does not reach back to
    // the first packet, so software starts again at the start's position instead and the pictures
    // returned since are returned again.
    //
    // Throws GpuFailure for a hardware fault, WallpaperContentError for a container that cannot be
    // read or a stream that cannot be decoded, and std::runtime_error when out of memory. After a
    // throw the decoder is not usable.
    std::optional<DecodedFrame> next(PictureSkip skip = PictureSkip::Nothing);

    // next() that gives up for now once it has decoded and dropped `dropLimit` pictures on the way to
    // the one it returns -- what a start part way in and a software resume both decode through. It
    // drops at least one before giving up. Dropping then: nothing is lost, and the next call of either
    // form carries on from there.
    struct Dropping {};
    std::variant<std::optional<DecodedFrame>, Dropping> next(uint32_t dropLimit, PictureSkip skip = PictureSkip::Nothing);

    // Starts the stream again: the next picture is its first, and a later hardware failure resumes
    // among the pictures returned since, never earlier ones. The path is kept, so a decoder that fell
    // back to software stays there. The codec is flushed, not reopened, and the container rewound;
    // the rewind reopens the container only when no seek is shown to reach its first video packet,
    // and then reads the file this decoder opened even if its path has since been deleted or renamed
    // over. The file must not be rewritten in place while this decoder holds it: nothing can detect
    // that, so replacing a wallpaper takes a new decoder or pipeline. Throws as next() does.
    void restart();

    // Whether the pictures since this decoder opened began part way in, at or before the constructor's
    // `startAt`, rather than at the stream's first picture. False again after restart(), and once a
    // start falls back to the first picture.
    bool startedMidStream() const { return m_startedAfter.has_value(); }

    // Where, on the timeline presentationTime() uses, the latest-ending video packet read so far that is
    // meant to be shown ends: once next() has run out, the stream's end, which a skipped last picture
    // leaves no other way to know. Nothing before a packet with a timestamp.
    std::optional<std::chrono::nanoseconds> readThrough() const { return m_readThrough; }

    Path path() const { return m_path; }

    // The highest level next() applies, decided by the codec; one asked above it is applied as this.
    // H.264 and HEVC: NonKey in software, Unreferenced while the stream is or may be decoded on the
    // device, where FFmpeg 9.0.2's Vulkan decode, given a reference picture skipped, binds a stand-in
    // sharing another picture's image and can destroy an image view the GPU still reads. MPEG-1, -2 and
    // -4 Part 2: Unreferenced, since resetsDecoder() reads none of their reset points to come back down
    // from deeper at. Nothing for every other codec: AV1 and VP9 ignore a level changed after opening.
    PictureSkip deepestSkip() const;

private:
    std::variant<std::optional<DecodedFrame>, Dropping> decode(std::optional<uint32_t> dropLimit, PictureSkip skip);
    // ffmpeg's format negotiation. A software codec's frame threads call it off the GPU thread, so on
    // that path it reads nothing of the decoder and writes only m_softwareFormatRefused.
    static AVPixelFormat chooseFormat(AVCodecContext* codec, const AVPixelFormat* offered);
    AVPixelFormat choose(AVCodecContext* codec, const AVPixelFormat* offered);
    bool attachHardwareFrames(AVCodecContext* codec);
    static int allocatePicture(AVCodecContext* codec, AVFrame* frame, int flags);
    bool holds(const AVBufferRef& frames, int width, int height) const;
    void releaseIdleFramesContexts() noexcept;
    void releaseRetiredCodec() noexcept;
    void openContainer();
    void openDecoder();
    void closeDecoder() noexcept;
    void retireDecoder() noexcept;
    void closeAll() noexcept;
    void reopenInSoftware();
    void seekAgain();
    // Positions the container for a decode that resumes after m_returned, and sets what it drops.
    // Software only: the hardware decoder seeks only in startAt().
    void seekToResume();
    void startAt(StreamPosition position);
    void startAtResetPoint(int64_t resume);
    // Gives up a start part way in: the next picture is the stream's first, as a plain open's is.
    void startAtFirstPicture();
    // Whether decoding forward `distance`, in stream time base, stays within kCatchUpPictures.
    bool withinCatchUp(int64_t distance) const;
    // How far back the search after one `back` before a position aims: a second, then twice as far.
    int64_t nextSeekBack(int64_t back) const;
    // A packet as read, which is how a later read recognises it.
    struct PacketMark {
        std::optional<int64_t> position;  // unset when the demuxer gave none
        int64_t dts;
        int64_t pts;
        int size;
        static PacketMark of(const AVPacket& packet);
        bool matches(const AVPacket& packet) const;
    };
    struct ResetPoint {
        PacketMark mark;
        int64_t time;
        bool fileStart;
    };
    // What one window of startAtResetPoint()'s search read: its first packet and its last reset point.
    struct Window {
        std::optional<PacketMark> first;
        std::optional<ResetPoint> last;
    };
    Window scanWindow(int64_t resume, const std::optional<PacketMark>& newer, bool fromStart, bool stopAtFirst);
    bool seekToPacket(const PacketMark& mark, std::optional<int64_t> target);
    bool skipToPacket(const PacketMark& mark);
    bool decodesOnDevice() const;
    int64_t streamStart() const;
    void rewind();
    bool seekToFirstPacket();
    bool seekLandsOnFirstPacket(int64_t target, int flags);
    bool admit(int64_t timestamp, std::optional<uint64_t> packet);
    void recordReturned(int64_t timestamp, std::optional<uint64_t> packet);
    // Whether the packet just read comes after a seek and before the first keyframe since; it is
    // skipped, never decoded.
    bool beforeLandingKeyframe() const;
    void recordLanding();
    // True means decode on the device. False means the path was decided as software, because
    // ffmpeg cannot decode on a device that still answers.
    bool attachDevice();
    bool deviceHealthy();
    // deviceHealthy() from a path that cannot throw, where any answer but a clean yes has to keep
    // ffmpeg's unbounded free from being entered.
    bool decodeWorkCanDrain() noexcept;
    int readVideoPacket();
    void numberPacket();
    void failDecode(const char* what, int err);

    Device& m_device;
    std::string m_file;
    // The file, opened once, and the I/O context that reads it. Every container reads through them
    // and closing one leaves them open, so a reopened container reads the same file after its path
    // is deleted or renamed over. ffmpeg is given the path only as a hint to probing and as text in
    // its messages.
    ffmpeg_file::Reader m_reader;
    AVFormatContext* m_format = nullptr;
    AVCodecContext* m_codec = nullptr;
    AVPacket* m_packet = nullptr;
    AVFrame* m_frame = nullptr;
    // Every hardware frames context installed on m_codec, referenced until nothing else does. A
    // stream that changes size or format mid-stream makes ffmpeg drop its own reference, and it then
    // frees each old picture's frame before the state that waits on that frame's semaphore: without
    // this reference the last one destroys the semaphore under that wait.
    struct HeldFrames {
        AVBufferRef* ref;
        // The smallest picture the device decodes under the profile these images were made for.
        VkExtent2D minCoded;
        // The last batch that could read its images, taken once this reference is the only one.
        std::optional<Device::SubmitTicket> lastReader;
    };
    std::vector<HeldFrames> m_framesContexts;
    // A hardware codec given up for software, freed once its last reader has completed: freeing it
    // waits on ffmpeg's decode batches, which can be ordered after this renderer's reads of their images.
    struct RetiredCodec {
        AVCodecContext* codec;
        Device::SubmitTicket lastReader;
    };
    std::optional<RetiredCodec> m_retiredCodec;
    PFN_vkGetPhysicalDeviceVideoFormatPropertiesKHR m_videoFormatProperties = nullptr;
    PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR m_videoCapabilities = nullptr;
    int m_stream = -1;
    // What a software restart resumes after, since the stream was opened or restarted: how many
    // pictures next() has returned; the last one's timestamp for as long as every one returned had a
    // timestamp above the one before; and the number of the packet it was decoded from, when that
    // packet was numbered. A decoder opened at a position starts as though every picture before it
    // had been returned: count 0, and the timestamp one below that picture's. Only a new pass, or a
    // resume that lost its place, zeroes the count: count 0 is how a start that has shown nothing is
    // told apart. `skipped` is a packet
    // sent with pictures being skipped since the count began: the count then no longer says which
    // picture was last returned.
    struct Returned {
        uint64_t count = 0;
        std::optional<int64_t> lastTimestamp;
        std::optional<uint64_t> lastPacket;
        bool skipped = false;
    };
    Returned m_returned;
    // The level the packets now sent are skipped at, which the one asked for reaches downwards onto
    // pictures that can reference skipped ones only at a packet resetsDecoder() accepts. Nothing after a
    // flush: the decoder then holds no picture a later one could miss.
    PictureSkip m_skipping = PictureSkip::Nothing;
    std::optional<std::chrono::nanoseconds> m_readThrough;
    // Video packets read since a picture was last returned.
    uint32_t m_readSinceReturned = 0;
    // Video packets read since the container was opened or rewound, which is each one's number.
    // Unset after a seek: numbers counted from there would not name the same packets as another
    // decoder's.
    std::optional<uint64_t> m_packetsRead = 0;
    // Packet 1 as last read, which is how a rewind recognises it.
    std::optional<PacketMark> m_firstPacket;
    // What a restarted software decode, or a decoder opened at a position, drops before it returns a
    // picture, in stream time base. Each resumes after m_returned's timestamp or packet, which cannot
    // change while it holds: none returns a picture.
    //
    // Seeked is a seek `back` before the resume point, not yet shown to have landed at or before
    // it. Packets before the first keyframe after it are skipped. The rest show the landing when
    // that keyframe is at or before the resume point -- no picture before a keyframe in decode
    // order is shown after it -- and none since is timestamped past it: a decode that then ends
    // with no picture has none left to return. NoPacket is until that keyframe is decoded.
    enum class Landing { NoPacket, NothingPast, Unproven };
    struct Seeked {
        int64_t back;
        Landing landing = Landing::NoPacket;
    };
    //
    // DropThrough ends at the first picture past the resume point. Unguarded: a freshly opened
    // software decoder that has to guess a stream's reordering depth could emit a picture at or
    // before the resume point after a later one, and it would be returned again. Not reproducible
    // on this machine.
    struct DropThrough {};
    struct DropThroughPacket {};
    struct DropCount {
        uint64_t remaining;  // never 0
    };
    std::variant<std::monostate, Seeked, DropThrough, DropThroughPacket, DropCount> m_resume;
    bool m_flushed = false;
    Path m_path = Path::Undecided;
    // Set when the device accepted the stream and ffmpeg was told to decode on it, cleared when a
    // frame arrives. A format negotiation that comes back without Vulkan while this is set is
    // ffmpeg retreating from a hardware setup that failed after the device's yes.
    bool m_awaitingHardwareSetup = false;
    // What next() must do before it decodes again, decided inside an ffmpeg call. Ordered: a reopen
    // in software also seeks, so it is never lowered to a seek.
    enum class Pending {
        Nothing,
        // A resume did not reach its picture: its seek was not shown to land at or before it, or a
        // pass matching by packet ended without meeting that packet. The codec is flushed, not
        // replaced, so a hardware decode stays on the hardware.
        SeekAgain,
        // The path was decided as software on a decoder opened for hardware.
        ReopenInSoftware,
    };
    Pending m_pending = Pending::Nothing;
    void require(Pending pending) { m_pending = std::max(m_pending, pending); }
    // While the pictures since the open began part way in: the timestamp the start dropped through,
    // which a software resume that has lost m_returned's timestamp starts from again.
    std::optional<int64_t> m_startedAfter;
    // A failure decided inside the format callback, which cannot throw through ffmpeg's frames;
    // rethrown when ffmpeg reports the error that follows. Hardware only, whose codec is
    // single-threaded and so calls back on the GPU thread.
    std::exception_ptr m_parkedFailure;
    // A software codec's stream offered no plain format. Set by the format callback on whichever
    // frame thread runs it; decode() turns it into the file's failure. Per codec: openDecoder() clears it.
    std::atomic<bool> m_softwareFormatRefused{false};
};

}
