// Device thread-affinity probe -- GOALS.md S2.11.
//
// `Device` aborts on any GPU call from a thread other than the one that created it.
// Nothing tested that. `assertGpuThread` has four production call sites and, until this
// file, zero test callers, so the guard could have been gutted to a no-op and every
// suite in the project would have stayed green.
//
// It is worth more than an ordinary invariant. GOALS.md S6.8 -- "no DBus handler touches
// the GPU" -- has no test of its own and cannot have one: the violating configuration
// kills the daemon before any assertion can run. Its entire enforcement story is this
// guard. So a silent regression here would not merely lose one check; it would quietly
// remove the mechanism another goal is documented as relying on.
//
// WHY A SEPARATE BINARY: the guard's whole behaviour is `std::abort()`. A case cannot be
// run in-process next to other checks, because the first one to fire takes the process
// down with every remaining check unreported. Each negative case therefore gets its own
// forked child whose only job is to die correctly.
//
// WHY THE FORK COMES FIRST: each child builds its own `Device` after forking, so no
// Vulkan state is ever inherited across a fork. The parent holds no device at all while
// the negative cases run -- the in-process positive case is deliberately last.
//
// NO CONTROL PATCH IS NEEDED, AND THAT IS THE POINT. The usual way to show a check can
// fail is to break the thing it guards and watch it go red, which means editing
// production code. Here the negative case *is* that mutation, expressed as a permanent
// test: the guard either fires or the child exits 0 and the check fails. Stubbing
// `assertGpuThread` to return early turns every negative case below red, which is the
// regression nothing previously caught -- but it is a property of the test, not a step
// anyone has to remember to run.

#include "vulkan/device.h"
#include "vulkan/frame_export.h"
#include "vulkan/offscreen.h"
#include "vulkan/validation_probe.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace {

using wallpaperx::Device;
using wallpaperx::OffscreenTargetManager;

// The sentence Device::assertGpuThread prints before aborting. Asserted alongside the
// signal because SIGABRT on its own is not evidence the guard fired: a null dereference
// in a broken child, a failed VMA assertion, or a validation-layer abort would all
// present identically. Matching the message is what separates "the guard fired" from
// "something died".
constexpr const char* kGuardSentence = "was made from a thread other than the one that created the Device";

constexpr uint32_t kWidth = 64;
constexpr uint32_t kHeight = 64;
constexpr int kMonitor = 0;

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

struct ChildResult {
    bool crashed = false;      // died by a signal
    int signal = 0;            // which one
    bool exited = false;       // returned normally
    int exitCode = -1;
    std::string stderrText;
};

// Runs `body` in a forked child with its stderr captured, and reports how it died.
//
// The child never returns from here: if `body` completes without aborting -- which is
// exactly the regression this file exists to catch -- it _exit(0)s, and the parent sees
// `exited` rather than `crashed`. _exit rather than exit so the parent's atexit handlers
// and static destructors do not run twice.
ChildResult runInChild(const std::function<void()>& body) {
    ChildResult result;

    int fds[2];
    if (pipe(fds) != 0) {
        result.stderrText = "pipe() failed";
        return result;
    }

    // Flushed before the fork, not after, and it is not cosmetic. When stdout is a pipe
    // -- which is how ctest runs this -- it is fully buffered, so everything printed so
    // far is still sitting in the parent's buffer. fork() copies that buffer into the
    // child, and the child flushes it on its way out, so every earlier "ok" line is
    // reprinted once per case. A reader (or a log scraper) sees each check reported
    // several times, with the count depending on how the output happens to be piped.
    std::fflush(stdout);

    const pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        result.stderrText = "fork() failed";
        return result;
    }

    if (pid == 0) {
        close(fds[0]);
        if (dup2(fds[1], STDERR_FILENO) == -1) _exit(126);
        close(fds[1]);
        body();
        // Reached only when the guard did not fire.
        _exit(0);
    }

    close(fds[1]);
    // Drained to EOF *before* waitpid. The other order deadlocks if the child ever writes
    // more than a pipe buffer: the child blocks in write(), the parent blocks in wait().
    // The guard's message is far short of that today, and relying on it would be a
    // latent hang for whoever makes the message longer.
    std::string captured;
    char buffer[512];
    while (true) {
        const ssize_t n = read(fds[0], buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        captured.append(buffer, static_cast<size_t>(n));
    }
    close(fds[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }

    result.stderrText = std::move(captured);
    if (WIFSIGNALED(status)) {
        result.crashed = true;
        result.signal = WTERMSIG(status);
    } else if (WIFEXITED(status)) {
        result.exited = true;
        result.exitCode = WEXITSTATUS(status);
    }
    return result;
}

// Asserts that a child died the way the guard makes things die, and names every other
// outcome distinguishably. A child that exits 0 means the guard did not fire; a child
// that dies by some other signal, or by SIGABRT without the guard's message, means
// something else killed it and this case proved nothing either way.
void expectGuardAbort(const ChildResult& result, const char* label, const char* what) {
    if (result.exited) {
        check(false, what);
        std::fprintf(stderr, "  the call returned instead of aborting (child exited %d) -- the guard did not fire\n",
                     result.exitCode);
        return;
    }
    if (!result.crashed) {
        check(false, what);
        std::fprintf(stderr, "  child neither exited nor died by a signal: %s\n", result.stderrText.c_str());
        return;
    }
    if (result.signal != SIGABRT) {
        check(false, what);
        std::fprintf(stderr, "  child died by signal %d (%s), not SIGABRT -- not the guard\n", result.signal,
                     strsignal(result.signal));
        return;
    }
    if (result.stderrText.find(kGuardSentence) == std::string::npos) {
        check(false, what);
        std::fprintf(stderr, "  child aborted, but not from the affinity guard. Its stderr was:\n  %s\n",
                     result.stderrText.c_str());
        return;
    }
    // The `what` label the call site passes to assertGpuThread. Checked so a case cannot
    // pass on some *other* guarded call aborting first -- which is how a test like this
    // rots into asserting only that the Device is pinned at all.
    if (result.stderrText.find(label) == std::string::npos) {
        check(false, what);
        std::fprintf(stderr, "  the guard fired, but named a different call than '%s':\n  %s\n", label,
                     result.stderrText.c_str());
        return;
    }
    check(true, what);
}

// Calls `body` on a thread that is not the one that created the device, and joins it.
// The thread is expected to abort the whole process from inside body -- the join is for
// the case where it does not, so the child reaches its _exit(0) and reports the miss.
void onAnotherThread(const std::function<void()>& body) {
    std::thread other(body);
    other.join();
}

}  // namespace

