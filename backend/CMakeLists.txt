cmake_minimum_required(VERSION 3.25)
project(wallpaperx-backend VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --------------------------------------------------------------------------- #
# Sanitizers (opt-in, separate build tree)
# --------------------------------------------------------------------------- #

# Set before add_subdirectory so the renderer is instrumented too -- a ThreadSanitizer
# build that misses the renderer would see the daemon's locks but none of the GPU-side
# state they protect, which is the half worth checking.
#
# TSan is the reason this exists. The daemon has two threads and a documented lock
# order, and TSan tests that order better than any hand-written test could: it flags an
# inversion the first time the two paths interleave, rather than on the run where the
# timing happens to be bad. Use a separate build tree:
#
#   cmake -S backend -B backend/build-tsan -DWALLPAPERX_SANITIZE=thread \
#         -DWALLPAPERX_BUILD_KDE_PLUGIN=OFF
#   TSAN_OPTIONS="suppressions=$PWD/backend/tsan.supp" ctest --test-dir backend/build-tsan -L lavapipe
#
# Lavapipe only: the uninstrumented NVIDIA driver reports races TSan cannot see into,
# and the noise buries anything real.
set(WALLPAPERX_SANITIZE "" CACHE STRING "Build with a sanitizer: thread, address, undefined, or empty")
if(WALLPAPERX_SANITIZE)
    add_compile_options(-fsanitize=${WALLPAPERX_SANITIZE} -g -fno-omit-frame-pointer)
    add_link_options(-fsanitize=${WALLPAPERX_SANITIZE})
    message(STATUS "  Sanitizer: ${WALLPAPERX_SANITIZE}")
endif()

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
    daemon/render_worker.cpp
    daemon/wallpaper_manager.cpp
)

target_include_directories(wallpaperx-daemon-lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${HTTPLIB_INCLUDE_DIR}
)

# Threads::Threads for the render worker. Current glibc folds pthread into libc so this
# links either way here, but relying on that makes the build silently platform-specific.
find_package(Threads REQUIRED)

