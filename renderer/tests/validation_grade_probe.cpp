// SPDX-License-Identifier: GPL-2.0-or-later

// Grades the grader. ValidationGrade has to separate two states that Device reports
// identically -- validationLayerActive() is false in both:
//
//   * this HOST has no validation layer to load. Nothing about this project is wrong;
//     only the GPU-side cleanliness of the run went unjudged. That is the ungraded
//     bucket, and it is not a failure.
//   * the layer IS installed and this build did not attach it -- a dropped debug
//     messenger, a mis-spelled proc name, an enableValidation that stopped being
//     forwarded. That silently retires the suite's only GPU-side oracle, the thing that
//     catches missing barriers, layout hazards and destroy-while-in-flight, so it is a
//     failure.
//
// Reading the second as the first turned a real regression into a green ctest "Skipped"
// for a day. Nothing checked the split until this probe.
//
// TWO PHASES, ONE BINARY, chosen by argv[1] and registered as a ctest entry each (per ICD).
// They cannot share a process: the two directions need opposite answers from the loader, and
// validationLayerAvailable() caches the first one it gets in a function-local static.
//   installed -- the layer visible to the loader, a Device built with validation OFF.
//   absent    -- the layer hidden from the loader, a Device built with validation ON.
//
// NOT WRAPPED IN gradedRun(). Every other probe grades its own Vulkan usage through the
// object this one is testing. Doing that here would let the thing under test judge its
// own test, and the installed phase deliberately builds a device with no validator -- so
// the enclosing grade would charge a failure for the very state the probe exists to
// construct.

#include "vulkan/device.h"
#include "support/check.h"
#include "support/validation_grade.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace {

using wallpaperx::Device;
using wallpaperx::testing::check;
using wallpaperx::testing::failureCount;
using wallpaperx::testing::ValidationGrade;
using wallpaperx::testing::validationLayerAvailable;
using wallpaperx::testing::validationWaived;

// ctest's SKIP_RETURN_CODE. This phase could not build the state it grades; it did not
// pass. Reporting 0 there would make "no conclusion" indistinguishable from "verified".
constexpr int kCannotAnswerExit = 77;

int g_comparisons = 0;

void graded(bool ok, std::string_view what) {
    ++g_comparisons;
    check(ok, what);
    if (ok) std::printf("ok   %.*s\n", static_cast<int>(what.size()), what.data());
}

// COVERAGE-ASSERT for both phases: each states how many comparisons its source makes, and
// a green run that made a different number fails here. The counts are fixed by the source,
// so an edit that folds a check away or returns early past one cannot go quiet. Skipped
// when something already failed, because a failure legitimately bypasses the checks that
// depend on it and the mismatch would then be noise on top of a real red line.
//
// CONTROL: the adopt() comparison in the installed phase replaced by `(void)adopted;`.
//       Observed: "this phase evaluated 7 of the 8 comparisons its source makes -- a check
//       stopped being reached", exit 1. Reverted. Note the mutation is a deleted check, not
//       an edited count: moving the constant would only prove the constant is read.
void checkComparisonCount(int expected) {
    if (failureCount() != 0 || g_comparisons == expected) return;
    char message[192];
    std::snprintf(message, sizeof(message),
                  "this phase evaluated %d of the %d comparisons its source makes -- a check stopped "
                  "being reached", g_comparisons, expected);
    wallpaperx::testing::fail(message);
}

// The two counters ValidationGrade can charge for one (counter, layerActive) state.
//
// A helper rather than three inline lines at each site because the verdict lands in
// ~ValidationGrade: the grade must be scoped and the counters read only after it dies.
// Reading them inside the scope yields zeros no matter what the policy decided, which is
// a check that cannot fail.
struct Verdict {
    int failures = 0;
    int ungraded = 0;
};

Verdict watchVerdict(const Device& device, std::string label) {
    Verdict verdict;
    {
        ValidationGrade grade(std::move(label), verdict.failures);
        grade.reportUngradedTo(verdict.ungraded);
        grade.watch(device);
    }
    return verdict;
}

