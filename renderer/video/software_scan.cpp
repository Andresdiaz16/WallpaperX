// SPDX-License-Identifier: GPL-2.0-or-later

#include "software_scan.h"

#include "ffmpeg_file.h"
#include "nal_bits.h"
#include "reset_point.h"

#include "../vulkan/gpu_fault.h"
#include "../wallpaper_file.h"

extern "C" {
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
}

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <utility>
#include <vector>

namespace wallpaperx {

namespace {

constexpr const char* kComponent = "SoftwareScan";

// Enough of a slice for its header, which ends well inside this.
constexpr size_t kSliceHeaderBytes = 1024;
constexpr size_t kParameterSetBytes = 64 * 1024;

unsigned ceilLog2(size_t n) {
    unsigned bits = 0;
    while ((size_t{1} << bits) < n) ++bits;
    return bits;
}

// What one codec's decoder keeps between packets, for the questions asked of it here.
class Follower {
public:
    virtual ~Follower() = default;
    // A parameter set from the extradata, the side data or the packet, which the decoder adopts.
    virtual void parameterSet(const uint8_t* unit, size_t size) = 0;
    virtual void startPacket() = 0;
    // A unit of a packet in decode order; true when the decoder would make up a picture for it.
    virtual bool packetUnit(const uint8_t* unit, size_t size) = 0;
    // Whether the decoder has decoded a picture from what the stream holds.
    bool decodedAny() const { return m_decodedAny; }

protected:
    bool m_decodedAny = false;
};

// ---------------------------------------------------------------------------------------------- H.264

class H264Follower final : public Follower {
public:
    void parameterSet(const uint8_t* unit, size_t size) override {
        if (size < 1) return;
        const int type = h264Type(unit);
        if (type == h264::kSps) readSps(unit, size);
        if (type == h264::kPps) readPps(unit, size);
    }

    void startPacket() override {
        m_pictureInPacket = Picture::None;
        m_idrCleared = false;
    }

    bool packetUnit(const uint8_t* unit, size_t size) override {
        if (size < 1) return false;
        const int type = h264Type(unit);
        if (type == h264::kSps || type == h264::kPps) {
            parameterSet(unit, size);
            return false;
        }
        if (type != h264::kSlice && type != h264::kIdr) return false;
        const bool idr = type == h264::kIdr;
        NalBits bits(unit, size, 1, kSliceHeaderBytes);
        if (bits.ue() != 0) return false;  // not a picture's first slice
        bits.ue();                         // slice_type
        const uint32_t ppsId = bits.ue();
        const auto pps = m_pps.find(ppsId);
        if (!bits.ok() || pps == m_pps.end()) return false;  // FFmpeg drops a slice naming no PPS it holds
        const auto sps = m_sps.find(pps->second);
        if (sps == m_sps.end()) return false;
        if (sps->second.separateColourPlane) bits.skip(2);
        const auto frameNum = static_cast<int64_t>(bits.u(sps->second.log2MaxFrameNum));
        bool field = false;
        bool bottom = false;
        if (!sps->second.frameMbsOnly) {
            field = bits.flag();
            if (field) bottom = bits.flag();
        }
        if (!bits.ok()) return false;

        // A second picture in one packet: FFmpeg decodes it after a first field still unpaired, or as an IDR after
        // a frame, and drops it otherwise.
        if (m_pictureInPacket != Picture::None) {
            const bool afterFirstField = m_pictureInPacket == Picture::Field && m_firstField;
            const bool idrAfterFrame = m_pictureInPacket == Picture::Frame && idr;
            if (!afterFirstField && !idrAfterFrame) return false;
        }

        if (idr && !m_idrCleared) {
            m_prevFrameNum = 0;
            m_idrCleared = true;
        }
        const int64_t maxFrameNum = int64_t{1} << sps->second.log2MaxFrameNum;
        // FFmpeg shortens a gap longer than the references the SPS allows, so a stream allowed none makes none.
        if (frameNum != m_prevFrameNum) {
            int64_t unwrapped = m_prevFrameNum;
            if (unwrapped > frameNum) unwrapped -= maxFrameNum;
            if (frameNum - unwrapped > sps->second.refFrames) {
                unwrapped = frameNum - sps->second.refFrames - 1;
                if (unwrapped < 0) unwrapped += maxFrameNum;
                m_prevFrameNum = unwrapped;
            }
        }
        if (!m_firstField && frameNum != m_prevFrameNum && frameNum != (m_prevFrameNum + 1) % maxFrameNum) return true;

        bool allocates = true;
        if (m_firstField) {
            const bool pairs = field && bottom != m_firstField->bottom && m_firstField->frameNum == frameNum;
            allocates = !pairs;
            m_firstField = pairs || !field ? std::nullopt : std::optional(PendingField{frameNum, bottom});
        } else if (field) {
            m_firstField = PendingField{frameNum, bottom};
        }
        m_prevFrameNum = frameNum;
        const bool second = allocates && m_pictureInPacket != Picture::None;
        m_pictureInPacket = field ? Picture::Field : Picture::Frame;
        if (!second) m_decodedAny = true;
        return second;
    }

private:
    struct Sps {
        unsigned log2MaxFrameNum = 4;
        int64_t refFrames = 0;
        bool frameMbsOnly = true;
        bool separateColourPlane = false;
    };
    struct PendingField {
        int64_t frameNum = 0;
        bool bottom = false;
    };
    enum class Picture { None, Field, Frame };

