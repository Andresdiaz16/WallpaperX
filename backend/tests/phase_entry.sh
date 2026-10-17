#!/bin/sh
# The entry named after a test binary whose phases are registered one ctest entry each
# (wallpaperx_add_phase_tests in backend/CMakeLists.txt).
#
#   phase_entry.sh <registered-phase-list> <program> [<arg>...]
#
# With WALLPAPERX_TEST_PHASE set by the caller it runs the program with that filter and returns its
# status, so control_run.sh --phase and test_phases.sh drive the whole binary as before. Unset, it
# compares the phases the program offers with the list registered at configure time and fails on
# any difference: a phase with no entry of its own would otherwise never run.
set -u
list="$1"
shift

if [ -n "${WALLPAPERX_TEST_PHASE-}" ]; then
    exec "$@"
fi

if [ ! -r "$list" ]; then
    echo "FAIL (harness): the registered phase list $list cannot be read"
    exit 1
fi

# A filter that can match nothing: the program offers every phase, runs none, fails, and names them.
out=$(WALLPAPERX_TEST_PHASE='(?!)' "$@" 2>&1)
status=$?
offered=$(printf '%s\n' "$out" | sed -n 's/^  offered phase: //p' | sort)
if [ -z "$offered" ]; then
    printf '%s\n' "$out"
    if [ "$status" -eq 77 ]; then
        echo "phase list not compared: the program skipped before offering any phase"
        exit 77
    fi
    echo "FAIL (harness): the program offered no phase (status $status), so its phase list cannot be compared"
    exit 1
fi

registered=$(sort "$list")
unregistered=$(printf '%s\n' "$offered" | grep -vxF -e "$registered")
stale=$(printf '%s\n' "$registered" | grep -vxF -e "$offered")
red=0
if [ -n "$unregistered" ]; then
    echo "FAIL: phase(s) the program offers with no ctest entry of their own, so a full run never runs them:"
    printf '  %s\n' $unregistered
    red=1
fi
if [ -n "$stale" ]; then
    echo "FAIL: registered phase entries the program no longer offers, each of which fails as matching nothing:"
    printf '  %s\n' $stale
    red=1
fi
if [ "$red" -eq 1 ]; then
    echo "Re-run the CMake configure step; if that does not fix it, the phase names are not literals" \
         "wallpaperx_add_phase_tests can read from the source"
    exit 1
fi
echo "phase list: the program offers $(printf '%s\n' "$offered" | wc -l) phases, each registered as an entry"
exit 0
