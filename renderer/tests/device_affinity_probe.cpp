// SPDX-License-Identifier: GPL-2.0-or-later

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
//       child that cannot build a Device is a host problem (UNRUN, exit 1); a pipe, fork
//       or waitpid that fails is a problem with this file or the machine (HARN, exit 1,
//       but never a claim about the guard); only a child that ran and did not abort is a
//       renderer regression (FAIL). The simpler alternative -- letting the Device
//       exception escape and reading the resulting SIGABRT -- is what produced the bug:
//       std::terminate aborts, so a machine with no ICD looked exactly like the guard
//       firing with the wrong message, five times over.
// CONTROL: (a) `VK_ICD_FILENAMES=/nonexistent/icd.json ./wallpaperx-device-affinity-probe`
//       -> every case reported "UNRUN:" naming the real cause, exit 1; before this
//       change the same command produced five FAILs reading "child aborted, but not from
//       the affinity guard". Deliberately exit 1 and not a ctest skip: a renderer host
//       with no ICD cannot test anything here, and every sibling probe fails outright in
//       that state -- reporting this one as Skipped would single it out as optional.
//       (b) the pid argument of waitpid replaced by -999999, which fails ECHILD -- a
//       non-EINTR error -> 6 "HARN:" lines and exit 1; before, `status` stayed 0,
//       WIFEXITED(0) was true with WEXITSTATUS(0) == 0, and every case reported "the call
//       returned instead of aborting (child exited 0) -- the guard did not fire".
//       Both reverted.
// CONTROL (the guarded call throwing instead of aborting): the create case's guarded
//       body replaced with `throw std::runtime_error("CONTROL: guard threw")`, which is
//       what a regression turning assertGpuThread's abort() into a throw would look like
//       -> "FAIL: creating an offscreen target from a non-GPU thread aborts" plus "the
//       guarded call threw instead of aborting", exit 1. With onAnotherThread's
//       capture-and-rethrow removed, the SAME mutation produced "child aborted, but not
//       from the affinity guard" and "terminate called after throwing an instance of
//       'std::runtime_error'" -- the throw died on the worker thread before any handler
//       here could classify it, so the whole GuardedPhase mechanism was inert for the
//       four cases whose call runs on a thread. Both mutations reverted.
// CONTROL (re-run after PR_SET_DUMPABLE was added to the child, 2026-08-06, because a flag
//       that quietened the aborts could just as easily have quietened the evidence):
//       assertGpuThread stubbed to `return;` -> 5 FAILs, each "the call returned instead of
//       aborting (child exited 0) -- the guard did not fire". The create case's guarded
//       body replaced by a throw -> 1 FAIL plus "the guarded call threw instead of
//       aborting: CONTROL: guard threw", the other six green. Both reverted. The guard
//       sentence and the call label are still read out of the child's captured stderr in
//       every passing case, so the redirect demonstrably survives the prctl.

#include "vulkan/device.h"
#include "vulkan/frame_export.h"
#include "vulkan/offscreen.h"
#include "support/forked_child.h"
#include "support/validation_grade.h"

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

// The guarded call returned by throwing rather than aborting. Distinct from every other
// code a child here can produce, and deliberately NOT the skip code: this is a guard
// regression and must be graded red.
constexpr int kGuardThrewExit = 125;

// The child could not arm itself against its parent's death. Its own code because it is
// neither a guard verdict nor a host limit: it is this harness failing to establish a
// precondition.
constexpr int kArmFailedExit = 124;

// The child's stderr could not be redirected onto the capture pipe. Named rather than
// spelled 126 at each grading site, because both codes come out of prepareForkedChild and a
// site that handles one and not the other reads the unhandled one as a case verdict.
constexpr int kDup2FailedExit = 126;

// Set for the duration of the call under test, so withDevice's handler can tell a
// setup failure from the guard failing to fire. A plain bool is enough: each child runs
// one case, and the marked region is entered once on the child's main thread.
bool g_inGuardedPhase = false;
// A one-way latch, deliberately with no destructor. An RAII pair looks right and is
// wrong here: [except.handle] runs destructors for the unwound scope BEFORE the handler
// body, so a resetting destructor cleared the flag on exactly the path that reads it and
// the guarded-call branch was unreachable. Nothing re-enters the marked region -- each
// child runs one case -- so there is nothing to reset.
struct GuardedPhase {
    GuardedPhase() { g_inGuardedPhase = true; }
};