    static void skipScalingList(NalBits& bits, int size) {
        int last = 8;
        int next = 8;
        for (int j = 0; j < size && bits.ok(); ++j) {
            if (next != 0) next = (last + bits.se() + 256) % 256;
            last = next == 0 ? last : next;
        }
    }

    void readSps(const uint8_t* unit, size_t size) {
        NalBits bits(unit, size, 1, kParameterSetBytes);
        const uint32_t profile = bits.u(8);
        bits.skip(16);
        const uint32_t id = bits.ue();
        Sps sps;
        constexpr uint32_t kHighProfiles[] = {100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135};
        if (std::find(std::begin(kHighProfiles), std::end(kHighProfiles), profile) != std::end(kHighProfiles)) {
            const uint32_t chroma = bits.ue();
            if (chroma == 3) sps.separateColourPlane = bits.flag();
            bits.ue();
            bits.ue();
            bits.flag();
            if (bits.flag()) {
                for (int i = 0; i < (chroma != 3 ? 8 : 12) && bits.ok(); ++i) {
                    if (bits.flag()) skipScalingList(bits, i < 6 ? 16 : 64);
                }
            }
        }
        const uint32_t log2MaxFrameNum = bits.ue() + 4;
        const uint32_t pocType = bits.ue();
        if (pocType == 0) {
            bits.ue();
        } else if (pocType == 1) {
            bits.flag();
            bits.se();
            bits.se();
            const uint32_t cycle = bits.ue();
            for (uint32_t i = 0; i < cycle && i < 256 && bits.ok(); ++i) bits.se();
        }
        sps.refFrames = bits.ue();
        bits.flag();
        bits.ue();
        bits.ue();
        sps.frameMbsOnly = bits.flag();
        if (!bits.ok() || id > 31 || log2MaxFrameNum > 16 || pocType > 2) return;
        sps.log2MaxFrameNum = log2MaxFrameNum;
        m_sps[id] = sps;
    }

    void readPps(const uint8_t* unit, size_t size) {
        NalBits bits(unit, size, 1, kParameterSetBytes);
        const uint32_t id = bits.ue();
        const uint32_t sps = bits.ue();
        if (!bits.ok() || id > 255 || sps > 31) return;
        m_pps[id] = sps;
    }

    std::map<uint32_t, Sps> m_sps;
    std::map<uint32_t, uint32_t> m_pps;
    // FFmpeg's prev_frame_num, which a flushed decoder holds at -1.
    int64_t m_prevFrameNum = -1;
    std::optional<PendingField> m_firstField;
    Picture m_pictureInPacket = Picture::None;
    bool m_idrCleared = false;
};

// ----------------------------------------------------------------------------------------------- HEVC

class HevcFollower final : public Follower {
public:
    void parameterSet(const uint8_t* unit, size_t size) override {
        if (size < 2 || hevcLayer(unit) != 0) return;
        const int type = hevcType(unit);
        if (type == hevc::kSps) readSps(unit, size);
        if (type == hevc::kPps) readPps(unit, size);
    }

