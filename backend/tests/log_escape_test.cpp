// S7.19: sanitizeForLog (backend/daemon/log_escape.h) returns valid UTF-8 for every input.
//
// Its own target rather than a phase of http_server_test, because sanitizeForLog is a pure
// function of bytes: this needs no session bus, no GPU and no port, so folding it into a
// tier that needs all three would make an exhaustive byte sweep cost a daemon startup and
// tie a pure-function regression to a host that has Vulkan.
//
// The property is load-bearing on three surfaces -- /config's path, GetStatus's path and
// the HTTP exception handler's `what` -- and until this file existed it was asserted on
// none of them. What makes it worth a goal rather than a spot check is the truncation case:
// a byte cap that splits a multi-byte character turns VALID input into invalid output, so
// the defect is reachable from an ordinary non-ASCII filename and not only from a hostile
// one.

#include "daemon/log_escape.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

using wallpaperx::LogFraming;
using wallpaperx::sanitizeForLog;

namespace {

int g_failures = 0;

void check(bool cond, const char* msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++g_failures;
    }
}

std::string toHex(std::string_view bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (const char c : bytes) {
        const auto b = static_cast<unsigned char>(c);
        out += kHex[b >> 4];
        out += kHex[b & 0x0F];
        out += ' ';
    }
    return out;
}

// A UTF-8 validator written from RFC 3629's table, not from log_escape.h's decoder. It is
// the second of two oracles on purpose -- see the annotation on runValidityChecks.
bool isValidUtf8(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        if (lead < 0x80) {
            i += 1;
            continue;
        }
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
            return false;
        }
        if (i + length > text.size()) return false;
        for (std::size_t j = 1; j < length; ++j) {
            const auto cont = static_cast<unsigned char>(text[i + j]);
            if ((cont & 0xC0) != 0x80) return false;
            code = (code << 6) | (cont & 0x3Fu);
        }
        if (code < lowest) return false;
        if (code >= 0xD800 && code <= 0xDFFF) return false;
        if (code > 0x10FFFF) return false;
        i += length;
    }
    return true;
}

// What the sweep actually did, so a future edit that narrows the corpus fails instead of
// going quiet. Counted, not estimated: the strength of a "for every input" check is exactly
// the number of inputs it saw.
struct Sweep {
    long long combinations = 0;
    long long truncatedOutputs = 0;  // outputs carrying the truncation marker
    long long escapingOutputs = 0;   // outputs carrying at least one \xNN escape
    long long rejected = 0;          // outputs that failed either oracle
    std::string firstRejectedInput;
    std::string firstRejectedWhy;
};

constexpr const char* kTruncationMarker = "... [truncated, ";

void sanitizeAndValidate(Sweep& sweep, std::string_view input, std::size_t cap, LogFraming framing) {
    const std::string out = sanitizeForLog(input, cap, framing);
    ++sweep.combinations;
    if (out.find(kTruncationMarker) != std::string::npos) ++sweep.truncatedOutputs;
    if (out.find("\\x") != std::string::npos) ++sweep.escapingOutputs;

    std::string why;
    if (!isValidUtf8(out)) {
        why = "the result is not valid UTF-8: " + toHex(out);
    } else {
        try {
            (void)nlohmann::json{{"v", out}}.dump();
        } catch (const std::exception& e) {
            why = std::string("json::dump() threw: ") + e.what();
        }
    }
    if (why.empty()) return;

    if (sweep.rejected == 0) {
        sweep.firstRejectedInput = toHex(input) + "(cap " + std::to_string(cap) + ", " +
                                   (framing == LogFraming::Quoted ? "Quoted" : "Bare") + ")";
        sweep.firstRejectedWhy = why;
    }
    ++sweep.rejected;
}

