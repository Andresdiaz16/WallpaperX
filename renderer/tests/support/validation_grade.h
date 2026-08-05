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
// route it away from its failure counter -- see reportHostProblemTo().

#include "vulkan/device.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

namespace wallpaperx::testing {

// True when the operator has explicitly accepted running without a validator. The TSan
// tier sets this: it builds without validation on purpose, so grading the layer's
// absence there turns the sanitizer red for the one condition it is configured to
// accept. Read here rather than at each site because every site needs it and one that
// forgot would be red for a reason that has nothing to do with its own subject.
inline bool validationGradeWaived() {
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

    // Route "no validator was loaded" to a host-problem counter instead of the failure
    // counter. For a probe that distinguishes "the code is broken" from "this host
    // cannot run the case" -- device_affinity_probe is the whole example -- a missing
    // layer package is squarely the second, and charging it to the failure counter makes
    // the probe accuse the code it is defending.
    void reportHostProblemTo(int& hostProblems) { m_hostProblems = &hostProblems; }

    // The probe legitimately expects no Device here and has already said so.
    void expectNoDevice() { m_deviceOptional = true; }

    ~ValidationGrade() {
        if (validationGradeWaived()) {
            std::printf("WARNING: %s: WALLPAPERX_ALLOW_NO_VALIDATION=1 -- the GPU-side oracle is "
                        "DISABLED for this run; a pass here is not evidence of GPU correctness\n",
                        m_label.c_str());
            return;
        }
        if (!m_counter) {
            if (!m_deviceOptional) {
                fail(m_failures, "no Vulkan device was ever created, so the validation oracle graded nothing");
            }
            return;
        }
        if (!m_layerActive) {
            fail(m_hostProblems ? *m_hostProblems : m_failures,
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
    void fail(int& counter, const char* why) {
        std::fprintf(stderr, "FAIL: %s: %s\n", m_label.c_str(), why);
        ++counter;
    }

    std::string m_label;
    int& m_failures;
    int* m_hostProblems = nullptr;
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
