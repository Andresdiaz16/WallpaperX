cmake_minimum_required(VERSION 3.25)
project(wallpaperx-backend VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --------------------------------------------------------------------------- #
# wallpaperx-renderer (subproject)
# --------------------------------------------------------------------------- #

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../renderer
                 ${CMAKE_CURRENT_BINARY_DIR}/renderer)

# --------------------------------------------------------------------------- #
# Dependencies
# --------------------------------------------------------------------------- #

find_package(PkgConfig REQUIRED)

# Qt6 — required for KDE plugin
find_package(Qt6 REQUIRED COMPONENTS Core DBus Quick Qml)
qt_standard_project_setup()

# ECM + KDE — required for KDE plugin
find_package(ECM REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH} ${CMAKE_MODULE_PATH})
include(KDEInstallDirs6)
include(KDECMakeSettings)

find_package(Plasma REQUIRED)
find_package(KF6 REQUIRED COMPONENTS Package)

# sdbus-c++
find_package(sdbus-c++ REQUIRED CONFIG)

# nlohmann/json
find_package(nlohmann_json REQUIRED)

# libzip
pkg_check_modules(LIBZIP REQUIRED libzip)

# cpp-httplib — vendored header-only
set(HTTPLIB_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
if(NOT EXISTS "${HTTPLIB_INCLUDE_DIR}/httplib.h")
    message(FATAL_ERROR
        "cpp-httplib not found at third_party/httplib.h\n"
        "Download: https://github.com/yhirose/cpp-httplib/releases\n"
        "Copy httplib.h into backend/third_party/"
    )
endif()

# --------------------------------------------------------------------------- #
# wallpaperx-daemon (placeholder — sources added in later steps)
# --------------------------------------------------------------------------- #

add_executable(wallpaperx-daemon
    daemon/main.cpp
)

target_include_directories(wallpaperx-daemon PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${HTTPLIB_INCLUDE_DIR}
)

target_link_libraries(wallpaperx-daemon PRIVATE
    wallpaperx-renderer
    Qt6::Core
    Qt6::DBus
    SDBusCpp::sdbus-c++
    nlohmann_json::nlohmann_json
    ${LIBZIP_LIBRARIES}
)

# --------------------------------------------------------------------------- #
# KDE plugin — libWallpaperX.so (placeholder — sources added in later steps)
# --------------------------------------------------------------------------- #

add_library(WallpaperX SHARED
    kde/plugin.cpp
)

target_include_directories(WallpaperX PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(WallpaperX PRIVATE
    Qt6::Core
    Qt6::Quick
    Qt6::Qml
    Qt6::DBus
)

set_target_properties(WallpaperX PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/kde
)

# --------------------------------------------------------------------------- #
# wpxctl CLI (placeholder — sources added in later steps)
# --------------------------------------------------------------------------- #

add_executable(wpxctl
    wpxctl/main.cpp
)

target_link_libraries(wpxctl PRIVATE
    Qt6::Core
    Qt6::DBus
    SDBusCpp::sdbus-c++
)

# --------------------------------------------------------------------------- #
# KDE package install
# --------------------------------------------------------------------------- #

plasma_install_package(kde/kpackage com.wallpaperx.wallpaper wallpapers wallpaper)

message(STATUS "wallpaperx-backend configured")
message(STATUS "  Qt6:     ${Qt6_VERSION}")
message(STATUS "  ECM:     ${ECM_VERSION}")
message(STATUS "  sdbus:   ${SDBusCpp_VERSION}")
