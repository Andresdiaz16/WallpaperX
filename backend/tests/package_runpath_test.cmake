# SPDX-License-Identifier: GPL-2.0-or-later
#
# S16.21: the plugin inside the assembled package has no RPATH or RUNPATH. glibc reads each empty
# entry of a run path as the loading process's working directory, so a build-tree run path would let
# plasmashell load libraries from wherever it was started.
#   cmake -DPACKAGE_DIR=<assembled package> -DMODULE_NAME=<module folder> -P package_runpath_test.cmake

find_program(READELF readelf)
if(NOT READELF)
    message("NOT VERIFIED: readelf is not installed, so the packaged plugin's run path is not read")
    return()
endif()
file(GLOB plugins "${PACKAGE_DIR}/contents/ui/${MODULE_NAME}/*.so")
# GOAL: S16.21 -- no .so in the package's module folder has a RPATH or RUNPATH entry.
# WHY-THIS-SHAPE: readelf's dynamic section, not `ldd`: ldd resolves through system paths too and
#   says nothing about which entry it used. A glob that found nothing would pass vacuously, so the
#   count of plugins read is asserted.
# CONTROL: 2026-11-05, backend/packaging/assemble_package.cmake without its file(RPATH_REMOVE): red,
#   "FAIL: the packaged plugin .../libwallpaperxplugin.so has no run path: (RUNPATH) Library runpath:
#   [::::::::::::::::::::::]".
list(LENGTH plugins count)
if(count EQUAL 0)
    message(FATAL_ERROR "FAIL: the package holds a plugin to read: nothing matches ${PACKAGE_DIR}/contents/ui/${MODULE_NAME}/*.so")
endif()
set(failures 0)
foreach(plugin IN LISTS plugins)
    execute_process(COMMAND "${READELF}" -d "${plugin}" OUTPUT_VARIABLE dynamic RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "FAIL: readelf could read ${plugin}: exit ${rc}")
    endif()
    string(REGEX MATCHALL "\\((RPATH|RUNPATH)\\)[^\n]*" paths "${dynamic}")
    if(paths)
        message("FAIL: the packaged plugin ${plugin} has no run path: ${paths}")
        math(EXPR failures "${failures} + 1")
    endif()
endforeach()
if(failures GREATER 0)
    message(FATAL_ERROR "${failures} check(s) failed")
endif()
message("package_no_runpath: ${count} plugin(s) read, none has a run path")
