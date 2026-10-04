# test_isolation_registry: every entry ctest would run in BUILD_DIR goes through isolated_run.sh,
# and every entry that asks Qt for a real display platform gets the nested compositor.
#
#   cmake -DCTEST=<ctest> -DBUILD_DIR=<top build dir> -DWRAPPER=<isolated_run.sh> -DMINIMUM=<n> -P this
#
# Read from ctest's own listing rather than from the CMake files, so an entry registered by any
# route -- a bare add_test(), a generated one -- is seen exactly as ctest will run it.
#
# GOAL: no entry ctest runs inherits the caller's session; none asking Qt for wayland or xcb runs
#       without the nested compositor; none sets LD_PRELOAD for the whole wrapper chain.
# WHY-THIS-SHAPE: grepping the CMake files for add_test( would miss what a module registers.
#       "Runs through the wrapper" is what ctest executes -- /bin/sh, the wrapper, --config and the
#       session configuration beside it -- not the wrapper's path appearing somewhere in the command.
#       The platform is read as Qt reads it, a ';' list of names each with optional ':' options,
#       from both properties that can set it; a path in the options is not a platform.
# CONTROL: observed 2026-10-01 in backend/CMakeLists.txt, each reverted. `add_test(NAME
#       control_bare_entry COMMAND true)` -> "FAIL: control_bare_entry does not run through ...";
#       an entry `COMMAND true <wrapper> --config <conf> -- true` -> the same line for it; one with
#       `--config` replaced by `--bus` -> the same line; `wallpaperx_add_test` with
#       QT_QPA_PLATFORM=wayland\;xcb\;offscreen -> "FAIL: ... asks Qt for a display platform but is
#       not registered with the nested compositor"; the same through ENVIRONMENT_MODIFICATION
#       QT_QPA_PLATFORM=set:wayland -> the same line; an ENVIRONMENT LD_PRELOAD -> "FAIL: ... sets
#       LD_PRELOAD"; and QT_QPA_PLATFORM=offscreen:configfile=/x/wayland-xcb/s.json stayed green
#       (a whole-text match had reported it). -DWRAPPER naming no file -> "FAIL (harness): ... does
#       not exist". In a scratch project, an entry `/bin/sh <wrapper> --private-network x --config
#       <conf> -- true`, which skips the wrapper's network step and passes on the shared network ->
#       "FAIL: bypass_first does not run through ...". 2026-10-04, control_batch, in all three trees:
#       -DBUILD_DIR naming a directory with no entries -> "does not include test_isolation_registry
#       itself" and "only 0 entries were listed".
# COVERAGE-ASSERT: the listing must contain this entry and at least MINIMUM entries, so a listing
#       of the wrong directory, or an empty one, cannot pass. MINIMUM is what the configure
#       registered, so it does not notice a configure that dropped a group for want of a
#       dependency; WALLPAPERX_REQUIRE_ALL_TESTS (isolated_test.cmake) fails that configure instead.

foreach(required CTEST BUILD_DIR WRAPPER MINIMUM)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "FAIL (harness): test_isolation_registry was run without -D${required}")
    endif()
endforeach()
# A bad path here is the harness's fault, and would otherwise read as every entry escaping the wrapper.
if(NOT EXISTS "${WRAPPER}")
    message(FATAL_ERROR "FAIL (harness): WRAPPER ${WRAPPER} does not exist")
endif()

file(REAL_PATH "${WRAPPER}" wrapper)
get_filename_component(support_dir "${wrapper}" DIRECTORY)
set(session_conf "${support_dir}/isolated_session.conf")
execute_process(COMMAND "${CTEST}" --show-only=json-v1
                WORKING_DIRECTORY "${BUILD_DIR}"
                OUTPUT_VARIABLE listing ERROR_VARIABLE listing_err RESULT_VARIABLE listing_rc)
if(NOT listing_rc EQUAL 0)
    message(FATAL_ERROR "FAIL (harness): ctest --show-only=json-v1 in ${BUILD_DIR} failed (${listing_rc}): ${listing_err}")
endif()
string(JSON count ERROR_VARIABLE json_err LENGTH "${listing}" tests)
if(json_err)
    message(FATAL_ERROR "FAIL (harness): ctest's listing did not parse: ${json_err}")
endif()

# Sets `out` to the values entry `i` gives `variable`, joined by newlines (a value may hold ';'):
# ENVIRONMENT's NAME=value elements, and ENVIRONMENT_MODIFICATION's NAME=op:value with the op
# dropped. A property whose value is a single string rather than an array is one element.
function(_wx_env_values i variable out)
    set(values "")
    string(JSON nprops ERROR_VARIABLE no_props LENGTH "${listing}" tests ${i} properties)
    if(NOT no_props AND nprops GREATER 0)
        math(EXPR last_prop "${nprops} - 1")
        foreach(p RANGE ${last_prop})
            string(JSON pname GET "${listing}" tests ${i} properties ${p} name)
            if(NOT pname STREQUAL "ENVIRONMENT" AND NOT pname STREQUAL "ENVIRONMENT_MODIFICATION")
                continue()
            endif()
            string(JSON ptype TYPE "${listing}" tests ${i} properties ${p} value)
            set(elements "")
            if(ptype STREQUAL "ARRAY")
                string(JSON n LENGTH "${listing}" tests ${i} properties ${p} value)
                if(n GREATER 0)
                    math(EXPR last_k "${n} - 1")
                    foreach(k RANGE ${last_k})
                        string(JSON e GET "${listing}" tests ${i} properties ${p} value ${k})
                        string(REPLACE ";" "\\;" e "${e}")
                        list(APPEND elements "${e}")
                    endforeach()
                endif()
            else()
                string(JSON e GET "${listing}" tests ${i} properties ${p} value)
                string(REPLACE ";" "\\;" e "${e}")
                list(APPEND elements "${e}")
            endif()
            foreach(e IN LISTS elements)
                if(NOT e MATCHES "^${variable}=(.*)$")
                    continue()
                endif()
                set(v "${CMAKE_MATCH_1}")
                if(pname STREQUAL "ENVIRONMENT_MODIFICATION")
                    if(NOT v MATCHES "^[a-z_]+:(.*)$")
                        continue()  # reset:, unset: -- no value
                    endif()
                    set(v "${CMAKE_MATCH_1}")
                endif()
                string(APPEND values "${v}\n")
            endforeach()
        endforeach()
    endif()
    set(${out} "${values}" PARENT_SCOPE)
