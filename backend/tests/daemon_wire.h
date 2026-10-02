// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace wallpaperx::testing {

// The bus name, path and interface the plugin talks to. Literals rather than the daemon's own
// constants: what is under test is that the item speaks the wire the daemon declares, and taking
// both sides from one symbol would let a rename pass unnoticed.
inline constexpr const char* kService = "io.wallpaperx.Daemon";
inline constexpr const char* kPath = "/io/wallpaperx/Daemon";
inline constexpr const char* kInterface = "io.wallpaperx.Daemon";

}  // namespace wallpaperx::testing
