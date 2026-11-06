# SPDX-License-Identifier: GPL-2.0-or-later
#
# The wallpaper package as shipped: kde/kpackage with the renderer module copied into
# contents/ui/<MODULE_NAME>/, the folder its QML imports by relative path, and build-id. Run at build time by
# wallpaperx-plasma-package.

foreach(var SOURCE_DIR MODULE_DIR PLUGIN_FILE MODULE_NAME PACKAGE_DIR)
    if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
        message(FATAL_ERROR "assemble_package.cmake: ${var} is not set")
    endif()
endforeach()

# A package whose QML names the module some other way would load nothing from this folder.
file(GLOB_RECURSE qml_files "${SOURCE_DIR}/*.qml")
set(importers 0)
foreach(qml IN LISTS qml_files)
    file(STRINGS "${qml}" lines REGEX "^[ \t]*import[ \t]+\"${MODULE_NAME}\"[ \t]*$")
    if(lines)
        math(EXPR importers "${importers} + 1")
    endif()
endforeach()
if(importers EQUAL 0)
    message(FATAL_ERROR "assemble_package.cmake: no file in ${SOURCE_DIR} imports \"${MODULE_NAME}\"")
endif()

file(REMOVE_RECURSE "${PACKAGE_DIR}")
file(COPY "${SOURCE_DIR}/" DESTINATION "${PACKAGE_DIR}")
file(GLOB qmltypes "${MODULE_DIR}/*.qmltypes")
file(COPY "${PLUGIN_FILE}" "${MODULE_DIR}/qmldir" ${qmltypes} DESTINATION "${PACKAGE_DIR}/contents/ui/${MODULE_NAME}")
# The build tree's run path is padding for an install to rewrite, and glibc reads each empty entry in
# it as plasmashell's working directory. The plugin needs only system libraries, so it gets none.
get_filename_component(plugin_name "${PLUGIN_FILE}" NAME)
file(RPATH_REMOVE FILE "${PACKAGE_DIR}/contents/ui/${MODULE_NAME}/${plugin_name}")

# Setup reinstalls the package whenever this differs from the installed package's own.
include("${CMAKE_CURRENT_LIST_DIR}/content_id.cmake")
wallpaperx_write_build_id("${PACKAGE_DIR}")