int main() {
    std::printf("device_affinity_probe: Device is pinned to its creating thread (S2.11)\n");

    // ---- Negative cases. Each is its own process, and each must die by the guard. ----

    expectGuardAbort(runInChild([] {
                         Device device(/*enableValidation=*/true);
                         OffscreenTargetManager targets(device);
                         onAnotherThread([&] { targets.create(kMonitor, kWidth, kHeight); });
                     }),
                     "OffscreenTargetManager::create",
                     "creating an offscreen target from a non-GPU thread aborts");

    expectGuardAbort(runInChild([] {
                         Device device(/*enableValidation=*/true);
                         OffscreenTargetManager targets(device);
                         targets.create(kMonitor, kWidth, kHeight);  // correct thread
                         onAnotherThread([&] { targets.destroy(kMonitor); });
                     }),
                     "OffscreenTargetManager::destroyTarget",
                     "destroying an offscreen target from a non-GPU thread aborts");

    expectGuardAbort(runInChild([] {
                         Device device(/*enableValidation=*/true);
                         onAnotherThread([&] { device.immediateSubmit([](VkCommandBuffer) {}); });
                     }),
                     "Device::immediateSubmit",
                     "submitting GPU work from a non-GPU thread aborts");

    expectGuardAbort(runInChild([] {
                         Device device(/*enableValidation=*/true);
                         OffscreenTargetManager targets(device);
                         const auto& target = targets.create(kMonitor, kWidth, kHeight);
                         onAnotherThread([&] { (void)wallpaperx::exportFrame(device, target); });
                     }),
                     "exportFrame", "exporting a frame from a non-GPU thread aborts");

    // ---- The handoff WallpaperManager's destructor depends on. ----
    //
    // ~WallpaperManager runs on the thread that owns the manager, not the render worker
    // that created the Device, and destroying the targets is GPU work. adoptGpuThread()
    // is what makes that legal. Both directions matter: after the handoff the new thread
    // must be allowed through, and -- the half that would otherwise go untested -- the
    // old one must not.
    expectGuardAbort(runInChild([] {
                         Device device(/*enableValidation=*/true);
                         OffscreenTargetManager targets(device);
                         targets.create(kMonitor, kWidth, kHeight);
                         std::thread adopter([&] { device.adoptGpuThread(); });
                         adopter.join();
                         // The creating thread is now the *wrong* one.
                         targets.destroy(kMonitor);
                     }),
                     "OffscreenTargetManager::destroyTarget",
                     "after adoptGpuThread the original creating thread is refused");

    {
        // The positive half of the handoff, in a child because a pass here leaves a
        // Device owned by a thread that is about to end.
        const ChildResult adopted = runInChild([] {
            Device device(/*enableValidation=*/true);
            OffscreenTargetManager targets(device);
            targets.create(kMonitor, kWidth, kHeight);
            std::atomic<bool> ok{false};
            std::thread worker([&] {
                device.adoptGpuThread();
                targets.destroy(kMonitor);  // must be allowed now
                ok = true;
            });
            worker.join();
            _exit(ok ? 0 : 1);
        });
        check(adopted.exited && adopted.exitCode == 0, "after adoptGpuThread the adopting thread is allowed through");
        if (!(adopted.exited && adopted.exitCode == 0)) {
            std::fprintf(stderr, "  child %s%d; stderr: %s\n", adopted.crashed ? "died by signal " : "exited ",
                         adopted.crashed ? adopted.signal : adopted.exitCode, adopted.stderrText.c_str());
        }
    }

    // ---- Positive case, in-process and last: the guard lets the right thread work. ----
    //
    // Without this the whole file is satisfied by a Device that aborts on *every* call,
    // which is not a guard, it is a brick.
    wallpaperx::resetValidationWarningCount();
    try {
        Device device(/*enableValidation=*/true);
        OffscreenTargetManager targets(device);
        const auto& target = targets.create(kMonitor, kWidth, kHeight);
        device.immediateSubmit([](VkCommandBuffer) {});
        const wallpaperx::ExportedFrame frame = wallpaperx::exportFrame(device, target);
        if (frame.fd >= 0) close(frame.fd);
        targets.destroy(kMonitor);
        check(true, "the creating thread performs the same four calls without aborting");

        if (!wallpaperx::validationLayerActive()) {
            std::printf("skip: validation-warning check -- VK_LAYER_KHRONOS_validation is not loadable here\n");
        } else {
            check(wallpaperx::validationWarningCount() == 0, "the positive path produces no validation warnings");
        }
    } catch (const std::exception& e) {
        check(false, "the creating thread performs the same four calls without aborting");
        std::fprintf(stderr, "  threw instead: %s\n", e.what());
    }

    if (failures == 0) {
        std::printf("device_affinity_probe: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "device_affinity_probe: %d check(s) failed\n", failures);
    return 1;
}
