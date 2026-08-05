#pragma once

#include <cstddef>
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
// Bytes >= 0x80 pass through unescaped: a wallpaper path is very often UTF-8, escaping it
// would make ordinary logs unreadable, and it is not the attack. C0 controls (which is
// where newline, carriage return and ESC live), DEL, and the backslash that introduces an
// escape are what get replaced.
//
// PRECONDITION, and it is what makes the pass-through safe: `text` is valid UTF-8. Every
// caller passes a string that arrived over the bus, and sd-bus validates STRING arguments
// as UTF-8 before a handler ever sees them. That matters in both directions. It lets this
// function leave multi-byte sequences alone, and it obliges it not to *create* invalid
// UTF-8 -- the output is spliced into D-Bus error replies, which must themselves be valid
// UTF-8 or they do not marshal, and a reply that fails to marshal leaves the caller with
// no error at all, blocked until its method timeout. Hence the boundary back-off below.
// If this is ever pointed at bytes that are not known-good UTF-8 -- a raw path read off
// the filesystem, which on Linux is arbitrary bytes -- backing off is not enough and the
// loop must escape ill-formed sequences too.
//
// Two things beyond the C0 controls are handled, because without them the client still
// chooses how the line reads:
//
//   - **The Unicode formatting and separator code points.** U+202A..U+202E and
//     U+2066..U+2069 reorder the *display* of everything after them, and U+2028/U+2029 are
//     honoured as line breaks by a great many log viewers. All are >= 0x80 and none
//     contains a control byte, so a byte-classifying filter passes them straight through.
//   - **A frame the client cannot close.** The escaped value is wrapped in double quotes
//     (with `"` itself escaped), and the truncation marker is appended *outside* the
//     closing quote. Otherwise the daemon's own annotations are written in the same
//     alphabet as the payload: a 60-byte path ending in the literal text
//     `... [truncated, 900000 bytes total]` reads exactly like a truncated one. Callers
//     therefore do not add quotes of their own -- the returned string arrives with them.
//
// `Bare` framing exists for the one shape that does not need the frame: a value that *is*
// a whole field, with no daemon-written text after it to imitate. MonitorState::lastError
// is the case -- it fills GetStatus's `last_error` key, which JSON quotes itself, and it
// ends the RenderFailed reply. Quoting there would show the user `"StaticRenderer: ..."`
// inside an already-quoted JSON string and buy nothing.
enum class LogFraming { Quoted, Bare };

inline std::string sanitizeForLog(std::string_view text, std::size_t maxBytes = 4096,
                                  LogFraming framing = LogFraming::Quoted) {
    const bool quoted = framing == LogFraming::Quoted;
    const bool truncated = text.size() > maxBytes;

    // Never cut inside a multi-byte sequence. Continuation bytes are 10xxxxxx, so walking
    // back off them lands on a lead byte; at most three steps, and it cannot pass 0.
    std::size_t cut = maxBytes;
    while (truncated && cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    const std::string_view head = truncated ? text.substr(0, cut) : text;

    // As UTF-8 encodings: U+2028..U+202E is E2 80 A8..AE (LINE SEPARATOR, PARAGRAPH
    // SEPARATOR, then the four bidi embeds/overrides and POP), and U+2066..U+2069 is
    // E2 81 A6..A9 (the isolates). Matched on the encoded bytes rather than by decoding to
    // code points, because those two three-byte runs are the whole set and a decoder here
    // would be more machinery than the thing it decides.
    auto displayControlAt = [&head](std::size_t i) {
        if (i + 2 >= head.size() || static_cast<unsigned char>(head[i]) != 0xE2) return false;
        const auto second = static_cast<unsigned char>(head[i + 1]);
        const auto third = static_cast<unsigned char>(head[i + 2]);
        if (second == 0x80) return third >= 0xA8 && third <= 0xAE;
        if (second == 0x81) return third >= 0xA6 && third <= 0xA9;
        return false;
    };

    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(head.size() + 24);
    if (quoted) out += '"';
    for (std::size_t i = 0; i < head.size(); ++i) {
        if (displayControlAt(i)) {
            // Escaped as the three bytes it is, so the escape is reversible and says
            // exactly what was on the wire.
            for (std::size_t j = i; j < i + 3; ++j) {
                const auto b = static_cast<unsigned char>(head[j]);
                out += "\\x";
                out += kHex[b >> 4];
                out += kHex[b & 0x0F];
            }
            i += 2;
            continue;
        }
        const char raw = head[i];
        const auto byte = static_cast<unsigned char>(raw);
        switch (byte) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (byte < 0x20 || byte == 0x7F) {
                    // Two hex digits, always -- \x7 and \x07 would be the same escape for
                    // different bytes if the width varied with the value.
                    out += "\\x";
                    out += kHex[byte >> 4];
                    out += kHex[byte & 0x0F];
                } else {
                    out += raw;
                }
                break;
        }
    }

    // The total length is part of the diagnostic, not decoration: without it a truncated
    // 300 000-character path and a genuine 4096-character one read identically, and the
    // first is a client bug worth seeing.
    //
    // The default cap is PATH_MAX rather than something tidier, so that no path a
    // filesystem can actually hold is ever truncated in the InvalidArgs text a user reads
    // to find their typo. It still removes the amplification that motivated the bound: a
    // 1 MB path costs 4 KB per log line instead of 1 MB, and it is the ~250x that matters,
    // not the last factor of ten.
    if (quoted) out += '"';
    if (truncated) {
        out += "... [truncated, ";
        out += std::to_string(text.size());
        out += " bytes total]";
    }
    return out;
}

}  // namespace wallpaperx
