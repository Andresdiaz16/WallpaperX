// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Runs only some of a test binary's phases, so a control aimed at one scenario does not pay for
// the whole entry. WALLPAPERX_TEST_PHASE holds an ECMAScript regular expression; a phase runs when
// the expression is found anywhere in its name, the way `ctest -R` matches entry names. Unset or
// empty: every phase runs and this header prints nothing, unless a phase name is wrong (below).
//
// When it is set, every selected phase is announced as it starts, and the end of the run prints
// which phases ran. A filter that selects none of the phases the run offered, or that does not
// parse, fails the run: never a pass, never a skip. A misspelt phase must not let a control go red,
// or green, for any reason other than the break.
//
// The end-of-run verdict is reached through test_report.h's report functions, which every main()
// already calls, so a binary cannot adopt phases and forget the check. It is armed by the first
// phase offered: a namespace or forked child that re-runs this binary offers none, and stays silent.

#include "tests/test_report.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace wallpaperx::testing {

namespace phase_detail {

struct State {
    bool read = false;
    bool active = false;
    bool valid = false;
    std::string pattern;
    std::string parseError;
    std::regex expression;
    std::vector<std::string> offered;
    std::vector<std::string> matched;
    std::vector<std::string> prerequisites;
    std::vector<std::string> duplicates;
    // Every name a phaseSelectedFor() call says depends on it; each must also be offered.
    std::vector<std::string> dependents;
};

inline State& state() {
    static State s;
    if (!s.read) {
        s.read = true;
        const char* value = std::getenv("WALLPAPERX_TEST_PHASE");
        if (value != nullptr && *value != '\0') {
            s.active = true;
            s.pattern = value;
            try {
                s.expression = std::regex(s.pattern, std::regex::ECMAScript);
                s.valid = true;
            } catch (const std::regex_error& e) {
                s.parseError = e.what();
            }
        }
    }
    return s;
}

inline bool matches(State& s, std::string_view name) {
    return s.valid && std::regex_search(name.begin(), name.end(), s.expression);
}

inline std::string joined(const std::vector<std::string>& names) {
    std::string out;
    for (const std::string& name : names) out += (out.empty() ? "" : ", ") + name;
    return out.empty() ? "none" : out;
}

// Prints the run's phase summary once and returns whether the phases make the run red. Cached, so
// the two report functions that ask it cannot print it twice or disagree. Without a filter it prints
// nothing unless a name is wrong.
// GOAL: a filter that selects nothing, does not parse, or cannot tell two phases apart fails the
//       run. False if any of them exits 0, which under a control is a green clean run that graded
//       nothing.
// WHY-THIS-SHAPE: a harness failure, not a skip: ctest counts a skip as passing, so a misspelt
//       phase would read to control_run.sh as "NOT RED" or as a clean restore.
// CONTROL: config_store, 2026-09-29, each reverted. The no-match branch's `red = true` made false ->
//       WALLPAPERX_TEST_PHASE='properties are writen' Passed in 0.02 s; the invalid-regex one ->
//       '(' Passed.
// GOAL: a repeated phase name, or a phaseSelectedFor() dependent no phase is offered as, fails every
//       run, unfiltered included. False if the ordinary ctest run passes over either and only a later
//       control run trips on it, or if a renamed phase silently stops pulling in its prerequisite.
// WHY-THIS-SHAPE: checked here at the end, not in offer(): prerequisites are offered before the
//       phases that name them.
// CONTROL: via control_batch.sh, 2026-09-29, reverted. crash_record_test's "report" and
//       "cleanExit" phases both renamed "killed", so the name is offered three times, --phase
//       '^onDisk$' -> "FAIL (harness): crash_record_test: phase name(s) offered twice: killed -- a
//       filter cannot tell them apart", Failed; with offer()'s second std::find (the already-listed
//       test) also removed -> the same line reads "killed, killed". daemon_system_test's
//       `phaseSelected("configServerLostPort")` renamed "configServerLostItsPort", --phase
//       '^sigintShutdown$' -> "FAIL (harness): daemon_system_test: phase(s) named as depending on a
//       prerequisite were never offered: configServerLostPort", Failed.
inline bool verdict() {
    static int cached = -1;
    if (cached >= 0) return cached == 1;
    State& s = state();
    bool red = false;
    if (!s.active) {
        // Nothing to summarise: every phase ran.
    } else if (!s.valid) {
        std::fprintf(stderr,
                     "FAIL (harness): %s: WALLPAPERX_TEST_PHASE='%s' is not a valid regular expression (%s), so no "
                     "phase ran -- the run is failed, not passed or skipped\n",
                     g_testName, s.pattern.c_str(), s.parseError.c_str());
        red = true;
    } else if (s.matched.empty()) {
        std::fprintf(stderr,
                     "FAIL (harness): %s: WALLPAPERX_TEST_PHASE='%s' matches none of the %zu phases this run offered "
                     "-- nothing was graded, so the run is failed, not passed or skipped. The phases:\n",
                     g_testName, s.pattern.c_str(), s.offered.size());
        for (const std::string& name : s.offered) std::fprintf(stderr, "  offered phase: %s\n", name.c_str());
        red = true;
    } else {
        std::printf("%s: WALLPAPERX_TEST_PHASE='%s' ran %zu of %zu phases: %s; as prerequisites: %s. The other "
                    "phases did not run, so this run grades only these.\n",
                    g_testName, s.pattern.c_str(), s.matched.size(), s.offered.size(), joined(s.matched).c_str(),
                    joined(s.prerequisites).c_str());
        std::fflush(stdout);
    }
    if (!s.duplicates.empty()) {
        std::fprintf(stderr, "FAIL (harness): %s: phase name(s) offered twice: %s -- a filter cannot tell them apart\n",
                     g_testName, joined(s.duplicates).c_str());
        red = true;
    }
    std::vector<std::string> neverOffered;
    for (const std::string& dependent : s.dependents) {
        const bool offered = std::find(s.offered.begin(), s.offered.end(), dependent) != s.offered.end();
        const bool listed = std::find(neverOffered.begin(), neverOffered.end(), dependent) != neverOffered.end();
        if (!offered && !listed) neverOffered.push_back(dependent);
    }
    if (!neverOffered.empty()) {
        std::fprintf(stderr,
                     "FAIL (harness): %s: phase(s) named as depending on a prerequisite were never offered: %s -- a "
                     "renamed phase no longer pulls its prerequisite in (or the run ended before offering it)\n",
                     g_testName, joined(neverOffered).c_str());
        red = true;
    }
    cached = red ? 1 : 0;
    return red;
}

inline bool offer(std::string_view name, std::initializer_list<std::string_view> dependents) {
    State& s = state();
    // Recorded with or without a filter, so the unfiltered runs that gate the code catch a repeated
    // name or a stale dependent, not only the control run that later trips over it.
    g_phaseFilterVerdict = &verdict;
    const std::string own(name);
    // Each repeated name listed once, however often it is offered.
    if (std::find(s.offered.begin(), s.offered.end(), own) != s.offered.end() &&
        std::find(s.duplicates.begin(), s.duplicates.end(), own) == s.duplicates.end()) {
        s.duplicates.push_back(own);
    }
    s.offered.push_back(own);
    for (const std::string_view dependent : dependents) s.dependents.emplace_back(dependent);
    if (!s.active) return true;
    if (matches(s, name)) {
        s.matched.push_back(own);
        std::printf("%s: phase '%s' runs (WALLPAPERX_TEST_PHASE='%s')\n", g_testName, own.c_str(), s.pattern.c_str());
        std::fflush(stdout);
        return true;
    }
    for (const std::string_view dependent : dependents) {
        if (matches(s, dependent)) {
            s.prerequisites.push_back(own);
            std::printf("%s: phase '%s' runs because the selected phase '%.*s' depends on it\n", g_testName, own.c_str(),
                        static_cast<int>(dependent.size()), dependent.data());
            std::fflush(stdout);
            return true;
        }
    }
    return false;
}

}  // namespace phase_detail