Verdict adoptVerdict(std::shared_ptr<const std::atomic<int>> counter, bool layerActive, std::string label) {
    Verdict verdict;
    {
        ValidationGrade grade(std::move(label), verdict.failures);
        grade.reportUngradedTo(verdict.ungraded);
        grade.adopt(std::move(counter), layerActive);
    }
    return verdict;
}

// Returns whether the phase could answer at all. False means the state could not be built
// on this host and the reason has been printed; main() turns that into ctest's Skipped,
// never into a pass.
// CONTROL (both phases, both skip arms): VK_ICD_FILENAMES=/nonexistent/icd.json -> "SKIP --
//       no Vulkan device could be created: Device: vkCreateInstance failed", exit 77.
//       VK_LAYER_PATH=/nonexistent, which hides the layer package from the loader ->
//       the installed phase prints its own SKIP and exits 77 while the absent phase runs
//       and passes. No source change for either.
bool runInstalledPhase() {
    if (!validationLayerAvailable()) {
        std::printf("validation_grade_probe: SKIP -- the loader does not offer VK_LAYER_KHRONOS_validation "
                    "on this host, so the installed-but-not-activated state cannot be built. Install the "
                    "Vulkan validation layers to run this phase.\n");
        return false;
    }

    std::unique_ptr<Device> inactive;
    try {
        inactive = std::make_unique<Device>(/*enableValidation=*/false);
    } catch (const std::exception& e) {
        std::printf("validation_grade_probe: SKIP -- no Vulkan device could be created: %s\n", e.what());
        return false;
    }

    // GOAL: a Device asked not to enable validation attaches no validator. This is what makes
    //       the state below reachable from a test with no production seam of any kind.
    // CONTROL: renderer/vulkan/device.cpp, createInstance() -- `if (enableValidation)` forced to
    //       `if (true)`. Observed: this line red, and the phase printed that the checks depending
    //       on the state were not reached, exit 1. Reverted.
    graded(!inactive->validationLayerActive(),
           "a Device built with validation disabled attaches no validator");
    if (inactive->validationLayerActive()) {
        std::printf("     the installed-but-not-activated state could not be built, so the checks that "
                    "grade it were not reached\n");
        return true;
    }

    // GOAL: an installed validation layer that this Device did not activate is charged as a
    //       FAILURE, not routed to the ungraded "this host cannot judge" bucket.
    // WHY-THIS-SHAPE: both buckets are asserted, not just failures. A grader that charged BOTH
    //       would satisfy a failures-only check while still reporting the run as unjudged, and
    //       the ungraded count is what a reader sees as a skip.
    // CONTROL: renderer/tests/support/validation_grade.h -- the `if (validationLayerAvailable())`
    //       arm in ~ValidationGrade deleted so the inactive-layer verdict falls through to
    //       m_ungraded, the pre-fix shape. Observed: both lines below red plus the adopt() line,
    //       3 failures, exit 1 -- and across the whole 50-entry ctest run that mutation reddened
    //       nothing but this probe's two installed entries. Reverted.
    const Verdict watched = watchVerdict(*inactive, "deliberate: installed but not activated (watch)");
    graded(watched.failures == 1,
           "a Device with no validator, on a host that HAS the layer, is charged as a failure");
    graded(watched.ungraded == 0,
           "that Device is not routed to the ungraded bucket, which would report the loss as a skip");

    // GOAL: the same state recorded through adopt() -- the entry point for a caller whose Device
    //       is owned elsewhere and already gone -- reaches the same verdict as watch().
    // WHY-THIS-SHAPE: the two entry points feed one destructor today, so this looks redundant;
    //       it is the check that stays honest if they ever stop doing so, which is exactly how
    //       a fix gets applied to one path and not the other.
    // CONTROL: the same deletion in validation_grade.h as above. Observed: this line red with the
    //       two above it. Reverted.
    const Verdict adopted = adoptVerdict(inactive->validationCounter(), inactive->validationLayerActive(),
                                         "deliberate: installed but not activated (adopt)");
    graded(adopted.failures == 1 && adopted.ungraded == 0,
           "adopt() reaches the same verdict as watch() for that state");

    std::unique_ptr<Device> active;
    try {
        active = std::make_unique<Device>(/*enableValidation=*/true);
    } catch (const std::exception& e) {
        check(false, "a Device that asks for validation can be built on a host where the layer is installed");
        std::fprintf(stderr, "  threw instead: %s\n", e.what());
        std::printf("     the checks that need an attached validator were not reached\n");
        return true;
    }

    // GOAL: the failure above is specific to a device with no validator. A device that DID
    //       attach one, with nothing to complain about, is charged to neither bucket.
    // WHY-THIS-SHAPE: without this, the primary check passes just as well against a grader that
    //       charges a failure unconditionally -- the obvious wrong way to make it green. It also
    //       asserts the production half of the same property: on a host advertising the layer, a
    //       Device that asks for validation must end up with a validator attached.
    // CONTROL: renderer/vulkan/device.cpp -- ` && false` appended to the `m_validationLayerActive`
    //       assignment, so the messenger is still created and merely reported as absent. Observed:
    //       both lines below red, 2 failures, exit 1. Reverted.
    graded(active->validationLayerActive(),
           "a Device built with validation enabled attaches a validator when the layer is installed");
    const Verdict clean = watchVerdict(*active, "validation_grade_probe (validator attached)");
    graded(clean.failures == 0 && clean.ungraded == 0,
           "an attached validator with no warnings is charged to neither bucket");

    // GOAL: WALLPAPERX_ALLOW_NO_VALIDATION=1 still excuses an absent validator, and the checks
    //       above are not quietly riding on it.
    // WHY-THIS-SHAPE: ~ValidationGrade tests the waiver BEFORE the availability arm, so an
    //       ambient waiver -- the sanitizer tier sets it on every test it enables, see
    //       backend/CMakeLists.txt -- makes the primary check above read zero failures. main()
    //       clears it; this check is what proves that clearing is load-bearing rather than
    //       decorative, by showing the outcome it changes. Asserting the waiver's effect also
    //       pins the arm order that keeps the sanitizer tier green.
    // CONTROL: renderer/tests/support/validation_grade.h -- the `if (validationWaived())` arm moved
    //       below the availability arm. Observed: exactly this line red, 1 failure, every check
    //       above it still green. Reverted. (The companion control for the scrub is on main()'s
    //       unsetenv.)
    setenv("WALLPAPERX_ALLOW_NO_VALIDATION", "1", 1);
    const Verdict waived = watchVerdict(*inactive, "validation_grade_probe (waived)");
    unsetenv("WALLPAPERX_ALLOW_NO_VALIDATION");
    graded(waived.failures == 0 && waived.ungraded == 0,
           "the operator's waiver excuses an absent validator instead of charging either bucket");
    // CONTROL: the unsetenv two lines above deleted -> this line red, 1 failure, exit 1. Reverted.
    graded(!validationWaived(), "the probe cleared the waiver switch again after grading its effect");

    checkComparisonCount(8);
    return true;
}

