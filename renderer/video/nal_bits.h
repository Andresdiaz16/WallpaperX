// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wallpaperx {

// NAL unit header fields. `unit` holds at least the header: one byte for H.264, two for HEVC.
inline int h264Type(const uint8_t* unit) { return unit[0] & 0x1f; }
inline int hevcType(const uint8_t* unit) { return (unit[0] >> 1) & 0x3f; }
inline int hevcLayer(const uint8_t* unit) { return ((unit[0] & 1) << 5) | (unit[1] >> 3); }

namespace h264 {
inline constexpr int kSlice = 1;
inline constexpr int kIdr = 5;
inline constexpr int kSei = 6;
inline constexpr int kSps = 7;
inline constexpr int kPps = 8;
}  // namespace h264

namespace hevc {
inline constexpr int kIdrWithLeading = 19;
inline constexpr int kIdrNoLeading = 20;
inline constexpr int kVps = 32;
inline constexpr int kSps = 33;
inline constexpr int kPps = 34;
inline bool isIdr(int type) { return type == kIdrWithLeading || type == kIdrNoLeading; }
}  // namespace hevc

// A NAL unit's payload after its `header` bytes, at most `limit` bytes of it, with its emulation-prevention bytes
// taken out, read as the standards' u(n), ue(v) and se(v). Reading past the end reads zeros and marks the reader
// spent, which every caller checks.
class NalBits {
public:
    NalBits(const uint8_t* unit, size_t size, size_t header, size_t limit) {
        size_t zeros = 0;
        for (size_t i = header; i < size && m_bytes.size() < limit; ++i) {
            if (zeros >= 2 && unit[i] == 3) {
                zeros = 0;
                continue;
            }
            zeros = unit[i] == 0 ? zeros + 1 : 0;
            m_bytes.push_back(unit[i]);
        }
    }

    uint32_t u(unsigned n) {
        uint32_t value = 0;
        for (unsigned i = 0; i < n; ++i) value = (value << 1) | bit();
        return value;
    }
    bool flag() { return bit() != 0; }
    void skip(size_t n) {
        m_bit += n;
        if (m_bit > m_bytes.size() * 8) m_spent = true;
    }
    uint32_t ue() {
        unsigned zeros = 0;
        while (bit() == 0) {
            if (m_spent || ++zeros > 31) {
                m_spent = true;
                return 0;
            }
        }
        return zeros == 0 ? 0 : ((1u << zeros) - 1) + u(zeros);
    }
    int32_t se() {
        const uint32_t k = ue();
        return (k & 1) != 0 ? static_cast<int32_t>((k + 1) / 2) : -static_cast<int32_t>(k / 2);
    }
    bool ok() const { return !m_spent; }
    size_t bitsLeft() const { return m_bit < m_bytes.size() * 8 ? m_bytes.size() * 8 - m_bit : 0; }

private:
    unsigned bit() {
        if (m_bit >= m_bytes.size() * 8) {
            m_spent = true;
            return 0;
        }
        const unsigned value = (m_bytes[m_bit / 8] >> (7 - m_bit % 8)) & 1;
        ++m_bit;
        return value;
    }

    std::vector<uint8_t> m_bytes;
    size_t m_bit = 0;
    bool m_spent = false;
};

}
