// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "device.h"
#include "drm_formats.h"
#include "memory.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace wallpaperx {

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
// formally still owns, which is undefined.
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

// One consumer's acquire, as a timeline semaphore: exported once and advanced once per picture,
// where exportFrameAcquireFd() above builds and exports a new binary semaphore every time. The
// consumer imports the payload once and waits on each picture's value; the queue-family acquire and
// layout still ride on that wait, as they do on the binary one.
//
// Every value signal() returns has already been reached when it returns, so a consumer never holds
// a value its payload will not get to -- not even after this producer dies, since an imported
// payload keeps its count. That is the only thing standing between a wrong value and a consumer
// whose GPU queue waits for ever: a GL consumer has no way to read the payload before it waits.
//
// GPU thread only, like everything that submits.
class AcquireTimeline {
public:
    // Whether `device` can export a timeline semaphore as an opaque descriptor.
    static bool supported(Device& device);

    // Throws runtime_error when !supported(device), and on the split exportFrame() uses.
    explicit AcquireTimeline(Device& device);
    ~AcquireTimeline();

    AcquireTimeline(const AcquireTimeline&) = delete;
    AcquireTimeline& operator=(const AcquireTimeline&) = delete;

    // A new descriptor naming this timeline's payload; the caller owns it. Each call is a new
    // descriptor for the same payload, so a consumer asking twice imports the same counter.
    int exportFd();

    // Signals the next value after everything submitted so far -- the render that published
    // `target` among it -- and waits for it. Throws runtime_error if the target has no published
    // frame, and GpuFailure when the wait does not complete; the value is then never handed out,
    // and the next call signals past it.
    uint64_t signal(const OffscreenTarget& target);

private:
    Device& m_device;
    VkSemaphore m_semaphore = VK_NULL_HANDLE;
    // The highest value ever submitted, reached or not: a timeline's signals must increase.
    uint64_t m_submitted = 0;
    // A signal whose wait did not complete, and so may still be pending when this is destroyed.
    std::optional<Device::SubmitTicket> m_unfinished;
};

}