// GOAL: S7.19 -- for ANY byte sequence, any cap and either framing, sanitizeForLog returns
//       valid UTF-8, so nlohmann::json::dump() cannot throw on a client-supplied string.
//       False if any input makes the result ill-formed.
// WHY-THIS-SHAPE: two oracles, and json::dump() is the primary one -- it is literally the
//       failure the goal names, so a check that only ran a validator of our own would be
//       asserting our idea of UTF-8 rather than the one the daemon's replies are built by.
//       The local validator is second because dump()'s error handling is a library default
//       that a version bump could change to replace-and-continue, at which point a
//       dump()-only check would go vacuous without failing. It is written from RFC 3629
//       rather than derived from log_escape.h's decoder, so it is not the code under test
//       grading itself. The corpus is exhaustive over one and two bytes rather than a
//       handful of hostile literals: overlongs, severed leads, lone continuations and the
//       0xF8..0xFF range are all in there without anyone having to think of them.
// CONTROL: log_escape.h's invalid-scalar branch changed from escapeByte(head[i]) to
//       `out += head[i]` -> "FAIL: sanitizeForLog returns valid UTF-8 that json::dump()
//       accepts, for every input in the sweep". Reverted.
// COVERAGE-ASSERT: the combination count, the number of outputs that actually truncated and
//       the number that actually escaped something are all asserted below. A corpus edited
//       down to ASCII would satisfy the validity check trivially and fail those three.
void runValidityChecks(Sweep& sweep) {
    constexpr std::size_t kCaps[] = {0, 1, 2, 3, 4, 8, 1024, 4096};
    constexpr LogFraming kFramings[] = {LogFraming::Quoted, LogFraming::Bare};

    // Every single byte, at every cap and framing.
    for (int b = 0; b < 256; ++b) {
        const std::string one(1, static_cast<char>(b));
        for (const std::size_t cap : kCaps) {
            for (const LogFraming framing : kFramings) sanitizeAndValidate(sweep, one, cap, framing);
        }
    }

    // Every two-byte sequence. Caps 1 and 2 put the cut inside it; 4096 leaves it whole.
    for (int first = 0; first < 256; ++first) {
        for (int second = 0; second < 256; ++second) {
            std::string pair;
            pair += static_cast<char>(first);
            pair += static_cast<char>(second);
            for (const std::size_t cap : {std::size_t{1}, std::size_t{2}, std::size_t{4096}}) {
                sanitizeAndValidate(sweep, pair, cap, LogFraming::Quoted);
            }
        }
    }

    // Sequences a two-byte sweep cannot reach: overlong three- and four-byte encodings,
    // surrogates, past U+10FFFF, and a valid character with its tail cut off.
    static const std::vector<std::string> kNamed = {
        std::string("\xE0\x80\x80"),          // overlong NUL
        std::string("\xF0\x80\x80\x80"),      // overlong NUL, four bytes
        std::string("\xED\xA0\x80"),          // U+D800, a lone surrogate
        std::string("\xED\xBF\xBF"),          // U+DFFF
        std::string("\xF4\x90\x80\x80"),      // U+110000, past the last code point
        std::string("\xF7\xBF\xBF\xBF"),      // U+1FFFFF
        std::string("\xE2\x82"),              // U+20AC with its last byte missing
        std::string("\xF0\x9D\x84"),          // U+1D11E with its last byte missing
        std::string("\x80\x80\x80\x80"),      // continuations with no lead
        std::string("a\xC3\x28"),             // a bad continuation after a valid lead
        std::string("\xE2\x80\xAE" "x\x7F"),  // U+202E and DEL, the display-control path
        std::string("caf\xC3\xA9.png"),       // ordinary non-ASCII, the common case
    };
    for (const std::string& text : kNamed) {
        for (const std::size_t cap : kCaps) {
            for (const LogFraming framing : kFramings) sanitizeAndValidate(sweep, text, cap, framing);
        }
    }

    check(sweep.rejected == 0,
          "sanitizeForLog returns valid UTF-8 that json::dump() accepts, for every input in the sweep");
    if (sweep.rejected != 0) {
        std::fprintf(stderr, "  %lld of %lld combinations rejected; first was %s -- %s\n", sweep.rejected,
                     sweep.combinations, sweep.firstRejectedInput.c_str(), sweep.firstRejectedWhy.c_str());
    }

    // 256*8*2 + 65536*3 + 12*8*2 = 200,896.
    constexpr long long kExpectedCombinations = 200896;
    check(sweep.combinations == kExpectedCombinations,
          "the validity sweep covered every combination it is written to cover");
    check(sweep.truncatedOutputs > 0 && sweep.escapingOutputs > 0,
          "the sweep actually exercised truncation and escaping, rather than only bytes that pass through");
    if (sweep.combinations != kExpectedCombinations || sweep.truncatedOutputs == 0 || sweep.escapingOutputs == 0) {
        std::fprintf(stderr, "  %lld combinations (expected %lld), %lld truncated, %lld escaping\n",
                     sweep.combinations, kExpectedCombinations, sweep.truncatedOutputs, sweep.escapingOutputs);
    }
}

