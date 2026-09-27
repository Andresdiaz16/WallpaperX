// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <cstddef>

// Makes the process's stdout and stderr unable to block it. Descriptors 1 and 2 become pipes (one
// shared pipe when both reach the same file, which keeps their lines in the order written) that
// one thread drains into the original destinations without ever blocking on them, so every writer
// -- stdio, iostreams, a raw write(2), ffmpeg, the Vulkan loader -- stays non-blocking with no call
// site knowing. While a destination is not accepting, whole lines queue up to a fixed bound and
// the newest are dropped past it; the count is written in their place once it accepts again.
namespace wallpaperx::logpump {

// Once, first thing in main, before any other thread exists and before anything is written.
// A closed stdout or stderr is replaced by /dev/null, so no later open() can take its number, and
// its lines are discarded. False (with nothing but that replacement done) if it could not start.
bool install();

// What a process on its way out gives the destinations to take what is queued: long enough for a
// reader that is merely slow, short enough that a stalled one barely delays the exit.
constexpr std::chrono::milliseconds kExitFlush{250};

// Waits at most `limit` for everything written to descriptors 1 and 2 so far to reach the
// destinations. An unfinished last line is ended. Returns at once if install() never succeeded,
// or on the pump's own thread, which cannot wait for itself.
// Async-signal-safe, so a handler can give what the process wrote last a chance to get out.
// Does not flush stdio's own buffers.
void flush(std::chrono::milliseconds limit) noexcept;

// Hands descriptors 1 and 2 back to the original destinations, then flush(kExitFlush) for what the
// pipes still hold. For what runs after main returns, whose output would otherwise die in the pipe
// with the process. Writes after this block on a stalled destination, as they would without the
// pump, so it is no call for a path that must end in bounded time while other threads run.
// Async-signal-safe; stdio is not flushed.
void release() noexcept;

// Async-signal-safe: `line` straight to stderr's destination, bypassing the queue, or dropped if
// that would block. For a handler about to _Exit, where anything queued is lost anyway.
void writeErrNow(const char* line, size_t size) noexcept;
template <size_t N>
void writeErrNow(const char (&line)[N]) noexcept {
    writeErrNow(line, N - 1);
}

}  // namespace wallpaperx::logpump
