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
// NO CONTROL PATCH IS NEEDED FOR THE GUARD CASES, AND THAT IS THE POINT. The usual way
// to show a check can fail is to break the thing it guards and watch it go red, which
// means editing production code. Here the negative case *is* that mutation, expressed as
// a permanent test: the guard either fires or the child exits 0 and the check fails.
// Stubbing `assertGpuThread` to return early turns every negative case below red, which
// is the regression nothing previously caught -- but it is a property of the test, not a
// step anyone has to remember to run.
//
// GOAL: X.4 -- a fault in this harness, or a host that cannot run these cases at all, is
//       never reported as a failure of the code under test.
// WHY-THIS-SHAPE: three outcomes that all used to collapse into "the guard is broken"
//       are now distinct, because each has a different reader and a different fix. A
//       child that cannot build a Device is a host problem (skip, exit 77); a pipe, fork
//       or waitpid that fails is a problem with this file or the machine (HARN, exit 1,
//       but never a claim about the guard); only a child that ran and did not abort is a
//       renderer regression (FAIL). The simpler alternative -- letting the Device
//       exception escape and reading the resulting SIGABRT -- is what produced the bug:
//       std::terminate aborts, so a machine with no ICD looked exactly like the guard
//       firing with the wrong message, five times over.
// CONTROL: (a) `VK_ICD_FILENAMES=/nonexistent/icd.json ./wallpaperx-device-affinity-probe`
//       -> 7 "skip:" lines and exit 77; before this change the same command produced
//       five FAILs reading "child aborted, but not from the affinity guard". (b) the pid
//       argument of waitpid replaced by -999999, which fails ECHILD -- a non-EINTR error
//       -> 6 "HARN:" lines and exit 1; before, `status` stayed 0, WIFEXITED(0) was true
//       with WEXITSTATUS(0) == 0, and every case reported "the call returned instead of
//       aborting (child exited 0) -- the guard did not fire". Both reverted.
// CONTROL (setup throwing inside body): `throw std::runtime_error("CONTROL: setup could
//       not complete")` placed in the destroyTarget case ahead of its targets.create()
//       -> "skip: destroying an offscreen target from a non-GPU thread aborts -- setup
//       for this case could not complete: ...", exit 77. Before body() was guarded the
//       same throw reached std::terminate and the parent reported "child aborted, but
//       not from the affinity guard" -- a host-capability failure charged to the
//       renderer. Reverted.

#include "vulkan/device.h"
#include "vulkan/frame_export.h"
#include "vulkan/offscreen.h"
#include "support/validation_probe.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <thread>

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

// The child could not build a Device at all, so the case proved nothing about the guard.
// Distinct from every other exit code a child here can produce: 0 means the guarded call
// returned (a real miss) and 126 means dup2 failed.
constexpr int kDeviceUnavailableExit = 77;

int failures = 0;
int harnessFaults = 0;
int skipped = 0;

