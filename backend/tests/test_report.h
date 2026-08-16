// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The pass / fail / skip reporting contract every test binary in backend/tests/ obeys, in
// one place.
//
// The contract has three parts:
//
//   1. Exit 77 means "a goal could not be exercised here", which ctest turns into SKIP
//      through the SKIP_RETURN_CODE property in backend/CMakeLists.txt. Exit 1 means a
//      check failed. Exit 0 means every goal ran and held.
//   2. A failed check outranks an unexercised goal. A run that is both broken and
//      incomplete must report as broken, or ctest's "Skipped" hides the failures.
//   3. The wording says out loud that a skip is not a pass. That sentence is the whole
//      point of the 77 channel, and it is the first thing a retyped copy drops.
//
// One definition rather than one per binary, for the reason tests/client/daemon_client.h
// gives: a contract six files each state separately is six things that can disagree, and
// a copy that drops part 3 or writes its skip line to the wrong stream still compiles,
// still passes, and still reads to a developer as coverage.
//
// Deliberately dependency-free -- <cstdio> and <string> only. It is included by the test
// that links nothing but nlohmann_json as well as by the ones that link the whole daemon,
// so anything heavier here would cost the light one its independence.
//
// What stays out: anything specific to one file's harness. daemon_system_test's
// phaseSetupFault(what, detail) prints two stdout lines about a dead fork and returns before the
// skip/pass summary; it composes with the two report functions below instead of being folded
// into them.

#include <cstdio>
#include <string>

namespace wallpaperx::testing {

// ctest reads this as SKIP (SKIP_RETURN_CODE in backend/CMakeLists.txt). Reserved for
// "this host could not set the check up", never for "the check did not like what it saw".
constexpr int kNotVerifiedExitCode = 77;

// Named in every line these functions print. Obviously wrong by default so a binary that
// forgets to set it prints "unnamed_test: all checks passed" -- visibly broken -- rather
// than an empty prefix that reads like formatting.
// CONTROL: a scratch program including this header, reporting without assigning here ->
//       `unnamed_test: all checks passed`, exit 0.
inline const char* g_testName = "unnamed_test";

// Checks that were exercised and did not hold.
inline int g_failures = 0;

// Goals this run could not exercise at all, because the host or the harness could not build
// the precondition -- as opposed to goals it exercised and that held. Not a check failure:
// reporting a fault of the host or the harness as a fault of the code under test points the
// reader at the wrong component.
inline int g_notVerified = 0;

inline void check(bool cond, const char* msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++g_failures;
    }
}

// A fault in the test file itself, or in a tool it drives, rather than in the code it grades.
// Counted apart from check() so the summary cannot send a reader to the component under test
// for a fault that is in the harness. Still red, never a skip: a harness that cannot be
// trusted must not go on issuing verdicts.
//
// Not the only such channel here. daemon_system_test keeps its own phaseSetupFault(what, detail)
// and g_phaseSetupFaults; the names are kept distinct from these on purpose, because the count a
// file prints is the one it reads and a site reaching for the wrong one would file a fault into a
// counter nobody summarises. reportNotVerifiedOrPassed() below is the backstop for that mistake.
//
// CONTROL: the increment below deleted -- kpackage_metadata_test, with its QML comment
//       stripper broken so its own self-test fails, went from `1 fault(s) in this test file,
//       not in the package` and exit 1 to `all checks passed` and exit 0; qml_lint_test, driven
//       by a stand-in linter whose `success` field disagrees with its diagnostics, likewise
//       went from exit 1 to exit 0 with the fault line still printed. Restored.
inline int g_harnessFaults = 0;

inline void harnessFailure(const std::string& what) {
    std::fprintf(stderr, "FAIL (harness): %s\n", what.c_str());
    ++g_harnessFaults;
}

// stdout with an explicit flush, while check() writes to stderr: the two streams are
// buffered differently, so a NOT VERIFIED line left in stdout's buffer can surface after
// FAIL lines that were printed later. Flushing at each stdout write is what keeps the
// reasons adjacent to the phase they came from under ctest.
inline void notVerified(const char* goals, const std::string& reason) {
    std::printf("%s: NOT VERIFIED -- %s could not be exercised: %s. This run establishes nothing about them, and "
                "is reported to ctest as a skip rather than as a pass.\n",
                g_testName, goals, reason.c_str());
    std::fflush(stdout);
    ++g_notVerified;
}

