#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace wallpaperx {

// Every string on this daemon's log that came from a DBus client passes through here.
//
// The log is described elsewhere in this tree as "the only place a failed request is
// visible", and it is writable by any client that can reach the bus: a path containing
// newlines was printed verbatim, so a client chose what the daemon's log said. One
// rejected call was enough to plant
//
//     wallpaperx-daemon: GPU: FAKE | Descriptor tier: Heap
//     wallpaperx-daemon: stopped cleanly
//
// as two apparently-genuine lines in the daemon's exact own format -- in a project where
// the resolved descriptor tier is the first thing anyone asks about when a bug reproduces
// on one GPU and not another. Terminal escapes rode the same channel, so merely reading
// the log in a terminal was enough to be affected.
//
// Size was the other half: a 1 000 000-character path was echoed twice (once on entry,
// once in the rejection) and repeated a third time in the DBus error reply, so five
// hostile calls wrote 2.4 MB. Bounding it here does not make the log un-floodable -- a
// loop still writes a line per call -- but it removes the ~2000x amplification that made
// one call enough.
//
// This decodes UTF-8 rather than classifying bytes, and the difference is the whole
// correctness argument. An earlier version passed every byte >= 0x80 through untouched,
// justified by a stated precondition that all callers hand it bus-validated UTF-8. That
// precondition was false: WallpaperManager::renderPending catches exceptions from
// resolveShaderDir, whose message embeds $WALLPAPERX_SHADER_DIR -- and an environment
// variable on Linux is arbitrary bytes. Those bytes reached MonitorState::lastError, and
// nlohmann::json::dump() throws type_error.316 on invalid UTF-8, so a single malformed
// env var made *every* GetStatus call fail for *every* client until a later SetWallpaper
// cleared the field. Deciding validity here, instead of assuming it upstream, is what
// removes that class rather than that instance.
//
// GUARANTEES, given any byte sequence at all:
//   - The result is valid UTF-8. Ill-formed input is escaped, never propagated, so the
//     output is safe to splice into a D-Bus error reply (which must marshal as UTF-8, and
//     a reply that fails to marshal leaves the caller with no error at all, blocked to its
//     method timeout) and safe to hand to nlohmann::json.
//   - The result contains no C0 control, no DEL, no C1 control, and no code point that
//     reorders or hides the text around it.
//   - An unescaped `"` in the result was written by this function, never by the payload.
//
// What gets escaped, and why each one is not merely cosmetic:
//
//   - **C0 controls and DEL.** Newline, carriage return and ESC live here; they are what
//     let a client forge whole log lines and drive a terminal.
//   - **C1 controls, U+0080..U+009F.** The 8-bit forms of the same thing: U+009B is CSI
//     and U+009D is OSC, so a terminal honouring them can be cleared, or have its window
//     title rewritten, by a string containing no C0 byte at all. Byte-classifying filters
//     miss these entirely -- they encode as C2 80..C2 9F, two bytes both >= 0x80.
//   - **The bidi and separator code points.** U+2028/U+2029 are honoured as line breaks by
//     many log viewers. U+202A..U+202E and U+2066..U+2069 reorder the *display* of
//     everything after them, and U+200E, U+200F and U+061C do the same at smaller scope --
//     enough to make a logged path read as a different path than the one on disk.
//   - **Backslash**, because it introduces every escape above and the escaping has to be
//     unambiguous to be reversible.
//
// Everything else >= 0x80 passes through: a wallpaper path is very often ordinary UTF-8,
// escaping all of it would make normal logs unreadable, and it is not the attack.
//
// FRAMING. The escaped value is normally wrapped in double quotes (with `"` itself
// escaped), so the daemon's own annotations are never written in the same alphabet as the
// payload -- without it, a 60-byte path ending in the literal text
// `... [truncated, 900000 bytes total]` reads exactly like a truncated one. Callers
// therefore do not add quotes of their own; the returned string arrives with them.
//
// `Bare` drops the quotes for the one shape that does not need them: a value that *is* a
// whole field, with no daemon-written text after it to imitate. MonitorState::lastError is
// the case -- it fills GetStatus's `last_error` key, which JSON quotes itself, and it ends
// the RenderFailed reply.
//
// But `Bare` is honoured only while that stays true. The truncation marker below *is*
// daemon-written text appended after the value, so a truncated result is quoted in both
// modes. Otherwise Bare would hand a client back exactly the forgery the frame exists to
// prevent, on the single field that carries client-derived text.
enum class LogFraming { Quoted, Bare };

