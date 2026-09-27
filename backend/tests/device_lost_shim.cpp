// SPDX-License-Identifier: GPL-2.0-or-later

// Preloaded into the real daemon to lose its GPU device on demand; a real hang would take the
// desktop down with it.
//
//   WALLPAPERX_TEST_LOSE_DEVICE=<path>  once <path> exists, every vkQueueSubmit the daemon makes
//   returns VK_ERROR_DEVICE_LOST for good; removing the file does not undo it. The first such
//   submission creates <path>.fired.
//   WALLPAPERX_TEST_HOLD_SUBMIT=<path>  every vkQueueSubmit waits until <path> exists, creating
//   <path>.held as it starts to, then goes on as above: a render held at its submission until the
//   test has staged what the loss should meet.
//   WALLPAPERX_TEST_HANG_RECORD_CLOSE=1 once a submission has been failed, closing a descriptor
//   open on an applying-*.json never returns: the render that saw the loss is still inside its
//   applying record when the stop's deadline ends the process.
//
// Only the result is faked, at the driver boundary, so the daemon takes a real loss's path; the
// batch never reaches the driver, so teardown frees a healthy device. ffmpeg's decode submits go
// through a table it fetched from the loader and are not intercepted; the daemon's next submit is.

#include "support/real_entry_point.h"

#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

std::atomic<bool> g_lost{false};

// The line, never blocking and never over earlier output; on a stream nobody drains it is dropped.
// A file is written through fd 2 itself: a reopen would get an offset of its own, 0, and write
// over the start of the log. A socket (journald's stream) cannot be reopened through /proc, so it
// gets MSG_DONTWAIT; anything else a non-blocking description of its own.
void announce() {
    static constexpr char kLosing[] = "device_lost_shim: failing every queue submission from now on\n";
    constexpr size_t kSize = sizeof(kLosing) - 1;
    struct stat st {};
    if (::fstat(STDERR_FILENO, &st) != 0) return;
    ssize_t ignored = 0;
    if (S_ISREG(st.st_mode)) {
        ignored = ::write(STDERR_FILENO, kLosing, kSize);
    } else if (S_ISSOCK(st.st_mode)) {
        ignored = ::send(STDERR_FILENO, kLosing, kSize, MSG_DONTWAIT | MSG_NOSIGNAL);
    } else {
        const int err = ::open("/proc/self/fd/2", O_WRONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
        if (err < 0) return;
        ignored = ::write(err, kLosing, kSize);
        ::close(err);
    }
    (void)ignored;
}

bool deviceLost() {
    if (g_lost.load(std::memory_order_acquire)) return true;
    const char* trigger = std::getenv("WALLPAPERX_TEST_LOSE_DEVICE");
    if (trigger == nullptr || *trigger == '\0' || ::access(trigger, F_OK) != 0) return false;
    if (!g_lost.exchange(true, std::memory_order_acq_rel)) {
        // "<trigger>.fired", then the line: a test that stalled stderr needs to know the loss
        // happened without the render thread blocking here.
        char fired[4096];
        if (std::snprintf(fired, sizeof(fired), "%s.fired", trigger) < static_cast<int>(sizeof(fired))) {
            const int marker = ::open(fired, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
            if (marker >= 0) ::close(marker);
        }
        announce();
    }
    return true;
}

void holdSubmission() {
    const char* gate = std::getenv("WALLPAPERX_TEST_HOLD_SUBMIT");
    if (gate == nullptr || *gate == '\0' || ::access(gate, F_OK) == 0) return;
    char held[4096];
    if (std::snprintf(held, sizeof(held), "%s.held", gate) < static_cast<int>(sizeof(held))) {
        const int marker = ::open(held, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
        if (marker >= 0) ::close(marker);
    }
    const timespec pause{0, 1000000};
    while (::access(gate, F_OK) != 0) ::nanosleep(&pause, nullptr);
}

bool isApplyingRecord(int fd) {
    char link[64];
    char target[4096];
    std::snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    const ssize_t n = ::readlink(link, target, sizeof(target) - 1);
    if (n <= 0) return false;
    target[n] = '\0';
    return std::strstr(target, "/applying-") != nullptr;
}

}  // namespace

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue queue, uint32_t submitCount,
                                                        const VkSubmitInfo* submits, VkFence fence) {
    static const auto real = wallpaperx::testing::realEntryPoint<PFN_vkQueueSubmit>("device_lost_shim", "vkQueueSubmit");
    holdSubmission();
    if (deviceLost()) return VK_ERROR_DEVICE_LOST;
    return real(queue, submitCount, submits, fence);
}

extern "C" int close(int fd) {
    using Fn = int (*)(int);
    static const Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "close"));
    const char* hang = std::getenv("WALLPAPERX_TEST_HANG_RECORD_CLOSE");
    if (hang != nullptr && std::strcmp(hang, "1") == 0 && g_lost.load(std::memory_order_acquire) && isApplyingRecord(fd)) {
        for (;;) ::pause();
    }
    return real != nullptr ? real(fd) : -1;
}
