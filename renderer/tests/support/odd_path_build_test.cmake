# X.33: the renderer's shaders and video fixtures build from a checkout whose path holds shell syntax.
#
#   cmake -DSOURCE=<renderer source> -DWORK=<scratch dir> -DGENERATOR=<g> -DCXX=<compiler>
#         -DMAKE_PROGRAM=<make or ninja> -P odd_path_build_test.cmake
#
# Copies SOURCE to WORK/q' +~@&'/renderer, configures it with its build tree inside that folder,
# and builds wallpaperx-shaders and wallpaperx-video-fixtures. WORK is emptied first, so every run
# builds from nothing, and removed after a pass; a failed run's tree stays for reading.

# Plain message() before FATAL_ERROR, whose text CMake wraps where a grep for the line cannot follow.
function(harness what)
    message("FAIL (harness): X.33: ${what}")
    message(FATAL_ERROR "renderer_odd_path_build could not run")
endfunction()

foreach(var SOURCE WORK GENERATOR CXX MAKE_PROGRAM)
    if(NOT ${var})
        harness("odd_path_build_test.cmake needs -D${var}")
    endif()
endforeach()

set(odd "${WORK}/q' +~@&'")
# COVERAGE-ASSERT: the folder must hold every character the goal names, so an edit that tames the
#       name cannot leave the entry passing on an ordinary path.
# CONTROL: test-side, by hand, 2026-10-04: the name made `q' +~@'` -> red, "FAIL (harness): X.33:
#       the scratch path .../q' +~@' has no '&'".
foreach(c "'" " " "+" "~" "@" "&")
    string(FIND "${odd}" "${c}" at)
    if(at EQUAL -1)
        harness("the scratch path ${odd} has no '${c}'")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${odd}/renderer")
# Every top-level entry but build trees: the one running this may sit inside SOURCE.
file(GLOB entries LIST_DIRECTORIES true "${SOURCE}/*")
foreach(entry IN LISTS entries)
    get_filename_component(name "${entry}" NAME)
    string(FIND "${WORK}/" "${entry}/" inside)
    if(name MATCHES "^build" OR inside EQUAL 0)
        continue()
    endif()
    file(COPY "${entry}" DESTINATION "${odd}/renderer")
endforeach()
if(NOT EXISTS "${odd}/renderer/CMakeLists.txt" OR NOT EXISTS "${odd}/renderer/shaders/fullscreen.vert")
    harness("the renderer could not be copied to ${odd}/renderer")
endif()

set(build "${odd}/build")
# Prints the end of a step's output and fails the check.
function(fail what log)
    string(LENGTH "${log}" n)
    if(n GREATER 4000)
        math(EXPR from "${n} - 4000")
        string(SUBSTRING "${log}" ${from} -1 log)
    endif()
    message("${log}")
    message("FAIL: X.33: ${what} (tree kept at ${build})")
    message(FATAL_ERROR "renderer_odd_path_build failed")
endfunction()

execute_process(COMMAND "${CMAKE_COMMAND}" -S "${odd}/renderer" -B "${build}" -G "${GENERATOR}"
                        "-DCMAKE_CXX_COMPILER=${CXX}" "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}"
                RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE out)
if(NOT result EQUAL 0)
    fail("the renderer did not configure at ${odd}" "${out}")
endif()

# GOAL: X.33 -- both targets build with the source and build trees under the odd folder.
# WHY-THIS-SHAPE: the source is copied rather than symlinked, so the shader inputs' paths hold the
#       characters too, not only the outputs'. The failure names the outputs make or Ninja reported,
#       so each break below is told apart from a fault elsewhere in the build.
# CONTROL: renderer/CMakeLists.txt, each through control_batch, 2026-10-04: VERBATIM removed from
#       the shader command -> red, "FAIL: X.33: wallpaperx-shaders did not build at ...; failed:
#       shaders/fullscreen_set.frag.spv] Error 1;..."; the joins put back to `${CMAKE_COMMAND} -E cat
#       ${JOIN_PARTS} > ...` without VERBATIM -> red, "wallpaperx-video-fixtures did not build ...;
#       failed: video_assets/counter_resize.h264] Error 1;..."; the `REPLACE "'"` line of the .mkv
#       list removed -> red, "... failed: video_assets/counter_shrink_grow.mkv] Error 254".
cmake_host_system_information(RESULT jobs QUERY NUMBER_OF_LOGICAL_CORES)
foreach(target wallpaperx-shaders wallpaperx-video-fixtures)
    execute_process(COMMAND "${CMAKE_COMMAND}" --build "${build}" --target ${target} -j ${jobs}
                    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE out)
    if(NOT result EQUAL 0)
        # The outputs make (`<file>] Error N`) or Ninja (`FAILED: <file>`) names as failed.
        string(REGEX MATCHALL "(video_assets|shaders)/[^] \n]+\\] Error [0-9]+|FAILED: [^\n]+" failed "${out}")
        list(JOIN failed "; " failed)
        fail("${target} did not build at ${odd}; failed: ${failed}" "${out}")
    endif()
endforeach()

# GOAL: X.33 -- a build that succeeds produced every shader and the .mkv: make does not check
#       that a custom command wrote its output.
# CONTROL: renderer/CMakeLists.txt through control_batch, 2026-10-04: the shader's `-o ${SHADER_SPV}`
#       made `-o ${SHADER_SPV}.x` -> red, "FAIL: X.33: fullscreen.vert.spv is missing or empty after
#       the build"; the .mkv written to `counter_shrink_grow.mkv.x` -> red, "FAIL: X.33:
#       counter_shrink_grow.mkv is missing or empty after the build".
foreach(shader fullscreen.vert fullscreen_set.frag fullscreen_heap.frag ycbcr_convert.vert ycbcr_convert.frag)
    set(size 0)
    if(EXISTS "${build}/shaders/${shader}.spv")
        file(SIZE "${build}/shaders/${shader}.spv" size)
    endif()
    if(NOT size GREATER 0)
        fail("${shader}.spv is missing or empty after the build" "")
    endif()
endforeach()
set(assets "${build}/video_assets")
set(size 0)
if(EXISTS "${assets}/counter_shrink_grow.mkv")
    file(SIZE "${assets}/counter_shrink_grow.mkv" size)
endif()
if(NOT size GREATER 0)
    fail("counter_shrink_grow.mkv is missing or empty after the build" "")
endif()

# GOAL: X.33 -- a joined fixture is its parts byte for byte.
# CONTROL: renderer/tests/support/concat_files.cmake through control_batch, 2026-10-04: `-E cat
#       ${parts}` made `-E cat ${parts} ${parts}` -> red, "FAIL: X.33: counter_jump.ts is not its
#       two parts joined".
execute_process(COMMAND "${CMAKE_COMMAND}" -E cat "${assets}/counter_jump_a.ts" "${assets}/counter_jump_b.ts"
                OUTPUT_FILE "${WORK}/counter_jump.expected" RESULT_VARIABLE result)
execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${assets}/counter_jump.ts" "${WORK}/counter_jump.expected"
                RESULT_VARIABLE differs)
if(NOT result EQUAL 0 OR NOT differs EQUAL 0)
    fail("counter_jump.ts is not its two parts joined" "")
endif()

file(REMOVE_RECURSE "${WORK}")
message("PASS: X.33: the renderer's shaders and video fixtures built under ${odd}")
