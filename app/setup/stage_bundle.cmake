# SPDX-License-Identifier: GPL-2.0-or-later
#
# Copies a backend build tree's bundle/daemon and bundle/plasma beside the app. Both or neither:
# first-run setup installs the two together and refuses a bundle with one missing.
#   cmake -DBUNDLE=<backend build>/bundle -DDEST=<the app's folder> -P stage_bundle.cmake

if(NOT IS_DIRECTORY "${BUNDLE}/daemon")
    message(FATAL_ERROR "${BUNDLE}/daemon does not exist: build that backend tree first, or clear "
                        "WALLPAPERX_BACKEND_BUILD_DIR")
endif()
if(NOT IS_DIRECTORY "${BUNDLE}/plasma")
    message(FATAL_ERROR "${BUNDLE}/plasma does not exist: that backend tree builds no wallpaper package. Configure "
                        "it with WALLPAPERX_BUILD_KDE_PLUGIN=ON and the frame importer, or clear "
                        "WALLPAPERX_BACKEND_BUILD_DIR")
endif()
file(REMOVE_RECURSE "${DEST}/daemon" "${DEST}/plasma")
file(COPY "${BUNDLE}/daemon" "${BUNDLE}/plasma" DESTINATION "${DEST}")