void check(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

// Reported separately from check() on purpose. A pipe that could not be opened is not
// evidence about Device::assertGpuThread, and counting it as a failed check would put a
// renderer bug on the screen when the actual fault is in this file or in the machine.
void harnessFault(const char* what, const char* detail) {
    // Both lines on stdout, deliberately. They used to be split across the two streams
    // with an fflush(stdout) in between to keep them adjacent, which stopped working the
    // moment this binary linked wallpaperx-test-support: that library redirects stderr
    // to a file drained only at a count() call and at exit, so the detail lines all
    // arrived in a block at the end, each one detached from the case it explains.
    // Keeping the pair on one stream makes the adjacency independent of any of that.
    std::printf("HARN: %s\n", what);
    std::printf("      could not run this case: %s -- this says nothing about the guard\n", detail);
    ++harnessFaults;
}

void skip(const char* what, const std::string& detail) {
    // Trimmed because `detail` is usually the child's captured stderr, which ends in the
    // newline the child wrote; printed raw it puts a blank line between every skip.
    const size_t end = detail.find_last_not_of(" \t\r\n");
    std::printf("skip: %s -- %s\n", what, end == std::string::npos ? detail.c_str() : detail.substr(0, end + 1).c_str());
    ++skipped;
}

struct ChildResult {
    bool crashed = false;      // died by a signal
    int signal = 0;            // which one
    bool exited = false;       // returned normally
    int exitCode = -1;
    bool harnessFailed = false;  // fork/pipe/waitpid failed -- the case never ran
    std::string harnessError;
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
        result.harnessFailed = true;
        result.harnessError = std::string("pipe() failed: ") + strerror(errno);
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
        result.harnessFailed = true;
        result.harnessError = std::string("fork() failed: ") + strerror(errno);
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
    pid_t waited = 0;
    while ((waited = waitpid(pid, &status, 0)) < 0 && errno == EINTR) {
    }
    result.stderrText = std::move(captured);
    if (waited < 0) {
        // Left as a harness fault rather than falling through, because `status` is still
        // 0 here and WIFEXITED(0) is true with WEXITSTATUS(0) == 0 -- indistinguishable
        // from a child that returned cleanly, which is precisely the "the guard did not
        // fire" verdict. A waitpid failure would have been reported as a renderer bug.
        result.harnessFailed = true;
        result.harnessError = std::string("waitpid() failed: ") + strerror(errno);
        return result;
    }
    if (WIFSIGNALED(status)) {
        result.crashed = true;
        result.signal = WTERMSIG(status);
    } else if (WIFEXITED(status)) {
        result.exited = true;
        result.exitCode = WEXITSTATUS(status);
    }
    return result;
}

// Builds the Device inside the child and hands it to `body`, or exits with a code the
// parent can tell apart from every real outcome.
//
// Constructing a Device is the one step in these children that fails for reasons having
// nothing to do with thread affinity: no ICD installed, no queue family with the needed
// bits, a driver that will not initialise. Uncaught, that exception escapes the child's
// body, std::terminate calls abort(), and the child dies by SIGABRT with no guard
// sentence on its stderr -- which expectGuardAbort reports as "child aborted, but not
// from the affinity guard", once per case. Five failures blaming assertGpuThread for the
// absence of a GPU. std::optional::emplace constructs in place, so this does not need
// Device to be movable.
void withDevice(const std::function<void(Device&)>& body) {
    std::optional<Device> device;
    try {
        device.emplace(/*enableValidation=*/true);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "device unavailable: %s\n", e.what());
        std::fflush(stderr);
        _exit(kDeviceUnavailableExit);
    }
    // body() is guarded too, and it has to be: several cases do real GPU work on the
    // correct thread before they reach the guarded call -- targets.create() in three of
    // them -- and OffscreenTargetManager::create throws on a host whose ICD builds an
    // instance and a device but cannot satisfy the image format or memory type it wants.
    // Guarding only the constructor left that throw escaping the lambda into
    // std::terminate -> abort(), which the parent reads as SIGABRT without the guard
    // sentence and reports as "child aborted, but not from the affinity guard": the same
    // host-capability-blamed-on-the-guard verdict this wrapper exists to prevent, just
    // one step later.
    //
    // Nothing here can mask a real regression. assertGpuThread aborts; it does not
    // throw. So any exception reaching this handler came from setup, and the exception
    // text is printed alongside the skip so a throw from somewhere unexpected is still
    // legible rather than silently swallowed.
    try {
        body(*device);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "setup for this case could not complete: %s\n", e.what());
        std::fflush(stderr);
        _exit(kDeviceUnavailableExit);
    }
}