int failures = 0;
int harnessFaults = 0;
int unrunnable = 0;
int ungraded = 0;   // cases that ran, whose validation output could not be judged

void check(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

// Reported separately from check() on purpose. A pipe that could not be opened is not
// evidence about Device::assertGpuThread, and counting it as a failed check would put a
// renderer bug on the screen when the actual fault is in this file or in the machine.
void harnessFault(const char* what, const char* detail) {
    // Both lines on stdout, deliberately. Split across stdout and stderr they need an
    // fflush between them to stay adjacent under ctest, because stdout is a pipe and
    // fully buffered while stderr is not -- and that ordering fix silently stopped
    // working once for reasons outside this function. One stream makes the adjacency
    // hold regardless of how either is buffered.
    std::printf("HARN: %s\n", what);
    std::printf("      could not run this case: %s -- this says nothing about the guard\n", detail);
    ++harnessFaults;
}

// Named "unrunnable" rather than "skip": the case did not run, and that is reported as a
// failure of the run, not as an acceptable outcome.
void unrunnableCase(const char* what, const std::string& detail) {
    // Trimmed because `detail` is usually the child's captured stderr, which ends in the
    // newline the child wrote; printed raw it puts a blank line between every skip.
    const size_t end = detail.find_last_not_of(" \t\r\n");
    std::printf("UNRUN: %s -- %s\n", what, end == std::string::npos ? detail.c_str() : detail.substr(0, end + 1).c_str());
    ++unrunnable;
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

// Everything the child side of a fork in this file must do before it runs anything else.
// Returns 0, or the exit code the child should die with.
//
// One function rather than a preamble copied into each fork site, so no site can arm its
// children differently from the others, or forget.
//
// Order is load-bearing: stderr is redirected first so a later failure is still visible on
// the parent's pipe, and the arming is as close to the fork as it can be because the window
// it closes opens at the fork.
//
// A refused PR_SET_PDEATHSIG is deliberately fatal to the case, not a warning: this child is
// about to build a Vulkan device and then abort on purpose, so an unarmed one that outlives
// a parent killed mid-run holds a GPU device indefinitely. Every grading site reports that
// through the harness channel, so the run says "this case could not be run" and exits 1 --
// it never becomes a verdict on the affinity guard. The cost is that a host which refuses
// the prctl can run none of the six cases, which is the intended trade: no coverage is
// better than coverage that leaks devices.
// CONTROL: armAgainstParentDeath made to return false -> 6 HARN lines, each "the child
//       could not arm itself against its parent's death", exit 1, and no FAIL. Reverted.
// CONTROL (the dup2 code, at every grading site): prepareForkedChild made to return
//       kDup2FailedExit -> the same 6 lines naming dup2. With the adopt case's branch for
//       it removed, that case instead read "FAIL: after adoptGpuThread the adopting thread
//       is allowed through" -- a guard verdict for a child that never ran. Both reverted.
int prepareForkedChild(pid_t parentBeforeFork, int stderrFd) {
    if (stderrFd >= 0 && dup2(stderrFd, STDERR_FILENO) == -1) return kDup2FailedExit;
    if (!wallpaperx::testing::armAgainstParentDeath(parentBeforeFork)) return kArmFailedExit;
    return 0;
}

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

    const pid_t parentBeforeFork = getpid();
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
        const int prep = prepareForkedChild(parentBeforeFork, fds[1]);
        if (prep != 0) _exit(prep);
        close(fds[1]);
        // Every negative case here ends in std::abort(), which is the point -- but on a
        // systemd host each one is a real SIGABRT, so the desktop pops a crash notification.
        // Five cases per run, times each build tree, and a developer running ctest gets a
        // screen of "wallpaperx-device-affinity-probe crashed" for a suite that passed. This
        // child never execs, so PR_SET_DUMPABLE is the half of suppressCoreDump() that does
        // the work here; the measurement behind both halves is on that function.
        //
        // It suppresses the dump only, and that is not assumed: every negative case requires
        // kGuardSentence AND its own call label in the captured stderr, so a flag that cost
        // the child's stderr would turn all five red rather than pass quietly. Called after
        // the redirect above so a failure before this point is still visible on the pipe.
        wallpaperx::testing::suppressCoreDump();
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
    // Which exit code a throw maps to depends on WHERE it came from, and the difference
    // is the whole point. A throw from setup is a host that cannot run the case (skip).
    // A throw from the guarded call is the guard failing to abort -- the exact regression
    // this file exists to catch. One try around the whole body mapped both to the skip
    // code, so a guard changed from abort() to throwing would have turned every negative
    // case into ctest "Skipped", i.e. green. Each case marks the boundary with a
    // GuardedPhase object immediately before the call under test.
    try {
        body(*device);
    } catch (const std::exception& e) {
        if (g_inGuardedPhase) {
            std::fprintf(stderr, "the guarded call threw instead of aborting: %s\n", e.what());
            std::fflush(stderr);
            _exit(kGuardThrewExit);
        }
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
    if (result.exited && result.exitCode == kGuardThrewExit) {
        check(false, what);
        std::printf("      the guarded call threw instead of aborting; stderr was:\n      %s\n",
                    result.stderrText.c_str());
        return;
    }
    if (result.exited && result.exitCode == kDeviceUnavailableExit) {
        unrunnableCase(what, result.stderrText.empty() ? std::string("no usable Vulkan device in the child")
                                             : result.stderrText);
        return;
    }
    if (result.exited && result.exitCode == kDup2FailedExit) {
        harnessFault(what, "dup2() of the capture pipe failed in the child");
        return;
    }
    // Ahead of the bare `result.exited` branch below, which would otherwise read this as
    // "the call returned instead of aborting" -- a guard verdict for a child that never ran
    // the call at all (X.3).
    if (result.exited && result.exitCode == kArmFailedExit) {
        harnessFault(what, "the child could not arm itself against its parent's death");
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
// Captures anything the thread throws and rethrows it on the caller, instead of letting
// it hit std::terminate on a thread nobody is catching for. Without this the whole
// GuardedPhase mechanism was inert for four of the five negative cases: their call under
// test runs here, so a guard that threw instead of aborting killed the child by
// terminate() and was graded as "aborted, but not from the affinity guard" -- the
// misattribution the phase marker exists to prevent.
void onAnotherThread(const std::function<void()>& body) {
    std::exception_ptr escaped;
    std::thread other([&] {
        try {
            body();
        } catch (...) {
            escaped = std::current_exception();
        }
    });
    other.join();
    if (escaped) std::rethrow_exception(escaped);
}

}  // namespace

int main() {
    std::printf("device_affinity_probe: Device is pinned to its creating thread (S2.11)\n");

    // ---- Negative cases. Each is its own process, and each must die by the guard. ----

    expectGuardAbort(runInChild([] {
                         withDevice([](Device& device) {
                             OffscreenTargetManager targets(device);
                             GuardedPhase phase;
                             onAnotherThread([&] { targets.create(kMonitor, kWidth, kHeight); });
                         });
                     }),
                     "OffscreenTargetManager::create",
                     "creating an offscreen target from a non-GPU thread aborts");

    expectGuardAbort(runInChild([] {
                         withDevice([](Device& device) {
                             OffscreenTargetManager targets(device);
                             targets.create(kMonitor, kWidth, kHeight);  // correct thread
                             GuardedPhase phase;
                             onAnotherThread([&] { targets.destroy(kMonitor); });
                         });
                     }),
                     "OffscreenTargetManager::destroyTarget",
                     "destroying an offscreen target from a non-GPU thread aborts");

    expectGuardAbort(runInChild([] {
                         withDevice([](Device& device) {
                             GuardedPhase phase;
                             onAnotherThread([&] { device.immediateSubmit([](VkCommandBuffer) {}); });
                         });
                     }),
                     "Device::immediateSubmit",
                     "submitting GPU work from a non-GPU thread aborts");

    expectGuardAbort(runInChild([] {
                         withDevice([](Device& device) {
                             OffscreenTargetManager targets(device);
                             const auto& target = targets.create(kMonitor, kWidth, kHeight);
                             GuardedPhase phase;
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
                             onAnotherThread([&] { device.adoptGpuThread(); });
                             // The creating thread is now the *wrong* one.
                             GuardedPhase phase;
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
                // Same capture-and-rethrow as every other cross-thread call here: a
                // throw from destroy() on a weak host must reach withDevice's setup
                // handler, not terminate the child and be read as a guard failure.
                onAnotherThread([&] {
                    device.adoptGpuThread();
                    targets.destroy(kMonitor);  // must be allowed now
                    ok = true;
                });
                _exit(ok ? 0 : 1);
            });
        });
        const char* const what = "after adoptGpuThread the adopting thread is allowed through";
        if (adopted.harnessFailed) {
            harnessFault(what, adopted.harnessError.c_str());
        } else if (adopted.exited && adopted.exitCode == kArmFailedExit) {
            // Same reason as in expectGuardAbort, and BOTH preamble codes are handled here:
            // a child that never got as far as the case must not be graded as the case
            // failing. This site had only the arming half, so a dup2 failure -- the other
            // thing prepareForkedChild can return -- was read as adoptGpuThread refusing the
            // adopting thread.
            harnessFault(what, "the child could not arm itself against its parent's death");
        } else if (adopted.exited && adopted.exitCode == kDup2FailedExit) {
            harnessFault(what, "dup2() of the capture pipe failed in the child");
        } else if (adopted.exited && adopted.exitCode == kDeviceUnavailableExit) {
            unrunnableCase(what, adopted.stderrText);
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
    // Construction is separated from the four calls so the two failure meanings stay
    // apart: a Device that cannot be built here means the same "could not run" the
    // children report, while a throw from any call *after* it is a real failure of the
    // positive path. Folding both into one try/catch reported a machine with no ICD as a
    // broken guard.
    // The explicit scope and the declaration order are both load-bearing: `grade` is
    // declared before the Device and destroyed after it, so the count it reads includes
    // whatever the layer says during ~OffscreenTargetManager and ~Device. Reading
    // device->validationWarnings() inside the block -- which is what this did -- samples
    // a counter still being written to and misses the teardown half entirely, the exact
    // mistake every sibling probe was changed to avoid.
    {
        wallpaperx::testing::ValidationGrade grade("device_affinity_probe (positive path)", failures);
        // A missing validation-layer package is neither a broken guard nor a case that
        // could not run: every case runs and passes, only their validation output goes
        // ungraded. It had been charged first to `failures` (which accuses
        // assertGpuThread) and then to `unrunnable` (which claims cases did not run);
        // both were false. Its own counter, with its own sentence.
        grade.reportUngradedTo(ungraded);
        std::optional<Device> device;
        try {
            device.emplace(/*enableValidation=*/true);
            grade.watch(*device);
        } catch (const std::exception& e) {
            // Already reported as unrunnable; tell grade not to report it a second time.
            grade.expectNoDevice();
            unrunnableCase("the creating thread performs the same four calls without aborting", e.what());
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
            } catch (const std::exception& e) {
                check(false, "the creating thread performs the same four calls without aborting");
                std::fprintf(stderr, "  threw instead: %s\n", e.what());
            }
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
    // Both environment buckets are REPORTED before either returns, so a host that is
    // missing an ICD for one case and the validation-layer package for another sees both
    // facts. Ordering them as two early returns hid whichever came second.
    //
    // They stay separate because they say different things. `ungraded` means the case
    // ran and passed and only its validation output went unjudged; `unrunnable` means the
    // case never ran at all. Folding either into the other produced a false sentence in
    // an earlier round -- first "the guard is broken", then "these cases did not run".
    if (ungraded > 0) {
        std::fprintf(stderr,
                     "device_affinity_probe: %d validation grade(s) could not be made -- see the reason above. "
                     "The cases that ran are still judged; their GPU-side cleanliness is not.\n",
                     ungraded);
    }
    if (unrunnable > 0) {
        // Names no single cause: each case printed its own reason above, and this bucket
        // collects any of them. Naming one reported a missing layer package as a missing
        // GPU.
        std::fprintf(stderr,
                     "device_affinity_probe: %d case(s) could not run -- see the reasons above. These are "
                     "environment failures, not verdicts on the affinity guard.\n",
                     unrunnable);
    }

    // Deliberately NOT a ctest skip. A host that cannot run these cases cannot run
    // anything else in renderer/tests either -- device_probe, offscreen_probe and the
    // rest all fail outright in that situation, and reporting this one as "Skipped"
    // would single it out as optional while its siblings went red for the same cause.
    // The messages above name the real reason; only the grade is shared.
    if (unrunnable > 0 || ungraded > 0) return 1;

    std::printf("device_affinity_probe: all checks passed\n");
    return 0;
}