target_link_libraries(wallpaperx-daemon-lib PUBLIC
    wallpaperx-renderer
    SDBusCpp::sdbus-c++
    nlohmann_json::nlohmann_json
    Threads::Threads
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
# Header-only test scaffolding shared with the renderer's probes (support/), for
# reading an exported frame's bytes back through its fd. Not part of the renderer
# library: nothing WallpaperX ships CPU-maps an exported frame, only a test does.
target_include_directories(wallpaperx-dbus-service-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../renderer/tests)
target_compile_definitions(wallpaperx-dbus-service-test PRIVATE
    WALLPAPERX_TEST_IMAGE_DIR="${CMAKE_CURRENT_BINARY_DIR}/test_assets"
)

# The system-level tier: spawns the real wallpaperx-daemon executable and drives it over
# the bus. It links the daemon library only for the DBus name/interface constants and
# the client-side test scaffolding -- deliberately not to call into the daemon's code,
# since the point of this tier is that the daemon under test is a separate program.
add_executable(wallpaperx-daemon-system-test tests/daemon_system_test.cpp)
target_link_libraries(wallpaperx-daemon-system-test PRIVATE wallpaperx-daemon-lib)
target_include_directories(wallpaperx-daemon-system-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../renderer/tests)
target_compile_definitions(wallpaperx-daemon-system-test PRIVATE
    WALLPAPERX_TEST_IMAGE_DIR="${CMAKE_CURRENT_BINARY_DIR}/test_assets"
    WALLPAPERX_DAEMON_BINARY="$<TARGET_FILE:wallpaperx-daemon>"
)
# Baking the path in is not enough -- without this the test can be built and run against
# a stale or missing daemon binary, and would report the daemon's absence as a bus
# timeout thirty seconds later.
add_dependencies(wallpaperx-daemon-system-test wallpaperx-daemon)

# These targets have no Q_OBJECT / Qt usage; AUTOMOC is on globally via
# qt_standard_project_setup(), so turn it off here to skip a pointless moc step.
if(WALLPAPERX_BUILD_KDE_PLUGIN)
    set_target_properties(wallpaperx-daemon-lib wallpaperx-daemon wallpaperx-dbus-service-test
                          wallpaperx-daemon-system-test
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

    # DoD 12 wants the Classic descriptor tier verified end to end *through the
    # daemon*, and no machine here resolves to it on its own: NVIDIA gives Heap and
    # Lavapipe gives Buffer, so without forcing it the whole daemon-side Classic path
    # is only ever exercised by a renderer probe. Same binary, same private bus, one
    # environment variable -- and dbus_service_test asserts the reported tier really is
    # Classic, so this cannot silently degrade into a second run of the default tier.
    add_test(NAME dbus_service_classic
             COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:wallpaperx-dbus-service-test>)
    set_tests_properties(dbus_service_classic PROPERTIES
        ENVIRONMENT "WALLPAPERX_FORCE_TIER=classic")

    # One lock across every bus-driven test, not one per tier. Two reasons, and the
    # second is the reason it is shared rather than merely present: they render from
    # generated PNGs under the same WALLPAPERX_TEST_IMAGE_DIR, so ctest -jN must not run
    # one while another is mid-write -- and daemon_system's responsiveness phase compares
    # intervals measured in tens of microseconds against a render queue's drain time. A
    # dbus_service run saturating a software rasteriser on the same host inflates the
    # first without inflating the second, which fails the check by accusing SetWallpaper
    # of rendering inline. Serialising them costs well under a second in total.
    set_tests_properties(dbus_service PROPERTIES RESOURCE_LOCK wallpaperx_bus_tests)
    set_tests_properties(dbus_service_classic PROPERTIES RESOURCE_LOCK wallpaperx_bus_tests)
    if(WALLPAPERX_LAVAPIPE_ICD)
        set_tests_properties(dbus_service_lavapipe PROPERTIES RESOURCE_LOCK wallpaperx_bus_tests)
    endif()

    # Same private-bus wrapper, one tier up: this one spawns the real daemon binary.
    # Its own dbus-run-session, so the daemon it starts cannot collide with the
    # in-process service above under ctest -jN -- each gets a bus of its own, and both
    # request the same well-known name.
    add_test(NAME daemon_system
             COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:wallpaperx-daemon-system-test>)
    set_tests_properties(daemon_system PROPERTIES RESOURCE_LOCK wallpaperx_bus_tests)

    # The cross-process content check only runs where the export is a LINEAR dma_buf,
    # which on the machines this is developed on means Lavapipe. On NVIDIA (OPAQUE_FD)
    # the twin still covers the process, wire and concurrency halves and says out loud
    # that it skipped the pixels.
    if(WALLPAPERX_LAVAPIPE_ICD)
        add_test(NAME daemon_system_lavapipe
                 COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:wallpaperx-daemon-system-test>)
        set_tests_properties(daemon_system_lavapipe PROPERTIES
            ENVIRONMENT "VK_ICD_FILENAMES=${WALLPAPERX_LAVAPIPE_ICD}"
            LABELS "lavapipe"
            RESOURCE_LOCK wallpaperx_bus_tests)
    endif()
else()
    message(WARNING
        "dbus-run-session not found -- ALL bus-driven tests are unregistered: dbus_service, "
        "dbus_service_classic, dbus_service_lavapipe, daemon_system, daemon_system_lavapipe. That "
        "removes the only coverage of the DBus wire contract, the DMA_BUF metadata, the Classic "
        "descriptor tier through the daemon, and the daemon's "
        "process-level behaviour (name acquisition, second-instance refusal, SIGTERM/SIGINT "
        "shutdown, cross-process frame import). A green ctest run without them means much less "
        "than it looks like. Install dbus.")
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
