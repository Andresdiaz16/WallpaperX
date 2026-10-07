# test_ci_namespace_probe: the continuous-integration jobs check that the runner can give tests a network
# of their own by running isolated_run.sh's own probe, so the check cannot drift from what the wrapper
# needs. Graded here because CI itself cannot be run from this machine.
#
#   cmake -DWRAPPER=<isolated_run.sh> -DCI=<.github/workflows/ci.yml> -P this
#
# GOAL: S15.357 -- every "Allow the tests their own namespaces" step in ci.yml runs
#       `sh renderer/tests/support/isolated_run.sh --config /dev/null -- true` (two jobs, two steps), and
#       that command is the wrapper's probe: it succeeds on a host that can make the namespaces and fails
#       with the wrapper's own words, exit 2, on one where it cannot.
# WHY-THIS-SHAPE: the step's line is matched exactly and counted per step, so a step that went back to a
#       hand-written probe, or lost the call, fails here. "Cannot make the namespaces" is staged by a PATH
#       without `ip`, the one tool the probe needs that a runner may lack; the wrapper refuses before it
#       starts anything.
# CONTROL: via control_batch.sh, 2026-10-06, each reverted: the second step's line made a hand-written
#       `unshare --user --map-root-user --net true` -> "FAIL: each of ci.yml's two namespace steps runs the
#       wrapper's own probe (1 of 2 do)"; isolated_run.sh's refusal reworded -> "FAIL: without ip the CI
#       step's probe fails with the wrapper's own words, exit 2 (...)"; isolated_run.sh made to exit 3 when
#       its config is /dev/null -> "FAIL: the CI step's probe succeeds on a host that can give a test its
#       own namespaces (exit 3: )".
# COVERAGE-ASSERT: two steps must have been found, or the count of calls means nothing.

foreach(required WRAPPER CI)
    if(NOT DEFINED ${required} OR NOT EXISTS "${${required}}")
        message(FATAL_ERROR "FAIL (harness): test_ci_namespace_probe was run without an existing -D${required}")
    endif()
endforeach()

set(probe_line "sh renderer/tests/support/isolated_run.sh --config /dev/null -- true")
file(STRINGS "${CI}" lines)
set(steps 0)
set(calls 0)
set(in_step FALSE)
foreach(line IN LISTS lines)
    if(line MATCHES "^ *- name: ")
        set(in_step FALSE)
        if(line MATCHES "- name: Allow the tests their own namespaces$")
            set(in_step TRUE)
            math(EXPR steps "${steps} + 1")
        endif()
    elseif(in_step)
        string(STRIP "${line}" stripped)
        if(stripped STREQUAL probe_line)
            math(EXPR calls "${calls} + 1")
        endif()
    endif()
endforeach()

set(failures 0)
if(NOT steps EQUAL 2)
    message("FAIL (harness): ci.yml holds ${steps} \"Allow the tests their own namespaces\" step(s), not 2")
    math(EXPR failures "${failures} + 1")
elseif(NOT calls EQUAL 2)
    message("FAIL: each of ci.yml's two namespace steps runs the wrapper's own probe (${calls} of 2 do)")
    math(EXPR failures "${failures} + 1")
endif()

# The repository root: the step's command is written relative to it.
get_filename_component(repo "${CI}/../../.." ABSOLUTE)
execute_process(COMMAND /bin/sh "${WRAPPER}" --config /dev/null -- true
                WORKING_DIRECTORY "${repo}" OUTPUT_VARIABLE o ERROR_VARIABLE e RESULT_VARIABLE r TIMEOUT 60)
if(NOT r EQUAL 0)
    message("FAIL: the CI step's probe succeeds on a host that can give a test its own namespaces (exit ${r}: ${e})")
    math(EXPR failures "${failures} + 1")
endif()

# A PATH holding everything the wrapper uses but ip.
set(bin "$ENV{TMPDIR}/ci-probe-bin")
file(REMOVE_RECURSE "${bin}")
file(MAKE_DIRECTORY "${bin}")
foreach(tool sh unshare readlink id cat env)
    find_program(found_${tool} ${tool} NO_CACHE)
    if(found_${tool})
        file(CREATE_LINK "${found_${tool}}" "${bin}/${tool}" SYMBOLIC)
    endif()
endforeach()
execute_process(COMMAND env "PATH=${bin}" /bin/sh "${WRAPPER}" --config /dev/null -- true
                WORKING_DIRECTORY "${repo}" OUTPUT_VARIABLE o ERROR_VARIABLE e RESULT_VARIABLE r TIMEOUT 60)
file(REMOVE_RECURSE "${bin}")
if(NOT r EQUAL 2 OR NOT e MATCHES "cannot be given a network of its own")
    message("FAIL: without ip the CI step's probe fails with the wrapper's own words, exit 2 (exit ${r}: ${e})")
    math(EXPR failures "${failures} + 1")
endif()

if(failures GREATER 0)
    message(FATAL_ERROR "test_ci_namespace_probe: ${failures} check(s) failed")
endif()
message("test_ci_namespace_probe: both namespace steps call the wrapper's probe, which passes here and fails without ip")