    void startPacket() override {
        m_lastEos = m_eos;
        m_eos = false;
        m_sliceSeen = false;
    }

    bool packetUnit(const uint8_t* unit, size_t size) override {
        if (size < 2 || hevcLayer(unit) != 0) return false;
        const int type = hevcType(unit);
        if (type == hevc::kSps || type == hevc::kPps) {
            parameterSet(unit, size);
            return false;
        }
        // The pictures held belong to the sequence an end of sequence or bitstream closes.
        if (type == kEndOfSequence || type == kEndOfBitstream) {
            m_held.clear();
            (m_sliceSeen ? m_eos : m_lastEos) = true;
            return false;
        }
        if (!isSlice(type)) return false;
        m_sliceSeen = true;
        return picture(unit, size, type);
    }

private:
    static constexpr int kRaslN = 8;
    static constexpr int kRaslR = 9;
    static constexpr int kBlaFirst = 16;
    static constexpr int kBlaLast = 18;
    static constexpr int kCra = 21;
    static constexpr int kEndOfSequence = 36;
    static constexpr int kEndOfBitstream = 37;

    // A short-term reference set as FFmpeg orders it: the pictures before, nearest first, then those after.
    struct ShortTerm {
        std::vector<int32_t> before;
        std::vector<bool> beforeUsed;
        std::vector<int32_t> after;
        std::vector<bool> afterUsed;
    };
    struct Sps {
        unsigned log2MaxPocLsb = 4;
        bool separateColourPlane = false;
        std::vector<ShortTerm> sets;
        bool longTerm = false;
        std::vector<uint32_t> longTermLsb;
        std::vector<bool> longTermUsed;
    };
    struct Pps {
        uint32_t sps = 0;
        bool outputFlag = false;
        unsigned extraBits = 0;
    };

    static bool isSlice(int type) { return type <= kRaslR || (type >= kBlaFirst && type <= kCra); }
    static bool isIrap(int type) { return type >= kBlaFirst && type <= 23; }
    static void skipProfileTierLevel(NalBits& bits, unsigned subLayers) {
        bits.skip(96);
        std::vector<std::pair<bool, bool>> present(subLayers);
        for (auto& [profile, level] : present) {
            profile = bits.flag();
            level = bits.flag();
        }
        if (subLayers > 0) bits.skip(2 * (8 - subLayers));
        for (const auto& [profile, level] : present) {
            if (profile) bits.skip(88);
            if (level) bits.skip(8);
        }
    }

    static void skipScalingListData(NalBits& bits) {
        for (int sizeId = 0; sizeId < 4 && bits.ok(); ++sizeId) {
            for (int matrixId = 0; matrixId < 6; matrixId += sizeId == 3 ? 3 : 1) {
                if (!bits.flag()) {
                    bits.ue();
                    continue;
                }
                const int coefficients = std::min(64, 1 << (4 + (sizeId << 1)));
                if (sizeId > 1) bits.se();
                for (int i = 0; i < coefficients && bits.ok(); ++i) bits.se();
            }
        }
    }

    // st_ref_pic_set(index), the SPS's own sets being `sets`; a slice's own set has index sets.size().
    static std::optional<ShortTerm> readShortTerm(NalBits& bits, size_t index, const std::vector<ShortTerm>& sets) {
        ShortTerm set;
        if (index != 0 && bits.flag()) {
            const size_t deltaIndex = index == sets.size() ? bits.ue() + size_t{1} : 1;
            if (deltaIndex > index) return std::nullopt;
            const ShortTerm& ref = sets[index - deltaIndex];
            const bool negative = bits.flag();
            const int32_t magnitude = static_cast<int32_t>(bits.ue()) + 1;
            if (magnitude > (1 << 15)) return std::nullopt;
            const int32_t deltaRps = negative ? -magnitude : magnitude;
            // The reference set's pictures in the standard's order -- before, then after -- plus itself.
            std::vector<int32_t> deltas = ref.before;
            deltas.insert(deltas.end(), ref.after.begin(), ref.after.end());
            deltas.push_back(0);
            for (const int32_t delta : deltas) {
                const bool used = bits.flag();
                const bool kept = used || bits.flag();
                const int32_t poc = delta + deltaRps;
                if (!kept || poc == 0) continue;
                (poc < 0 ? set.before : set.after).push_back(poc);
                (poc < 0 ? set.beforeUsed : set.afterUsed).push_back(used);
            }
        } else {
            const uint32_t before = bits.ue();
            const uint32_t after = bits.ue();
            if (before > 16 || after > 16) return std::nullopt;
            int32_t poc = 0;
            for (uint32_t i = 0; i < before; ++i) {
                poc -= static_cast<int32_t>(bits.ue()) + 1;
                set.before.push_back(poc);
                set.beforeUsed.push_back(bits.flag());
            }
            poc = 0;
            for (uint32_t i = 0; i < after; ++i) {
                poc += static_cast<int32_t>(bits.ue()) + 1;
                set.after.push_back(poc);
                set.afterUsed.push_back(bits.flag());
            }
        }
        if (!bits.ok()) return std::nullopt;
        sortNearestFirst(set.before, set.beforeUsed, std::greater<>());
        sortNearestFirst(set.after, set.afterUsed, std::less<>());
        return set;
    }