// Whether the phase `name` runs. Always true when WALLPAPERX_TEST_PHASE is unset. Names must be
// unique within a binary; a repeated name fails the run, filtered or not.
inline bool phaseSelected(std::string_view name) { return phase_detail::offer(name, {}); }

// For a phase that later phases rely on (shared state, a fixture it leaves): it also runs when any
// of `dependents` is selected, and is announced as a prerequisite rather than counted as selected.
// Each dependent must be the name of a phase the run offers; one that is not fails the run.
inline bool phaseSelectedFor(std::string_view name, std::initializer_list<std::string_view> dependents) {
    return phase_detail::offer(name, dependents);
}

// Runs one phase with its own catch: an exception escaping it fails that phase by name, and the
// phases after it still run. Selection stays with the caller (phaseSelected), which may need the
// answer for more than this call.
// GOAL: S15.328's sibling -- an exception escaping one phase fails that phase by name and ends no
//       other. False if a throw in one phase reaches main's catch-all, which names nothing and
//       cancels every later phase.
// CONTROL: test-side, a phase made to throw at its start: dbus_service_test's descriptorCap, --phase
//       '^(descriptorCap|requestShare)$' -> "FAIL: descriptorCap: unexpected exception: control", and
//       requestShare still announced and run; with this catch removed as well, the run ends in main's
//       "FAIL: unexpected exception: control" and requestShare never runs. The same throw in
//       video_wallpaper_test's undecodable phase -> "FAIL: undecodable: unexpected exception: control",
//       resize still run; in daemon_install_test's checkPlainInstall -> "FAIL: plain install: unexpected
//       exception: control".
template <class Phase>
void guardedPhase(std::string_view name, Phase&& run) {
    try {
        run();
    } catch (const std::exception& e) {
        check(false, (std::string(name) + ": unexpected exception: " + e.what()).c_str());
    }
}

}  // namespace wallpaperx::testing
