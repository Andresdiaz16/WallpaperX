// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Picture identity for the video probes. The counter fixtures draw their own frame number into
// every picture: an 8x6 grid of cells, where the top-left 4x3 hold bits 0-11 (bright is 1) and the
// 4x3 to their right hold the same bits inverted. A cell is read at its centre, so the grid scales
// with the picture, and a reading is valid only when every bit and its inverse disagree -- which a
// black target, a blend of two pictures or a misplaced blit cannot satisfy.
//
// The expected sequence is decodeCounterReference(): libavcodec driven directly, never
// VideoDecoder, whose decisions are made from the timestamps and packet numbers a check must not
// grade it with.

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace wallpaperx::testing {

inline constexpr int kCounterBits = 12;

// `brightness(column, row)` is 0-255 at that cell's centre. -1 for an unreadable picture.
inline int readCounter(const std::function<int(int, int)>& brightness, int threshold, int margin) {
    int value = 0;
    for (int bit = 0; bit < kCounterBits; ++bit) {
        const int plain = brightness(bit % 4, bit / 4);
        const int inverted = brightness(4 + bit % 4, bit / 4);
        if (std::abs(plain - threshold) < margin || std::abs(inverted - threshold) < margin) return -1;
        const bool set = plain > threshold;
        if (set == (inverted > threshold)) return -1;
        if (set) value |= 1 << bit;
    }
    return value;
}

// A tightly-packed BGRA readback. Full-range RGB, so the fixture's dark and bright codes are
// near 0 and 255.
inline int counterFromBgra(const std::vector<unsigned char>& pixels, uint32_t width, uint32_t height) {
    if (pixels.size() < static_cast<size_t>(width) * height * 4) return -1;
    return readCounter(
        [&](int column, int row) {
            const size_t x = (2 * static_cast<size_t>(column) + 1) * width / 16;
            const size_t y = (2 * static_cast<size_t>(row) + 1) * height / 12;
            const size_t i = (y * width + x) * 4;
            return (pixels[i] + pixels[i + 1] + pixels[i + 2]) / 3;
        },
        128, 60);
}

struct CounterReference {
    std::vector<int> ids;  // per picture, in output order; -1 where unreadable
    std::vector<uint32_t> widths;  // each distinct picture width, in order of first appearance
    struct Size {
        int width;
        int height;
    };
    std::vector<Size> sizes;  // per picture, in output order, with the stream's cropping applied
    int bidirectional = 0;  // pictures coded as B
    int untimestamped = 0;
    int nonIncreasing = 0;  // timestamped pictures not above the timestamped picture before
    // The most pictures from one keyframe up to the next, or to the end.
    int longestKeyframeGap = 0;

    bool distinctAndReadable() const {
        std::vector<int> sorted = ids;
        std::sort(sorted.begin(), sorted.end());
        return !sorted.empty() && sorted.front() >= 0 && std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end();
    }
};

inline std::optional<CounterReference> decodeCounterReference(const std::string& file, std::string& why) {
    AVFormatContext* format = nullptr;
    AVCodecContext* codec = nullptr;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    struct Free {
        AVFormatContext*& format;
        AVCodecContext*& codec;
        AVPacket*& packet;
        AVFrame*& frame;
        ~Free() {
            av_frame_free(&frame);
            av_packet_free(&packet);
            avcodec_free_context(&codec);
            avformat_close_input(&format);
        }
    } guard{format, codec, packet, frame};

    if (packet == nullptr || frame == nullptr) {
        why = "out of memory";
        return std::nullopt;
    }
    if (avformat_open_input(&format, file.c_str(), nullptr, nullptr) < 0 ||
        avformat_find_stream_info(format, nullptr) < 0) {
        why = "the reference decode cannot open " + file;
        return std::nullopt;
    }
    const int stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const AVCodec* decoder = stream >= 0 ? avcodec_find_decoder(format->streams[stream]->codecpar->codec_id) : nullptr;
    codec = decoder != nullptr ? avcodec_alloc_context3(decoder) : nullptr;
    if (codec == nullptr || avcodec_parameters_to_context(codec, format->streams[stream]->codecpar) < 0 ||
        avcodec_open2(codec, decoder, nullptr) < 0) {
        why = "the reference decode cannot open a decoder for " + file;
        return std::nullopt;
    }

    CounterReference reference;
    std::optional<int64_t> lastTimestamp;
    int sinceKeyframe = 0;
    auto take = [&]() -> bool {
        for (;;) {
            const int err = avcodec_receive_frame(codec, frame);
            if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) return true;
            if (err < 0) {
                why = "the reference decode failed on " + file;
                return false;
            }
            if (frame->format != AV_PIX_FMT_YUV420P && frame->format != AV_PIX_FMT_YUVJ420P) {
                why = "the reference decode of " + file + " is not 8-bit 4:2:0";
                return false;
            }
            const int width = frame->width;
            const int height = frame->height;
            reference.ids.push_back(readCounter(
                [&](int column, int row) {
                    const int x = (2 * column + 1) * width / 16;
                    const int y = (2 * row + 1) * height / 12;
                    return static_cast<int>(frame->data[0][static_cast<size_t>(y) * frame->linesize[0] + x]);
                },
                125, 50));
            reference.sizes.push_back({width, height});
            if (std::find(reference.widths.begin(), reference.widths.end(), static_cast<uint32_t>(width)) ==
                reference.widths.end()) {
                reference.widths.push_back(static_cast<uint32_t>(width));
            }
            if (frame->pict_type == AV_PICTURE_TYPE_B) ++reference.bidirectional;
            const int64_t timestamp = frame->best_effort_timestamp;
            if (timestamp == AV_NOPTS_VALUE) {
                ++reference.untimestamped;
            } else {
                if (lastTimestamp && timestamp <= *lastTimestamp) ++reference.nonIncreasing;
                lastTimestamp = timestamp;
            }
            if ((frame->flags & AV_FRAME_FLAG_KEY) != 0 && reference.ids.size() > 1) {
                reference.longestKeyframeGap = std::max(reference.longestKeyframeGap, sinceKeyframe);
                sinceKeyframe = 0;
            }
            ++sinceKeyframe;
            av_frame_unref(frame);
        }
    };
    while (av_read_frame(format, packet) >= 0) {
        const bool video = packet->stream_index == stream;
        if (video) avcodec_send_packet(codec, packet);
        av_packet_unref(packet);
        if (video && !take()) return std::nullopt;
    }
    avcodec_send_packet(codec, nullptr);
    if (!take()) return std::nullopt;
    reference.longestKeyframeGap = std::max(reference.longestKeyframeGap, sinceKeyframe);
    return reference;
}

}  // namespace wallpaperx::testing
