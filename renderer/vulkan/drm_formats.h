// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>

// The fourcc vocabulary the exporter and the importer must agree on, in a header that pulls in
// neither Vulkan nor EGL so both sides can include it. They used to hold private copies, which
// drifted: the exporter can emit 10-bit formats the importer refuses, and nothing named the other.

namespace wallpaperx {

// DRM_FORMAT_MOD_INVALID (from drm_fourcc.h, redefined here to avoid a libdrm
// dependency): the well-known "no/unknown modifier" sentinel. Used to mark
// ExportedFrame::modifier as not-populated -- distinct from 0, which is the
// valid DRM_FORMAT_MOD_LINEAR and would otherwise be mistaken for a real layout.
inline constexpr uint64_t kDrmModifierInvalid = (1ULL << 56) - 1;

// The most memory planes an exported dma_buf may span. The importer describes exactly one plane
// to EGL, so a two-plane layout -- the efficient one on AMD and Intel -- would export cleanly and
// then fail every import, permanently and with nothing reporting why.
inline constexpr uint32_t kMaxExportedPlanes = 1;

// DRM fourcc codes (from drm_fourcc.h, redefined to avoid a libdrm dependency). The
// DRM naming is little-endian channel order, so DRM's "ARGB8888" is the same byte
// layout as Vulkan's VK_FORMAT_B8G8R8A8_UNORM -- they are not a typo for each other.
inline constexpr uint32_t kDrmFormatArgb8888 = 'A' | ('R' << 8) | ('2' << 16) | ('4' << 24);
inline constexpr uint32_t kDrmFormatXrgb8888 = 'X' | ('R' << 8) | ('2' << 16) | ('4' << 24);
inline constexpr uint32_t kDrmFormatAbgr8888 = 'A' | ('B' << 8) | ('2' << 16) | ('4' << 24);
inline constexpr uint32_t kDrmFormatXbgr8888 = 'X' | ('B' << 8) | ('2' << 16) | ('4' << 24);
inline constexpr uint32_t kDrmFormatAbgr2101010 = 'A' | ('B' << 8) | ('3' << 16) | ('0' << 24);
inline constexpr uint32_t kDrmFormatXbgr2101010 = 'X' | ('B' << 8) | ('3' << 16) | ('0' << 24);

}  // namespace wallpaperx