bool runAbsentPhase() {
    if (validationLayerAvailable()) {
        std::printf("validation_grade_probe: SKIP -- the loader still lists VK_LAYER_KHRONOS_validation "
                    "with VK_LOADER_LAYERS_DISABLE set for it, so a host that cannot supply a validator "
                    "cannot be simulated here (a loader without the layer filter would do this).\n");
        return false;
    }

    std::unique_ptr<Device> device;
    try {
        device = std::make_unique<Device>(/*enableValidation=*/true);
    } catch (const std::exception& e) {
        std::printf("validation_grade_probe: SKIP -- no Vulkan device could be created: %s\n", e.what());
        return false;
    }

    // GOAL: the opposite verdict. A host with no validation layer to offer is routed to the
    //       ungraded bucket and is NOT accused of a defect, however loudly the other direction
    //       is charged.
    // WHY-THIS-SHAPE: the layer is hidden through the loader's own filter rather than by
    //       uninstalling the package, because the code under test asks the loader -- from
    //       validationLayerAvailable()'s position the two are the same question. It needs its own
    //       process: that answer is cached in a function-local static, so a single binary that
    //       set the variable mid-run would keep answering with whatever it saw first.
    // CONTROL: renderer/tests/support/validation_grade.h -- `if (validationLayerAvailable())`
    //       forced to `if (true)`, i.e. every unwatched device accused. Observed: the two lines
    //       below red, 2 failures, exit 1, while the installed phase stayed green -- the two
    //       directions are independent. Reverted. Separately, with no mutation at all, this
    //       phase's pass was reproduced with the layer hidden a second way
    //       (VK_LAYER_PATH=/nonexistent), where the installed phase skips instead.
    // CONTROL (the line below specifically): renderer/vulkan/device.h -- m_validationLayerActive
    //       given the initialiser `true`, so a Device claims a validator it never attached.
    //       Observed: that line red together with the ungraded-bucket line, 2 failures. Reverted.
    graded(!device->validationLayerActive(),
           "with no layer for the loader to offer, a Device that asks for validation gets none");
    const Verdict verdict = watchVerdict(*device, "deliberate: no validator available on this host");
    graded(verdict.ungraded == 1,
           "a host that cannot supply a validator is routed to the ungraded bucket");
    graded(verdict.failures == 0,
           "that host is not charged as a failure of the code that built the device");

    checkComparisonCount(3);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    // The verdicts under test are printed to stderr by ValidationGrade while this probe's own
    // "ok" lines go to stdout, which ctest gives a pipe and glibc therefore buffers in full --
    // so without this every FAIL line arrives before the run that produced it.
    setvbuf(stdout, nullptr, _IOLBF, 0);

    const std::string_view phase = argc > 1 ? argv[1] : "";
    if (phase != "installed" && phase != "absent") {
        std::fprintf(stderr, "usage: %s installed|absent\n", argv[0]);
        return 2;
    }

    // Both loader variables are set HERE, before any Vulkan call, rather than as ctest
    // ENVIRONMENT properties: the binary then means the same thing run by hand, and an edit to
    // CMakeLists.txt cannot quietly turn the absent entry into a second copy of the installed
    // one. Order is load-bearing -- the loader reads the filter when layers are enumerated, and
    // validationLayerAvailable() caches the first answer it gets.
    if (phase == "absent") {
        setenv("VK_LOADER_LAYERS_DISABLE", "VK_LAYER_KHRONOS_validation", 1);
    } else {
        unsetenv("VK_LOADER_LAYERS_DISABLE");
    }
    // The waiver short-circuits the arm this probe grades, so an operator (or the sanitizer
    // tier) who set it would make the installed phase read zero failures. Cleared for the whole
    // process; the waiver check re-sets it around its own grade.
    // CONTROL: this line removed and the probe run under WALLPAPERX_ALLOW_NO_VALIDATION=1 ->
    //       "a Device with no validator ... is charged as a failure" and the adopt() line went
    //       red, 2 failures, exit 1. So the scrub is load-bearing, and an ambient waiver reds
    //       this probe rather than passing it for the wrong reason. Reverted.
    unsetenv("WALLPAPERX_ALLOW_NO_VALIDATION");

    std::printf("validation_grade_probe (%.*s): the FAIL: lines labelled \"deliberate:\" below are the "
                "verdicts under test -- a run that prints none of them is the regression.\n",
                static_cast<int>(phase.size()), phase.data());

    bool answered = false;
    try {
        answered = phase == "installed" ? runInstalledPhase() : runAbsentPhase();
    } catch (const std::exception& e) {
        // Reported, not left to terminate: an escaping exception here would take the stderr this
        // probe exists to produce with it, and it is a fault of this harness, not a verdict on
        // the grader.
        std::fprintf(stderr, "FAIL: validation_grade_probe: harness fault: %s -- no conclusion about the "
                             "grading policy\n", e.what());
        return 1;
    }

    if (failureCount() > 0) {
        return wallpaperx::testing::reportFailures("validation_grade_probe") == 0 ? 0 : 1;
    }
    // Checked after the failure count on purpose: a phase that recorded a failure and then could
    // not finish must not be reported as a skip.
    if (!answered) return kCannotAnswerExit;
    std::printf("validation_grade_probe: all checks passed\n");
    return 0;
}