    template <typename Order>
    static void sortNearestFirst(std::vector<int32_t>& pocs, std::vector<bool>& used, Order order) {
        std::vector<size_t> index(pocs.size());
        std::iota(index.begin(), index.end(), size_t{0});
        std::stable_sort(index.begin(), index.end(), [&](size_t a, size_t b) { return order(pocs[a], pocs[b]); });
        std::vector<int32_t> sortedPocs;
        std::vector<bool> sortedUsed;
        for (const size_t i : index) {
            sortedPocs.push_back(pocs[i]);
            sortedUsed.push_back(used[i]);
        }
        pocs = std::move(sortedPocs);
        used = std::move(sortedUsed);
    }

    void readSps(const uint8_t* unit, size_t size) {
        NalBits bits(unit, size, 2, kParameterSetBytes);
        bits.skip(4);
        const uint32_t subLayers = bits.u(3);
        bits.flag();
        skipProfileTierLevel(bits, subLayers);
        const uint32_t id = bits.ue();
        Sps sps;
        const uint32_t chroma = bits.ue();
        if (chroma == 3) sps.separateColourPlane = bits.flag();
        bits.ue();
        bits.ue();
        if (bits.flag()) {
            for (int i = 0; i < 4; ++i) bits.ue();
        }
        bits.ue();
        bits.ue();
        const uint32_t log2MaxPocLsb = bits.ue() + 4;
        const bool orderingPerLayer = bits.flag();
        for (uint32_t i = orderingPerLayer ? 0 : subLayers; i <= subLayers; ++i) {
            bits.ue();
            bits.ue();
            bits.ue();
        }
        for (int i = 0; i < 6; ++i) bits.ue();
        if (bits.flag() && bits.flag()) skipScalingListData(bits);
        bits.flag();
        bits.flag();
        if (bits.flag()) {
            bits.skip(8);
            bits.ue();
            bits.ue();
            bits.flag();
        }
        const uint32_t sets = bits.ue();
        if (!bits.ok() || id > 15 || log2MaxPocLsb > 16 || sets > 64) return;
        for (uint32_t i = 0; i < sets; ++i) {
            std::optional<ShortTerm> set = readShortTerm(bits, i, sps.sets);
            if (!set) return;
            sps.sets.push_back(std::move(*set));
        }
        sps.longTerm = bits.flag();
        if (sps.longTerm) {
            const uint32_t count = bits.ue();
            if (count > 32) return;
            for (uint32_t i = 0; i < count; ++i) {
                sps.longTermLsb.push_back(bits.u(log2MaxPocLsb));
                sps.longTermUsed.push_back(bits.flag());
            }
        }
        if (!bits.ok()) return;
        sps.log2MaxPocLsb = log2MaxPocLsb;
        m_sps[id] = std::move(sps);
    }

    void readPps(const uint8_t* unit, size_t size) {
        NalBits bits(unit, size, 2, kParameterSetBytes);
        const uint32_t id = bits.ue();
        Pps pps;
        pps.sps = bits.ue();
        bits.flag();
        pps.outputFlag = bits.flag();
        pps.extraBits = bits.u(3);
        if (!bits.ok() || id > 63 || pps.sps > 15) return;
        m_pps[id] = pps;
    }

