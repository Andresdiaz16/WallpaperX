#pragma once

// Grades a Device's validation-warning count, at the right time, with the guard that
// makes the count mean anything, and in a shape where the verdict cannot be bypassed.
//
// WHY A CALLABLE AND NOT A BARE RAII OBJECT. The grade has to happen after ~Device --
// the layer's most valuable complaints (a resource still in flight, an object destroyed
// twice, a leaked allocation) are only emitted during destruction. An RAII object placed
// before the Device gets that ordering for free, but publishing its verdict through a
// caller-owned `int&` that the caller must remember to read *after* the object dies is a
// contract the compiler cannot enforce. It was not enforced: five sites returned a
// success value from inside the graded scope, so the destructor printed FAIL to stderr
// while the probe exited 0 -- the exact wrong-green outcome this helper exists to
// prevent. gradedRun owns the counter, runs the body in its own frame, lets the grade
// destruct, and only then folds its verdict into the body's result. A `return` inside the
// body returns from the lambda, so it cannot outrun the grade.
//
// WHY THE LAYER GATE IS BUILT IN. `warnings == 0` is true both when the GPU work was
// clean and when no validator was ever loaded, so a probe that grades the count without
// checking validationLayerActive() reports green on any host missing the layer package.
// Fourteen grade sites were in exactly that state. Folding the gate in means there is no
// API here that grades a count alone.
//
// THREE OUTCOMES, NOT TWO. "No Device was built", "no validator was listening" and "the
// device produced warnings" are distinct failures with distinct fixes. The middle one is
// a host problem, not a renderer regression, so a probe that separates those buckets can
// route it into a counter of its own -- see reportUngradedTo().

#include "vulkan/device.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

namespace wallpaperx::testing {

// True when the operator has explicitly accepted running without a validator: a host
// with no vulkan-validation-layers package, or the TSan tier, which builds its bus tests
// without validation on purpose.
//
// THE ONE DEFINITION for both test trees. backend/tests/client/daemon_client.h used to
// carry a byte-identical copy under a comment saying the two tiers must agree on what
// the switch means; two copies of that meaning in one namespace is exactly how they stop
// agreeing, so that header now includes this one. Lives here rather than there because
// the renderer probes cannot reach backend/tests, and the backend tests already have
// renderer/tests on their include path.
inline bool validationWaived() {
    const char* env = std::getenv("WALLPAPERX_ALLOW_NO_VALIDATION");
    return env != nullptr && std::strcmp(env, "1") == 0;
}

class ValidationGrade {
public:
    ValidationGrade(std::string label, int& failures) : m_label(std::move(label)), m_failures(failures) {}

    ValidationGrade(const ValidationGrade&) = delete;
    ValidationGrade& operator=(const ValidationGrade&) = delete;

    // Call immediately after constructing the Device. Taking the shared counter rather
    // than a snapshot is what lets the grade run after the Device is gone.
    void watch(const Device& device) {
        m_counter = device.validationCounter();
        m_layerActive = device.validationLayerActive();
    }

    // Accept a counter captured elsewhere -- for a caller that had to snapshot it as its
    // own scope unwound (a manager owning the Device, say) rather than holding the Device
    // directly. Same three outcomes, same wording, one policy.
    void adopt(std::shared_ptr<const std::atomic<int>> counter, bool layerActive) {
        m_counter = std::move(counter);
        m_layerActive = layerActive;
    }

    // Route "no validator was loaded" to a counter of its own.
    //
    // Deliberately NOT the caller's "could not run" bucket, which was the previous
    // shape: on a host with a working GPU but no layer package every case runs and
    // passes, so reporting them as not-run is simply false. And deliberately not the
    // failure bucket either, which accuses the code under test. It is a third outcome --
    // the work happened, the grading did not -- and it needs a counter and a sentence
    // that say exactly that.
    void reportUngradedTo(int& ungraded) { m_ungraded = &ungraded; }

    // The probe legitimately expects no Device here and has already said so.
    void expectNoDevice() { m_deviceOptional = true; }

    ~ValidationGrade() {
        if (!m_counter) {
            if (!m_deviceOptional) {
                fail(m_failures, "no Vulkan device was ever created, so the validation oracle graded nothing");
            }
            return;
        }
        if (!m_layerActive) {
            // The waiver is checked HERE and nowhere else. As a guard at the top of this
            // destructor it also skipped the warning count below -- so on a TSan host
            // where the layer IS loaded, the layer printed its errors, the counter
            // recorded them, and the grade returned before reading it. Every probe
            // exited 0 over a run whose stderr was full of validation errors. The switch
            // is named "allow no validation"; it may excuse the absence of a validator
            // and nothing else.
            //
            // CONTROL: the layer left loaded, and TWO edits in device.cpp to make the
            // count nonzero -- INFO added to debugMessengerInfo's messageSeverity AND
            // debugCallback's `severity < WARNING` early return lowered to INFO. Both are
            // needed; the first alone changes nothing, because the callback discards
            // sub-WARNING messages before counting. An earlier version of this comment
            // named only the first, which would have made a maintainer re-running it
            // conclude the fix was bogus. With those two edits and
            // WALLPAPERX_ALLOW_NO_VALIDATION=1: 4 "FAIL: ... Vulkan validation
            // warning(s)" lines, exit 1. Restoring the blanket `if (validationWaived())
            // return;` at the top of this destructor, same environment: exit 0, no FAIL.
            // All reverted.
            if (validationWaived()) {
                announceWaiverOnce();
                return;
            }
            fail(m_ungraded ? *m_ungraded : m_failures,
                 "VK_LAYER_KHRONOS_validation is not loaded, so a zero-warning result validates nothing");
            return;
        }
        const int warnings = m_counter->load();
        if (warnings > 0) {
            std::fprintf(stderr, "FAIL: %s: %d Vulkan validation warning(s)\n", m_label.c_str(), warnings);
            ++m_failures;
        }
    }

private:
    // Once per process. Printed per grade site it appeared six times in one
    // descriptor_allocator_probe run, which teaches the reader to skim past the one line
    // that must not be skimmed.
    static void announceWaiverOnce() {
        static bool announced = false;
        if (announced) return;
        announced = true;
        std::printf("WARNING: WALLPAPERX_ALLOW_NO_VALIDATION=1 and no validator is loaded -- the GPU-side "
                    "oracle is DISABLED for this run; a pass is not evidence of GPU correctness\n");
    }

    void fail(int& counter, const char* why) {
        std::fprintf(stderr, "FAIL: %s: %s\n", m_label.c_str(), why);
        ++counter;
    }

    std::string m_label;
    int& m_failures;
    int* m_ungraded = nullptr;
    std::shared_ptr<const std::atomic<int>> m_counter;
    bool m_layerActive = false;
    bool m_deviceOptional = false;
};

// Runs `body(grade)` with a grader that is destroyed before its verdict is folded in.
// `body` returns whether its own checks passed; the result is that AND a clean grade.
template <class Body>
bool gradedRun(std::string label, Body&& body) {
    int failures = 0;
    bool bodyOk = false;
    {
        ValidationGrade grade(std::move(label), failures);
        bodyOk = body(grade);
    }
    return bodyOk && failures == 0;
}

}  // namespace wallpaperx::testing