// Prints the failure summary, if there is one. Returns true when the run must go red.
//
// Split from reportNotVerifiedOrPassed() rather than folded into it because two callers put
// their own red channel between the two halves, and they order it differently:
// kpackage_metadata_test prints its harness-fault line after this summary and returns 1 for
// either, daemon_system_test returns 1 here and never reaches its harness line. A single
// do-everything epilogue would have to pick one of those orders for both.
[[nodiscard]] inline bool reportCheckFailures() {
    if (g_failures > 0) {
        std::fprintf(stderr, "%s: %d check(s) failed\n", g_testName, g_failures);
        return true;
    }
    return false;
}

// Prints the harness-fault summary, if there is one. Returns true when the run must go red.
//
// `whereClause` completes "N fault(s) ...", as a whole clause rather than a noun, for the same
// reason reportNotVerifiedOrPassed() takes one: the callers must keep saying different things
// -- one of them drives an external tool and has to name it as a candidate culprit, the other
// has no tool to blame.
[[nodiscard]] inline bool reportHarnessFaults(const char* whereClause) {
    if (g_harnessFaults > 0) {
        std::fprintf(stderr, "%s: %d fault(s) %s\n", g_testName, g_harnessFaults, whereClause);
        return true;
    }
    return false;
}

// What most callers pass: the goals that could not run are a property of the machine.
constexpr const char* kSkipReasonHost = "goal(s) could not be exercised on this host (listed above)";

// The tail of main(): exit 77 naming the goals that never ran, or 0 and the all-passed line.
//
// `skipClause` is a whole clause, not a noun, because http_server_test must not say "on this
// host": its skip reasons are a mix of host limits and preconditions the run could not
// build, and naming one cause for all of them points a reader at the wrong component.
// `passDetail` is appended to the all-passed line for the one file that reports how much it
// covered, so a run that quietly stopped sweeping is visible.
[[nodiscard]] inline int reportNotVerifiedOrPassed(const char* skipClause = kSkipReasonHost,
                                                   const std::string& passDetail = "") {
    // Part 2 of the contract, enforced here rather than trusted to each main(): reaching
    // this with failures pending would return 77 and let ctest print "Skipped" over a run
    // that had red in it. Unreachable from all six callers today -- they all ask
    // reportCheckFailures() first -- which is exactly why it is a guard and not a comment.
    // CONTROL: a scratch program that failed a check and then called this directly, skipping
    //       reportCheckFailures() -> the line below and exit 1, not 77.
    if (g_failures > 0) {
        std::fprintf(stderr, "%s: reached the skip/pass summary with %d failed check(s) -- reporting the run as "
                             "failed, not skipped\n",
                     g_testName, g_failures);
        return 1;
    }
    // The same guard for the harness channel, and it needs to be here more than the one above
    // does: harnessFailure() is reachable from any file that includes this header, while only
    // the files that also call reportHarnessFaults() read the count back. A fault recorded and
    // never read would otherwise leave this returning 0 or 77 over a harness known to be
    // broken.
    // CONTROL: harnessFailure() called once at the top of daemon_system_test's main(), which
    //       summarises only its own g_phaseSetupFaults -> the line below and exit 1. With this
    //       branch disabled and the same call in place, the run reported `Passed`. Both reverted.
    if (g_harnessFaults > 0) {
        std::fprintf(stderr, "%s: reached the skip/pass summary with %d unreported harness fault(s) -- reporting "
                             "the run as failed, not skipped\n",
                     g_testName, g_harnessFaults);
        return 1;
    }
    if (g_notVerified > 0) {
        std::printf("%s: every check that ran passed, but %d %s -- reported to ctest as a skip, not a pass.\n",
                    g_testName, g_notVerified, skipClause);
        return kNotVerifiedExitCode;
    }
    // No fflush on either line, unlike notVerified() above: both are the last write before
    // main() returns, and nothing can be printed after them to be reordered against.
    std::printf("%s: all checks passed%s\n", g_testName, passDetail.c_str());
    return 0;
}

}  // namespace wallpaperx::testing
