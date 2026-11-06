# SPDX-License-Identifier: GPL-2.0-or-later
#
# The daemon's half of the bundle the app's first-run setup copies: bin/wallpaperx-daemon, the shaders
# where the daemon looks for them from bin/, and build-id. Run at build time by wallpaperx-daemon-bundle.

foreach(var DAEMON_FILE SHADER_DIR SHADER_DIR_FROM_BINDIR OUT_DIR)
    if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
        message(FATAL_ERROR "assemble_daemon.cmake: ${var} is not set")
    endif()
endforeach()

set(shaders "${OUT_DIR}/bin/${SHADER_DIR_FROM_BINDIR}")
cmake_path(NORMAL_PATH shaders)
cmake_path(IS_PREFIX OUT_DIR "${shaders}" NORMALIZE inside)
if(NOT inside)
    message(FATAL_ERROR "assemble_daemon.cmake: the daemon looks for its shaders at ${SHADER_DIR_FROM_BINDIR} "
                        "from its bin directory, which is outside the bundle; configure with "
                        "CMAKE_INSTALL_BINDIR and CMAKE_INSTALL_DATADIR both relative")
endif()

file(REMOVE_RECURSE "${OUT_DIR}")
file(COPY "${DAEMON_FILE}" DESTINATION "${OUT_DIR}/bin")
get_filename_component(daemon_name "${DAEMON_FILE}" NAME)
# The build tree's run path is padding for the install to rewrite, and glibc reads each empty entry
# in it as the working directory. lib/ is where a release will put the libraries it bundles; none
# are bundled yet, so the daemon uses the system's and no lib/ is made.
file(RPATH_SET FILE "${OUT_DIR}/bin/${daemon_name}" NEW_RPATH "$ORIGIN/../lib")
file(GLOB spv "${SHADER_DIR}/*.spv")
if(NOT spv)
    message(FATAL_ERROR "assemble_daemon.cmake: no compiled shaders in ${SHADER_DIR}")
endif()
file(COPY ${spv} DESTINATION "${shaders}")

# A source build of the app names the daemon's copy by this id.
include("${CMAKE_CURRENT_LIST_DIR}/content_id.cmake")
wallpaperx_write_build_id("${OUT_DIR}")
