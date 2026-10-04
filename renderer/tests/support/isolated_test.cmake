# Registers ctest entries that run cut off from the desktop session: every entry in this project
# goes through one of the two functions below, never through add_test() directly, and
# test_isolation_registry fails if one does not.
#
#   wallpaperx_add_test(NAME <name> COMMAND <program> [<arg>...])
#   wallpaperx_add_display_test(NAME <name> COMMAND <program> [<arg>...])
#
# Both run the command through isolated_run.sh, which gives it a private HOME and XDG directories,
# a network of its own, no live display, and a private bus with service activation disabled. The display form also
# starts a nested headless KWin for entries that need a real platform -- OpenGL, which the
# offscreen platform cannot give. <program> may be a target name, as with add_test().

include_guard(GLOBAL)

# The one source of these paths for every directory, test_isolation_registry's -DWRAPPER included.
# Cache, not plain variables: under the guard this file runs once, in whichever directory includes
# it first, and a plain variable would stay in that directory's scope.
set(WALLPAPERX_ISOLATION_SUPPORT_DIR "${CMAKE_CURRENT_LIST_DIR}" CACHE INTERNAL "")
set(WALLPAPERX_ISOLATED_RUN "${CMAKE_CURRENT_LIST_DIR}/isolated_run.sh" CACHE INTERNAL "")
set(WALLPAPERX_ISOLATED_SESSION_CONF "${CMAKE_CURRENT_LIST_DIR}/isolated_session.conf" CACHE INTERNAL "")

find_program(WALLPAPERX_DBUS_RUN_SESSION dbus-run-session
             DOC "Gives every ctest entry a private session bus")
find_program(WALLPAPERX_KWIN_WAYLAND kwin_wayland
             DOC "The nested compositor the display entries run under")
if(NOT WALLPAPERX_DBUS_RUN_SESSION)
    message(WARNING "dbus-run-session not found -- every ctest entry runs with no session bus at all.")
endif()

# Reads the caller's arguments from _wx_argc and _wx_arg<N>, which the two public functions copy
# out of their ARGV<N>: forwarding them as ${ARGV} would drop an empty argument, and an empty
# argument is a real one (handle_type_env_default passes "").
function(_wallpaperx_isolated_test mode)
    if(_wx_argc LESS 4 OR NOT _wx_arg0 STREQUAL "NAME" OR NOT _wx_arg2 STREQUAL "COMMAND")
        message(FATAL_ERROR "wallpaperx_add_test: expected NAME <name> COMMAND <program> [<arg>...]")
    endif()
    set(program "${_wx_arg3}")
    if(TARGET "${program}")
        set(program "$<TARGET_FILE:${program}>")
    endif()
    # /bin/sh is matched literally by isolation_registry.cmake.
    set(prefix /bin/sh "${WALLPAPERX_ISOLATED_RUN}" --config "${WALLPAPERX_ISOLATED_SESSION_CONF}")
    if(WALLPAPERX_DBUS_RUN_SESSION)
        list(APPEND prefix --bus "${WALLPAPERX_DBUS_RUN_SESSION}")
    endif()
    if(mode STREQUAL "display")
        # Passed even when not found: the script then says so on every run, rather than the entry
        # quietly skipping for want of a display.
        list(APPEND prefix --display "${WALLPAPERX_KWIN_WAYLAND}")
    endif()
    # Through EVAL with every argument bracket-quoted, for the same empty argument.
    set(code "add_test(NAME [==[${_wx_arg1}]==] COMMAND")
    foreach(arg IN LISTS prefix)
        string(APPEND code " [==[${arg}]==]")
    endforeach()
    string(APPEND code " -- [==[${program}]==]")
    math(EXPR last "${_wx_argc} - 1")
    if(last GREATER_EQUAL 4)
        foreach(i RANGE 4 ${last})
            string(APPEND code " [==[${_wx_arg${i}}]==]")
        endforeach()
    endif()
    string(APPEND code ")")
    cmake_language(EVAL CODE "${code}")
endfunction()

function(wallpaperx_add_test)
    set(_wx_argc ${ARGC})
    math(EXPR last "${ARGC} - 1")
    foreach(i RANGE ${last})
        set(_wx_arg${i} "${ARGV${i}}")
    endforeach()
    _wallpaperx_isolated_test(bus)
endfunction()

