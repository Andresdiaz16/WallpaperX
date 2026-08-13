// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace wallpaperx {

// Every client-supplied string the daemon logs or returns passes through here.
//
// Guarantees, for ANY byte sequence -- the input is not assumed to be valid UTF-8, because
// callers exist that pass environment bytes:
//   - The result is valid UTF-8, so it always marshals into a D-Bus reply and survives
//     nlohmann::json::dump(). Ill-formed input is escaped, never propagated.
//   - No C0 control, DEL, C1 control (U+009B CSI and U+009D OSC drive a terminal without
//     any C0 byte), or code point that reorders the display around it -- U+2028/U+2029,
//     U+202A..U+202E, U+2066..U+2069, U+200E/U+200F, U+061C.
//   - An unescaped `"` in the result was written by this function, never by the payload.
//
// Other bytes >= 0x80 pass through: wallpaper paths are usually ordinary UTF-8 and
// escaping all of it would make normal logs unreadable.
//
// Framing wraps the value in quotes so the daemon's own annotations are never in the same
// alphabet as the payload -- callers add no quotes of their own. `Bare` drops them for a
// value that fills a whole field (lastError, which JSON quotes itself). Truncation forces
// quotes back on in both modes, because the marker is daemon-written text after the value
// and a path can otherwise contain a convincing copy of it.
enum class LogFraming { Quoted, Bare };

namespace detail {

// One decoded UTF-8 scalar, or a rejection. `length` is always >= 1 so a caller advancing
// by it cannot loop forever; `valid == false` always pairs with `length == 1`, so a run of
// garbage escapes byte by byte instead of swallowing the good bytes after it.
struct Utf8Scalar {
    std::uint32_t code;
    std::size_t length;
    bool valid;
};

inline Utf8Scalar decodeUtf8(std::string_view text, std::size_t i) {
    const auto byteAt = [&text](std::size_t k) { return static_cast<unsigned char>(text[k]); };
    const unsigned char lead = byteAt(i);
    if (lead < 0x80) return {lead, 1, true};

    std::size_t length = 0;
    std::uint32_t code = 0;
    std::uint32_t lowest = 0;
    if ((lead & 0xE0) == 0xC0) {
        length = 2;
        code = lead & 0x1Fu;
        lowest = 0x80;
    } else if ((lead & 0xF0) == 0xE0) {
        length = 3;
        code = lead & 0x0Fu;
        lowest = 0x800;
    } else if ((lead & 0xF8) == 0xF0) {
        length = 4;
        code = lead & 0x07u;
        lowest = 0x10000;
    } else {
        // A continuation byte with no lead, or 0xF8..0xFF, which UTF-8 never uses.
        return {0, 1, false};
    }

    if (i + length > text.size()) return {0, 1, false};
    for (std::size_t j = 1; j < length; ++j) {
        const unsigned char cont = byteAt(i + j);
        if ((cont & 0xC0) != 0x80) return {0, 1, false};
        code = (code << 6) | (cont & 0x3Fu);
    }

    // Overlong encodings, surrogates and anything past U+10FFFF pass a
    // length-and-continuation check but are not valid UTF-8, and both consumers of this
    // output reject them.
    if (code < lowest) return {0, 1, false};
    if (code >= 0xD800 && code <= 0xDFFF) return {0, 1, false};
    if (code > 0x10FFFF) return {0, 1, false};
    return {code, length, true};
}

// Code points that choose how the line *renders* rather than what it says. None contains a
// control byte, so a byte-classifying filter passes every one through.
inline bool isDisplayControl(std::uint32_t code) {
    if (code >= 0x0080 && code <= 0x009F) return true;  // C1 controls: CSI, OSC, ...
    if (code == 0x061C) return true;                    // ARABIC LETTER MARK
    if (code == 0x200E || code == 0x200F) return true;  // LEFT/RIGHT-TO-LEFT MARK
    if (code >= 0x2028 && code <= 0x202E) return true;  // separators, embeddings, overrides
    if (code >= 0x2066 && code <= 0x2069) return true;  // isolates
    return false;
}

}  // namespace detail

// Whether the whole input decodes as UTF-8. nlohmann::json::dump() throws on anything else,
// so a caller putting a raw string into JSON checks this first: sanitizeForLog is
// display-safe but not round-trip-safe, and a persisted path needs the bytes back exactly.
inline bool isValidUtf8(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const detail::Utf8Scalar scalar = detail::decodeUtf8(text, i);
        if (!scalar.valid) return false;
        i += scalar.length;
    }
    return true;
}

inline std::string sanitizeForLog(std::string_view text, std::size_t maxBytes = 4096,
                                  LogFraming framing = LogFraming::Quoted) {
    const bool truncated = text.size() > maxBytes;

    // Back off to a sequence boundary: continuation bytes are 10xxxxxx, at most three
    // steps. Not needed for validity (the decoder would escape a severed tail) but it stops
    // an ordinary truncation turning a legitimate last character into `\xNN` escapes.
    std::size_t cut = maxBytes;
    while (truncated && cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    const std::string_view head = truncated ? text.substr(0, cut) : text;

    // Truncation forces the frame back on even in Bare mode -- see FRAMING above.
    const bool quoted = framing == LogFraming::Quoted || truncated;

    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(head.size() + 32);
    const auto escapeByte = [&out](unsigned char b) {
        out += "\\x";
        out += kHex[b >> 4];
        out += kHex[b & 0x0F];
    };

    if (quoted) out += '"';
    for (std::size_t i = 0; i < head.size();) {
        const detail::Utf8Scalar scalar = detail::decodeUtf8(head, i);
        if (!scalar.valid) {
            // One byte at a time, as the byte it is: the escape stays reversible and says
            // exactly what was on the wire, which is the point of logging it at all.
            escapeByte(static_cast<unsigned char>(head[i]));
            i += scalar.length;
            continue;
        }
        if (scalar.code < 0x80) {
            const char raw = head[i];
            switch (raw) {
                case '\\': out += "\\\\"; break;
                case '"': out += "\\\""; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (scalar.code < 0x20 || scalar.code == 0x7F) {
                        // Two hex digits, always -- \x7 and \x07 would otherwise be the
                        // same escape for different bytes.
                        escapeByte(static_cast<unsigned char>(raw));
                    } else {
                        out += raw;
                    }
                    break;
            }
            i += 1;
            continue;
        }
        if (detail::isDisplayControl(scalar.code)) {
            for (std::size_t j = 0; j < scalar.length; ++j) {
                escapeByte(static_cast<unsigned char>(head[i + j]));
            }
            i += scalar.length;
            continue;
        }
        out.append(head.substr(i, scalar.length));
        i += scalar.length;
    }
    if (quoted) out += '"';

    // The total length is diagnostic: without it a truncated 300 000-byte path and a
    // genuine 4096-byte one read identically.
    //
    // `maxBytes` bounds the INPUT; output is up to 4x that, since an escaped byte becomes
    // four characters. The default is PATH_MAX so no real path is ever truncated in the
    // error a user reads to find their typo.
    if (truncated) {
        out += "... [truncated, ";
        out += std::to_string(text.size());
        out += " bytes total]";
    }
    return out;
}

}  // namespace wallpaperx