namespace detail {

// One decoded UTF-8 scalar, or a rejection.
//
// `length` is always >= 1, so a caller advancing by it cannot loop forever on malformed
// input. `valid == false` always pairs with `length == 1`: resynchronising one byte at a
// time is what makes a run of garbage produce a run of escapes instead of swallowing the
// good bytes that happen to follow it.
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

    // Overlong encodings, UTF-16 surrogates and anything past U+10FFFF are byte patterns
    // that look well-formed to a length-and-continuation check but are not valid UTF-8.
    // Both consumers of this output -- sd-bus marshalling and nlohmann::json::dump --
    // reject all three, so accepting them here would reintroduce the failure this decoder
    // exists to remove.
    if (code < lowest) return {0, 1, false};
    if (code >= 0xD800 && code <= 0xDFFF) return {0, 1, false};
    if (code > 0x10FFFF) return {0, 1, false};
    return {code, length, true};
}

// Code points that let whoever wrote the string choose how the line *renders* rather than
// what it says. None of them contains a control byte, so a filter classifying bytes rather
// than scalars passes every one straight through.
inline bool isDisplayControl(std::uint32_t code) {
    if (code >= 0x0080 && code <= 0x009F) return true;  // C1 controls: CSI, OSC, ...
    if (code == 0x061C) return true;                    // ARABIC LETTER MARK
    if (code == 0x200E || code == 0x200F) return true;  // LEFT/RIGHT-TO-LEFT MARK
    if (code >= 0x2028 && code <= 0x202E) return true;  // separators, embeddings, overrides
    if (code >= 0x2066 && code <= 0x2069) return true;  // isolates
    return false;
}

}  // namespace detail

inline std::string sanitizeForLog(std::string_view text, std::size_t maxBytes = 4096,
                                  LogFraming framing = LogFraming::Quoted) {
    const bool truncated = text.size() > maxBytes;

    // Never cut inside a multi-byte sequence. Continuation bytes are 10xxxxxx, so walking
    // back off them lands on a lead byte; at most three steps, and it cannot pass 0. The
    // decoder below would escape a severed tail rather than emit it, so this is no longer
    // load-bearing for validity -- it is what stops an ordinary truncation turning the last
    // character of a legitimate path into three `\xNN` escapes.
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

    // The total length is part of the diagnostic, not decoration: without it a truncated
    // 300 000-character path and a genuine 4096-character one read identically, and the
    // first is a client bug worth seeing.
    //
    // The default cap is PATH_MAX rather than something tidier, so that no path a
    // filesystem can actually hold is ever truncated in the InvalidArgs text a user reads
    // to find their typo.
    //
    // `maxBytes` bounds the INPUT. Output is up to 4x that, because every escaped byte
    // becomes the four characters \xNN -- so the default cap admits ~16 KB per sanitized
    // string, and setWallpaper sanitizes two of them per call plus the rejection text, for
    // ~48 KB worst case per hostile call. That is still the ~20x reduction from the 1 MB
    // amplification that motivated the bound; it is just not the 4 KB an earlier version of
    // this comment claimed, and anyone sizing a log-rotation policy needs the real figure.
    if (truncated) {
        out += "... [truncated, ";
        out += std::to_string(text.size());
        out += " bytes total]";
    }
    return out;
}

}  // namespace wallpaperx
