// SPDX-License-Identifier: GPL-2.0-or-later

#include "ffmpeg_log.h"

#include "log_escape.h"

extern "C" {
#include <libavutil/log.h>
}

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

namespace wallpaperx::ffmpeg_log {

namespace {

constexpr int kLevel = AV_LOG_WARNING;
constexpr std::size_t kMaxLine = 1024;

struct State {
    // FFmpeg logs from its decoder threads as well as the caller's.
    std::mutex mutex;
    // A line FFmpeg is still writing: one message may come in several calls.
    char pending[kMaxLine]{};
    std::size_t pendingSize = 0;
    int printPrefix = 1;
    int tokens = kBurst;
    std::chrono::steady_clock::time_point refilled{};
    std::uint64_t held = 0;
};

// Nothing to destroy, so a decoder thread still logging while the process exits finds it intact.
constinit State g_state;

bool takeToken(State& s, std::chrono::steady_clock::time_point now) {
    if (s.tokens == kBurst) {
        s.refilled = now;
    } else {
        const auto earned = (now - s.refilled) / kRefill;
        s.tokens = static_cast<int>(std::min<std::int64_t>(kBurst, s.tokens + earned));
        s.refilled += earned * kRefill;
    }
    if (s.tokens == 0) return false;
    --s.tokens;
    return true;
}

void writeLine(State& s, std::string_view text) {
    try {
        const std::string line = sanitizeForLog(text, kMaxLine, LogFraming::Bare);
        if (s.held != 0) {
            std::fprintf(stderr, "wallpaperx-daemon: ffmpeg (%llu more not logged): %s\n",
                         static_cast<unsigned long long>(s.held), line.c_str());
        } else {
            std::fprintf(stderr, "wallpaperx-daemon: ffmpeg: %s\n", line.c_str());
        }
        s.held = 0;
    } catch (...) {
        // Out of memory: the line is lost, never the decoder that sent it.
        ++s.held;
    }
}

void onMessage(void* context, int level, const char* format, va_list args) {
    if (level > kLevel) return;
    State& s = g_state;
    std::lock_guard<std::mutex> lock(s.mutex);

    char piece[kMaxLine];
    const int length = av_log_format_line2(context, level, format, args, piece, sizeof piece, &s.printPrefix);
    if (length > 0) {
        const std::size_t room = kMaxLine - s.pendingSize;
        const std::size_t taken = std::min({static_cast<std::size_t>(length), sizeof piece - 1, room});
        std::memcpy(s.pending + s.pendingSize, piece, taken);
        s.pendingSize += taken;
    }
    if (s.pendingSize == 0) return;
    const bool ended = s.pending[s.pendingSize - 1] == '\n';
    if (!ended && s.pendingSize < kMaxLine) return;

    const std::string_view text(s.pending, ended ? s.pendingSize - 1 : s.pendingSize);
    s.pendingSize = 0;
    if (takeToken(s, std::chrono::steady_clock::now())) {
        writeLine(s, text);
    } else {
        ++s.held;
    }
}

}  // namespace

void install() { av_log_set_callback(onMessage); }

}  // namespace wallpaperx::ffmpeg_log
