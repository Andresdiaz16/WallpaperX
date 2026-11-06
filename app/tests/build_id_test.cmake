# SPDX-License-Identifier: GPL-2.0-or-later
#
# S16.11: wallpaperx_write_build_id (backend/packaging/content_id.cmake), which both bundle halves
# are stamped with, is a function of the files' contents, and the staged bundle's ids are that
# function of what it ships.
#   cmake -DCONTENT_ID=<content_id.cmake> -DSCRATCH=<dir> [-DBUNDLE=<folder holding daemon/ and plasma/>] -P build_id_test.cmake

include("${CONTENT_ID}")
set(d "${SCRATCH}/build-id-test")
file(REMOVE_RECURSE "${d}")
file(WRITE "${d}/a" "one")
file(WRITE "${d}/sub/b" "two")
file(CREATE_LINK a "${d}/l" SYMBOLIC)

set(failures 0)
macro(fail message)
    message("FAIL: ${message}")
    math(EXPR failures "${failures} + 1")
endmacro()
macro(id_of dir out)
    wallpaperx_write_build_id("${dir}")
    file(READ "${dir}/build-id" ${out})
endmacro()

id_of("${d}" first)
id_of("${d}" second)
string(REGEX MATCH "^[0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]\n$" shape "${first}")
# CONTROL: 2026-11-05, backend/packaging/content_id.cmake cutting the hash to 15 characters: red,
#   "FAIL: the build id is 16 lowercase hex characters".
if(shape STREQUAL "")
    fail("the build id is 16 lowercase hex characters: '${first}'")
endif()
if(NOT first STREQUAL second)
    fail("the same files give the same build id: '${first}' then '${second}'")
endif()

file(WRITE "${d}/sub/b" "twO")
id_of("${d}" changed)
# CONTROL: 2026-11-05, backend/packaging/content_id.cmake listing names without content hashes: red,
#   "FAIL: a changed file content changes the build id".
if(changed STREQUAL first)
    fail("a changed file content changes the build id")
endif()
file(WRITE "${d}/sub/b" "two")
id_of("${d}" back)
if(NOT back STREQUAL first)
    fail("restoring the content restores the build id")
endif()

file(WRITE "${d}/c" "")
id_of("${d}" added)
if(added STREQUAL first)
    fail("an added empty file changes the build id")
endif()
file(REMOVE "${d}/c")

file(REMOVE "${d}/l")
file(CREATE_LINK sub/b "${d}/l" SYMBOLIC)
id_of("${d}" relinked)
# CONTROL: 2026-11-05, backend/packaging/content_id.cmake listing a link without its target: red,
#   "FAIL: a changed link target changes the build id".
if(relinked STREQUAL first)
    fail("a changed link target changes the build id")
endif()

# The staged halves: each carries an id, and it is the content id of the files beside it.
if(BUNDLE)
    foreach(half daemon plasma/wallpapers/com.wallpaperx.wallpaper)
        set(shipped_file "${BUNDLE}/${half}/build-id")
        if(NOT EXISTS "${shipped_file}")
            message("FAIL: the staged ${half} carries a build-id")
            math(EXPR failures "${failures} + 1")
            continue()
        endif()
        file(READ "${shipped_file}" shipped)
        set(copy "${SCRATCH}/build-id-test-copy")
        file(REMOVE_RECURSE "${copy}")
        file(COPY "${BUNDLE}/${half}/" DESTINATION "${copy}")
        id_of("${copy}" recomputed)
        if(NOT shipped STREQUAL recomputed)
            fail("the staged ${half}'s build-id is the content id of its files: '${shipped}' vs '${recomputed}'")
        endif()
        file(REMOVE_RECURSE "${copy}")
    endforeach()
else()
    message("NOT VERIFIED: no staged bundle (configure the app with WALLPAPERX_BACKEND_BUILD_DIR); only the function was checked")
endif()

# GOAL: S16.22 -- staging a backend tree that lacks one bundle half fails, naming that half, and
#   stages nothing.
# WHY-THIS-SHAPE: the staging script run directly on a scratch bundle; going through the app's build
#   would need a second backend tree configured without the plugin.
# CONTROL: 2026-11-05, app/setup/stage_bundle.cmake without its bundle/plasma check: red, "FAIL: staging
#   a bundle without plasma/ fails naming it: exit 1, said 'CMake Error at .../stage_bundle.cmake:17 (file)".
set(stage "${SCRATCH}/stage-test")
file(REMOVE_RECURSE "${stage}")
file(WRITE "${stage}/only-daemon/daemon/bin/x" "x")
file(WRITE "${stage}/only-plasma/plasma/x" "x")
foreach(case "only-daemon;plasma" "only-plasma;daemon")
    list(GET case 0 have)
    list(GET case 1 lacks)
    execute_process(COMMAND ${CMAKE_COMMAND} -DBUNDLE=${stage}/${have} -DDEST=${stage}/dest-${have}
                            -P ${CMAKE_CURRENT_LIST_DIR}/../setup/stage_bundle.cmake
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(rc EQUAL 0 OR NOT err MATCHES "/${have}/${lacks}[ \n]+does not exist")
        fail("staging a bundle without ${lacks}/ fails naming it: exit ${rc}, said '${err}'")
    endif()
    if(EXISTS "${stage}/dest-${have}/daemon" OR EXISTS "${stage}/dest-${have}/plasma")
        fail("staging a bundle without ${lacks}/ staged nothing")
    endif()
endforeach()
file(REMOVE_RECURSE "${stage}")

file(REMOVE_RECURSE "${d}")
if(failures GREATER 0)
    message(FATAL_ERROR "${failures} check(s) failed")
endif()
message("build_id: all checks passed")