// Asserts that a child died the way the guard makes things die, and names every other
// outcome distinguishably. A child that exits 0 means the guard did not fire; a child
// that dies by some other signal, or by SIGABRT without the guard's message, means
// something else killed it and this case proved nothing either way.
void expectGuardAbort(const ChildResult& result, const char* label, const char* what) {
    if (result.harnessFailed) {
        harnessFault(what, result.harnessError.c_str());
        return;
    }
    if (result.exited && result.exitCode == kDeviceUnavailableExit) {
        skip(what, result.stderrText.empty() ? std::string("no usable Vulkan device in the child")
                                             : result.stderrText);
        return;
    }
    if (result.exited && result.exitCode == 126) {
        harnessFault(what, "dup2() of the capture pipe failed in the child");
        return;
    }
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
                         withDevice([](Device& device) {
                             OffscreenTargetManager targets(device);
                             onAnotherThread([&] { targets.create(kMonitor, kWidth, kHeight); });
                         });
                     }),
                     "OffscreenTargetManager::create",
                     "creating an offscreen target from a non-GPU thread aborts");

    expectGuardAbort(runInChild([] {
                         withDevice([](Device& device) {
                             OffscreenTargetManager targets(device);
                             targets.create(kMonitor, kWidth, kHeight);  // correct thread
                             onAnotherThread([&] { targets.destroy(kMonitor); });
                         });
                     }),
                     "OffscreenTargetManager::destroyTarget",
                     "destroying an offscreen target from a non-GPU thread aborts");

    expectGuardAbort(runInChild([] {
                         withDevice([](Device& device) {
                             onAnotherThread([&] { device.immediateSubmit([](VkCommandBuffer) {}); });
                         });
                     }),
                     "Device::immediateSubmit",
                     "submitting GPU work from a non-GPU thread aborts");

    expectGuardAbort(runInChild([] {
                         withDevice([](Device& device) {
                             OffscreenTargetManager targets(device);
                             const auto& target = targets.create(kMonitor, kWidth, kHeight);
                             onAnotherThread([&] { (void)wallpaperx::exportFrame(device, target); });
                         });
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
                         withDevice([](Device& device) {
                             OffscreenTargetManager targets(device);
                             targets.create(kMonitor, kWidth, kHeight);
                             std::thread adopter([&] { device.adoptGpuThread(); });
                             adopter.join();
                             // The creating thread is now the *wrong* one.
                             targets.destroy(kMonitor);
                         });
                     }),
                     "OffscreenTargetManager::destroyTarget",
                     "after adoptGpuThread the original creating thread is refused");

    {
        // The positive half of the handoff, in a child because a pass here leaves a
        // Device owned by a thread that is about to end.
        const ChildResult adopted = runInChild([] {
            withDevice([](Device& device) {
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
        });
        const char* const what = "after adoptGpuThread the adopting thread is allowed through";
        if (adopted.harnessFailed) {
            harnessFault(what, adopted.harnessError.c_str());
        } else if (adopted.exited && adopted.exitCode == kDeviceUnavailableExit) {
            skip(what, adopted.stderrText);
        } else {
            check(adopted.exited && adopted.exitCode == 0, what);
            if (!(adopted.exited && adopted.exitCode == 0)) {
                std::fprintf(stderr, "  child %s%d; stderr: %s\n", adopted.crashed ? "died by signal " : "exited ",
                             adopted.crashed ? adopted.signal : adopted.exitCode, adopted.stderrText.c_str());
            }
        }
    }

    // ---- Positive case, in-process and last: the guard lets the right thread work. ----
    //
    // Without this the whole file is satisfied by a Device that aborts on *every* call,
    // which is not a guard, it is a brick.
    wallpaperx::resetValidationWarningCount();
    // Construction is separated from the four calls so the two failure meanings stay
    // apart: a Device that cannot be built here means the same "could not run" the
    // children report, while a throw from any call *after* it is a real failure of the
    // positive path. Folding both into one try/catch reported a machine with no ICD as a
    // broken guard.
    std::optional<Device> device;
    try {
        device.emplace(/*enableValidation=*/true);
    } catch (const std::exception& e) {
        skip("the creating thread performs the same four calls without aborting", e.what());
    }
    if (device) {
        try {
            OffscreenTargetManager targets(*device);
            const auto& target = targets.create(kMonitor, kWidth, kHeight);
            device->immediateSubmit([](VkCommandBuffer) {});
            const wallpaperx::ExportedFrame frame = wallpaperx::exportFrame(*device, target);
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
    }

    if (failures > 0) {
        std::fprintf(stderr, "device_affinity_probe: %d check(s) failed\n", failures);
        return 1;
    }
    // Harness faults outrank the skip: they mean this file could not do its job, which is
    // not the same as a host that cannot run it, and must not be reported as a pass.
    if (harnessFaults > 0) {
        std::fprintf(stderr, "device_affinity_probe: %d case(s) could not be run by the harness -- "
                             "no conclusion about the affinity guard either way\n", harnessFaults);
        return 1;
    }
    if (skipped > 0) {
        std::printf("device_affinity_probe: %d case(s) skipped -- no usable Vulkan device on this host\n", skipped);
        return 77;  // SKIP_RETURN_CODE, so ctest says "Skipped" rather than "Passed"
    }
    std::printf("device_affinity_probe: all checks passed\n");
    return 0;
}
