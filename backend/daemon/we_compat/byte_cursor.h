// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace wallpaperx::we {

// Reads little-endian fields from untrusted bytes. Every read is checked against what is left, and a
// failed read leaves the position where it was -- so once one fails, every later read at least as long
// fails too, and a run of fixed-size reads can be checked by its last.
class ByteCursor {
public:
    explicit ByteCursor(std::string_view bytes) : m_bytes(bytes) {}

    std::size_t position() const { return m_at; }
    std::size_t remaining() const { return m_bytes.size() - m_at; }
    bool atEnd() const { return m_at == m_bytes.size(); }

    std::optional<std::string_view> take(std::size_t count)
    {
        if (count > remaining()) return std::nullopt;
        const std::string_view out = m_bytes.substr(m_at, count);
        m_at += count;
        return out;
    }

    std::optional<std::uint32_t> u32()
    {
        const auto raw = take(4);
        if (!raw) return std::nullopt;
        const auto* b = reinterpret_cast<const unsigned char*>(raw->data());
        return std::uint32_t(b[0]) | std::uint32_t(b[1]) << 8 | std::uint32_t(b[2]) << 16 | std::uint32_t(b[3]) << 24;
    }

    std::optional<std::int32_t> i32()
    {
        const auto value = u32();
        return value ? std::optional(std::bit_cast<std::int32_t>(*value)) : std::nullopt;
    }

    std::optional<float> f32()
    {
        const auto value = u32();
        return value ? std::optional(std::bit_cast<float>(*value)) : std::nullopt;
    }

private:
    std::string_view m_bytes;
    std::size_t m_at = 0;
};

} // namespace wallpaperx::we