    // One picture's first slice segment; a later segment of a picture says nothing new here.
    bool picture(const uint8_t* unit, size_t size, int type) {
        NalBits bits(unit, size, 2, kSliceHeaderBytes);
        if (!bits.flag()) return false;
        if (isIrap(type)) bits.flag();
        const auto pps = m_pps.find(bits.ue());
        if (!bits.ok() || pps == m_pps.end()) return false;  // FFmpeg drops a slice naming no PPS it holds
        const auto found = m_sps.find(pps->second.sps);
        if (found == m_sps.end()) return false;
        const Sps& sps = found->second;
        bits.skip(pps->second.extraBits);
        bits.ue();
        if (pps->second.outputFlag) bits.flag();
        if (sps.separateColourPlane) bits.skip(2);

        const int64_t maxLsb = int64_t{1} << sps.log2MaxPocLsb;
        int64_t poc = 0;
        ShortTerm shortTerm;
        struct LongTerm {
            int64_t poc;
            bool used;
            bool fullPoc;
        };
        std::vector<LongTerm> longTerm;
        if (!hevc::isIdr(type)) {
            const auto lsb = static_cast<int64_t>(bits.u(sps.log2MaxPocLsb));
            poc = pocOf(lsb, maxLsb, type);
            if (!bits.flag()) {
                std::optional<ShortTerm> own = readShortTerm(bits, sps.sets.size(), sps.sets);
                if (!own) return false;
                shortTerm = std::move(*own);
            } else if (!sps.sets.empty()) {
                const uint32_t index = sps.sets.size() > 1 ? bits.u(ceilLog2(sps.sets.size())) : 0;
                if (index >= sps.sets.size()) return false;
                shortTerm = sps.sets[index];
            } else {
                return false;
            }
            if (sps.longTerm) {
                const uint32_t fromSps = sps.longTermLsb.empty() ? 0 : bits.ue();
                const uint32_t own = bits.ue();
                if (fromSps > sps.longTermLsb.size() || own > 32) return false;
                int64_t msbDelta = 0;
                for (uint32_t i = 0; i < fromSps + own; ++i) {
                    LongTerm entry{};
                    if (i < fromSps) {
                        const uint32_t index =
                            sps.longTermLsb.size() > 1 ? bits.u(ceilLog2(sps.longTermLsb.size())) : 0;
                        if (index >= sps.longTermLsb.size()) return false;
                        entry.poc = sps.longTermLsb[index];
                        entry.used = sps.longTermUsed[index];
                    } else {
                        entry.poc = bits.u(sps.log2MaxPocLsb);
                        entry.used = bits.flag();
                    }
                    entry.fullPoc = bits.flag();
                    if (entry.fullPoc) {
                        int64_t delta = bits.ue();
                        if (i != 0 && i != fromSps) delta += msbDelta;
                        entry.poc += poc - delta * maxLsb - lsb;
                        msbDelta = delta;
                    }
                    longTerm.push_back(entry);
                }
            }
        }
        if (!bits.ok()) return false;

        if (isIrap(type)) {
            m_noRaslOutput = hevc::isIdr(type) || type <= kBlaLast || (type == kCra && m_lastEos);
        }
        // Left out by FFmpeg, as a RASL picture of an IRAP picture with nothing before it is.
        if ((type == kRaslN || type == kRaslR) && m_noRaslOutput) return false;
        if (hevc::isIdr(type)) m_held.clear();
        if (layerZeroTemporalReference(unit, type)) m_pocTid0 = poc;

        // FFmpeg's ff_hevc_frame_rps(): each picture the reference sets name, in this order, is found among those
        // held or made up -- by an IRAP picture, or one kept only for later pictures. A picture of another kind
        // missing one it uses is dropped instead, and with it whatever its sets had not yet named.
        std::vector<int64_t> named;
        const auto name = [&](int64_t wanted, bool used, bool fullPoc) {
            const int64_t mask = fullPoc ? -1 : maxLsb - 1;
            const bool held = std::any_of(m_held.begin(), m_held.end(), [&](int64_t at) { return (at & mask) == wanted; });
            if (!held && (isIrap(type) || !used)) return std::optional<bool>(true);
            if (!held) return std::optional<bool>(false);
            named.push_back(fullPoc ? wanted : *std::find_if(m_held.begin(), m_held.end(), [&](int64_t at) {
                return (at & mask) == wanted;
            }));
            return std::optional<bool>();
        };
        const auto each = [&](const std::vector<int32_t>& deltas, const std::vector<bool>& used) -> std::optional<bool> {
            for (size_t i = 0; i < deltas.size(); ++i) {
                if (const std::optional<bool> ended = name(poc + deltas[i], used[i], true)) return ended;
            }
            return std::nullopt;
        };
        std::optional<bool> ended = each(shortTerm.before, shortTerm.beforeUsed);
        if (!ended) ended = each(shortTerm.after, shortTerm.afterUsed);
        for (size_t i = 0; !ended && i < longTerm.size(); ++i) {
            ended = name(longTerm[i].poc, longTerm[i].used, longTerm[i].fullPoc);
        }
        if (ended && *ended) return true;
        if (!ended) {
            named.push_back(poc);
            m_decodedAny = true;
        }
        m_held = std::move(named);
        return false;
    }

