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

# Daemon logic lives in a static lib so both the executable and the tests link it
# (mirrors the renderer lib + probes layout).
add_library(wallpaperx-daemon-lib STATIC
    daemon/dbus_service.cpp
)

target_include_directories(wallpaperx-daemon-lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${HTTPLIB_INCLUDE_DIR}
)

target_link_libraries(wallpaperx-daemon-lib PUBLIC
    wallpaperx-renderer
    SDBusCpp::sdbus-c++
    nlohmann_json::nlohmann_json
    ${LIBZIP_LIBRARIES}
)

# The daemon talks DBus through sdbus-c++ (via wallpaperx-daemon-lib), not Qt --
# Qt is reserved for the separate WallpaperX KDE plugin .so below.
add_executable(wallpaperx-daemon daemon/main.cpp)
target_link_libraries(wallpaperx-daemon PRIVATE wallpaperx-daemon-lib)

# --------------------------------------------------------------------------- #
# Tests
# --------------------------------------------------------------------------- #

enable_testing()

add_executable(wallpaperx-dbus-service-test tests/dbus_service_test.cpp)
target_link_libraries(wallpaperx-dbus-service-test PRIVATE wallpaperx-daemon-lib)

# These targets have no Q_OBJECT / Qt usage; AUTOMOC is on globally via
# qt_standard_project_setup(), so turn it off here to skip a pointless moc step.
set_target_properties(wallpaperx-daemon-lib wallpaperx-daemon wallpaperx-dbus-service-test
    PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

# The DBus service test needs a session bus. dbus-run-session gives each run a
# private one (isolated from the developer's real session, and works headless in
# CI). If it isn't installed, skip registering the test rather than fail the build.
find_program(DBUS_RUN_SESSION dbus-run-session)
if(DBUS_RUN_SESSION)
    add_test(NAME dbus_service
             COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:wallpaperx-dbus-service-test>)
else()
    message(STATUS "  dbus-run-session not found -- dbus_service test skipped (install dbus)")
endif()

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
