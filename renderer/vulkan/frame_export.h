// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "drm_formats.h"
#include "memory.h"

#include <array>
#include <cstdint>
#include <vector>

namespace wallpaperx {

class Device;
struct OffscreenTarget;

// The DRM fourcc naming a compositor needs for `format`, or 0 if this format has no
// mapping here. Returning 0 rather than guessing is deliberate: an importer told the
// wrong fourcc renders swapped channels or garbage, whereas 0 means "unknown" and
// makes the export fail loudly. Callers must treat 0 as an error, not a default.
uint32_t drmFourccForVkFormat(VkFormat format);

// Where one plane of a DMA-BUF frame lives within the exported memory. Multi-planar
// modifiers (common on Intel, e.g. compressed layouts with a separate metadata plane)
// need one of these per plane, which is why this is a list rather than a single
// stride -- a scalar stride silently describes only single-plane layouts.
struct ExportedFramePlane {
    uint64_t offset = 0;
    uint64_t rowPitch = 0;
};

struct ExportedFrame {
    enum class HandleType { OpaqueFd, DmaBuf };

    int fd = -1;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t memorySize = 0;
    HandleType type = HandleType::OpaqueFd;

    // The channel order, reported on BOTH paths: an opaque fd carries no layout but its bytes
    // still have an order, and an importer that guesses draws red and blue transposed.
    uint32_t drmFormat = 0;

    // Which GPU allocated this memory. Reported on both paths, enforced only on the opaque
    // one, where importing on any other device cannot work.
    std::array<uint8_t, VK_UUID_SIZE> deviceUuid{};

    // Which image these bytes are, distinct from `generation`, which counts renders. A
    // consumer holding several buffers keys its imports on this: the same id is the same
    // memory and needs no second import, a new id is a buffer it has never seen.
    uint64_t bufferId = 0;

    // DMA-BUF layout metadata (AMD/Intel), describing how to interpret the bytes
    // behind `fd`. Populated on the DmaBuf path only; the OpaqueFd (NVIDIA) path
    // leaves them at their unset sentinels because an opaque fd is importable only
    // by another Vulkan/GL context on the same device, which already knows the
    // layout. Note that modifier's sentinel is kDrmModifierInvalid rather than 0 --
    // 0 is the valid DRM_FORMAT_MOD_LINEAR, so a zero default would be
    // indistinguishable from a genuine linear layout.
    uint64_t modifier = kDrmModifierInvalid;
    std::vector<ExportedFramePlane> planes;
};

// Exports target.image's backing memory as a new file descriptor. Every call
// produces a fresh fd via a fresh vkGetMemoryFdKHR call (Vulkan's own contract
// -- see Key Gotchas: "Reusing fd after import" in PLAN.md); the caller owns
// the returned fd and must close() it if it isn't handed off to an importer.
//
// Throws GpuFailure when the driver is what failed -- the export entry point missing, a
// modifier absent from the driver's own list, a result the fd query is not permitted to
// return -- and a plain runtime_error for everything else here: a broken invariant, a
// layout the consumer refused, or the host running out of file descriptors. The daemon
// marks its GPU unhealthy on the first and not the second.
//
// Reads the target, never writes it, and submits nothing: the ownership release that
// makes the contents readable outside this device is part of the render that produced
// them (OffscreenTargetManager::render). Exporting a target no render has published
// hands out memory the graphics queue still owns.
//
// It does wait, though, for every submission outstanding on the device: renders do not, so
// this is what makes the returned descriptor name bytes that have actually been drawn. A
// receiver reading them on the host has no other ordering available, and one reading them on
// a GPU still needs the acquire below for ownership and layout.
ExportedFrame exportFrame(Device& device, const OffscreenTarget& target);

// A descriptor the consumer waits on before it samples the frame `target` published, and which
// carries the queue-family acquire reversing that render's release. -1 when this device has no
// VK_KHR_external_semaphore_fd, which leaves the consumer reading memory the graphics queue
// formally still owns -- undefined, and the state everything was in before this existed.
//
// Already signalled when it is returned, which is what makes a binary semaphore safe to send
// across a process boundary: it is signalled by its own submit here and the descriptor is taken
// afterwards, so a producer that dies next leaves nothing for the consumer to wait on forever.
//
// One payload per call, never shared. A binary semaphore is consumed by the first wait, so two
// consumers -- or one consumer fetching the same generation twice, which the KDE plugin does on
// every scene change -- must each be given their own.
//
// Submits, unlike exportFrame(), and that is the point: a semaphore signal's first synchronization
// scope covers everything earlier in submission order on the queue, so this is what puts the
// render's release before the consumer's acquire. It publishes nothing and touches no target
// state.
//
// The caller owns the descriptor and must close() it. Throws on the same split exportFrame() uses;
// throws runtime_error if the target has no published frame, which is a caller bug.
int exportFrameAcquireFd(Device& device, const OffscreenTarget& target);

}