# Sets `out` to the number of ctest entries registered so far in `dir` and every directory below
# it: test_isolation_registry's floor, taken from what this configure registered rather than typed
# in, so a host that lacks an optional dependency is not failed for entries it never had.
function(wallpaperx_registered_test_count out dir)
    get_property(tests DIRECTORY "${dir}" PROPERTY TESTS)
    list(LENGTH tests n)
    get_property(subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(sub IN LISTS subdirs)
        wallpaperx_registered_test_count(below "${sub}")
        math(EXPR n "${n} + ${below}")
    endforeach()
    set(${out} ${n} PARENT_SCOPE)
endfunction()

# Sets `out` to `text` escaped as XML element text, for a path written into a bus configuration.
function(wallpaperx_xml_text out text)
    string(REPLACE "&" "&amp;" text "${text}")
    string(REPLACE "<" "&lt;" text "${text}")
    string(REPLACE ">" "&gt;" text "${text}")
    set(${out} "${text}" PARENT_SCOPE)
endfunction()

# Every branch that leaves a group of entries unregistered for want of a dependency says so through
# this. test_isolation_registry's floor follows what was registered, so it cannot notice such a
# drop; with WALLPAPERX_REQUIRE_ALL_TESTS on, the configure fails instead.
option(WALLPAPERX_REQUIRE_ALL_TESTS "Fail the configure when a group of ctest entries cannot be registered" OFF)
function(wallpaperx_tests_not_registered)
    string(JOIN "" text ${ARGN})
    if(WALLPAPERX_REQUIRE_ALL_TESTS)
        message(FATAL_ERROR "${text} (WALLPAPERX_REQUIRE_ALL_TESTS is on)")
    endif()
    message(WARNING "${text}")
endfunction()

# test_isolation_registry, for the calling project. Call it last in the project's CMake file: its
# floor is the number of entries registered when it is called, plus 1 for itself.
function(wallpaperx_add_isolation_registry)
    wallpaperx_registered_test_count(registered ${PROJECT_SOURCE_DIR})
    math(EXPR registered "${registered} + 1")
    wallpaperx_add_test(NAME test_isolation_registry
                        COMMAND ${CMAKE_COMMAND} -DCTEST=${CMAKE_CTEST_COMMAND} -DBUILD_DIR=${CMAKE_BINARY_DIR}
                                -DWRAPPER=${WALLPAPERX_ISOLATED_RUN} -DMINIMUM=${registered}
                                -P ${WALLPAPERX_ISOLATION_SUPPORT_DIR}/isolation_registry.cmake)
    set_tests_properties(test_isolation_registry PROPERTIES LABELS "novulkan" TIMEOUT 60)
endfunction()

# The wrapper driven from outside with a made-up caller (test_isolation_wrapper), and, where there
# is a dbus-run-session, what its private-bus runs leave in /tmp and the paths they accept
# (test_isolation_wrapper_tmp). Needs the wallpaperx-wrapper-probe target (renderer/CMakeLists.txt).
function(wallpaperx_add_isolation_wrapper_tests)
    wallpaperx_add_test(NAME test_isolation_wrapper
                        COMMAND ${CMAKE_COMMAND} -DWRAPPER=${WALLPAPERX_ISOLATED_RUN}
                                -DCONF=${WALLPAPERX_ISOLATED_SESSION_CONF}
                                -DPROBE=$<TARGET_FILE:wallpaperx-wrapper-probe>
                                -P ${WALLPAPERX_ISOLATION_SUPPORT_DIR}/isolation_wrapper_test.cmake)
    set_tests_properties(test_isolation_wrapper PROPERTIES LABELS "novulkan")
    if(WALLPAPERX_DBUS_RUN_SESSION)
        wallpaperx_add_test(NAME test_isolation_wrapper_tmp
                            COMMAND ${CMAKE_COMMAND} -DWRAPPER=${WALLPAPERX_ISOLATED_RUN}
                                    -DCONF=${WALLPAPERX_ISOLATED_SESSION_CONF}
                                    -DBUS=${WALLPAPERX_DBUS_RUN_SESSION}
                                    -P ${WALLPAPERX_ISOLATION_SUPPORT_DIR}/isolation_wrapper_tmp_test.cmake)
        set_tests_properties(test_isolation_wrapper_tmp PROPERTIES LABELS "novulkan" TIMEOUT 120)
    else()
        wallpaperx_tests_not_registered("dbus-run-session not found -- test_isolation_wrapper_tmp (X.25, X.30) is not registered")
    endif()
endfunction()

function(wallpaperx_add_display_test)
    set(_wx_argc ${ARGC})
    math(EXPR last "${ARGC} - 1")
    foreach(i RANGE ${last})
        set(_wx_arg${i} "${ARGV${i}}")
    endforeach()
    _wallpaperx_isolated_test(display)
endfunction()