// GOAL: truncating VALID UTF-8 cuts at a character boundary, so an ordinary non-ASCII name
//       that happens to straddle the cap loses its last character rather than having it
//       rewritten as \xNN escapes. False if the result carries any escape at all here: the
//       input is printable ASCII plus one multi-byte character, none of which sanitizeForLog
//       has any other reason to escape.
// WHY-THIS-SHAPE: this is NOT covered by the validity sweep above, and cannot be. Cutting a
//       character in half leaves a severed lead byte, which the decoder rejects and escapes
//       byte by byte -- so the output stays valid UTF-8 and every oracle in runValidityChecks
//       is satisfied by a truncation that mangled the value. What distinguishes the two is
//       the exact result, so the whole string is compared rather than scanned for escapes.
//       Both framings, because truncation is specified to force the quotes back on even in
//       Bare mode, and asserting the exact text is what makes that testable at all.
// CONTROL: the `while (truncated && cut > 0 && ...)` back-off loop deleted from
//       sanitizeForLog -> "FAIL: truncating valid UTF-8 cuts at a character boundary rather
//       than severing a character into escapes", with `"aaaaaaaa\xe2\x82"...` against the
//       expected `"aaaaaaaa"...`. Reverted.
// COVERAGE-ASSERT: the straddle count is asserted. Every offset inside every character
//       length is what makes this a statement about the back-off rather than about one
//       lucky cap, and a corpus quietly reduced to the boundary-aligned offset 0 -- the one
//       case that passes with the back-off deleted -- fails the count instead.
void runTruncationBoundaryChecks() {
    static const std::vector<std::string> kChars = {
        std::string("\xC3\xA9"),          // U+00E9
        std::string("\xE2\x82\xAC"),      // U+20AC
        std::string("\xF0\x9D\x84\x9E"),  // U+1D11E
    };
    const std::string prefix(8, 'a');
    const std::string suffix(8, 'b');

    int straddles = 0;
    int mangled = 0;
    std::string firstMangled;
    for (const std::string& ch : kChars) {
        const std::string input = prefix + ch + suffix;
        for (std::size_t offset = 0; offset < ch.size(); ++offset) {
            const std::size_t cap = prefix.size() + offset;
            const std::string expected = "\"" + prefix + "\"" + kTruncationMarker + std::to_string(input.size()) +
                                         " bytes total]";
            for (const LogFraming framing : {LogFraming::Quoted, LogFraming::Bare}) {
                ++straddles;
                const std::string out = sanitizeForLog(input, cap, framing);
                if (out == expected) continue;
                if (mangled == 0) firstMangled = "got " + out + ", wanted " + expected;
                ++mangled;
            }
        }
    }

    check(mangled == 0,
          "truncating valid UTF-8 cuts at a character boundary rather than severing a character into escapes");
    if (mangled != 0) std::fprintf(stderr, "  %d of %d cuts mangled the value; first: %s\n", mangled, straddles,
                                   firstMangled.c_str());

    // (2 + 3 + 4) offsets * 2 framings.
    constexpr int kExpectedStraddles = 18;
    check(straddles == kExpectedStraddles, "every cut offset inside every character length was exercised");
    if (straddles != kExpectedStraddles) {
        std::fprintf(stderr, "  %d cut offsets exercised, expected %d\n", straddles, kExpectedStraddles);
    }
}

}  // namespace

int main() {
    // Nothing here depends on the host, so there is no skip path: every check can be
    // answered on any machine that can run the binary at all.
    Sweep sweep;
    try {
        runValidityChecks(sweep);
        runTruncationBoundaryChecks();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: unexpected exception: %s\n", e.what());
        ++g_failures;
    }

    if (g_failures > 0) {
        std::fprintf(stderr, "log_escape_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("log_escape_test: all checks passed (%lld sanitize/dump combinations, %lld truncated, %lld "
                "escaping)\n",
                sweep.combinations, sweep.truncatedOutputs, sweep.escapingOutputs);
    return 0;
}
