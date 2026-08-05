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
inline std::string sanitizeForLog(std::string_view text, std::size_t maxBytes = 256) {
    const bool truncated = text.size() > maxBytes;
    const std::string_view head = truncated ? text.substr(0, maxBytes) : text;

    std::string out;
    out.reserve(head.size() + 16);
    for (const char raw : head) {
        const auto byte = static_cast<unsigned char>(raw);
        switch (byte) {
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (byte < 0x20 || byte == 0x7F) {
                    // Two hex digits, always -- \x7 and \x07 would be the same escape for
                    // different bytes if the width varied with the value.
                    static constexpr char kHex[] = "0123456789abcdef";
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
    // 300 000-character path and a genuine 256-character one read identically, and the
    // first is a client bug worth seeing.
    if (truncated) {
        out += "... [truncated, ";
        out += std::to_string(text.size());
        out += " bytes total]";
    }
    return out;
}

}  // namespace wallpaperx
