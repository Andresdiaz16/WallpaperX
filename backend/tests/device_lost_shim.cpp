// SPDX-License-Identifier: GPL-2.0-or-later

// Preloaded into the real daemon to lose its GPU device on demand; a real hang would take the
// desktop down with it.
//
//   WALLPAPERX_TEST_LOSE_DEVICE=<path>  once <path> exists, every vkQueueSubmit the daemon makes
//   returns VK_ERROR_DEVICE_LOST for good; removing the file does not undo it. The first such
//   submission creates <path>.fired.
//
// Only the result is faked, at the driver boundary, so the daemon takes a real loss's path; the
// batch never reaches the driver, so teardown frees a healthy device. ffmpeg's decode submits go
// through a table it fetched from the loader and are not intercepted; the daemon's next submit is.

#include "support/real_entry_point.h"

#include <vulkan/vulkan.h>

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace {

std::atomic<bool> g_lost{false};

bool deviceLost() {
    if (g_lost.load(std::memory_order_acquire)) return true;
    const char* trigger = std::getenv("WALLPAPERX_TEST_LOSE_DEVICE");
    if (trigger == nullptr || *trigger == '\0' || ::access(trigger, F_OK) != 0) return false;
    if (!g_lost.exchange(true, std::memory_order_acq_rel)) {
        // "<trigger>.fired", and the line through a non-blocking description of its own: a test
        // that stalled stderr needs to know the loss happened without the render thread blocking here.
        char fired[4096];
        if (std::snprintf(fired, sizeof(fired), "%s.fired", trigger) < static_cast<int>(sizeof(fired))) {
            const int marker = ::open(fired, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
            if (marker >= 0) ::close(marker);
        }
        static constexpr char kLosing[] = "device_lost_shim: failing every queue submission from now on\n";
        const int err = ::open("/proc/self/fd/2", O_WRONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
        const ssize_t ignored = ::write(err >= 0 ? err : STDERR_FILENO, kLosing, sizeof(kLosing) - 1);
        (void)ignored;
        if (err >= 0) ::close(err);
    }
    return true;
}

}  // namespace

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue queue, uint32_t submitCount,
                                                        const VkSubmitInfo* submits, VkFence fence) {
    static const auto real = wallpaperx::testing::realEntryPoint<PFN_vkQueueSubmit>("device_lost_shim", "vkQueueSubmit");
    if (deviceLost()) return VK_ERROR_DEVICE_LOST;
    return real(queue, submitCount, submits, fence);
}
