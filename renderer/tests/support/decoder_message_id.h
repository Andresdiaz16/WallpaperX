// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The one validation message ffmpeg's Vulkan decoder raises that this project excuses, and
// nothing else.
//
// Split out of video_probe_support.h so the backend's tests can name it too. That header brings
// in the renderer probes' own reporter (support/check.h), which declares a second `check()` and
// `g_failures` in the same namespace as backend/tests/test_report.h -- so a backend test that
// included it would not compile, and a backend test that re-typed the id would be the second
// copy of the thing this project keeps in one place on purpose.
//
// Dependency-free by design: anything added here is added to every test that excuses the message.

namespace wallpaperx::testing {

// ffmpeg's Vulkan decoder binds one reference slot per DPB picture where this driver's profile
// wants a single layered image. Reproducible with the stock `ffmpeg -hwaccel vulkan` command
// line, so it is a defect in a dependency. Named exactly, so any other complaint still fails.
inline constexpr const char* kExcusedDecoderMessageId = "VUID-VkVideoBeginCodingInfoKHR-flags-07244";

}  // namespace wallpaperx::testing
