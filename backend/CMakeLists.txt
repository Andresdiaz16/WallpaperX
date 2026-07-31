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

# The daemon and its tests use no Qt and no KDE Frameworks -- they talk DBus through
# sdbus-c++. Only the Plasma wallpaper plugin and wpxctl do. Making that split explicit
# lets a headless machine (CI above all) configure and test the daemon without pulling
# in Qt6 + ECM + Plasma + KF6, which is what kept the DBus tests -- including the only
# coverage of the DMA_BUF wire fields -- out of CI entirely.
option(WALLPAPERX_BUILD_KDE_PLUGIN "Build the Plasma wallpaper plugin and wpxctl (needs Qt6/ECM/Plasma/KF6)" ON)

if(WALLPAPERX_BUILD_KDE_PLUGIN)
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
endif()

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
    daemon/wallpaper_manager.cpp
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

# Where StaticRenderer looks for compiled SPIR-V when nothing overrides it. Points
# into the build tree, which is right for running from a build and wrong for an
# install -- $WALLPAPERX_SHADER_DIR covers the gap until packaging (Step 14) gives
# the shaders a real installed location.
target_compile_definitions(wallpaperx-daemon-lib PRIVATE
    WALLPAPERX_DEFAULT_SHADER_DIR="${WALLPAPERX_SHADER_OUTPUT_DIR}"
)

# The .spv files are produced by a custom target, so nothing links them in -- without
# this the daemon builds fine and then fails at the first SetWallpaper.
add_dependencies(wallpaperx-daemon-lib wallpaperx-shaders)

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
target_compile_definitions(wallpaperx-dbus-service-test PRIVATE
    WALLPAPERX_TEST_IMAGE_DIR="${CMAKE_CURRENT_BINARY_DIR}/test_assets"
)

# These targets have no Q_OBJECT / Qt usage; AUTOMOC is on globally via
# qt_standard_project_setup(), so turn it off here to skip a pointless moc step.
if(WALLPAPERX_BUILD_KDE_PLUGIN)
    set_target_properties(wallpaperx-daemon-lib wallpaperx-daemon wallpaperx-dbus-service-test
        PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
endif()

# The DBus service test needs a session bus. dbus-run-session gives each run a
# private one (isolated from the developer's real session, and works headless in
# CI). If it isn't installed, skip registering the test rather than fail the build.
find_program(DBUS_RUN_SESSION dbus-run-session)
if(DBUS_RUN_SESSION)
    add_test(NAME dbus_service
             COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:wallpaperx-dbus-service-test>)

    # The test now renders and exports a real frame, so it exercises the export path
    # end to end -- and on the owner's NVIDIA GPU that path is OPAQUE_FD only. The
    # Lavapipe twin is the only place the DMA_BUF branch (and its DRM modifier
    # metadata) crosses the DBus wire, which is the half a KDE plugin on AMD/Intel
    # will actually consume.
    if(WALLPAPERX_LAVAPIPE_ICD)
        add_test(NAME dbus_service_lavapipe
                 COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:wallpaperx-dbus-service-test>)
        set_tests_properties(dbus_service_lavapipe PROPERTIES
            ENVIRONMENT "VK_ICD_FILENAMES=${WALLPAPERX_LAVAPIPE_ICD}"
            LABELS "lavapipe")
    endif()

    # Both variants render from the same generated PNG under WALLPAPERX_TEST_IMAGE_DIR,
    # so keep ctest -jN from running one while the other is mid-write.
    set_tests_properties(dbus_service PROPERTIES RESOURCE_LOCK dbus_service_test_image)
    if(WALLPAPERX_LAVAPIPE_ICD)
        set_tests_properties(dbus_service_lavapipe PROPERTIES RESOURCE_LOCK dbus_service_test_image)
    endif()
else()
    message(STATUS "  dbus-run-session not found -- dbus_service test skipped (install dbus)")
endif()

# --------------------------------------------------------------------------- #
# KDE plugin — libWallpaperX.so, wpxctl, KDE package install
# (placeholders — sources added in later steps)
# --------------------------------------------------------------------------- #

if(WALLPAPERX_BUILD_KDE_PLUGIN)
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

    add_executable(wpxctl
        wpxctl/main.cpp
    )

    target_link_libraries(wpxctl PRIVATE
        Qt6::Core
        Qt6::DBus
        SDBusCpp::sdbus-c++
    )

    plasma_install_package(kde/kpackage com.wallpaperx.wallpaper wallpapers wallpaper)
endif()

message(STATUS "wallpaperx-backend configured")
if(WALLPAPERX_BUILD_KDE_PLUGIN)
    message(STATUS "  Qt6:     ${Qt6_VERSION}")
    message(STATUS "  ECM:     ${ECM_VERSION}")
else()
    message(STATUS "  KDE plugin: disabled (WALLPAPERX_BUILD_KDE_PLUGIN=OFF) -- daemon + tests only")
endif()
message(STATUS "  sdbus:   ${SDBusCpp_VERSION}")