    // HEVC's PicOrderCntVal from prevTid0Pic, as FFmpeg derives it.
    int64_t pocOf(int64_t lsb, int64_t maxLsb, int type) const {
        const int64_t prevLsb = ((m_pocTid0 % maxLsb) + maxLsb) % maxLsb;
        const int64_t prevMsb = m_pocTid0 - prevLsb;
        int64_t msb = prevMsb;
        if (lsb < prevLsb && prevLsb - lsb >= maxLsb / 2) {
            msb = prevMsb + maxLsb;
        } else if (lsb > prevLsb && lsb - prevLsb > maxLsb / 2) {
            msb = prevMsb - maxLsb;
        }
        if (type >= kBlaFirst && type <= kBlaLast) msb = 0;
        return msb + lsb;
    }

    // Whether this picture becomes prevTid0Pic: temporal layer 0, and not a sub-layer non-reference, RADL or
    // RASL picture.
    static bool layerZeroTemporalReference(const uint8_t* unit, int type) {
        const int temporalId = (unit[1] & 7) - 1;
        constexpr int kNotReferences[] = {0, 2, 4, 6, 7, kRaslN, kRaslR};
        return temporalId == 0 && std::find(std::begin(kNotReferences), std::end(kNotReferences), type) ==
                                      std::end(kNotReferences);
    }

