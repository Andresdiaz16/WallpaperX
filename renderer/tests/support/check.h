// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// One pass/fail reporter for the probe suite.
//
// Five TUs had each hand-rolled the same `int g_failures` + `void check(bool, const char*)`
// pair, with message formats that had already drifted apart ("FAIL: %s" in four of them,
// "FAIL: image_bounds_probe: %s" in the fifth), so the same event printed differently
// depending on which probe produced it and any change to how a failure is reported had to
// be made five times.
//
// Deliberately NOT folded in: device_affinity_probe's reporter. It prints an `ok  ` line
// per case and feeds three counters besides `failures` (harness faults, unrunnable cases,
// ungraded cases) -- unifying it would need a verbosity flag and a counter selector, and
// would change what a passing run of that probe prints.

#include <cstdio>
#include <string_view>

namespace wallpaperx::testing {

// One counter per process. Each probe is its own executable and no probe links another,
// so a single counter is the whole story; if two probes are ever linked into one binary
// this needs a scope.
inline int g_failures = 0;

// Records a failure that is not the result of a condition -- a setup step that could not
// run, a case that could not be reached. Same counter, so it cannot be forgotten in the
// exit code.
inline void fail(std::string_view what) {
    std::fprintf(stderr, "FAIL: %.*s\n", static_cast<int>(what.size()), what.data());
    ++g_failures;
}

inline void check(bool condition, std::string_view what) {
    if (condition) return;
    fail(what);
}

inline int failureCount() { return g_failures; }

// Prints the summary line every probe ends with and returns the count, so the caller can
// `return reportFailures("x") == 0 ? 0 : 1;` without restating the format.
inline int reportFailures(const char* probe) {
    if (g_failures > 0) {
        std::fprintf(stderr, "%s: %d check(s) failed\n", probe, g_failures);
    }
    return g_failures;
}

}  // namespace wallpaperx::testing
