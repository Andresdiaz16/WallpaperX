// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>

namespace wallpaperx {

// How deep a monitor's ring of images is, and what makes it that deep.
//
// Here rather than inside WallpaperManager because the KDE plugin sizes its import cache against
// this number: a cache shallower than the ring is refilled every time the ring comes round, which
// is the per-picture import the cache exists to remove, and nothing fails or logs when that
// happens. That header cannot be included from the plugin -- it pulls in Vulkan, which the plugin
// neither links nor needs -- so the number lives where both sides can read it. Raising the depth
// now also raises what each consumer keeps imported.

// How many of a monitor's images one consumer may hold at once. Two, because a consumer
// that has fetched a new frame goes on sampling the old one until the new one is on screen.
//
// A third hand-over is REFUSED, not made room for: the daemon has no evidence the consumer
// has stopped reading its oldest image, and drawing into one it is still sampling is the
// tearing this ring exists to prevent. What the cap buys is the depth below -- without a
// per-consumer bound, no depth guarantees a second consumer anything.
inline constexpr std::size_t kMaxHeldPerClient = 2;

// Consumers a monitor is sized for. Two: the desktop, and one auxiliary reader -- a
// screenshot tool, a settings preview, a debugging fetch over busctl.
inline constexpr std::size_t kFullConsumers = 2;

// Images per monitor: the producer's reserve, plus a full hold for each consumer sized for.
// Nothing is ever taken back, so a consumer that fetches its fill and goes quiet keeps those
// images for the life of the daemon; at depth three that one consumer left the desktop
// refused forever, with no release coming to announce that it may ask again. A third silent
// consumer cannot be survived at any depth for the same reason, which is where the
// arithmetic stops paying: each one costs ~8 MB per monitor at 1080p.
inline constexpr std::uint32_t kTargetDepth = 1 + static_cast<std::uint32_t>(kFullConsumers * kMaxHeldPerClient);

}  // namespace wallpaperx