    std::map<uint32_t, Sps> m_sps;
    std::map<uint32_t, Pps> m_pps;
    // The pictures held for reference, by PicOrderCntVal.
    std::vector<int64_t> m_held;
    int64_t m_pocTid0 = 0;
    bool m_noRaslOutput = false;
    // FFmpeg starts and flushes with an end of sequence just seen.
    bool m_eos = true;
    bool m_lastEos = true;
    bool m_sliceSeen = false;
};

struct Container {
    ffmpeg_file::Reader reader;
    AVFormatContext* format = nullptr;
    AVPacket* packet = nullptr;
    // The container before the file it reads through.
    ~Container() {
        av_packet_free(&packet);
        avformat_close_input(&format);
        reader.close();
    }
};

// One read from the first packet. With `readPastLead`, the packets before the first IDR that VideoDecoder reads past
// are passed over as it does; nothing when they end other than at an IDR, where it sends them after all.
std::optional<std::optional<SoftwareStretches>> scanOnce(const std::string& file, const std::atomic<bool>& stop,
                                                         bool readPastLead) {
    Container container;
    container.reader.open(file, kComponent);
    ffmpeg_file::openContainer(container.format, container.reader.io(), file, kComponent);
    const int stream = av_find_best_stream(container.format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream == AVERROR_STREAM_NOT_FOUND) {
        throw WallpaperContentError(describeRejection(kComponent, "the wallpaper has no video stream", file));
    }
    if (stream < 0) ffmpeg_file::throwFileFailure(kComponent, "cannot pick a video stream", stream, file);

    const AVCodecParameters& codec = *container.format->streams[stream]->codecpar;
    std::unique_ptr<Follower> follower;
    if (codec.codec_id == AV_CODEC_ID_H264) follower = std::make_unique<H264Follower>();
    if (codec.codec_id == AV_CODEC_ID_HEVC) follower = std::make_unique<HevcFollower>();
    SoftwareStretches found;
    if (!follower) return found;
    // Their packets are never read, which in an interleaved file is most of what a read would cost.
    for (unsigned i = 0; i < container.format->nb_streams; ++i) {
        if (static_cast<int>(i) != stream) container.format->streams[i]->discard = AVDISCARD_ALL;
    }

    const CodecHeader own = CodecHeader::of(codec);
    const auto adopt = [&](const CodecHeader& header) {
        eachExtradataUnit(header, [&](const uint8_t* unit, size_t size) {
            follower->parameterSet(unit, size);
            return false;
        });
    };
    adopt(own);
    // The extradata the codec reads packets by, and the one whose parameter sets it holds: VideoDecoder's
    // header() and codecHeader(), which on one pass from the first packet are both the last side data sent.
    std::vector<uint8_t> current(own.extradata, own.extradata + own.size);

    container.packet = av_packet_alloc();
    if (container.packet == nullptr) ffmpeg_file::throwOutOfMemory(kComponent);
    AVPacket& packet = *container.packet;
    bool inLead = readPastLead;
    bool passedLead = false;
    // The first picture lacked a reference: what is left to learn is whether a reset point hands the stream back.
    bool seekingReset = false;
    for (;;) {
        if (stop.load(std::memory_order_relaxed)) return std::optional<SoftwareStretches>();
        av_packet_unref(&packet);
        const int err = ffmpeg_file::readPacket(*container.format, &packet);
        if (err == AVERROR_EOF) {
            if (inLead && passedLead) return std::nullopt;
            found.missingReferenceThroughout = seekingReset;
            return found;
        }
        if (err < 0) ffmpeg_file::throwFileFailure(kComponent, "cannot read the wallpaper", err, file);
        if (packet.stream_index != stream) continue;

        size_t sideSize = 0;
        const uint8_t* side = av_packet_get_side_data(&packet, AV_PKT_DATA_NEW_EXTRADATA, &sideSize);
        if (sideSize == 0) side = nullptr;
        if (inLead) {
            // By the extradata in effect there, the packet's own side data included, as the decoder's header().
            const CodecHeader here = side != nullptr ? CodecHeader{&codec, side, sideSize}
                                                     : CodecHeader{&codec, current.data(), current.size()};
            if (neverShownBeforeReset(here, packet)) {
                // Never sent: the IDR is handed its side data, so its change is at a reset point.
                if (side != nullptr) {
                    current.assign(side, side + sideSize);
                    adopt(here);
                }
                passedLead = true;
                continue;
            }
            inLead = false;
            if (passedLead && !resetsDecoder(here, packet)) return std::nullopt;
        }

        if (side != nullptr) {
            const CodecHeader held{&codec, current.data(), current.size()};
            const CodecHeader carried{&codec, side, sideSize};
            if (!seekingReset && differInParameterSets(held, carried) && !resetsDecoder(carried, packet)) {
                found.parameterSetsChangeOffReset = true;
            }
            current.assign(side, side + sideSize);
            adopt(carried);
        }

        // FFmpeg refuses the whole of a packet whose units cannot all be split.
        const CodecHeader layout{&codec, current.data(), current.size()};
        if (!eachPacketUnit(layout, packet, [](const uint8_t*, size_t) { return false; })) continue;
        if (seekingReset) {
            if (resetsDecoder(layout, packet)) return found;
            continue;
        }
        follower->startPacket();
        bool madeUp = false;
        eachPacketUnit(layout, packet, [&](const uint8_t* unit, size_t size) {
            madeUp = follower->packetUnit(unit, size);
            return madeUp;
        });
        if (madeUp) {
            found.missingReference = true;
            // Pictures before it reached the device. Without any, the device gets the stream only at a reset point.
            if (follower->decodedAny()) return found;
            seekingReset = true;
        }
    }
}

}  // namespace

std::optional<SoftwareStretches> scanSoftwareStretches(const std::string& file, const std::atomic<bool>& stop) {
    if (std::optional<std::optional<SoftwareStretches>> once = scanOnce(file, stop, true)) return *once;
    return *scanOnce(file, stop, false);
}

}
