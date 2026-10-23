// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// A NAL length-size change built for the video probes: counter_ps.h264 rewritten as FLV whose avcC
// declares one length size for pictures 0-119 and another, as NEW_EXTRADATA, from 120.

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace wallpaperx::testing {

// NAL units of an Annex B access unit, start codes and trailing zero bytes removed.
inline std::vector<std::vector<uint8_t>> annexBUnits(const uint8_t* data, size_t size) {
    std::vector<size_t> starts;
    for (size_t at = 0; at + 3 <= size; ++at) {
        if (data[at] == 0 && data[at + 1] == 0 && data[at + 2] == 1) {
            starts.push_back(at + 3);
            at += 2;
        }
    }
    std::vector<std::vector<uint8_t>> units;
    for (size_t i = 0; i < starts.size(); ++i) {
        size_t end = i + 1 < starts.size() ? starts[i + 1] - 3 : size;
        while (end > starts[i] && data[end - 1] == 0) --end;
        units.emplace_back(data + starts[i], data + end);
    }
    return units;
}

inline std::vector<uint8_t> avcRecord(const std::vector<uint8_t>& sps, const std::vector<uint8_t>& pps, int lengthSize) {
    std::vector<uint8_t> out = {1, sps[1], sps[2], sps[3], static_cast<uint8_t>(0xfc | (lengthSize - 1)), 0xe1};
    for (const auto* unit : {&sps, &pps}) {
        if (unit == &pps) out.push_back(1);
        out.push_back(static_cast<uint8_t>(unit->size() >> 8));
        out.push_back(static_cast<uint8_t>(unit->size()));
        out.insert(out.end(), unit->begin(), unit->end());
    }
    return out;
}

// An avcC record holding both parameter-set pairs, so a change between two such records is the length size alone.
inline std::vector<uint8_t> avcRecordBoth(const std::vector<uint8_t> (&sps)[2], const std::vector<uint8_t> (&pps)[2],
                                          int lengthSize) {
    std::vector<uint8_t> out = {1, sps[0][1], sps[0][2], sps[0][3], static_cast<uint8_t>(0xfc | (lengthSize - 1)), 0xe2};
    for (const auto* set : {&sps, &pps}) {
        if (set == &pps) out.push_back(2);
        for (const auto& unit : *set) {
            out.push_back(static_cast<uint8_t>(unit.size() >> 8));
            out.push_back(static_cast<uint8_t>(unit.size()));
            out.insert(out.end(), unit.begin(), unit.end());
        }
    }
    return out;
}

// `source`, counter_ps.h264 (IDR every 30; SPS/PPS id 0 for pictures 0-119, id 1 from 120) as FLV, its parameter sets
// out of band: the first part under an avcC declaring `firstPrefix`-byte lengths, the second under one
// declaring `secondPrefix` carried as NEW_EXTRADATA on picture 120. With `sameParameterSets` both records carry both
// pairs, so only the length size changes. An empty string, with the reason printed, when it cannot be built.
inline std::string buildLengthChangeFlv(const std::string& source, const std::filesystem::path& out, int firstPrefix,
                                        int secondPrefix, bool sameParameterSets = false) {
    AVFormatContext* in = nullptr;
    if (avformat_open_input(&in, source.c_str(), nullptr, nullptr) < 0) return {};
    struct In {
        AVFormatContext*& f;
        ~In() { avformat_close_input(&f); }
    } closeIn{in};
    std::vector<std::vector<std::vector<uint8_t>>> slices;  // per picture, its non-parameter-set units
    std::vector<uint8_t> sps[2], pps[2];
    std::vector<bool> idr;
    AVPacket* packet = av_packet_alloc();
    while (packet != nullptr && av_read_frame(in, packet) >= 0) {
        const int part = slices.size() < 120 ? 0 : 1;
        std::vector<std::vector<uint8_t>> kept;
        bool isIdr = false;
        for (auto& unit : annexBUnits(packet->data, static_cast<size_t>(packet->size))) {
            if (unit.empty()) continue;
            const int type = unit[0] & 0x1f;
            if (type == 7 || type == 8) {
                auto& slot = type == 7 ? sps[part] : pps[part];
                if (slot.empty()) slot = unit;
                continue;
            }
            isIdr = isIdr || type == 5;
            if (unit.size() > 0xffff) {
                av_packet_free(&packet);
                return {};
            }
            kept.push_back(std::move(unit));
        }
        slices.push_back(std::move(kept));
        idr.push_back(isIdr);
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    if (slices.size() != 300 || sps[0].size() < 4 || sps[1].size() < 4 || pps[0].empty() || pps[1].empty() ||
        sps[0] == sps[1]) {
        return {};
    }
    AVFormatContext* flv = nullptr;
    if (avformat_alloc_output_context2(&flv, nullptr, "flv", out.c_str()) < 0) return {};
    struct Out {
        AVFormatContext*& f;
        ~Out() {
            if (f != nullptr && f->pb != nullptr) avio_closep(&f->pb);
            avformat_free_context(f);
        }
    } closeOut{flv};
    AVStream* stream = avformat_new_stream(flv, nullptr);
    if (stream == nullptr) return {};
    const std::vector<uint8_t> first =
        sameParameterSets ? avcRecordBoth(sps, pps, firstPrefix) : avcRecord(sps[0], pps[0], firstPrefix);
    const std::vector<uint8_t> second =
        sameParameterSets ? avcRecordBoth(sps, pps, secondPrefix) : avcRecord(sps[1], pps[1], secondPrefix);
    stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    stream->codecpar->codec_id = AV_CODEC_ID_H264;
    stream->codecpar->width = 128;
    stream->codecpar->height = 96;
    stream->codecpar->extradata = static_cast<uint8_t*>(av_mallocz(first.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (stream->codecpar->extradata == nullptr) return {};
    std::memcpy(stream->codecpar->extradata, first.data(), first.size());
    stream->codecpar->extradata_size = static_cast<int>(first.size());
    stream->time_base = AVRational{1, 1000};
    if (avio_open(&flv->pb, out.c_str(), AVIO_FLAG_WRITE) < 0 || avformat_write_header(flv, nullptr) < 0) return {};
    for (size_t i = 0; i < slices.size(); ++i) {
        const int prefix = i < 120 ? firstPrefix : secondPrefix;
        std::vector<uint8_t> bytes;
        for (const auto& unit : slices[i]) {
            for (int b = prefix - 1; b >= 0; --b) bytes.push_back(static_cast<uint8_t>(unit.size() >> (8 * b)));
            bytes.insert(bytes.end(), unit.begin(), unit.end());
        }
        AVPacket* pkt = av_packet_alloc();
        if (pkt == nullptr || av_new_packet(pkt, static_cast<int>(bytes.size())) < 0) {
            av_packet_free(&pkt);
            return {};
        }
        std::memcpy(pkt->data, bytes.data(), bytes.size());
        pkt->pts = pkt->dts = av_rescale(static_cast<int64_t>(i), 1000, 30);
        pkt->duration = 33;
        if (idr[i]) pkt->flags |= AV_PKT_FLAG_KEY;
        if (i == 120) {
            uint8_t* side = av_packet_new_side_data(pkt, AV_PKT_DATA_NEW_EXTRADATA, second.size());
            if (side == nullptr) {
                av_packet_free(&pkt);
                return {};
            }
            std::memcpy(side, second.data(), second.size());
        }
        const int err = av_interleaved_write_frame(flv, pkt);
        av_packet_free(&pkt);
        if (err < 0) return {};
    }
    if (av_write_trailer(flv) < 0) return {};
    return out.string();
}

}  // namespace wallpaperx::testing
