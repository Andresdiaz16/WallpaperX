// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>

// FFmpeg's own messages, made fit for the daemon's log. A damaged file is decoded again on every
// pass, and FFmpeg reports each bad packet of it, so left alone one wallpaper writes to the journal
// for as long as it plays.
namespace wallpaperx::ffmpeg_log {

// Warnings and worse only; the first kBurst lines, then one a kRefill. Lines held back are counted
// and the count rides on the next line let through.
constexpr int kBurst = 10;
constexpr std::chrono::seconds kRefill{60};

// Routes FFmpeg's messages to stderr, so through the log pump, one line per write with anything
// unsafe to print escaped. Call once, before any thread that can run FFmpeg exists:
// av_log_set_callback() is a plain store FFmpeg reads unsynchronised.
void install();

}  // namespace wallpaperx::ffmpeg_log