endfunction()

set(failures 0)
set(graded 0)
set(display_entries 0)
set(saw_self FALSE)
# Built apart from the loop: RANGE of an empty listing's -1 is a CMake error, not an empty loop, and
# an empty listing must reach the FAIL lines below.
set(indices "")
if(count GREATER 0)
    math(EXPR last "${count} - 1")
    foreach(i RANGE ${last})
        list(APPEND indices ${i})
    endforeach()
endif()
foreach(i IN LISTS indices)
    string(JSON name GET "${listing}" tests ${i} name)
    if(name STREQUAL "test_isolation_registry")
        set(saw_self TRUE)
    endif()
    string(JSON argc ERROR_VARIABLE no_command LENGTH "${listing}" tests ${i} command)
    if(no_command)
        message("FAIL: ${name} has no command ctest can show, so nothing says how it runs")
        math(EXPR failures "${failures} + 1")
        continue()
    endif()
    set(interpreter "")
    set(script "")
    set(config_flag "")
    set(config "")
    set(display FALSE)
    set(separated FALSE)
    string(JSON interpreter GET "${listing}" tests ${i} command 0)
    if(argc GREATER 3)
        string(JSON script GET "${listing}" tests ${i} command 1)
        string(JSON config_flag GET "${listing}" tests ${i} command 2)
        string(JSON config GET "${listing}" tests ${i} command 3)
        math(EXPR last_arg "${argc} - 1")
        foreach(a RANGE 1 ${last_arg})
            string(JSON arg GET "${listing}" tests ${i} command ${a})
            if(arg STREQUAL "--")
                set(separated TRUE)
                break()
            endif()
            if(arg STREQUAL "--display")
                set(display TRUE)
            endif()
        endforeach()
    endif()
    foreach(path script config)
        if(EXISTS "${${path}}")
            file(REAL_PATH "${${path}}" ${path})
        endif()
    endforeach()
    # The literal isolated_test.cmake emits: an entry that names the script but runs something
    # else first is that something else, with the caller's session.
    if(NOT interpreter STREQUAL "/bin/sh" OR NOT script STREQUAL wrapper OR NOT config_flag STREQUAL "--config" OR
       NOT config STREQUAL session_conf OR NOT separated)
        message("FAIL: ${name} does not run through ${WRAPPER} with its session configuration -- it would inherit "
                "the caller's HOME, display, bus and network")
        math(EXPR failures "${failures} + 1")
        continue()
    endif()
    # An entry that names wayland or xcb as its Qt platform needs a real display; without the nested
    # compositor it would reach none, or the caller's if one leaked through.
    set(wants_display FALSE)
    _wx_env_values(${i} QT_QPA_PLATFORM platforms)
    string(REPLACE "\n" ";" platforms "${platforms}")
    foreach(item IN LISTS platforms)
        string(REGEX REPLACE ":.*$" "" bare "${item}")
        string(STRIP "${bare}" bare)
        if(bare MATCHES "^(wayland|xcb)")
            set(wants_display TRUE)
        endif()
    endforeach()
    if(wants_display AND NOT display)
        message("FAIL: ${name} asks Qt for a display platform but is not registered with the nested compositor")
        math(EXPR failures "${failures} + 1")
    endif()
    # A per-entry preload reaches /bin/sh, dbus-run-session and the bus daemon too, not just the
    # test: a sanitizer runtime preloaded into the shell turns its exit into a leak report.
    _wx_env_values(${i} LD_PRELOAD preloads)
    if(NOT preloads STREQUAL "")
        message("FAIL: ${name} sets LD_PRELOAD through its environment, which reaches every process of the wrapper "
                "chain; put it after the wrapper's -- instead (env LD_PRELOAD=... <program>)")
        math(EXPR failures "${failures} + 1")
    endif()
    if(display)
        math(EXPR display_entries "${display_entries} + 1")
    endif()
    math(EXPR graded "${graded} + 1")
endforeach()

message("test_isolation_registry: ${graded} of ${count} entries run isolated, ${display_entries} of them under a nested compositor")
# The listing is of the tree this entry belongs to, and holds at least as many entries as that
# tree is known to register: a listing of the wrong directory, or an empty one, grades nothing.
if(NOT saw_self)
    message("FAIL: ctest's listing of ${BUILD_DIR} does not include test_isolation_registry itself, so it is not this tree's")
    math(EXPR failures "${failures} + 1")
endif()
if(count LESS MINIMUM)
    message("FAIL: only ${count} entries were listed, fewer than the ${MINIMUM} this tree registers at the least")
    math(EXPR failures "${failures} + 1")
endif()
if(failures GREATER 0)
    message(FATAL_ERROR "test_isolation_registry: ${failures} failure(s)")
endif()
