#pragma once

// Grades a Device's validation-warning count, at the right time and with the guard that
// makes the count mean anything.
//
// WHY AN OBJECT AND NOT A FUNCTION. The grade has to happen after ~Device: the layer's
// most valuable complaints -- a resource still in flight, an object destroyed twice, a
// leaked allocation -- are only emitted during destruction. Declare this BEFORE the
// Device it watches and reverse destruction order puts the grade after it, for free, on
// every exit path including a throw. Thirteen hand-written copies of that ordering
// existed before this header; one of them (modifier_roundtrip_probe) had drifted into a
// check that could never fail, because the capture sat after a `return` its own mode
// never reached, and nothing made the divergence visible.
//
// WHY THE LAYER GATE IS BUILT IN. `warnings == 0` is true both when the GPU work was
// clean and when no validator was ever loaded, so a probe that grades the count without
// checking validationLayerActive() reports green on any host missing the layer package.
// Fourteen grade sites were in exactly that state. Making the gate part of the helper
// means a site cannot forget it -- there is no API here that grades a count alone.
//
// THREE OUTCOMES, NOT TWO. "No Device was built", "no validator was listening" and "the
// device produced warnings" are distinct failures with distinct fixes, and collapsing
// them is how a missing ICD came to be reported as a missing layer package.

#include "vulkan/device.h"

#include <atomic>
#include <cstdio>
#include <memory>
#include <string>

namespace wallpaperx::testing {

class ValidationGrade {
public:
    // `failures` is the probe's own counter; this adds to it rather than reporting
    // independently, so a probe's exit status keeps its single source of truth.
    ValidationGrade(std::string label, int& failures) : m_label(std::move(label)), m_failures(failures) {}

    ValidationGrade(const ValidationGrade&) = delete;
    ValidationGrade& operator=(const ValidationGrade&) = delete;

    // Call immediately after constructing the Device. Taking the shared counter rather
    // than a snapshot is what lets the grade run after the Device is gone.
    void watch(const Device& device) {
        m_counter = device.validationCounter();
        m_layerActive = device.validationLayerActive();
    }

    // Set when a probe legitimately expects no Device on this host and has already
    // reported that itself; suppresses the "no Device was built" failure only.
    void expectNoDevice() { m_deviceOptional = true; }

    ~ValidationGrade() {
        if (!m_counter) {
            if (!m_deviceOptional) {
                fail("no Vulkan device was ever created, so the validation oracle graded nothing");
            }
            return;
        }
        if (!m_layerActive) {
            fail("VK_LAYER_KHRONOS_validation is not loaded, so a zero-warning result validates nothing");
            return;
        }
        const int warnings = m_counter->load();
        if (warnings > 0) {
            std::fprintf(stderr, "FAIL: %s: %d Vulkan validation warning(s)\n", m_label.c_str(), warnings);
            ++m_failures;
        }
    }

private:
    void fail(const char* why) {
        std::fprintf(stderr, "FAIL: %s: %s\n", m_label.c_str(), why);
        ++m_failures;
    }

    std::string m_label;
    int& m_failures;
    std::shared_ptr<const std::atomic<int>> m_counter;
    bool m_layerActive = false;
    bool m_deviceOptional = false;
};

}  // namespace wallpaperx::testing
