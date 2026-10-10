cmake_minimum_required(VERSION 3.25)
project(wallpaperx-backend VERSION 0.1.0 LANGUAGES CXX)

# Optimised with debug info unless the caller chose; the sanitizer tree below passes its own.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

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
#   ctest --test-dir backend/build-tsan
#
# No TSAN_OPTIONS on that command line, deliberately. The suppressions file and the
# validation waiver are attached to each test as an ENVIRONMENT property further down,
# and a per-test ENVIRONMENT overrides the same variable exported by the caller -- so a
# hand-set TSAN_OPTIONS here would be silently discarded while appearing to take effect.
# Tests that would load the proprietary ICD are disabled rather than left to fail: under
# TSan that driver SEGVs inside its own uninstrumented code, which is red for a reason
# unrelated to this project. Watch the configure output for the "TSan tier:" line -- if
# it warns that every test was disabled, a green ctest run here means nothing.
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

# Every ctest entry here is registered through wallpaperx_add_test() or
# wallpaperx_add_display_test(), never add_test(): they run it with a private HOME and XDG
# directories, a network of its own, no live display and a private bus that can start no service. Run from a desktop
# session, a bare entry inherits that session -- and a bus that can activate services starts real
# desktop daemons as the user. test_isolation and test_isolation_registry check both halves.
include(${CMAKE_CURRENT_SOURCE_DIR}/../renderer/tests/support/isolated_test.cmake)

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
    # Only enable_testing(), the BUILD_TESTING option and ECM's `appstreamtest` entry, a bare
    # add_test() outside the isolation wrapper. This project enables testing itself, and that entry
    # checks the installed files' metadata, which a ctest run never installs.
    set(KDE_SKIP_TEST_SETTINGS TRUE)
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

# daemon/log_escape.h and daemon/ring_depth.h, header-only. A target rather than a bare
# include path because the plugin needs both: it logs text that arrived over the bus exactly
# as the daemon does, and it sizes its import cache against the daemon's ring depth.
add_library(wallpaperx-log-escape INTERFACE)
target_include_directories(wallpaperx-log-escape INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# --------------------------------------------------------------------------- #
# wallpaperx-daemon (placeholder — sources added in later steps)
# --------------------------------------------------------------------------- #

# Daemon logic lives in a static lib so both the executable and the tests link it
# (mirrors the renderer lib + probes layout).
add_library(wallpaperx-daemon-lib STATIC
    daemon/config_preserve.cpp
    daemon/config_store.cpp
    daemon/config_writer.cpp
    daemon/crash_record.cpp
    daemon/dbus_service.cpp
    daemon/kept_rows.cpp
    daemon/http_server.cpp
    daemon/log_pump.cpp
    daemon/render_worker.cpp
    daemon/wallpaper_manager.cpp
    daemon/wallpaper_properties.cpp
)

target_include_directories(wallpaperx-daemon-lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${HTTPLIB_INCLUDE_DIR}
)

# Threads::Threads for the render worker. Current glibc folds pthread into libc so this
# links either way here, but relying on that makes the build silently platform-specific.
find_package(Threads REQUIRED)

# --------------------------------------------------------------------------- #
# Frame import (Step 12)
# --------------------------------------------------------------------------- #
#
# The half of frame sharing that runs in the compositor: an fd the daemon exported becomes a GL
# texture. Its own library, with no Qt in it, so a probe can drive it against a real daemon with
# no scene graph, no window and no Plasma -- the import is where the driver-specific failures
# live, and mixing it into the QQuickItem would make those reachable only through plasmashell.
pkg_check_modules(EGL QUIET IMPORTED_TARGET egl)
find_package(OpenGL QUIET COMPONENTS OpenGL)
if(EGL_FOUND AND OpenGL_OpenGL_FOUND)
    # buffer_claims.cpp rides along for the same reason: which of the daemon's images this
    # process still has a live import of is decided with no Qt and no GPU, so it can be graded
    # without either.
    add_library(wallpaperx-frame-import STATIC kde/frame_import.cpp kde/buffer_claims.cpp)
    # Same reason as wallpaperxplugin: this ends up inside the QML plugin's shared object.
    set_target_properties(wallpaperx-frame-import PROPERTIES POSITION_INDEPENDENT_CODE ON)
    # The renderer's directory is on the path for vulkan/drm_formats.h alone -- fourcc constants
    # with no Vulkan in them, shared so the exporter and this importer cannot drift apart.
    target_include_directories(wallpaperx-frame-import PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/kde
                               ${CMAKE_CURRENT_SOURCE_DIR}/../renderer)
    # The imported target carries EGL's include dirs, library dir and -lEGL together; PUBLIC
    # hands all three to the probe and the plugin, which include <EGL/egl.h> themselves.
    target_link_libraries(wallpaperx-frame-import PUBLIC PkgConfig::EGL OpenGL::GL)
    set(WALLPAPERX_HAVE_FRAME_IMPORT ON)

    # The probe lives here rather than beside the renderer's, because it is the importer that is
    # under test and the importer is this repo's. It borrows the renderer's shader and asset
    # directories through the cache variables that module exports.
    add_executable(wallpaperx-frame-import-probe tests/frame_import_probe.cpp)
    target_link_libraries(wallpaperx-frame-import-probe PRIVATE
        wallpaperx-frame-import wallpaperx-renderer)
    add_dependencies(wallpaperx-frame-import-probe wallpaperx-shaders)
    target_include_directories(wallpaperx-frame-import-probe PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../renderer/tests)
    # Its own asset directory, not the renderer's: that one is a plain variable in the renderer's
    # scope and arrives here empty, which showed up as a write to "/frame_import_gradient.png".
    # A separate directory is also what keeps this probe off the path its lavapipe twin writes.
    set(WALLPAPERX_IMPORT_ASSET_DIR ${CMAKE_CURRENT_BINARY_DIR}/import_test_assets)
    file(MAKE_DIRECTORY ${WALLPAPERX_IMPORT_ASSET_DIR})
    target_compile_definitions(wallpaperx-frame-import-probe PRIVATE
        WALLPAPERX_SHADER_DIR="${WALLPAPERX_SHADER_OUTPUT_DIR}"
        WALLPAPERX_TEST_IMAGE_DIR="${WALLPAPERX_IMPORT_ASSET_DIR}")

    # Registered even though it currently skips on this host: an unregistered probe is one
    # nobody runs, and the pairing it needs (a GL context on the same driver the Vulkan device
    # used) is a property of the machine that can change without this file changing. It reports
    # 77 with the reason when it cannot import, which ctest reads as a skip; the probe itself
    # needs no Qt.
    # One entry per import path, each PINNING the one it covers rather than letting the device
    # decide -- a host that resolves to one vendor would otherwise leave the other path covered by
    # nothing. Both are registered unconditionally so the gap is a named skip rather than an
    # absent entry; the lavapipe twin below only adds a second device for the dma_buf path.
    # no-tsan on both: the label says "needs no lavapipe ICD", not "opens no driver" -- these two
    # take whichever GL and Vulkan drivers the host resolves to, which here is the proprietary
    # one. Under TSan its OpenGL library takes a SEGV inside libtsan's own calloc interceptor on
    # a thread that library started, and the runtime aborts with "nested bug in the same thread"
    # before any check runs. A crash inside the sanitizer is not a report, so no suppression can
    # reach it. What that costs: under TSan the lavapipe twin below is the only import path left,
    # so the opaque one is graded in the unsanitized trees only.
    wallpaperx_add_test(NAME frame_import_probe COMMAND wallpaperx-frame-import-probe)
    set_tests_properties(frame_import_probe PROPERTIES
        ENVIRONMENT "WALLPAPERX_FORCE_HANDLE_TYPE=opaque"
        LABELS "novulkan;no-tsan"
        SKIP_RETURN_CODE 77)

    wallpaperx_add_test(NAME frame_import_probe_dmabuf COMMAND wallpaperx-frame-import-probe)
    set_tests_properties(frame_import_probe_dmabuf PROPERTIES
        ENVIRONMENT "WALLPAPERX_FORCE_HANDLE_TYPE=dmabuf"
        LABELS "novulkan;no-tsan"
        SKIP_RETURN_CODE 77)

    # Descriptor growth across hundreds of imports, successful and refused. It opens the host's Vulkan
    # driver, so no `novulkan`; unlabelled, the TSan block disables it, which the proprietary GL
    # library's crash under TSan (above) requires anyway. The lavapipe twin below covers the dma_buf
    # branch in CI.
    add_executable(wallpaperx-frame-import-fd-test tests/frame_import_fd_test.cpp)
    target_link_libraries(wallpaperx-frame-import-fd-test PRIVATE wallpaperx-frame-import wallpaperx-renderer)
    add_dependencies(wallpaperx-frame-import-fd-test wallpaperx-shaders)
    target_include_directories(wallpaperx-frame-import-fd-test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../renderer/tests)
    target_compile_definitions(wallpaperx-frame-import-fd-test PRIVATE
        WALLPAPERX_SHADER_DIR="${WALLPAPERX_SHADER_OUTPUT_DIR}"
        WALLPAPERX_TEST_IMAGE_DIR="${WALLPAPERX_IMPORT_ASSET_DIR}")
    wallpaperx_add_test(NAME frame_import_fds COMMAND wallpaperx-frame-import-fd-test)
    set_tests_properties(frame_import_fds PROPERTIES
        ENVIRONMENT "WALLPAPERX_FORCE_HANDLE_TYPE=opaque"
        SKIP_RETURN_CODE 77)

    # The generation rules are pure functions, so this needs no GPU and no daemon -- but it does
    # need the item, hence the KDE plugin guard.
    if(WALLPAPERX_BUILD_KDE_PLUGIN)
        add_executable(wallpaperx-frame-generation-test tests/frame_generation_test.cpp)
        target_link_libraries(wallpaperx-frame-generation-test PRIVATE
            wallpaperxplugin Qt6::Core Qt6::Quick Qt6::Qml)
        target_include_directories(wallpaperx-frame-generation-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        wallpaperx_add_test(NAME frame_generation COMMAND wallpaperx-frame-generation-test)
        set_tests_properties(frame_generation PROPERTIES LABELS "novulkan" SKIP_RETURN_CODE 77)
    endif()

    if(WALLPAPERX_LAVAPIPE_ICD)
        wallpaperx_add_test(NAME frame_import_probe_lavapipe COMMAND wallpaperx-frame-import-probe)
        set_tests_properties(frame_import_probe_lavapipe PROPERTIES
            ENVIRONMENT "VK_ICD_FILENAMES=${WALLPAPERX_LAVAPIPE_ICD};WALLPAPERX_FORCE_HANDLE_TYPE=dmabuf"
            LABELS "lavapipe"
            SKIP_RETURN_CODE 77)
        wallpaperx_add_test(NAME frame_import_fds_lavapipe COMMAND wallpaperx-frame-import-fd-test)
        set_tests_properties(frame_import_fds_lavapipe PROPERTIES
            ENVIRONMENT "VK_ICD_FILENAMES=${WALLPAPERX_LAVAPIPE_ICD};WALLPAPERX_FORCE_HANDLE_TYPE=dmabuf"
            LABELS "lavapipe"
            SKIP_RETURN_CODE 77)
    endif()
else()
    # Named rather than silently skipped: without this the plugin cannot display anything, so a
    # build missing it is not a smaller build, it is one that cannot do the step's whole job.
    message(WARNING
        "EGL or OpenGL not found -- the frame importer is not built, and with it the only path "
        "from a rendered frame to the desktop. Install the EGL and OpenGL development packages.")
    set(WALLPAPERX_HAVE_FRAME_IMPORT OFF)
endif()

target_link_libraries(wallpaperx-daemon-lib PUBLIC
    wallpaperx-renderer
    wallpaperx-log-escape
    SDBusCpp::sdbus-c++
    nlohmann_json::nlohmann_json
    Threads::Threads
    ${LIBZIP_LIBRARIES}
)

include(GNUInstallDirs)
set(WALLPAPERX_INSTALL_SHADER_DIR "${CMAKE_INSTALL_DATADIR}/wallpaperx/shaders")
# Relative to the executable, so the daemon finds its shaders under whatever prefix it was
# installed to. The install below refuses a prefix under which this path no longer holds.
cmake_path(ABSOLUTE_PATH CMAKE_INSTALL_BINDIR BASE_DIRECTORY "${CMAKE_INSTALL_PREFIX}" NORMALIZE
           OUTPUT_VARIABLE _wx_configured_bindir)
cmake_path(ABSOLUTE_PATH WALLPAPERX_INSTALL_SHADER_DIR BASE_DIRECTORY "${CMAKE_INSTALL_PREFIX}" NORMALIZE
           OUTPUT_VARIABLE _wx_configured_shader_dir)
file(RELATIVE_PATH WALLPAPERX_SHADER_DIR_FROM_BINDIR "${_wx_configured_bindir}" "${_wx_configured_shader_dir}")

# Where StaticRenderer looks for compiled SPIR-V when nothing overrides it: the build tree's
# shaders for an executable that is in the build tree, and the installed ones, found relative to
# the executable, for any other.
target_compile_definitions(wallpaperx-daemon-lib PRIVATE
    WALLPAPERX_DEFAULT_SHADER_DIR="${WALLPAPERX_SHADER_OUTPUT_DIR}"
    WALLPAPERX_BUILD_RUNTIME_DIR="$<TARGET_FILE_DIR:wallpaperx-daemon>"
    WALLPAPERX_INSTALLED_SHADER_DIR_FROM_BINDIR="${WALLPAPERX_SHADER_DIR_FROM_BINDIR}"
    # /health reports this. Taken from project(... VERSION) rather than written into the
    # source, so the string a user quotes in a bug report cannot be a version the build
    # stopped being. http_server.cpp #errors if it is missing rather than defaulting.
    WALLPAPERX_VERSION="${PROJECT_VERSION}"
)

# The .spv files are produced by a custom target, so nothing links them in -- without
# this the daemon builds fine and then fails at the first SetWallpaper.
add_dependencies(wallpaperx-daemon-lib wallpaperx-shaders)

# The daemon talks DBus through sdbus-c++ (via wallpaperx-daemon-lib), not Qt --
# Qt is reserved for the separate WallpaperX KDE plugin .so below.
add_executable(wallpaperx-daemon daemon/main.cpp)
target_link_libraries(wallpaperx-daemon PRIVATE wallpaperx-daemon-lib)

# `cmake --install <build> --prefix ~/.local --component daemon` yields a runnable daemon.

# share/, not lib/: systemd --user searches $XDG_DATA_HOME/systemd/user, so a ~/.local prefix
# works as /usr does, and ~/.config/systemd/user stays the user's own place to override it.
set(WALLPAPERX_SYSTEMD_USER_UNIT_DIR "${CMAKE_INSTALL_DATADIR}/systemd/user"
    CACHE STRING "Where the systemd user unit is installed, relative to the prefix or absolute")
# Before anything is copied, so a refused install leaves nothing behind. The unit is generated
# here, not at configure time, because ExecStart names the binary under the prefix the install
# actually uses; and written straight to its destination, since the build tree may not be the
# installing user's to write.
install(CODE "
    set(_wx_bindir \"${CMAKE_INSTALL_BINDIR}\")
    set(_wx_shader_dir \"${WALLPAPERX_INSTALL_SHADER_DIR}\")
    set(_wx_shader_dir_from_bindir \"${WALLPAPERX_SHADER_DIR_FROM_BINDIR}\")
    set(_wx_unit_dir \"${WALLPAPERX_SYSTEMD_USER_UNIT_DIR}\")
    set(_wx_unit_template \"${CMAKE_CURRENT_SOURCE_DIR}/packaging/wallpaperx-daemon.service.in\")
" COMPONENT daemon)
install(CODE [[
    # A relative prefix is taken from the directory the install runs in, as CMake's own rules
    # below take it; the unit needs the absolute path.
    cmake_path(ABSOLUTE_PATH CMAKE_INSTALL_PREFIX NORMALIZE OUTPUT_VARIABLE _wx_prefix)
    cmake_path(ABSOLUTE_PATH _wx_bindir BASE_DIRECTORY "${_wx_prefix}" NORMALIZE
               OUTPUT_VARIABLE _wx_bin)
    cmake_path(ABSOLUTE_PATH _wx_shader_dir BASE_DIRECTORY "${_wx_prefix}" NORMALIZE
               OUTPUT_VARIABLE _wx_shaders)
    # The daemon applies its compiled-in path to /proc/self/exe, which the kernel has resolved
    # through every symbolic link, so the comparison is between resolved paths. Under DESTDIR it is
    # between the paths as written: links in a staging tree say nothing about the target system.
    # A directory the install has yet to create resolves as the part of it that exists, plus the rest.
    function(_wx_resolve path out)
        if(NOT "$ENV{DESTDIR}" STREQUAL "")
            set(${out} "${path}" PARENT_SCOPE)
            return()
        endif()
        set(_head "${path}")
        set(_tail "")
        while(NOT EXISTS "${_head}")
            cmake_path(GET _head FILENAME _name)
            cmake_path(GET _head PARENT_PATH _head)
            if(_tail STREQUAL "")
                set(_tail "${_name}")
            else()
                set(_tail "${_name}/${_tail}")
            endif()
        endwhile()
        file(REAL_PATH "${_head}" _real)
        if(NOT _tail STREQUAL "")
            cmake_path(APPEND _real "${_tail}")
            cmake_path(NORMAL_PATH _real)
        endif()
        set(${out} "${_real}" PARENT_SCOPE)
    endfunction()
    _wx_resolve("${_wx_bin}" _wx_found)
    cmake_path(APPEND _wx_found "${_wx_shader_dir_from_bindir}")
    cmake_path(NORMAL_PATH _wx_found)
    _wx_resolve("${_wx_found}" _wx_found)
    _wx_resolve("${_wx_shaders}" _wx_real_shaders)
    if(NOT _wx_found STREQUAL _wx_real_shaders)
        message(FATAL_ERROR "cannot install to ${_wx_prefix}: the daemon installed to ${_wx_bin} "
                            "would look for its shaders in ${_wx_found}, but they are installed to "
                            "${_wx_shaders}. Install with the configured prefix, configure with "
                            "CMAKE_INSTALL_BINDIR and CMAKE_INSTALL_DATADIR both relative, and do "
                            "not make the bin directory a symbolic link to another place.")
    endif()

    set(_wx_exe "${_wx_bin}/wallpaperx-daemon")
    # systemd refuses these in an executable path however they are escaped ("Executable path
    # contains special characters"), so the unit could never load. * ? [ are its glob characters.
    set(_wx_refused 34 39 42 63 91 92 127)
    foreach(_wx_code RANGE 1 31)
        list(APPEND _wx_refused ${_wx_code})
    endforeach()
    foreach(_wx_code IN LISTS _wx_refused)
        string(ASCII ${_wx_code} _wx_char)
        string(FIND "${_wx_exe}" "${_wx_char}" _wx_at)
        if(NOT _wx_at EQUAL -1)
            message(FATAL_ERROR "systemd cannot run an executable whose path contains a quote, a "
                                "backslash, a control character or one of * ? [: ${_wx_exe}")
        endif()
    endforeach()
    # systemd ignores a unit line that is not valid UTF-8, and counts noncharacters as invalid.
    # Checked on the bytes spelled out in hex, one "xx " per byte, which CMake's regex can match.
    string(HEX "${_wx_exe}" _wx_hex)
    string(REGEX REPLACE "(..)" "\\1 " _wx_hex "${_wx_hex}")
    set(_wx_cont "[89ab][0-9a-f] ")
    string(CONCAT _wx_utf8 "^([0-7][0-9a-f] |c[2-9a-f] ${_wx_cont}|d[0-9a-f] ${_wx_cont}"
           "|e0 [ab][0-9a-f] ${_wx_cont}|e[1-9a-cef] ${_wx_cont}${_wx_cont}|ed [89][0-9a-f] ${_wx_cont}"
           "|f0 [9ab][0-9a-f] ${_wx_cont}${_wx_cont}|f[1-3] ${_wx_cont}${_wx_cont}${_wx_cont}"
           "|f4 8[0-9a-f] ${_wx_cont}${_wx_cont})*$")
    set(_wx_nonchar " (ef b7 [9a][0-9a-f]|ef bf b[ef]|f[0-4] [89ab]f bf b[ef]) ")
    if(NOT _wx_hex MATCHES "${_wx_utf8}" OR " ${_wx_hex}" MATCHES "${_wx_nonchar}")
        message(FATAL_ERROR "systemd cannot load a unit whose executable path is not valid UTF-8: ${_wx_exe}")
    endif()
    # Anything else is quoted, with % doubled (specifiers are expanded inside quotes too) and
    # the ":" prefix, which stops $ in the path being taken for a variable in argv[0].
    if(_wx_exe MATCHES "^[A-Za-z0-9/._+,:=@~-]+$")
        set(WALLPAPERX_DAEMON_EXEC_START "${_wx_exe}")
    else()
        string(REPLACE "%" "%%" _wx_exe "${_wx_exe}")
        set(WALLPAPERX_DAEMON_EXEC_START ":\"${_wx_exe}\"")
    endif()
    file(READ "${_wx_unit_template}" _wx_unit_text)
    string(CONFIGURE "${_wx_unit_text}" _wx_unit_text @ONLY)

    # What install(FILES) would do: written under DESTDIR, recorded in the manifest without it,
    # 0644 whatever the umask.
    cmake_path(ABSOLUTE_PATH _wx_unit_dir BASE_DIRECTORY "${_wx_prefix}" NORMALIZE)
    set(_wx_unit "${_wx_unit_dir}/wallpaperx-daemon.service")
    set(_wx_staged_unit "$ENV{DESTDIR}${_wx_unit}")
    message(STATUS "Installing: ${_wx_staged_unit}")
    file(WRITE "${_wx_staged_unit}" "${_wx_unit_text}")
    file(CHMOD "${_wx_staged_unit}" PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
    list(APPEND CMAKE_INSTALL_MANIFEST_FILES "${_wx_unit}")
]] COMPONENT daemon)
install(TARGETS wallpaperx-daemon RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT daemon)
install(DIRECTORY ${WALLPAPERX_SHADER_OUTPUT_DIR}/ DESTINATION ${WALLPAPERX_INSTALL_SHADER_DIR}
        COMPONENT daemon FILES_MATCHING PATTERN "*.spv")

# --------------------------------------------------------------------------- #
# Tests
# --------------------------------------------------------------------------- #

enable_testing()

add_executable(wallpaperx-dbus-service-test tests/dbus_service_test.cpp)
target_link_libraries(wallpaperx-dbus-service-test PRIVATE wallpaperx-daemon-lib)
# The test defines sd_bus_send to fail one reply on demand; exported, or libsdbus-c++ binds
# libsystemd's.
target_link_options(wallpaperx-dbus-service-test PRIVATE "LINKER:--export-dynamic-symbol=sd_bus_send")
# Header-only test scaffolding shared with the renderer's probes (support/), for
# reading an exported frame's bytes back through its fd. Not part of the renderer
# library: nothing WallpaperX ships CPU-maps an exported frame, only a test does.
target_include_directories(wallpaperx-dbus-service-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../renderer/tests)
target_compile_definitions(wallpaperx-dbus-service-test PRIVATE
    WALLPAPERX_TEST_IMAGE_DIR="${CMAKE_CURRENT_BINARY_DIR}/test_assets"
)
# The GL importer, to read an opaque-fd frame's pixels on the GPU that exported it
# (tests/served_pixels.h); without it such a frame's contents are reported NOT VERIFIED.
if(WALLPAPERX_HAVE_FRAME_IMPORT)
    target_link_libraries(wallpaperx-dbus-service-test PRIVATE wallpaperx-frame-import)
    target_compile_definitions(wallpaperx-dbus-service-test PRIVATE WALLPAPERX_HAVE_FRAME_IMPORT=1)
endif()

# Step 7's HTTP config server. In-process, like dbus_service_test, and for the same
# reason: half of what it must establish is that /config and GetStatus agree, which needs
# both channels live at once. It binds only ephemeral ports -- never the shipped 18462 --
# so a developer's running daemon and a parallel ctest entry cannot turn a port conflict
# into a failure that looks like a defect.
add_executable(wallpaperx-http-server-test tests/http_server_test.cpp)
target_link_libraries(wallpaperx-http-server-test PRIVATE wallpaperx-daemon-lib)
target_include_directories(wallpaperx-http-server-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../renderer/tests)
target_compile_definitions(wallpaperx-http-server-test PRIVATE
    WALLPAPERX_TEST_IMAGE_DIR="${CMAKE_CURRENT_BINARY_DIR}/test_assets"
    # /health reports a version, and it must be the one this build actually is. Taken
    # from the same project(... VERSION) the daemon library compiles in, so a check that
    # http_server.cpp stopped deriving it from the build -- a hardcoded literal, say --
    # fails here.
    WALLPAPERX_EXPECTED_VERSION="${PROJECT_VERSION}"
)

# S7.9's only lever is a failed allocation inside a handler, and it gets there by replacing
# ::operator new/delete in the test binary. A sanitized runtime owns those two functions:
# its versions carry the shadow-memory bookkeeping the tool reasons from, and a replacement
# that forwards to malloc/free silently removes it. Measured under -fsanitize=thread: 90
# "data race in operator delete" reports against threads that were correctly synchronised,
# which is a gate that is red when the code is right. So the instrument is compiled out
# here and the S7.9 phase reports itself as unexercised, which SKIP_RETURN_CODE turns into
# a ctest Skip -- the same trade renderer/CMakeLists.txt makes for image_bounds_probe.
# Any non-empty value counts: an allowlist of sanitizer names is the maintenance burden
# that produced a bug there, and over-skipping announces itself in the test's output.
if(WALLPAPERX_SANITIZE)
    target_compile_definitions(wallpaperx-http-server-test PRIVATE WALLPAPERX_ALLOCATION_INSTRUMENT=0)
endif()

# S7.19, on its own because sanitizeForLog is a pure function of bytes: no bus, no GPU, no
# port. Folding it into either tier above would put an exhaustive byte sweep behind a daemon
# startup and make a pure-function regression unreachable on a host without Vulkan. No
# SKIP_RETURN_CODE and no RESOURCE_LOCK -- nothing here can be inconclusive, and it competes
# with nothing.
add_executable(wallpaperx-log-escape-test tests/log_escape_test.cpp)
target_link_libraries(wallpaperx-log-escape-test PRIVATE wallpaperx-daemon-lib)

# Step 8's persistence layer, and on its own for the reason above: ConfigStore is a file
# and a parser, and WallpaperManager's save path is GPU-free, so none of it needs a bus, a
# port or a device. It does carry SKIP_RETURN_CODE -- a process running as root cannot make
# a write fail, and that phase must report itself unexercised rather than pass.
add_executable(wallpaperx-config-store-test tests/config_store_test.cpp)
target_link_libraries(wallpaperx-config-store-test PRIVATE wallpaperx-daemon-lib)

# Each caller's share of the render worker's queue, on the manager's record alone.
add_executable(wallpaperx-request-share-test tests/request_share_test.cpp)
target_link_libraries(wallpaperx-request-share-test PRIVATE wallpaperx-daemon-lib)

# The same trade the http-server test makes above, for the same measured reason. S10.16's
# allocation half has one lever -- std::thread's constructor allocates before it asks the kernel
# for anything -- and reaching it means replacing ::operator new/delete in this binary. A
# sanitized runtime owns those two, so under a sanitizer the instrument is compiled out, the
# phase reports itself unexercised, and SKIP_RETURN_CODE turns that into a ctest Skip.
if(WALLPAPERX_SANITIZE)
    target_compile_definitions(wallpaperx-config-store-test PRIVATE WALLPAPERX_ALLOCATION_INSTRUMENT=0)
endif()

# The crash-loop breaker's on-disk half: the record the render thread holds while a wallpaper
# is being applied, and what the fatal-signal handler does to it. Its own binary because every
# signal case is a forked child that dies on purpose, and folding those into a tier that also
# holds a Vulkan device or a bus connection means forking a process that owns both.
#
# It needs renderer/tests on the include path for support/forked_child.h, which is where the
# PR_SET_PDEATHSIG preamble and the core-dump suppression live -- shared with the renderer's
# own forking probe rather than copied.
# log_writer_fault.cpp replaces libc's memrchr in this binary, to fault on the log writer's thread.
add_executable(wallpaperx-crash-record-test tests/crash_record_test.cpp tests/log_writer_fault.cpp)
target_link_libraries(wallpaperx-crash-record-test PRIVATE wallpaperx-daemon-lib)
target_include_directories(wallpaperx-crash-record-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../renderer/tests)
# Nothing here is a QObject, and with WALLPAPERX_BUILD_KDE_PLUGIN=ON KDECMakeSettings turns
# AUTOMOC on for every target in this directory.
set_target_properties(wallpaperx-crash-record-test PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

# Under a sanitizer the runtime installs its own SIGSEGV/SIGABRT handlers and chooses the
# process's exit status, so "the child still died of that signal" grades the sanitizer rather
# than the daemon. The checks that read the death signal report themselves unexercised there
# instead of accusing the handler; the record checks, which run before the chain reaches the
# sanitizer's handler, are unaffected and still run.
if(WALLPAPERX_SANITIZE)
    target_compile_definitions(wallpaperx-crash-record-test PRIVATE WALLPAPERX_SIGNAL_EXIT_IS_THE_SIGNAL=false)
else()
    # A stand-in for a sanitizer's death-callback registration, which a sanitized build has for real.
    target_sources(wallpaperx-crash-record-test PRIVATE tests/sanitizer_death_fake.cpp)
    target_compile_definitions(wallpaperx-crash-record-test PRIVATE WALLPAPERX_FAKE_SANITIZER_DEATH=1)
endif()

# The log writer against a destination that refuses a write or has lost its reader, in forked
# children. log_pump_write_fault.cpp replaces libc's write(2) in this binary to stage the refusal.
add_executable(wallpaperx-log-pump-test tests/log_pump_test.cpp tests/log_pump_write_fault.cpp)
target_link_libraries(wallpaperx-log-pump-test PRIVATE wallpaperx-daemon-lib)
target_include_directories(wallpaperx-log-pump-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../renderer/tests)
set_target_properties(wallpaperx-log-pump-test PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

# The refusal half of the same feature: what a start does with a record it finds. Separate
# from the binary above because this one drives a WallpaperManager and renders on the GPU,
# while that one forks children that die on purpose -- and forking a process that owns a
# Vulkan device is not a thing to do for convenience.
add_executable(wallpaperx-crash-quarantine-test tests/crash_quarantine_test.cpp)
target_link_libraries(wallpaperx-crash-quarantine-test PRIVATE wallpaperx-daemon-lib)
# renderer/tests for support/test_image.h, the shared PNG fixture writer.
target_include_directories(wallpaperx-crash-quarantine-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../renderer/tests)
set_target_properties(wallpaperx-crash-quarantine-test PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

# RenderWorker's stop for a lost device, in process: interposes vkQueueSubmit to lose the device
# and count what is submitted after the worker reports it.
add_executable(wallpaperx-device-loss-worker-test tests/device_loss_worker_test.cpp)
target_link_libraries(wallpaperx-device-loss-worker-test PRIVATE wallpaperx-daemon-lib)
target_include_directories(wallpaperx-device-loss-worker-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../renderer/tests)
set_target_properties(wallpaperx-device-loss-worker-test PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

# Step 9's Plasma wallpaper package (kde/kpackage), checked as text: that its metadata
# parses, declares the structure Plasma looks for, and agrees with the install rule below
# and with the plugin id the app's activation script will ask for.
#
# Deliberately outside the if(WALLPAPERX_BUILD_KDE_PLUGIN) guard, unlike everything else
# that concerns the KDE plugin. Nothing here needs Qt, ECM, Plasma or kpackagetool6 -- it
# reads files -- and the sanitizer tree is configured with that option OFF, so putting it
# inside would leave the one tier most likely to be run after an edit blind to a package
# that had stopped installing under the id metadata.json declares.
#
# It links nlohmann_json alone rather than wallpaperx-daemon-lib: the daemon library drags
# in the renderer and the compiled shaders, none of which a text check needs, and the
# lighter target keeps this runnable wherever the source tree is.
add_executable(wallpaperx-kpackage-metadata-test tests/kpackage_metadata_test.cpp)
target_link_libraries(wallpaperx-kpackage-metadata-test PRIVATE nlohmann_json::nlohmann_json)
# Stated here rather than inherited. The targets that link wallpaperx-daemon-lib get this
# directory from its PUBLIC include list; this one deliberately does not link it, and with
# WALLPAPERX_BUILD_KDE_PLUGIN=ON it still compiled because KDECMakeSettings turns on
# CMAKE_INCLUDE_CURRENT_DIR. With the option OFF -- the hosted runner and the sanitizer tree,
# the two configurations this target exists to survive -- KDE's settings are never included
# and `#include "tests/test_report.h"` does not resolve, which fails the whole backend build.
target_include_directories(wallpaperx-kpackage-metadata-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# The files under test are source, not build output, and ctest runs in the build tree. This
# is the only way the test can find them; without it there is nothing to look at.
target_compile_definitions(wallpaperx-kpackage-metadata-test PRIVATE
    WALLPAPERX_BACKEND_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    # The install rules as CMake evaluated them for this tree. A rule sealed inside an if() no build
    # satisfies still reads as live in the source text, and is absent here. Meaningful only where the
    # plugin is built: elsewhere the package rule is rightly absent.
    WALLPAPERX_INSTALL_SCRIPT="${CMAKE_CURRENT_BINARY_DIR}/cmake_install.cmake"
    WALLPAPERX_KDE_PLUGIN_BUILT=$<BOOL:${WALLPAPERX_BUILD_KDE_PLUGIN}>
)

# The QML the wallpaper package ships (kde/kpackage/contents/ui/) put through Qt's QML linter.
# Nothing else in this tree ever compiles those files -- Plasma loads them -- so a property
# named wrongly in a binding or read back under the wrong name is accepted at load time and
# silently shows nothing. What qmllint does *not* catch is written out in the test's own
# header: a name reached through a `var`, and every read and write in a file where a nested
# function declares a `const` or `let`, as main.qml's request handler does.
#
# Outside the if(WALLPAPERX_BUILD_KDE_PLUGIN) guard for the reason kpackage_metadata is: the
# sanitizer tree and the hosted runner both configure with that option OFF.
add_executable(wallpaperx-qml-lint-test tests/qml_lint_test.cpp)
target_link_libraries(wallpaperx-qml-lint-test PRIVATE nlohmann_json::nlohmann_json)
# Same reason as the kpackage_metadata target above: this links no daemon library, so nothing
# else puts backend/ on the include path. The renderer's tests directory is on it for
# support/forked_child.h, which arms the qmllint child against this test dying without
# unwinding (X.8); that header is POSIX-only and pulls in no renderer code.
target_include_directories(wallpaperx-qml-lint-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../renderer/tests)
# WallpaperSurface.qml imports the project's own QML module, and qmllint can only resolve a
# module's types from the type information the build generates. Passed as the module's parent
# directory because that is what an import path means: com.wallpaperx resolves to com/wallpaperx
# beneath it. Left undefined when this build produces no module, and the entry then reports the
# file as unlintable rather than failing it.
if(WALLPAPERX_BUILD_KDE_PLUGIN AND WALLPAPERX_HAVE_FRAME_IMPORT)
    target_compile_definitions(wallpaperx-qml-lint-test PRIVATE
        WALLPAPERX_QML_MODULE_PATH="${CMAKE_CURRENT_BINARY_DIR}/qml")
    # The type information the -I path points at is generated by this target.
    add_dependencies(wallpaperx-qml-lint-test wallpaperxplugin)
endif()

# The Plasma wallpaper QML's own behaviour, as against whether it parses: Qt Quick Test loads
# main.qml headless and drives the states it can show (S10.10). Outside the KDE plugin guard for
# the reason the lint test is, and QUIET rather than REQUIRED so a host without Qt's test module
# -- the hosted runner installs no Qt at all -- configures exactly as before and simply does not
# register the entry.
find_package(Qt6 QUIET COMPONENTS QuickTest Qml)
if(Qt6QuickTest_FOUND)
    add_executable(wallpaperx-qml-states-test tests/qml_states_test.cpp)
    target_link_libraries(wallpaperx-qml-states-test PRIVATE Qt6::QuickTest Qt6::Qml Threads::Threads)
    # The stub daemon in that file answers on the port main.qml's daemonOrigin names, which is
    # the only way to reach the two decisions refresh() makes before apply() is called.
    target_include_directories(wallpaperx-qml-states-test PRIVATE ${HTTPLIB_INCLUDE_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
    # Explicit, not inherited: qt_standard_project_setup() -- which turns AUTOMOC on globally --
    # is inside the KDE-plugin guard, so a -DWALLPAPERX_BUILD_KDE_PLUGIN=OFF tree (the sanitizer
    # tier's) would otherwise fail to link the Q_OBJECT stub this target now contains.
    set_target_properties(wallpaperx-qml-states-test PROPERTIES AUTOMOC ON)
    # Where the tst_*.qml files live, compiled in rather than passed at run time so the entry
    # cannot be registered against a directory that moved.
    target_compile_definitions(wallpaperx-qml-states-test PRIVATE
        QUICK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/qml")
else()
    message(STATUS "  QML state tests: disabled (no Qt6 QuickTest module) -- S10.10 stays uncovered here")
endif()

# Qt is asked where it keeps its tools before any directory is guessed; a hand-written list can
# only ever describe layouts someone thought of. Not REQUIRED and not a link dependency, so a
# host with no Qt configures exactly as before. The literal hints stay for the other direction
# -- a host with qmllint but no Qt CMake package -- and because Arch keeps Qt 6's tools in
# /usr/lib/qt6/bin, off PATH.
if(NOT QT6_INSTALL_PREFIX)
    find_package(Qt6 QUIET COMPONENTS Qml)
endif()
set(WALLPAPERX_QMLLINT_HINTS /usr/lib/qt6/bin /usr/lib64/qt6/bin /usr/lib/x86_64-linux-gnu/qt6/bin)
if(QT6_INSTALL_PREFIX AND QT6_INSTALL_BINS)
    list(PREPEND WALLPAPERX_QMLLINT_HINTS "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}")
endif()
find_program(WALLPAPERX_QMLLINT_EXECUTABLE
    NAMES qmllint qmllint-qt6
    HINTS ${WALLPAPERX_QMLLINT_HINTS}
    DOC "Qt's QML linter, used by the qml_lint test to grade the Plasma wallpaper QML")

# find_program establishes a name; the test needs a capability. A qmllint predating Qt 6's
# --json writes usage text to stderr and nothing to stdout, so accepting it unprobed skips the
# test forever while the warning below -- the one signal that says this coverage is absent --
# stays silent, suppressed by exactly the case that needs it.
#
# The verdict goes in a plain variable and the cache entry is never cleared: it may have been
# set with -D, and clearing it replaces a developer's chosen linter with a fresh system search
# on the next configure -- silently, and it is how the test's stand-in linters are supplied.
set(WALLPAPERX_QMLLINT_FOR_TEST "${WALLPAPERX_QMLLINT_EXECUTABLE}")
if(WALLPAPERX_QMLLINT_EXECUTABLE)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/qmllint_probe.qml" "import QtQml\nQtObject {}\n")
    execute_process(
        COMMAND "${WALLPAPERX_QMLLINT_EXECUTABLE}" --json - "${CMAKE_CURRENT_BINARY_DIR}/qmllint_probe.qml"
        OUTPUT_VARIABLE WALLPAPERX_QMLLINT_PROBE_OUT
        # Captured only to keep an old qmllint's usage text out of the configure log.
        ERROR_VARIABLE WALLPAPERX_QMLLINT_PROBE_ERR
        RESULT_VARIABLE WALLPAPERX_QMLLINT_PROBE_RESULT
        TIMEOUT 15)
    file(REMOVE "${CMAKE_CURRENT_BINARY_DIR}/qmllint_probe.qml")
    # The exit status is deliberately not consulted for the accept/reject decision: a working
    # qmllint exits nonzero for a file with findings and still emits the report, so the output
    # is the only usable answer -- the same reason the test itself grades the JSON body rather
    # than the status. A substring, not parsed JSON: the weakest property the test's own reader
    # needs, so this stays strictly more permissive than the test and a tool accepted here but
    # rejected there still ends in that test's named skip rather than a false pass.
    string(FIND "${WALLPAPERX_QMLLINT_PROBE_OUT}" "\"files\"" WALLPAPERX_QMLLINT_PROBE_FILES)
    if(WALLPAPERX_QMLLINT_PROBE_FILES LESS 0)
        # Only to name the cause: empty output arrives equally from a pre---json qmllint, one
        # that could not start, and one killed at the TIMEOUT. CMake stores a number for a
        # process that ran and a message for one that did not.
        if(WALLPAPERX_QMLLINT_PROBE_RESULT MATCHES "^[0-9]+$")
            set(WALLPAPERX_QMLLINT_WHY "it did not answer --json with a \"files\" report")
        else()
            set(WALLPAPERX_QMLLINT_WHY "it could not be run to completion (${WALLPAPERX_QMLLINT_PROBE_RESULT})")
        endif()
        message(STATUS "Rejecting ${WALLPAPERX_QMLLINT_EXECUTABLE}: ${WALLPAPERX_QMLLINT_WHY}")
        set(WALLPAPERX_QMLLINT_FOR_TEST "")
    endif()
endif()

if(NOT WALLPAPERX_QMLLINT_FOR_TEST)
    message(WARNING
        "no usable qmllint that understands --json -- the qml_lint test cannot lint the Plasma wallpaper QML "
        "(the lint half of S10.9/S10.12) and will report itself to ctest as SKIPPED, not passed. If a STATUS line "
        "above says one was rejected, it names the cause; -UWALLPAPERX_QMLLINT_EXECUTABLE searches again. Otherwise "
        "install Qt 6's qmllint to get that coverage.")
endif()

# The files under test are source, not build output, and ctest runs in the build tree. The
# linter path is resolved here so a host without it produces a named skip, not an exec failure
# inside a fork.
target_compile_definitions(wallpaperx-qml-lint-test PRIVATE
    WALLPAPERX_BACKEND_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    WALLPAPERX_QMLLINT_EXECUTABLE="${WALLPAPERX_QMLLINT_FOR_TEST}"
)
# Qt's own QML directory, so the linter can be run with only it and the build tree's module on its
# import path: its default path would otherwise find a system-wide install of com.wallpaperx first.
if(QT6_INSTALL_PREFIX AND QT6_INSTALL_QML)
    target_compile_definitions(wallpaperx-qml-lint-test PRIVATE
        WALLPAPERX_QT_QML_IMPORT_DIR="${QT6_INSTALL_PREFIX}/${QT6_INSTALL_QML}")
endif()

# The system-level tier: spawns the real wallpaperx-daemon executable and drives it over
# the bus. It links the daemon library only for the DBus name/interface constants and
# the client-side test scaffolding -- deliberately not to call into the daemon's code,
# since the point of this tier is that the daemon under test is a separate program.
add_executable(wallpaperx-daemon-system-test tests/daemon_system_test.cpp)
target_link_libraries(wallpaperx-daemon-system-test PRIVATE wallpaperx-daemon-lib)
target_include_directories(wallpaperx-daemon-system-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../renderer/tests)
# S6.35 needs a session bus it is allowed to destroy under a running daemon, which the
# one dbus-run-session hands the test is not: the test's own connection lives on it.
# Located here rather than found at runtime so a host without dbus-daemon produces a
# named skip in the test's output instead of an exec failure inside a fork.
find_program(DBUS_DAEMON_EXECUTABLE dbus-daemon)
if(NOT DBUS_DAEMON_EXECUTABLE)
    # This warning is emitted once and then cached, so it must not be the only signal --
    # and it no longer is: the test itself now exits 77 when it cannot start a private bus,
    # and SKIP_RETURN_CODE below turns that into a ctest SKIP. A host without dbus-daemon
    # therefore reports daemon_system as skipped rather than passed, every run, which is
    # the difference between "S6.35 held" and "S6.35 never ran".
    message(WARNING
        "dbus-daemon not found -- daemon_system_test cannot exercise the connection-loss goal (S6.35) "
        "and will report itself to ctest as SKIPPED, not passed. Install dbus to get that coverage.")
    set(DBUS_DAEMON_EXECUTABLE "")
endif()

target_compile_definitions(wallpaperx-daemon-system-test PRIVATE
    WALLPAPERX_TEST_IMAGE_DIR="${CMAKE_CURRENT_BINARY_DIR}/test_assets"
    WALLPAPERX_DAEMON_BINARY="$<TARGET_FILE:wallpaperx-daemon>"
    WALLPAPERX_DBUS_DAEMON_BINARY="${DBUS_DAEMON_EXECUTABLE}"
    # The configuration every entry's bus runs on; the nested bus this test starts uses it too.
    WALLPAPERX_ISOLATED_SESSION_CONF="${WALLPAPERX_ISOLATED_SESSION_CONF}"
)
# Baking the path in is not enough -- without this the test can be built and run against
# a stale or missing daemon binary, and would report the daemon's absence as a bus
# timeout thirty seconds later.
add_dependencies(wallpaperx-daemon-system-test wallpaperx-daemon)
# As dbus_service_test: reads the real daemon's opaque-fd frame through the plugin's GL importer.
if(WALLPAPERX_HAVE_FRAME_IMPORT)
    target_link_libraries(wallpaperx-daemon-system-test PRIVATE wallpaperx-frame-import)
    target_compile_definitions(wallpaperx-daemon-system-test PRIVATE WALLPAPERX_HAVE_FRAME_IMPORT=1)
endif()

# --------------------------------------------------------------------------- #
# Step 13 Stage E1: a video wallpaper drawn by the daemon
# --------------------------------------------------------------------------- #
#
# Its own binary rather than another phase of dbus_service_test, for one reason. On this GPU
# ffmpeg's Vulkan decoder raises VUID-VkVideoBeginCodingInfoKHR-flags-07244 on every decode, so
# any process that draws a video stops being able to grade "zero validation warnings" the way
# that file does. This binary excuses that one message id -- by id, through the validation
# layer's own filter, announced on every run -- and keeping it separate leaves dbus_service_test's
# oracle exactly as strict as it was.
set(WALLPAPERX_BACKEND_VIDEO_ASSET_DIR ${CMAKE_CURRENT_BINARY_DIR}/video_assets)
find_program(WALLPAPERX_FFMPEG_EXECUTABLE ffmpeg)
if(WALLPAPERX_FFMPEG_EXECUTABLE)
    file(MAKE_DIRECTORY ${WALLPAPERX_BACKEND_VIDEO_ASSET_DIR})
    # Four vertical bands over 320x120, drawn against a 64x48 monitor. Covering that monitor
    # crops to the middle half, so a correct draw shows green then yellow and no red or blue at
    # all: a stretch, a letterbox, an off-centre crop and a mirrored draw each put a band where
    # the check does not expect one. Tagged with setparams rather than -color_*, which libx264
    # drops silently -- the same trap renderer/CMakeLists.txt records for its own fixtures.
    add_custom_command(
        OUTPUT ${WALLPAPERX_BACKEND_VIDEO_ASSET_DIR}/wallpaper_bands.mp4
        COMMAND ${WALLPAPERX_FFMPEG_EXECUTABLE} -y -v error
                -f lavfi -i color=c=black:size=320x240:rate=10:duration=1
                -vf "drawbox=x=0:y=0:w=320:h=60:color=0xFF0000@1:t=fill,drawbox=x=0:y=60:w=320:h=60:color=0x00FF00@1:t=fill,drawbox=x=0:y=120:w=320:h=60:color=0xFFFF00@1:t=fill,drawbox=x=0:y=180:w=320:h=60:color=0x0000FF@1:t=fill,format=yuv420p,setparams=colorspace=bt709:color_primaries=bt709:color_trc=bt709:range=tv"
                -c:v libx264 -profile:v baseline -g 5 -pix_fmt yuv420p
                ${WALLPAPERX_BACKEND_VIDEO_ASSET_DIR}/wallpaper_bands.mp4
        COMMENT "Generating video wallpaper fixture wallpaper_bands.mp4"
        VERBATIM
    )
    # The same bands in a file that holds exactly one picture. A session opened on it parks after
    # its first picture and decodes no more, which is what lets the phases that count opens and
    # decodes inside one render pass attribute every count to that pass: with a file that keeps
    # playing, the clock decodes on the same thread between the counter reset and the read, and no
    # counter can say which of the two did it. Also the fixture for "a one-picture video is shown
    # once and never decoded again".
    add_custom_command(
        OUTPUT ${WALLPAPERX_BACKEND_VIDEO_ASSET_DIR}/wallpaper_one_picture.mp4
        COMMAND ${WALLPAPERX_FFMPEG_EXECUTABLE} -y -v error
                -f lavfi -i color=c=black:size=320x240:rate=10:duration=1
                -vf "drawbox=x=0:y=0:w=320:h=60:color=0xFF0000@1:t=fill,drawbox=x=0:y=60:w=320:h=60:color=0x00FF00@1:t=fill,drawbox=x=0:y=120:w=320:h=60:color=0xFFFF00@1:t=fill,drawbox=x=0:y=180:w=320:h=60:color=0x0000FF@1:t=fill,format=yuv420p,setparams=colorspace=bt709:color_primaries=bt709:color_trc=bt709:range=tv"
                -frames:v 1 -c:v libx264 -profile:v baseline -g 5 -pix_fmt yuv420p
                ${WALLPAPERX_BACKEND_VIDEO_ASSET_DIR}/wallpaper_one_picture.mp4
        COMMENT "Generating video wallpaper fixture wallpaper_one_picture.mp4"
        VERBATIM
    )
    # The same bands again, at one picture every two seconds. The gap is the point: it is longer --
    # by more than twice -- than the interval after which video_wallpaper_test's daemon decides
    # nothing is collecting a monitor's pictures (750 ms there; production's two seconds is not what
    # that test runs with),
    # so a daemon that measured that interval from the last HAND-OVER -- rather than from the oldest
    # picture nobody has taken -- would call its own consumer gone between two pictures of this file
    # and freeze it. Every other fixture here plays ten pictures a second, where the two rules agree.
    # Three pictures, so the check crosses the gap twice without the entry taking a minute.
    add_custom_command(
        OUTPUT ${WALLPAPERX_BACKEND_VIDEO_ASSET_DIR}/wallpaper_slow.mp4
        COMMAND ${WALLPAPERX_FFMPEG_EXECUTABLE} -y -v error
                -f lavfi -i color=c=black:size=320x240:rate=1/2:duration=6
                -vf "drawbox=x=0:y=0:w=320:h=60:color=0xFF0000@1:t=fill,drawbox=x=0:y=60:w=320:h=60:color=0x00FF00@1:t=fill,drawbox=x=0:y=120:w=320:h=60:color=0xFFFF00@1:t=fill,drawbox=x=0:y=180:w=320:h=60:color=0x0000FF@1:t=fill,format=yuv420p,setparams=colorspace=bt709:color_primaries=bt709:color_trc=bt709:range=tv"
                -c:v libx264 -profile:v baseline -g 1 -pix_fmt yuv420p -r 1/2
                ${WALLPAPERX_BACKEND_VIDEO_ASSET_DIR}/wallpaper_slow.mp4
        COMMENT "Generating video wallpaper fixture wallpaper_slow.mp4"
        VERBATIM
    )
    # A file that declares a rate no screen can show: 240 pictures timed at 1000 a second. The
    # daemon's floor on the gap between pictures caps it at 120 a second, so this plays in full and
    # slower than its author meant. Without a floor near what a display can do, a file like this one
    # -- and wallpapers are exactly the thing this project plans to take from a workshop -- drives
    # the single GPU thread at whatever rate it asks for, which the QA pass measured as 688 renders
    # a second and a twelvefold rise in every other client's frame-fetch latency.
    add_custom_command(
        OUTPUT ${WALLPAPERX_BACKEND_VIDEO_ASSET_DIR}/wallpaper_fast.mp4
        COMMAND ${WALLPAPERX_FFMPEG_EXECUTABLE} -y -v error
                -f lavfi -i color=c=black:size=320x240:rate=1000:duration=0.24
                -vf "drawbox=x=0:y=0:w=320:h=120:color=0xFF0000@1:t=fill,drawbox=x=0:y=120:w=320:h=120:color=0x00FF00@1:t=fill,format=yuv420p,setparams=colorspace=bt709:color_primaries=bt709:color_trc=bt709:range=tv"
                -c:v libx264 -profile:v baseline -g 30 -pix_fmt yuv420p -r 1000
                ${WALLPAPERX_BACKEND_VIDEO_ASSET_DIR}/wallpaper_fast.mp4
        COMMENT "Generating video wallpaper fixture wallpaper_fast.mp4"
        VERBATIM
    )
    # The other end of the same clamp: three pictures eight seconds apart, which is past the
    # five-second ceiling on one picture's interval. A gap that long is read as a discontinuity
    # rather than a pause, so the wallpaper advances after five seconds instead of eight -- which is
    # what keeps a timestamp jump from freezing a wallpaper for as long as the jump says.
    add_custom_command(
        OUTPUT ${WALLPAPERX_BACKEND_VIDEO_ASSET_DIR}/wallpaper_gap.mp4
        COMMAND ${WALLPAPERX_FFMPEG_EXECUTABLE} -y -v error
                -f lavfi -i color=c=black:size=320x240:rate=1/8:duration=24
                -vf "drawbox=x=0:y=0:w=320:h=120:color=0xFF0000@1:t=fill,drawbox=x=0:y=120:w=320:h=120:color=0x00FF00@1:t=fill,format=yuv420p,setparams=colorspace=bt709:color_primaries=bt709:color_trc=bt709:range=tv"
                -c:v libx264 -profile:v baseline -g 1 -pix_fmt yuv420p -r 1/8
                ${WALLPAPERX_BACKEND_VIDEO_ASSET_DIR}/wallpaper_gap.mp4
        COMMENT "Generating video wallpaper fixture wallpaper_gap.mp4"
        VERBATIM
    )
    # A 120 a second file whose container rounds timestamps to whole milliseconds -- Matroska does
    # -- so its gaps read 8, 9, 8 ms. The floor held on each gap alone raises every 8 to 8.33 and
    # leaves the 9, and plays the file at 116 a second; held between due times it plays at 120.
    add_custom_command(
        OUTPUT ${WALLPAPERX_BACKEND_VIDEO_ASSET_DIR}/wallpaper_rate120_ms.mkv
        COMMAND ${WALLPAPERX_FFMPEG_EXECUTABLE} -y -v error
                -f lavfi -i color=c=black:size=320x240:rate=120:duration=3
                -vf "drawbox=x=0:y=0:w=320:h=120:color=0xFF0000@1:t=fill,format=yuv420p,setparams=colorspace=bt709:color_primaries=bt709:color_trc=bt709:range=tv"
                -c:v libx264 -profile:v baseline -g 30 -pix_fmt yuv420p
                ${WALLPAPERX_BACKEND_VIDEO_ASSET_DIR}/wallpaper_rate120_ms.mkv
        COMMENT "Generating video wallpaper fixture wallpaper_rate120_ms.mkv"
        VERBATIM
    )
    # 240 pictures a second with regular timestamps, twice the cap at speed 1: the file whose own time
    # trails the capped drawing time by design, which is where the schedule's late test and the lag it
    # banks are graded.
    add_custom_command(
        OUTPUT ${WALLPAPERX_BACKEND_VIDEO_ASSET_DIR}/wallpaper_rate240.mp4
        COMMAND ${WALLPAPERX_FFMPEG_EXECUTABLE} -y -v error
                -f lavfi -i color=c=black:size=320x240:rate=240:duration=3
                -vf "drawbox=x=0:y=0:w=320:h=120:color=0xFF0000@1:t=fill,format=yuv420p,setparams=colorspace=bt709:color_primaries=bt709:color_trc=bt709:range=tv"
                -c:v libx264 -profile:v baseline -g 30 -pix_fmt yuv420p -r 240
                ${WALLPAPERX_BACKEND_VIDEO_ASSET_DIR}/wallpaper_rate240.mp4
        COMMENT "Generating video wallpaper fixture wallpaper_rate240.mp4"
        VERBATIM
    )
    # Twelve seconds of 4K whose only keyframe is its first: a video resumed 150 pictures in decodes
    # them all to catch up, which is what makes a catch-up that holds the GPU in one piece visible.
    add_custom_command(
        OUTPUT ${WALLPAPERX_BACKEND_VIDEO_ASSET_DIR}/wallpaper_4k_long_gop.mp4
        COMMAND ${WALLPAPERX_FFMPEG_EXECUTABLE} -y -v error
                -f lavfi -i testsrc2=size=3840x2160:rate=30:duration=12
                -c:v libx264 -preset ultrafast -crf 32 -g 1000 -keyint_min 1000 -sc_threshold 0 -pix_fmt yuv420p
                ${WALLPAPERX_BACKEND_VIDEO_ASSET_DIR}/wallpaper_4k_long_gop.mp4
        COMMENT "Generating video wallpaper fixture wallpaper_4k_long_gop.mp4"
        VERBATIM
    )
    add_custom_target(wallpaperx-backend-video-fixtures
                      DEPENDS ${WALLPAPERX_BACKEND_VIDEO_ASSET_DIR}/wallpaper_4k_long_gop.mp4
                              ${WALLPAPERX_BACKEND_VIDEO_ASSET_DIR}/wallpaper_rate240.mp4
                              ${WALLPAPERX_BACKEND_VIDEO_ASSET_DIR}/wallpaper_bands.mp4
                              ${WALLPAPERX_BACKEND_VIDEO_ASSET_DIR}/wallpaper_one_picture.mp4
                              ${WALLPAPERX_BACKEND_VIDEO_ASSET_DIR}/wallpaper_slow.mp4
                              ${WALLPAPERX_BACKEND_VIDEO_ASSET_DIR}/wallpaper_fast.mp4
                              ${WALLPAPERX_BACKEND_VIDEO_ASSET_DIR}/wallpaper_gap.mp4
                              ${WALLPAPERX_BACKEND_VIDEO_ASSET_DIR}/wallpaper_rate120_ms.mkv)
else()
    message(STATUS "  ffmpeg CLI: not found -- the video wallpaper fixture is not generated, and "
                   "video_wallpaper will report itself to ctest as a skip rather than a pass")
endif()

add_executable(wallpaperx-video-wallpaper-test tests/video_wallpaper_test.cpp)
target_link_libraries(wallpaperx-video-wallpaper-test PRIVATE wallpaperx-daemon-lib)
# renderer/tests for the shared probe scaffolding: the frame mapper, the cover-crop
# expectation, the PNG fixture writer and the validation grade.
target_include_directories(wallpaperx-video-wallpaper-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../renderer/tests)
# Its own asset directory, not the one dbus_service_test and http_server_test share: this
# entry holds no lock with them, and a directory-scoped fixture written by one while another
# reads it is how a parallel ctest turns into a failure that looks like a defect.
target_compile_definitions(wallpaperx-video-wallpaper-test PRIVATE
    WALLPAPERX_TEST_IMAGE_DIR="${CMAKE_CURRENT_BINARY_DIR}/video_test_assets"
    WALLPAPERX_VIDEO_ASSET_DIR="${WALLPAPERX_BACKEND_VIDEO_ASSET_DIR}")
if(WALLPAPERX_FFMPEG_EXECUTABLE)
    add_dependencies(wallpaperx-video-wallpaper-test wallpaperx-backend-video-fixtures)
endif()
# The renderer's counter fixture, which draws its frame number into every picture: what lets the
# resume phases say which picture a served frame is. Its directory is the renderer's binary directory,
# which this file names when it adds that subproject.
target_compile_definitions(wallpaperx-video-wallpaper-test PRIVATE
    WALLPAPERX_COUNTER_VIDEO="${CMAKE_CURRENT_BINARY_DIR}/renderer/video_assets/counter_long.mp4")
if(TARGET wallpaperx-video-fixtures)
    add_dependencies(wallpaperx-video-wallpaper-test wallpaperx-video-fixtures)
endif()
# The GL importer, to read an opaque-fd frame's pixels back on the driver that exported it; a dma_buf
# frame is mapped instead. Without it the resume phases report the opaque path as not answerable.
if(WALLPAPERX_HAVE_FRAME_IMPORT)
    target_link_libraries(wallpaperx-video-wallpaper-test PRIVATE wallpaperx-frame-import)
    target_compile_definitions(wallpaperx-video-wallpaper-test PRIVATE WALLPAPERX_HAVE_FRAME_IMPORT=1)
endif()

# The HTTP tier reads the same clip, for the one thing only it can answer: what /health says about a
# monitor showing a video. Attached here rather than at that target's own definition because the
# fixture directory is not known until this point in the file.
target_compile_definitions(wallpaperx-http-server-test PRIVATE
    WALLPAPERX_VIDEO_ASSET_DIR="${WALLPAPERX_BACKEND_VIDEO_ASSET_DIR}")
if(WALLPAPERX_FFMPEG_EXECUTABLE)
    add_dependencies(wallpaperx-http-server-test wallpaperx-backend-video-fixtures)
endif()

# Step 13 Stage E5's shutdown watchdog needs a stop that genuinely cannot finish, in the real daemon:
# a library preloaded into it that parks the manager's teardown inside a decoder free. Test-only --
# nothing shipped links it, and it does nothing unless the test sets its variable.
add_library(wallpaperx-test-wedge-shim SHARED tests/wedge_shim.cpp)
# ffmpeg's headers only: the shim tells the daemon's own decoder context from the ones ffmpeg opens
# while probing a file, which is a field of AVCodecContext. It calls through dlsym, never by link.
pkg_check_modules(WALLPAPERX_SHIM_LIBAV REQUIRED IMPORTED_TARGET libavcodec libavutil)
target_link_libraries(wallpaperx-test-wedge-shim PRIVATE ${CMAKE_DL_LIBS} PkgConfig::WALLPAPERX_SHIM_LIBAV)
target_include_directories(wallpaperx-test-wedge-shim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../renderer/tests)
set_target_properties(wallpaperx-test-wedge-shim PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_compile_definitions(wallpaperx-daemon-system-test PRIVATE
    WALLPAPERX_VIDEO_ASSET_DIR="${WALLPAPERX_BACKEND_VIDEO_ASSET_DIR}"
    WALLPAPERX_WEDGE_SHIM="$<TARGET_FILE:wallpaperx-test-wedge-shim>")
add_dependencies(wallpaperx-daemon-system-test wallpaperx-test-wedge-shim)

# daemon_system_test's frameAllocationFailure phase fails one chosen allocation on the daemon's
# frame-reply thread (tests/alloc_fail_shim.cpp). Test-only, and inert unless the test names the
# control file it arms.
add_library(wallpaperx-test-alloc-fail-shim SHARED tests/alloc_fail_shim.cpp)
target_link_libraries(wallpaperx-test-alloc-fail-shim PRIVATE ${CMAKE_DL_LIBS})
target_include_directories(wallpaperx-test-alloc-fail-shim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(wallpaperx-test-alloc-fail-shim PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_compile_definitions(wallpaperx-daemon-system-test PRIVATE
    WALLPAPERX_ALLOC_FAIL_SHIM="$<TARGET_FILE:wallpaperx-test-alloc-fail-shim>")
add_dependencies(wallpaperx-daemon-system-test wallpaperx-test-alloc-fail-shim)

# The device-loss exit needs a lost device in the real daemon, and hanging a real GPU takes
# the desktop with it: a library preloaded into it that fails the daemon's queue submissions as
# VK_ERROR_DEVICE_LOST once a trigger file exists. Test-only, like the wedge shim; Vulkan's headers
# only, since it reaches the loader through dlsym.
add_library(wallpaperx-test-device-lost-shim SHARED tests/device_lost_shim.cpp)
# Found again here: the renderer's find_package() is scoped to its own directory.
find_package(Vulkan REQUIRED)
target_link_libraries(wallpaperx-test-device-lost-shim PRIVATE ${CMAKE_DL_LIBS} Vulkan::Headers)
target_include_directories(wallpaperx-test-device-lost-shim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../renderer/tests)
set_target_properties(wallpaperx-test-device-lost-shim PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
# daemon_system_test holds a render at its submission, and loses the device, during a failed start.
target_compile_definitions(wallpaperx-daemon-system-test PRIVATE
    WALLPAPERX_DEVICE_LOST_SHIM="$<TARGET_FILE:wallpaperx-test-device-lost-shim>")
add_dependencies(wallpaperx-daemon-system-test wallpaperx-test-device-lost-shim)

# What that library's one line does to the stream it lands on. Links the Vulkan loader only for the
# entry point the shim interposes; the fake submit never reaches a driver.
add_executable(wallpaperx-device-lost-shim-test tests/device_lost_shim_test.cpp)
target_link_libraries(wallpaperx-device-lost-shim-test PRIVATE Vulkan::Vulkan)
target_include_directories(wallpaperx-device-lost-shim-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                           ${CMAKE_CURRENT_SOURCE_DIR}/../renderer/tests)
set_target_properties(wallpaperx-device-lost-shim-test PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_compile_definitions(wallpaperx-device-lost-shim-test PRIVATE
    WALLPAPERX_DEVICE_LOST_SHIM="$<TARGET_FILE:wallpaperx-test-device-lost-shim>")
add_dependencies(wallpaperx-device-lost-shim-test wallpaperx-test-device-lost-shim)
# AddressSanitizer refuses to start unless its runtime is first in the initial library list, and
# the children of this entry and of daemon_system preload shims; each test keeps an inherited
# LD_PRELOAD ahead of them. Set after the wrapper's `--`, through env, so it reaches the test alone:
# in the entry's environment it would reach /bin/sh and the bus as well, whose exit the leak checker
# reports.
set(_wx_asan_preload "")
if(WALLPAPERX_SANITIZE STREQUAL "address")
    execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=libasan.so
                    OUTPUT_VARIABLE _wx_asan_runtime OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(IS_ABSOLUTE "${_wx_asan_runtime}" AND EXISTS "${_wx_asan_runtime}")
        set(_wx_asan_preload env "LD_PRELOAD=${_wx_asan_runtime}")
    else()
        message(WARNING "No AddressSanitizer runtime found to preload; device_lost_shim and daemon_system's "
                        "preloading phases will not start here")
    endif()
endif()
wallpaperx_add_test(NAME device_lost_shim COMMAND ${_wx_asan_preload} $<TARGET_FILE:wallpaperx-device-lost-shim-test>)
# no-tsan: preloading an uninstrumented library into a sanitized binary is untried.
set_tests_properties(device_lost_shim PROPERTIES LABELS "novulkan;no-tsan" SKIP_RETURN_CODE 77)

# The real daemon losing its device, exiting 75, and being started again on the same HOME.
add_executable(wallpaperx-device-loss-restart-test tests/device_loss_restart_test.cpp)
target_link_libraries(wallpaperx-device-loss-restart-test PRIVATE wallpaperx-daemon-lib)
target_include_directories(wallpaperx-device-loss-restart-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../renderer/tests)
set_target_properties(wallpaperx-device-loss-restart-test PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_compile_definitions(wallpaperx-device-loss-restart-test PRIVATE
    WALLPAPERX_DAEMON_BINARY="$<TARGET_FILE:wallpaperx-daemon>"
    WALLPAPERX_DEVICE_LOST_SHIM="$<TARGET_FILE:wallpaperx-test-device-lost-shim>"
    WALLPAPERX_WEDGE_SHIM="$<TARGET_FILE:wallpaperx-test-wedge-shim>"
    WALLPAPERX_VIDEO_ASSET_DIR="${WALLPAPERX_BACKEND_VIDEO_ASSET_DIR}")
add_dependencies(wallpaperx-device-loss-restart-test wallpaperx-daemon wallpaperx-test-device-lost-shim
                 wallpaperx-test-wedge-shim)

# The install: runs `cmake --install` on this build tree into scratch prefixes and starts the copy.
find_program(SYSTEMD_ANALYZE_EXECUTABLE systemd-analyze)
if(NOT SYSTEMD_ANALYZE_EXECUTABLE)
    set(SYSTEMD_ANALYZE_EXECUTABLE "")
endif()
add_executable(wallpaperx-daemon-install-test tests/daemon_install_test.cpp)
target_link_libraries(wallpaperx-daemon-install-test PRIVATE wallpaperx-daemon-lib)
target_include_directories(wallpaperx-daemon-install-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../renderer/tests)
set_target_properties(wallpaperx-daemon-install-test PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_compile_definitions(wallpaperx-daemon-install-test PRIVATE
    WALLPAPERX_CMAKE_COMMAND="${CMAKE_COMMAND}"
    WALLPAPERX_BUILD_DIR="${CMAKE_BINARY_DIR}"
    WALLPAPERX_SHADER_OUTPUT_DIR="${WALLPAPERX_SHADER_OUTPUT_DIR}"
    # The layout this tree installs, so an overridden one is not graded as a broken install.
    WALLPAPERX_TEST_UNIT_DIR="${WALLPAPERX_SYSTEMD_USER_UNIT_DIR}"
    WALLPAPERX_TEST_BINDIR="${CMAKE_INSTALL_BINDIR}"
    WALLPAPERX_TEST_SHADER_DIR="${WALLPAPERX_INSTALL_SHADER_DIR}"
    WALLPAPERX_SYSTEMD_ANALYZE="${SYSTEMD_ANALYZE_EXECUTABLE}"
    WALLPAPERX_VIDEO_ASSET_DIR="${WALLPAPERX_BACKEND_VIDEO_ASSET_DIR}"
    # For a scratch configure of this project, which must succeed on whatever host built this one.
    WALLPAPERX_BACKEND_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    WALLPAPERX_CMAKE_GENERATOR="${CMAKE_GENERATOR}"
    WALLPAPERX_BUILD_KDE_PLUGIN_SETTING="${WALLPAPERX_BUILD_KDE_PLUGIN}")
add_dependencies(wallpaperx-daemon-install-test wallpaperx-daemon)

if(WALLPAPERX_FFMPEG_EXECUTABLE)
    add_dependencies(wallpaperx-daemon-system-test wallpaperx-backend-video-fixtures)
    add_dependencies(wallpaperx-device-loss-restart-test wallpaperx-backend-video-fixtures)
    add_dependencies(wallpaperx-daemon-install-test wallpaperx-backend-video-fixtures)
endif()

# These targets have no Q_OBJECT / Qt usage; AUTOMOC is on globally via
# qt_standard_project_setup(), so turn it off here to skip a pointless moc step.
if(WALLPAPERX_BUILD_KDE_PLUGIN)
    set_target_properties(wallpaperx-daemon-lib wallpaperx-daemon wallpaperx-dbus-service-test
                          wallpaperx-daemon-system-test wallpaperx-http-server-test
                          wallpaperx-log-escape-test wallpaperx-config-store-test wallpaperx-request-share-test
                          wallpaperx-kpackage-metadata-test wallpaperx-qml-lint-test
                          wallpaperx-video-wallpaper-test
        PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
endif()

# buffer_claims.cpp is compiled straight in rather than linked from wallpaperx-frame-import:
# that library carries EGL and OpenGL, and this decides which images the process still holds
# with neither. Building it here is what keeps the entry registered on a host that has no EGL.
add_executable(wallpaperx-buffer-claims-test tests/buffer_claims_test.cpp kde/buffer_claims.cpp)
target_include_directories(wallpaperx-buffer-claims-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(wallpaperx-buffer-claims-test PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

# `novulkan` claims the test opens no Vulkan driver. CI and the TSan block are allow-lists
# keyed on it, so attaching it is also how a test gets selected -- never attach it for that
# reason. Verify with VK_LOADER_DEBUG=driver: no ICD load line.
#
# Registered outside the dbus-run-session guard below: this one talks to nothing.
wallpaperx_add_test(NAME log_escape COMMAND $<TARGET_FILE:wallpaperx-log-escape-test>)

# Outside the guard for the strongest version of log_escape's reason: no bus, no window, no
# driver, no filesystem. `novulkan` because the sanitizer block below selects by label and an
# unlabelled entry is disabled there.
wallpaperx_add_test(NAME buffer_claims COMMAND $<TARGET_FILE:wallpaperx-buffer-claims-test>)
# The other way out of the ledger's startup lifetime, which a process leaves only once.
wallpaperx_add_test(NAME buffer_claims_departure_first
         COMMAND $<TARGET_FILE:wallpaperx-buffer-claims-test> departure-first)
set_tests_properties(buffer_claims_departure_first PROPERTIES LABELS "novulkan" SKIP_RETURN_CODE 77)

# Reads the linked daemon binary for calls that copy a decoded picture into system memory (S13.9).
# Opens no driver, hence `novulkan`.
wallpaperx_add_test(NAME no_host_readback
         COMMAND ${CMAKE_COMMAND} -DBINARY=$<TARGET_FILE:wallpaperx-daemon>
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/no_host_readback.cmake)
set_tests_properties(no_host_readback PROPERTIES LABELS "novulkan")
set_tests_properties(buffer_claims PROPERTIES LABELS "novulkan"
                     SKIP_RETURN_CODE 77)
# `novulkan`, for the same reason the five renderer entries carry it: the TSan block below
# is an allow-list on labels, so an unlabelled test is DISABLED there. sanitizeForLog is a
# pure function of bytes and opens no driver, so the proprietary-ICD SEGV that forces that
# tier to lavapipe-only cannot reach it -- and the documented manual fallback
# `ctest -L 'lavapipe|novulkan'` needs the label to select it too.
set_tests_properties(log_escape PROPERTIES LABELS "novulkan")

# Also outside the guard: it talks to no bus either, and every check it makes is
# deterministic, so it needs no RESOURCE_LOCK. `novulkan` for the same reason log_escape
# carries it: the TSan block below is an allow-list on labels, and nothing here opens a
# driver.
wallpaperx_add_test(NAME config_store COMMAND $<TARGET_FILE:wallpaperx-config-store-test>)
set_tests_properties(config_store PROPERTIES LABELS "novulkan"
                     SKIP_RETURN_CODE 77)

# Each caller's share of the render worker's queue, on the manager's own record: no bus, no device.
# Outside the guard and `novulkan` for config_store's reasons. One phase sleeps out the manager's
# fixed ten-second quiet period, so the entry takes about eleven seconds.
wallpaperx_add_test(NAME request_share COMMAND $<TARGET_FILE:wallpaperx-request-share-test>)
set_tests_properties(request_share PROPERTIES LABELS "novulkan")

# Outside the guard too: forked children, signals and files, with no bus, port or driver.
# `novulkan` because the sanitizer block below selects by label and an unlabelled entry is
# disabled there. SKIP_RETURN_CODE because a host that cannot make a scratch directory, or a
# filesystem with no symlinks, is this test failing to set up rather than the breaker being
# wrong. No RESOURCE_LOCK: it competes for nothing and measures no timing.
wallpaperx_add_test(NAME crash_record COMMAND $<TARGET_FILE:wallpaperx-crash-record-test>)
set_tests_properties(crash_record PROPERTIES LABELS "novulkan"
                     SKIP_RETURN_CODE 77)
# The same reasons as crash_record: forked children and files only. Its CPU-time bound is a quarter
# of the window, far from a correct writer's few milliseconds, so it takes no timing lock.
wallpaperx_add_test(NAME log_pump COMMAND $<TARGET_FILE:wallpaperx-log-pump-test>)
set_tests_properties(log_pump PROPERTIES LABELS "novulkan" SKIP_RETURN_CODE 77)

# Its companion needs a device, so it is registered the way the other GPU entries are: no
# label on the default-ICD run, which is what disables it in the TSan tree (the proprietary
# driver SEGVs there inside its own uninstrumented code), and a labelled lavapipe run that
# survives both that tree and the hosted CI runner. SKIP_RETURN_CODE because a host with no
# working device must report the render-dependent goals unexercised rather than green. No
# RESOURCE_LOCK: it measures no timing and writes its fixtures into a scratch directory of
# its own, so it competes with nothing.
wallpaperx_add_test(NAME crash_quarantine COMMAND $<TARGET_FILE:wallpaperx-crash-quarantine-test>)
set_tests_properties(crash_quarantine PROPERTIES SKIP_RETURN_CODE 77)
if(WALLPAPERX_LAVAPIPE_ICD)
    wallpaperx_add_test(NAME crash_quarantine_lavapipe COMMAND $<TARGET_FILE:wallpaperx-crash-quarantine-test>)
    set_tests_properties(crash_quarantine_lavapipe PROPERTIES
        ENVIRONMENT "VK_ICD_FILENAMES=${WALLPAPERX_LAVAPIPE_ICD}"
        LABELS "lavapipe"
        SKIP_RETURN_CODE 77)
endif()

# Registered as crash_quarantine is, for the same reasons: a device, a scratch directory of its
# own, and no timing measured.
wallpaperx_add_test(NAME device_loss_worker COMMAND $<TARGET_FILE:wallpaperx-device-loss-worker-test>)
set_tests_properties(device_loss_worker PROPERTIES SKIP_RETURN_CODE 77)
if(WALLPAPERX_LAVAPIPE_ICD)
    wallpaperx_add_test(NAME device_loss_worker_lavapipe COMMAND $<TARGET_FILE:wallpaperx-device-loss-worker-test>)
    set_tests_properties(device_loss_worker_lavapipe PROPERTIES
        ENVIRONMENT "VK_ICD_FILENAMES=${WALLPAPERX_LAVAPIPE_ICD}"
        LABELS "lavapipe"
        SKIP_RETURN_CODE 77)
endif()

# Outside the guard as well, and for the strongest version of the reason: this one opens no
# bus, no port and no driver, and reads only files. `novulkan` because the TSan block below
# selects tests by label and an unlabelled test is disabled there. SKIP_RETURN_CODE because
# a source tree that is not where the build recorded it is this test failing to set up, not
# the package being wrong, and must not print as a package defect.
wallpaperx_add_test(NAME kpackage_metadata COMMAND $<TARGET_FILE:wallpaperx-kpackage-metadata-test>)
set_tests_properties(kpackage_metadata PROPERTIES LABELS "novulkan"
                     SKIP_RETURN_CODE 77)

# Outside the guard for the same reason as kpackage_metadata, with the same two properties:
# `novulkan` so the sanitizer block below selects it, and SKIP_RETURN_CODE because a host that
# cannot look -- the hosted runner installs no Qt -- has not established the lint half of
# S10.9/S10.12 and must not report that it has. Why it is a separate entry rather than a phase
# of kpackage_metadata is in that test's own header.
wallpaperx_add_test(NAME qml_lint COMMAND $<TARGET_FILE:wallpaperx-qml-lint-test>)
set_tests_properties(qml_lint PROPERTIES LABELS "novulkan"
                     SKIP_RETURN_CODE 77)

# The one file importing this project's own QML module, as its own entry: folded into the one
# above, a build with no module withholds main.qml's and config.qml's verdicts too. Registered
# unconditionally, because this row also carries checks that need no module at all -- that the
# package still ships the file, and that it has no lint directives -- and guarding the entry on
# the module took those away too. Without a module the binary reports a named skip.
wallpaperx_add_test(NAME qml_lint_surface COMMAND $<TARGET_FILE:wallpaperx-qml-lint-test> surface)
set_tests_properties(qml_lint_surface PROPERTIES LABELS "novulkan"
                     SKIP_RETURN_CODE 77)

# Registered only where Qt's test module exists. The offscreen platform is set here rather than
# left to the environment: with a real display the plugin would open a window on the developer's
# desktop, and on a host with none the run would fail for a reason unrelated to the checks.
if(Qt6QuickTest_FOUND)
    wallpaperx_add_test(NAME qml_states COMMAND $<TARGET_FILE:wallpaperx-qml-states-test>)
    # RESOURCE_LOCK, and it is the same lock http_config and daemon_system carry rather than one
    # of its own: this entry now binds 127.0.0.1:18462, the port main.qml's daemonOrigin names
    # and the port those two put a real daemon on. Under ctest -jN without it, whichever started
    # second would fail to bind -- here that is reported as a skip, and there as a daemon that
    # could not serve its config port.
    #
    # SKIP_REGULAR_EXPRESSION rather than SKIP_RETURN_CODE: Qt Quick Test exits 0 when cases
    # skip, so a host where something else already holds 18462 would otherwise show green for a
    # run in which the origin and body-size cases never happened. The trade-off is deliberate
    # and worth stating: on such a host the whole entry reads Skipped, including the twenty-odd
    # cases that did run and pass. Inconclusive is not green, and the sentinel is printed only
    # when the stub could not be brought up at all.
    #
    # no-tsan: loading the second .qml file starts another QQmlThread, and ThreadSanitizer's
    # own thread registry aborts on the reused pthread handle -- "CHECK failed:
    # sanitizer_thread_registry.cpp:186" inside QThread::start, reproducibly, after the first
    # file's cases pass. That is the runtime dying, not a report, so nothing in tsan.supp can
    # reach it; and because it lands after one file, the sanitizer tier was already grading
    # only that file. The entry runs in full in the unsanitized trees.
    set_tests_properties(qml_states PROPERTIES LABELS "novulkan;no-tsan"
                         ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
                         RESOURCE_LOCK wallpaperx_bus_tests
                         SKIP_REGULAR_EXPRESSION "wallpaperx-qml-states: NO STUB DAEMON --")
endif()

# The entries below need a session bus, which wallpaperx_add_test() gives each run through
# dbus-run-session: private, unable to activate anything, and working headless in CI. If it isn't
# installed, skip registering them rather than fail the build.
if(WALLPAPERX_DBUS_RUN_SESSION)
    wallpaperx_add_test(NAME dbus_service
             COMMAND $<TARGET_FILE:wallpaperx-dbus-service-test>)
    # Explicitly cleared, for the same reason handle_type_env_default clears its own
    # override: ctest merges each test's ENVIRONMENT into the caller's, so a developer
    # who exported WALLPAPERX_FORCE_TIER while debugging the Classic path would
    # otherwise turn this entry -- and the lavapipe twin below -- into further copies of
    # dbus_service_classic. All three would pass, the tier assertion included, while
    # Heap and Buffer went entirely unexercised through the daemon.
    set_tests_properties(dbus_service PROPERTIES ENVIRONMENT "WALLPAPERX_FORCE_TIER=")

    # The test now renders and exports a real frame, so it exercises the export path
    # end to end -- and on the owner's NVIDIA GPU that path is OPAQUE_FD only. The
    # Lavapipe twin is the only place the DMA_BUF branch (and its DRM modifier
    # metadata) crosses the DBus wire, which is the half a KDE plugin on AMD/Intel
    # will actually consume.
    if(WALLPAPERX_LAVAPIPE_ICD)
        wallpaperx_add_test(NAME dbus_service_lavapipe
                 COMMAND $<TARGET_FILE:wallpaperx-dbus-service-test>)
        set_tests_properties(dbus_service_lavapipe PROPERTIES
            ENVIRONMENT "VK_ICD_FILENAMES=${WALLPAPERX_LAVAPIPE_ICD};WALLPAPERX_FORCE_TIER="
            LABELS "lavapipe")
    endif()

    # The dma_buf export path on the GPU this project actually targets. The default entry
    # above exports an opaque fd there, so the layout negotiation -- which layout the driver
    # picks, whether it is one the consumer reported, what a report that excludes it does --
    # was reachable only on Lavapipe, whose single usable layout cannot express most of it.
    #
    # Registered only now: it used to fail on a check that compared memory_size against
    # lseek, which S5.9 records as over-reporting on this driver's page-rounded backing
    # object. That check is an upper bound now, so this configuration is honest here.
    wallpaperx_add_test(NAME dbus_service_dmabuf
             COMMAND $<TARGET_FILE:wallpaperx-dbus-service-test>)
    set_tests_properties(dbus_service_dmabuf PROPERTIES
        ENVIRONMENT "WALLPAPERX_FORCE_HANDLE_TYPE=dmabuf;WALLPAPERX_FORCE_TIER="
        SKIP_RETURN_CODE 77)

    # DoD 12 wants the Classic descriptor tier verified end to end *through the
    # daemon*, and no machine here resolves to it on its own: NVIDIA gives Heap and
    # Lavapipe gives Buffer, so without forcing it the whole daemon-side Classic path
    # is only ever exercised by a renderer probe. Same binary, same private bus, one
    # environment variable -- and dbus_service_test asserts the reported tier really is
    # Classic, so this cannot silently degrade into a second run of the default tier.
    wallpaperx_add_test(NAME dbus_service_classic
             COMMAND $<TARGET_FILE:wallpaperx-dbus-service-test>)
    set_tests_properties(dbus_service_classic PROPERTIES
        ENVIRONMENT "WALLPAPERX_FORCE_TIER=classic")

    # The Classic tier needs a lavapipe twin of its own, and not for coverage symmetry.
    # The TSan tier can only run lavapipe entries (the proprietary driver SEGVs under
    # instrumentation), so without this the daemon-side Classic path -- the one DoD 12
    # exists to cover -- is the single descriptor tier the sanitizer never sees. A race
    # in the Classic descriptor update path would be invisible to the gate built to
    # find exactly that.
    if(WALLPAPERX_LAVAPIPE_ICD)
        wallpaperx_add_test(NAME dbus_service_classic_lavapipe
                 COMMAND $<TARGET_FILE:wallpaperx-dbus-service-test>)
        set_tests_properties(dbus_service_classic_lavapipe PROPERTIES
            ENVIRONMENT "VK_ICD_FILENAMES=${WALLPAPERX_LAVAPIPE_ICD};WALLPAPERX_FORCE_TIER=classic"
            LABELS "lavapipe"
            RESOURCE_LOCK wallpaperx_bus_tests)
    endif()

    # One lock across every bus-driven test, not one per tier. Two reasons, and the
    # second is the reason it is shared rather than merely present: they render from
    # generated PNGs under the same WALLPAPERX_TEST_IMAGE_DIR, so ctest -jN must not run
    # one while another is mid-write -- and daemon_system's responsiveness phase takes a
    # ratio of contended to uncontended call times, whose per-call floors a neighbour
    # saturating the same rasteriser moves. Every entry here also holds bounded waits that
    # a busy host can expire. Serialising them costs well under a second in total.
    set_tests_properties(dbus_service PROPERTIES RESOURCE_LOCK wallpaperx_bus_tests)
    set_tests_properties(dbus_service_classic PROPERTIES RESOURCE_LOCK wallpaperx_bus_tests)
    if(WALLPAPERX_LAVAPIPE_ICD)
        set_tests_properties(dbus_service_lavapipe PROPERTIES RESOURCE_LOCK wallpaperx_bus_tests)
    endif()

    # 77 means a goal could not be asked on this host: S6.12's unreadable-file clause under
    # root, or S6.24's content half on an export whose bytes are not CPU-addressable (this
    # GPU's OPAQUE_FD, a tiled modifier, a non-seekable or unmappable fd).
    set_tests_properties(dbus_service dbus_service_classic PROPERTIES SKIP_RETURN_CODE 77)
    if(WALLPAPERX_LAVAPIPE_ICD)
        set_tests_properties(dbus_service_lavapipe dbus_service_classic_lavapipe
                             PROPERTIES SKIP_RETURN_CODE 77)
    endif()

    # Step 7's HTTP config server. Needs a session bus for the same reason the entries
    # above do -- it compares /config against GetStatus, and GetStatus is only reachable
    # over the wire.
    wallpaperx_add_test(NAME http_config
             COMMAND $<TARGET_FILE:wallpaperx-http-server-test>)
    # 77 for the phases that cannot be exercised on a given host: no non-loopback
    # interface (S7.1), or no working Vulkan, which leaves the tier and render-window
    # goals unexercised. ctest turns that into SKIP; without it those runs would exit
    # non-zero and read as a failure of the HTTP server.
    set_tests_properties(http_config PROPERTIES
        ENVIRONMENT "WALLPAPERX_FORCE_TIER="
        RESOURCE_LOCK wallpaperx_bus_tests
        SKIP_RETURN_CODE 77)

    # The lavapipe twin exists for the sanitizer tier, not for ICD coverage: the HTTP
    # behaviour does not vary by driver, but this file is the only place httplib's worker
    # threads read WallpaperManager concurrently with a writer, and under TSan only
    # lavapipe-labelled entries run. Without this the torn-read phase -- the one written
    # against a concurrency defect -- is the one thing the race detector never sees.
    if(WALLPAPERX_LAVAPIPE_ICD)
        wallpaperx_add_test(NAME http_config_lavapipe
                 COMMAND $<TARGET_FILE:wallpaperx-http-server-test>)
        set_tests_properties(http_config_lavapipe PROPERTIES
            ENVIRONMENT "VK_ICD_FILENAMES=${WALLPAPERX_LAVAPIPE_ICD};WALLPAPERX_FORCE_TIER="
            LABELS "lavapipe"
            RESOURCE_LOCK wallpaperx_bus_tests
            SKIP_RETURN_CODE 77)
    endif()

    # Same private-bus wrapper, one tier up: this one spawns the real daemon binary.
    # Its own dbus-run-session, so the daemon it starts cannot collide with the
    # in-process service above under ctest -jN -- each gets a bus of its own, and both
    # request the same well-known name.
    wallpaperx_add_test(NAME daemon_system
             COMMAND ${_wx_asan_preload} $<TARGET_FILE:wallpaperx-daemon-system-test>)
    # SKIP_RETURN_CODE, for the same reason the renderer probes carry it: the test exits 77
    # when a goal could not be exercised at all on this host (no dbus-daemon, so no private
    # bus to destroy under a running daemon -- S6.35). Without it that run exits non-zero
    # and reads as a daemon failure; with it ctest says Skipped, which is neither a pass
    # nor an accusation. A run with real failures still returns 1 and is still red.
    set_tests_properties(daemon_system PROPERTIES RESOURCE_LOCK wallpaperx_bus_tests
                         SKIP_RETURN_CODE 77)

    # The twin reads the cross-process frame the other way: a LINEAR dma_buf, mapped, where
    # the NVIDIA entry above imports its OPAQUE_FD through the plugin's GL importer.
    if(WALLPAPERX_LAVAPIPE_ICD)
        wallpaperx_add_test(NAME daemon_system_lavapipe
                 COMMAND ${_wx_asan_preload} $<TARGET_FILE:wallpaperx-daemon-system-test>)
        set_tests_properties(daemon_system_lavapipe PROPERTIES
            ENVIRONMENT "VK_ICD_FILENAMES=${WALLPAPERX_LAVAPIPE_ICD}"
            LABELS "lavapipe"
            RESOURCE_LOCK wallpaperx_bus_tests
            SKIP_RETURN_CODE 77)
    endif()

    # Device loss, exit 75, and the restart that brings the wallpapers back. Private bus
    # and the shared lock for daemon_system's reasons; the binary also refuses any other bus.
    wallpaperx_add_test(NAME device_loss_restart
             COMMAND $<TARGET_FILE:wallpaperx-device-loss-restart-test>)
    set_tests_properties(device_loss_restart PROPERTIES RESOURCE_LOCK wallpaperx_bus_tests
                         SKIP_RETURN_CODE 77)
    if(WALLPAPERX_LAVAPIPE_ICD)
        wallpaperx_add_test(NAME device_loss_restart_lavapipe
                 COMMAND $<TARGET_FILE:wallpaperx-device-loss-restart-test>)
        set_tests_properties(device_loss_restart_lavapipe PROPERTIES
            ENVIRONMENT "VK_ICD_FILENAMES=${WALLPAPERX_LAVAPIPE_ICD}"
            LABELS "lavapipe"
            RESOURCE_LOCK wallpaperx_bus_tests
            SKIP_RETURN_CODE 77)
    endif()
    # The same binary's crash phases on their own: a crash's last lines through the log writer.
    wallpaperx_add_test(NAME crash_log_flush
             COMMAND $<TARGET_FILE:wallpaperx-device-loss-restart-test> crash-flush)
    set_tests_properties(crash_log_flush PROPERTIES RESOURCE_LOCK wallpaperx_bus_tests SKIP_RETURN_CODE 77)
    if(WALLPAPERX_LAVAPIPE_ICD)
        wallpaperx_add_test(NAME crash_log_flush_lavapipe
                 COMMAND $<TARGET_FILE:wallpaperx-device-loss-restart-test> crash-flush)
        set_tests_properties(crash_log_flush_lavapipe PROPERTIES
            ENVIRONMENT "VK_ICD_FILENAMES=${WALLPAPERX_LAVAPIPE_ICD}"
            LABELS "lavapipe"
            RESOURCE_LOCK wallpaperx_bus_tests
            SKIP_RETURN_CODE 77)
    endif()

    # Private bus and the shared lock for daemon_system's reasons: it starts the installed daemon.
    # The lock also keeps the two entries apart, since every install rewrites this tree's
    # install_manifest_daemon.txt and the test reads it back. The Lavapipe entry is what puts the
    # install and unit checks, which need no GPU, in continuous integration.
    wallpaperx_add_test(NAME daemon_install
             COMMAND $<TARGET_FILE:wallpaperx-daemon-install-test>)
    set_tests_properties(daemon_install PROPERTIES RESOURCE_LOCK wallpaperx_bus_tests SKIP_RETURN_CODE 77)
    if(WALLPAPERX_LAVAPIPE_ICD)
        wallpaperx_add_test(NAME daemon_install_lavapipe
                 COMMAND $<TARGET_FILE:wallpaperx-daemon-install-test>)
        set_tests_properties(daemon_install_lavapipe PROPERTIES
            ENVIRONMENT "VK_ICD_FILENAMES=${WALLPAPERX_LAVAPIPE_ICD}"
            LABELS "lavapipe"
            RESOURCE_LOCK wallpaperx_bus_tests
            SKIP_RETURN_CODE 77)
    endif()

    # Step 13 Stage E1: a `video` wallpaper drawn end to end. Private bus and the shared
    # RESOURCE_LOCK for the same reasons the entries above carry them -- it renders, and it
    # requests the same well-known name. SKIP_RETURN_CODE because two things it needs can be
    # absent: the ffmpeg command line that generates its fixture, and a host whose Vulkan
    # stack comes up at all; neither may read as a pass.
    wallpaperx_add_test(NAME video_wallpaper
             COMMAND $<TARGET_FILE:wallpaperx-video-wallpaper-test>)
    set_tests_properties(video_wallpaper PROPERTIES
        ENVIRONMENT "WALLPAPERX_FORCE_TIER="
        RESOURCE_LOCK wallpaperx_bus_tests
        SKIP_RETURN_CODE 77)

    # The lavapipe twin grades the other export path: a LINEAR dma_buf, whose pixels are mapped,
    # where this GPU's OPAQUE_FD export is read through the plugin's GL importer. It is also a
    # software decode, which is the other half of the decoder this daemon may be handed.
    if(WALLPAPERX_LAVAPIPE_ICD)
        wallpaperx_add_test(NAME video_wallpaper_lavapipe
                 COMMAND $<TARGET_FILE:wallpaperx-video-wallpaper-test>)
        set_tests_properties(video_wallpaper_lavapipe PROPERTIES
            ENVIRONMENT "VK_ICD_FILENAMES=${WALLPAPERX_LAVAPIPE_ICD};WALLPAPERX_FORCE_TIER="
            LABELS "lavapipe"
            RESOURCE_LOCK wallpaperx_bus_tests
            SKIP_RETURN_CODE 77)
    endif()

    # The QQuickItem driven by a stub daemon answering GetFrame with a frame the importer must
    # refuse. Here for the private bus: on the developer's real bus it would reach their daemon.
    if(WALLPAPERX_BUILD_KDE_PLUGIN AND WALLPAPERX_HAVE_FRAME_IMPORT)
        add_executable(wallpaperx-item-import-failure-test tests/item_import_failure_test.cpp)
        # The plugin is not linked: the item is reached the way Plasma reaches it, off the QML
        # path, so this also grades that the built module loads.
        target_link_libraries(wallpaperx-item-import-failure-test PRIVATE
            Qt6::Core Qt6::DBus Qt6::Gui Qt6::Qml Qt6::Quick)
        target_include_directories(wallpaperx-item-import-failure-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_definitions(wallpaperx-item-import-failure-test PRIVATE
            WALLPAPERX_QML_MODULE_PATH="${CMAKE_CURRENT_BINARY_DIR}/qml")
        add_dependencies(wallpaperx-item-import-failure-test wallpaperxplugin)
        # The display form: a nested headless KWin of the entry's own, never the desktop's, because
        # the item imports only under OpenGL and the offscreen platform has none.
        wallpaperx_add_display_test(NAME item_import_failure
                 COMMAND $<TARGET_FILE:wallpaperx-item-import-failure-test>)
        # A fallback chain, not offscreen alone: offscreen forces the software scene graph, which
        # refuses the RHI, and the item imports only under OpenGL -- so a host with no nested
        # compositor lands on offscreen and reports the named skip. Semicolons escaped; ctest splits
        # on bare ones. Nothing is displayed: a render-control window has no native window behind it.
        #
        # QT_NO_XDG_DESKTOP_PORTAL because Qt's theme asks the portal for a colour scheme, which on a
        # bus that can activate services starts one that inherits this test's stdout -- ctest then
        # never sees the pipe close. The entry's private bus has no service directories, so it
        # cannot activate the portal; this keeps Qt from asking at all.
        set_tests_properties(item_import_failure PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=wayland\;xcb\;offscreen;QT_NO_XDG_DESKTOP_PORTAL=1"
            LABELS "novulkan"
            RESOURCE_LOCK wallpaperx_bus_tests
            SKIP_RETURN_CODE 77)

        # The same item again, driven by a stub that only ever REFUSES. It lives in a window --
        # an item that is not on the screen does not fetch at all -- but under a render control
        # that is never initialised, so no scene-graph pass ever runs and no GL is needed. That is
        # why the offscreen platform is fine here where the entries above cannot use it.
        add_executable(wallpaperx-item-refusal-retry-test tests/item_refusal_retry_test.cpp)
        target_link_libraries(wallpaperx-item-refusal-retry-test PRIVATE
            Qt6::Core Qt6::DBus Qt6::Gui Qt6::Qml Qt6::Quick)
        target_include_directories(wallpaperx-item-refusal-retry-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_definitions(wallpaperx-item-refusal-retry-test PRIVATE
            WALLPAPERX_QML_MODULE_PATH="${CMAKE_CURRENT_BINARY_DIR}/qml")
        add_dependencies(wallpaperx-item-refusal-retry-test wallpaperxplugin)
        wallpaperx_add_test(NAME item_refusal_retry
                 COMMAND $<TARGET_FILE:wallpaperx-item-refusal-retry-test>)
        set_tests_properties(item_refusal_retry PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_NO_XDG_DESKTOP_PORTAL=1"
            LABELS "novulkan"
            RESOURCE_LOCK wallpaperx_bus_tests
            SKIP_RETURN_CODE 77)
        # The same binary on a bus of its own inside the entry's private one, whose policy refuses
        # GetNameOwner: what the item does when it cannot learn who owns the daemon's name. The nested
        # bus has no service directories either (deny_owner_lookup.conf.in includes the isolated one).
        pkg_check_modules(WALLPAPERX_TEST_LIBSYSTEMD REQUIRED IMPORTED_TARGET libsystemd)
        target_link_libraries(wallpaperx-item-refusal-retry-test PRIVATE PkgConfig::WALLPAPERX_TEST_LIBSYSTEMD)
        # libdbus, for the interposer that reads (and, in the slow-lookup runs, redirects) every owner
        # lookup of the daemon's name as QtDBus hands it over; exported so QtDBus's own call lands on it.
        pkg_check_modules(WALLPAPERX_TEST_DBUS1 REQUIRED IMPORTED_TARGET dbus-1)
        target_link_libraries(wallpaperx-item-refusal-retry-test PRIVATE PkgConfig::WALLPAPERX_TEST_DBUS1)
        target_link_options(wallpaperx-item-refusal-retry-test PRIVATE
            "LINKER:--export-dynamic-symbol=dbus_connection_send_with_reply")
        configure_file(tests/deny_owner_lookup.conf.in ${CMAKE_CURRENT_BINARY_DIR}/deny_owner_lookup.conf @ONLY)
        wallpaperx_add_test(NAME item_refusal_retry_lookup
                 COMMAND /bin/sh ${WALLPAPERX_ISOLATION_SUPPORT_DIR}/private_bus.sh ${WALLPAPERX_DBUS_RUN_SESSION}
                         ${CMAKE_CURRENT_BINARY_DIR}/deny_owner_lookup.conf --
                         $<TARGET_FILE:wallpaperx-item-refusal-retry-test> lookup-failed)
        set_tests_properties(item_refusal_retry_lookup PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_NO_XDG_DESKTOP_PORTAL=1"
            LABELS "novulkan"
            RESOURCE_LOCK wallpaperx_bus_tests
            SKIP_RETURN_CODE 77)
        # The same binary's scripted runs, each a process of its own: the plugin's daemon watch, its
        # one-line latch and its timeline record are process-wide.
        foreach(refusal_run slow-lookup slow-retry-lookup stale-refusal patience refusal-runs refusal-replaced
                            refusal-log refusal-away refusal-replaced-retry refusal-monitor)
            wallpaperx_add_test(NAME item_refusal_retry_${refusal_run}
                     COMMAND $<TARGET_FILE:wallpaperx-item-refusal-retry-test> ${refusal_run})
            set_tests_properties(item_refusal_retry_${refusal_run} PROPERTIES
                ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_NO_XDG_DESKTOP_PORTAL=1"
                LABELS "novulkan"
                RESOURCE_LOCK wallpaperx_bus_tests
                SKIP_RETURN_CODE 77)
        endforeach()

        # The same item again, driven by a stub that REFUSES the plugin's import-modifier report.
        # No Vulkan: the frame it serves is a memfd with synthesized fields, because what is under
        # test is what the plugin does with the daemon's answer, not what it does with the pixels.
        #
        # wallpaperx-frame-import is linked so the test can ask this host's EGL the same question
        # the plugin asks: a driver that publishes no importable layout makes the plugin decline to
        # report at all, by design, and without that gate the run would grade the host.
        add_executable(wallpaperx-item-modifier-report-test tests/item_modifier_report_test.cpp)
        target_link_libraries(wallpaperx-item-modifier-report-test PRIVATE
            wallpaperx-frame-import Qt6::Core Qt6::DBus Qt6::Gui Qt6::Qml Qt6::Quick)
        target_include_directories(wallpaperx-item-modifier-report-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_definitions(wallpaperx-item-modifier-report-test PRIVATE
            WALLPAPERX_QML_MODULE_PATH="${CMAKE_CURRENT_BINARY_DIR}/qml")
        add_dependencies(wallpaperx-item-modifier-report-test wallpaperxplugin)
        wallpaperx_add_display_test(NAME item_modifier_report
                 COMMAND $<TARGET_FILE:wallpaperx-item-modifier-report-test>)
        set_tests_properties(item_modifier_report PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=wayland\;xcb\;offscreen;QT_NO_XDG_DESKTOP_PORTAL=1"
            LABELS "novulkan"
            RESOURCE_LOCK wallpaperx_bus_tests
            SKIP_RETURN_CODE 77)

        # The same item again, driven by a stub that announces ImportModifiersChanged. Its own
        # binary rather than another phase of item_modifier_report: the plugin's report latch is
        # process-wide, so a run can be in the refused state or the accepted one, and the file
        # above ends with a withdrawal on every frame -- which would make "no further report
        # arrived" unaskable. No Vulkan, for the same reason as the file above.
        add_executable(wallpaperx-item-modifier-retry-test tests/item_modifier_retry_test.cpp)
        target_link_libraries(wallpaperx-item-modifier-retry-test PRIVATE
            wallpaperx-frame-import Qt6::Core Qt6::DBus Qt6::Gui Qt6::Qml Qt6::Quick)
        target_include_directories(wallpaperx-item-modifier-retry-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_definitions(wallpaperx-item-modifier-retry-test PRIVATE
            WALLPAPERX_QML_MODULE_PATH="${CMAKE_CURRENT_BINARY_DIR}/qml")
        add_dependencies(wallpaperx-item-modifier-retry-test wallpaperxplugin)
        wallpaperx_add_display_test(NAME item_modifier_retry
                 COMMAND $<TARGET_FILE:wallpaperx-item-modifier-retry-test>)
        set_tests_properties(item_modifier_retry PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=wayland\;xcb\;offscreen;QT_NO_XDG_DESKTOP_PORTAL=1"
            LABELS "novulkan"
            RESOURCE_LOCK wallpaperx_bus_tests
            SKIP_RETURN_CODE 77)

        # The same item, driven by a stub that records SetOutputSize. The binary configures the
        # offscreen platform itself with two screens at different scales, which is the only way to
        # make a scale change headless, so it needs no GL and no display.
        add_executable(wallpaperx-item-size-report-test tests/item_size_report_test.cpp)
        target_link_libraries(wallpaperx-item-size-report-test PRIVATE
            Qt6::Core Qt6::DBus Qt6::Gui Qt6::Qml Qt6::Quick)
        target_include_directories(wallpaperx-item-size-report-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_definitions(wallpaperx-item-size-report-test PRIVATE
            WALLPAPERX_QML_MODULE_PATH="${CMAKE_CURRENT_BINARY_DIR}/qml")
        add_dependencies(wallpaperx-item-size-report-test wallpaperxplugin)
        wallpaperx_add_test(NAME item_size_report
                 COMMAND $<TARGET_FILE:wallpaperx-item-size-report-test>)
        set_tests_properties(item_size_report PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_NO_XDG_DESKTOP_PORTAL=1"
            LABELS "novulkan"
            RESOURCE_LOCK wallpaperx_bus_tests
            SKIP_RETURN_CODE 77)

        # Stand-ins for Plasma's task manager and KWindowSystem QML modules, for the tests below that
        # load ScreenWindows.qml with no compositor: they serve the rows a test sets and record what
        # the file asked for. Built into a directory of their own that only those tests put on their
        # import path; never installed.
        set(WALLPAPERX_STANDIN_QML_DIR ${CMAKE_CURRENT_BINARY_DIR}/test_qml)
        qt_add_qml_module(wallpaperx_taskmanager_standin
            URI org.kde.taskmanager
            VERSION 1.0
            PLUGIN_TARGET wallpaperx_taskmanager_standin
            NO_PLUGIN_OPTIONAL
            SOURCES tests/qml_standins/taskmanager_standin.h tests/qml_standins/taskmanager_standin.cpp
            OUTPUT_DIRECTORY ${WALLPAPERX_STANDIN_QML_DIR}/org/kde/taskmanager)
        qt_add_qml_module(wallpaperx_kwindowsystem_standin
            URI org.kde.kwindowsystem
            VERSION 1.0
            PLUGIN_TARGET wallpaperx_kwindowsystem_standin
            NO_PLUGIN_OPTIONAL
            SOURCES tests/qml_standins/kwindowsystem_standin.h
            OUTPUT_DIRECTORY ${WALLPAPERX_STANDIN_QML_DIR}/org/kde/kwindowsystem)
        foreach(standin wallpaperx_taskmanager_standin wallpaperx_kwindowsystem_standin)
            # lib<plugin>.so is the name the QML engine looks for, as with wallpaperxplugin.
            set_target_properties(${standin} PROPERTIES PREFIX "lib")
            target_link_libraries(${standin} PRIVATE Qt6::Core Qt6::Qml)
            # The generated type registration includes the headers by bare name.
            target_include_directories(${standin} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests/qml_standins)
        endforeach()

        # The window report (SetOutputFocus) and the immediate reports at setOutput, from the item's
        # side, against tests/report_stub.h. Offscreen and no GL, like item_size_report.
        add_executable(wallpaperx-item-window-report-test tests/item_window_report_test.cpp)
        target_link_libraries(wallpaperx-item-window-report-test PRIVATE
            Qt6::Core Qt6::DBus Qt6::Gui Qt6::Qml Qt6::Quick)
        target_include_directories(wallpaperx-item-window-report-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_definitions(wallpaperx-item-window-report-test PRIVATE
            WALLPAPERX_QML_MODULE_PATH="${CMAKE_CURRENT_BINARY_DIR}/qml")
        add_dependencies(wallpaperx-item-window-report-test wallpaperxplugin)
        wallpaperx_add_test(NAME item_window_report COMMAND $<TARGET_FILE:wallpaperx-item-window-report-test>)
        # The same binary with no session bus, a process of its own: QtDBus connects once per process.
        wallpaperx_add_test(NAME item_window_report_no_bus
                 COMMAND $<TARGET_FILE:wallpaperx-item-window-report-test> no-bus)
        set_tests_properties(item_window_report item_window_report_no_bus PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_NO_XDG_DESKTOP_PORTAL=1"
            LABELS "novulkan"
            RESOURCE_LOCK wallpaperx_bus_tests
            SKIP_RETURN_CODE 77)

        # The packaged QML around the renderer, run for real against a stub daemon on both channels.
        # Binds 127.0.0.1:18462, main.qml's port, hence the shared lock.
        add_executable(wallpaperx-plasma-surface-test tests/plasma_surface_test.cpp)
        target_link_libraries(wallpaperx-plasma-surface-test PRIVATE
            Qt6::Core Qt6::DBus Qt6::Gui Qt6::Qml Qt6::Quick Threads::Threads)
        target_include_directories(wallpaperx-plasma-surface-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${HTTPLIB_INCLUDE_DIR})
        target_compile_definitions(wallpaperx-plasma-surface-test PRIVATE
            WALLPAPERX_QML_MODULE_PATH="${CMAKE_CURRENT_BINARY_DIR}/qml"
            WALLPAPERX_STANDIN_QML_PATH="${WALLPAPERX_STANDIN_QML_DIR}"
            WALLPAPERX_BACKEND_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        add_dependencies(wallpaperx-plasma-surface-test wallpaperxplugin
                         wallpaperx_taskmanager_standin wallpaperx_kwindowsystem_standin)
        wallpaperx_add_test(NAME plasma_surface COMMAND $<TARGET_FILE:wallpaperx-plasma-surface-test>)
        # The same binary's config-named run, a process of its own: the change watch's first owner
        # lookup answers once per process.
        wallpaperx_add_test(NAME plasma_surface_named COMMAND $<TARGET_FILE:wallpaperx-plasma-surface-test> config-named)
        # The failed-read retry, a process of its own so its stub can close and reopen the port; and
        # with no session bus, which QtDBus decides once per process.
        wallpaperx_add_test(NAME plasma_surface_retry COMMAND $<TARGET_FILE:wallpaperx-plasma-surface-test> config-retry)
        wallpaperx_add_test(NAME plasma_surface_retry_no_bus
                 COMMAND $<TARGET_FILE:wallpaperx-plasma-surface-test> config-retry-no-bus)
        set_tests_properties(plasma_surface plasma_surface_named plasma_surface_retry plasma_surface_retry_no_bus PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_NO_XDG_DESKTOP_PORTAL=1"
            LABELS "novulkan"
            RESOURCE_LOCK wallpaperx_bus_tests
            SKIP_RETURN_CODE 77)

        # ScreenWindows.qml against Plasma's real task manager model, on the nested KWin the display
        # form starts -- with two outputs, and with KWin's permission checks off so this process may
        # read the window list as plasmashell does. Real windows come from a client process.
        add_executable(wallpaperx-screen-windows-kwin-test tests/screen_windows_kwin_test.cpp)
        target_link_libraries(wallpaperx-screen-windows-kwin-test PRIVATE
            Qt6::Core Qt6::DBus Qt6::Gui Qt6::Qml Qt6::Quick)
        target_include_directories(wallpaperx-screen-windows-kwin-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_definitions(wallpaperx-screen-windows-kwin-test PRIVATE
            WALLPAPERX_QML_MODULE_PATH="${CMAKE_CURRENT_BINARY_DIR}/qml"
            WALLPAPERX_BACKEND_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        add_dependencies(wallpaperx-screen-windows-kwin-test wallpaperxplugin)
        wallpaperx_add_display_test(NAME screen_windows_kwin COMMAND $<TARGET_FILE:wallpaperx-screen-windows-kwin-test>)
        set_tests_properties(screen_windows_kwin PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=wayland;KWIN_WAYLAND_NO_PERMISSION_CHECKS=1;WALLPAPERX_TEST_KWIN_OUTPUTS=2;QT_NO_XDG_DESKTOP_PORTAL=1"
            LABELS "novulkan"
            RESOURCE_LOCK wallpaperx_bus_tests
            SKIP_RETURN_CODE 77)

        # The import entries skip when their GL is not on the exporting GPU; this one fails, so a
        # nested compositor that moved clients off the real GPU cannot pass the suite quietly. No
        # label: it needs a hardware GPU, and skips on a host whose Vulkan has only CPU devices.
        add_executable(wallpaperx-display-gl-device-test tests/display_gl_device_test.cpp)
        target_link_libraries(wallpaperx-display-gl-device-test PRIVATE Qt6::Gui Vulkan::Vulkan PkgConfig::EGL OpenGL::GL)
        target_include_directories(wallpaperx-display-gl-device-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        wallpaperx_add_display_test(NAME display_gl_device COMMAND $<TARGET_FILE:wallpaperx-display-gl-device-test>)
        set_tests_properties(display_gl_device PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=wayland\;xcb\;offscreen;QT_NO_XDG_DESKTOP_PORTAL=1"
            SKIP_RETURN_CODE 77)

        # ScreenWindows.qml's own logic, in Qt Quick Test against the stand-ins. Needs no bus and no
        # port; registered here only because the stand-ins are built here.
        if(Qt6QuickTest_FOUND)
            add_executable(wallpaperx-qml-screen-windows-test tests/qml_screen_windows_test.cpp)
            target_link_libraries(wallpaperx-qml-screen-windows-test PRIVATE Qt6::QuickTest Qt6::Qml)
            target_compile_definitions(wallpaperx-qml-screen-windows-test PRIVATE
                QUICK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/qml_windows"
                WALLPAPERX_STANDIN_QML_PATH="${WALLPAPERX_STANDIN_QML_DIR}")
            add_dependencies(wallpaperx-qml-screen-windows-test
                             wallpaperx_taskmanager_standin wallpaperx_kwindowsystem_standin)
            wallpaperx_add_test(NAME qml_screen_windows COMMAND $<TARGET_FILE:wallpaperx-qml-screen-windows-test>)
            # A wide screen, not the offscreen platform's square default, which a transposed
            # geometry would pass.
            set_tests_properties(qml_screen_windows PROPERTIES
                ENVIRONMENT "QT_QPA_PLATFORM=offscreen:configfile=${CMAKE_CURRENT_SOURCE_DIR}/tests/qml_windows/offscreen_screens.json"
                LABELS "novulkan;no-tsan")
        endif()

        # The same item, driven by a stub that serves a frame the importer ACCEPTS -- a real
        # Vulkan export -- so that what the scene graph does with the imported GL objects when it
        # drops them is observable. Needs Vulkan as well as OpenGL, and needs both to resolve to
        # the SAME device: an opaque fd is importable only by the driver that produced it, so this
        # reports a named skip on a host where they differ rather than a pass.
        #
        # No LABELS, for the reason the renderer's GPU probes carry none: it cannot run on the
        # hosted CI runner, which has no Vulkan device that a GL context can be paired with.
        #
        # QuickPrivate is asked for by name because Qt only loads its private module packages on
        # request. It is needed for QQuickItemPrivate::paintNode, the only route to the QSGTexture
        # an item handed the scene graph -- Qt's private API, deliberately, so that the plugin
        # never grows a test-only accessor for it.
        find_package(Qt6 QUIET COMPONENTS QuickPrivate)
        if(Qt6QuickPrivate_FOUND)
            add_executable(wallpaperx-item-import-release-test tests/item_import_release_test.cpp)
            # wallpaperx-frame-import so the test can ask whether THIS host can import at all:
            # without it, a plugin that stopped importing and a host that never could are the
            # same observation, and the run skips green for both.
            target_link_libraries(wallpaperx-item-import-release-test PRIVATE
                wallpaperx-renderer wallpaperx-frame-import
                Qt6::Core Qt6::DBus Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickPrivate
                PkgConfig::EGL OpenGL::GL)
            target_include_directories(wallpaperx-item-import-release-test PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../renderer/tests)
            # The shader directory and an asset directory of their own: both binaries now RENDER
            # into the target their stub exports, because an acquire descriptor is a hand-over of a
            # published frame and exportFrameAcquireFd refuses a target that published nothing.
            # They share this directory and write differently-named fixtures into it; both entries
            # hold the wallpaperx_bus_tests lock, so neither can be mid-write while the other reads.
            set(WALLPAPERX_ITEM_ASSET_DIR ${CMAKE_CURRENT_BINARY_DIR}/item_scene_assets)
            file(MAKE_DIRECTORY ${WALLPAPERX_ITEM_ASSET_DIR})
            target_compile_definitions(wallpaperx-item-import-release-test PRIVATE
                WALLPAPERX_QML_MODULE_PATH="${CMAKE_CURRENT_BINARY_DIR}/qml"
                WALLPAPERX_SHADER_DIR="${WALLPAPERX_SHADER_OUTPUT_DIR}"
                WALLPAPERX_TEST_IMAGE_DIR="${WALLPAPERX_ITEM_ASSET_DIR}")
            add_dependencies(wallpaperx-item-import-release-test wallpaperxplugin wallpaperx-shaders)
            wallpaperx_add_display_test(NAME item_import_release
                     COMMAND $<TARGET_FILE:wallpaperx-item-import-release-test>)
            set_tests_properties(item_import_release PROPERTIES
                ENVIRONMENT "QT_QPA_PLATFORM=wayland\;xcb\;offscreen;QT_NO_XDG_DESKTOP_PORTAL=1"
                RESOURCE_LOCK wallpaperx_bus_tests
                SKIP_RETURN_CODE 77)

            # The same scene again, for what the item TELLS the desktop rather than for what it
            # frees: the claim that a frame is on screen must be withdrawn when the scene graph
            # takes the node away, and must survive a monitor-id change that only invalidates the
            # staleness marks. Its own binary rather than another phase of the one above, because
            # grading the first direction means detaching the item from its window -- which
            # destroys the node and texture that file's whole check is about.
            add_executable(wallpaperx-item-frame-shown-test tests/item_frame_shown_test.cpp)
            target_link_libraries(wallpaperx-item-frame-shown-test PRIVATE
                wallpaperx-renderer wallpaperx-frame-import
                Qt6::Core Qt6::DBus Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickPrivate
                PkgConfig::EGL OpenGL::GL)
            target_include_directories(wallpaperx-item-frame-shown-test PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../renderer/tests)
            target_compile_definitions(wallpaperx-item-frame-shown-test PRIVATE
                WALLPAPERX_QML_MODULE_PATH="${CMAKE_CURRENT_BINARY_DIR}/qml"
                WALLPAPERX_SHADER_DIR="${WALLPAPERX_SHADER_OUTPUT_DIR}"
                WALLPAPERX_TEST_IMAGE_DIR="${WALLPAPERX_ITEM_ASSET_DIR}")
            add_dependencies(wallpaperx-item-frame-shown-test wallpaperxplugin wallpaperx-shaders)
            wallpaperx_add_display_test(NAME item_frame_shown
                     COMMAND $<TARGET_FILE:wallpaperx-item-frame-shown-test>)
            set_tests_properties(item_frame_shown PROPERTIES
                ENVIRONMENT "QT_QPA_PLATFORM=wayland\;xcb\;offscreen;QT_NO_XDG_DESKTOP_PORTAL=1"
                RESOURCE_LOCK wallpaperx_bus_tests
                SKIP_RETURN_CODE 77)

            # The same scene once more, for what the item TELLS THE DAEMON when it lets an image
            # go. Three entries from one binary, each naming its phase, because the ledger the
            # plugin keeps is a process-wide singleton with no reset and the daemon-restart phase
            # has to be the first bus-name owner change its process ever sees -- phases sharing a
            # process would carry each other's state.
            add_executable(wallpaperx-item-release-ledger-test tests/item_release_ledger_test.cpp)
            target_link_libraries(wallpaperx-item-release-ledger-test PRIVATE
                wallpaperx-renderer wallpaperx-frame-import
                Qt6::Core Qt6::DBus Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickPrivate
                PkgConfig::EGL OpenGL::GL)
            target_include_directories(wallpaperx-item-release-ledger-test PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../renderer/tests)
            target_compile_definitions(wallpaperx-item-release-ledger-test PRIVATE
                WALLPAPERX_QML_MODULE_PATH="${CMAKE_CURRENT_BINARY_DIR}/qml"
                WALLPAPERX_SHADER_DIR="${WALLPAPERX_SHADER_OUTPUT_DIR}"
                WALLPAPERX_TEST_IMAGE_DIR="${WALLPAPERX_ITEM_ASSET_DIR}")
            add_dependencies(wallpaperx-item-release-ledger-test wallpaperxplugin wallpaperx-shaders)
            # late-reply-alive and late-reply-replaced wait out the plugin's 25 s fetch patience, so each
            # takes about 30 s.
            foreach(release_phase repeat-import repeat-many dropped-frame in-flight-monitor-change daemon-restart
                    late-reply late-reply-alive late-reply-replaced)
                wallpaperx_add_display_test(NAME item_release_${release_phase}
                         COMMAND $<TARGET_FILE:wallpaperx-item-release-ledger-test> ${release_phase})
                set_tests_properties(item_release_${release_phase} PROPERTIES
                    ENVIRONMENT "QT_QPA_PLATFORM=wayland\;xcb\;offscreen;QT_NO_XDG_DESKTOP_PORTAL=1"
                    RESOURCE_LOCK wallpaperx_bus_tests
                    SKIP_RETURN_CODE 77)
            endforeach()

            # The same scene once more, for what the item does when it STOPS BEING ON THE SCREEN
            # and for what it tells the daemon when the desktop stops drawing what it hands over.
            # Two entries from one binary, each naming its phase, for item_release_ledger's reason:
            # the plugin's release ledger and its record of which daemon it is talking to are
            # process-wide singletons with no reset.
            add_executable(wallpaperx-item-offscreen-test tests/item_offscreen_test.cpp)
            target_link_libraries(wallpaperx-item-offscreen-test PRIVATE
                wallpaperx-renderer wallpaperx-frame-import
                Qt6::Core Qt6::DBus Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickPrivate
                PkgConfig::EGL OpenGL::GL)
            target_include_directories(wallpaperx-item-offscreen-test PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../renderer/tests)
            target_compile_definitions(wallpaperx-item-offscreen-test PRIVATE
                WALLPAPERX_QML_MODULE_PATH="${CMAKE_CURRENT_BINARY_DIR}/qml"
                WALLPAPERX_SHADER_DIR="${WALLPAPERX_SHADER_OUTPUT_DIR}"
                WALLPAPERX_TEST_IMAGE_DIR="${WALLPAPERX_ITEM_ASSET_DIR}")
            add_dependencies(wallpaperx-item-offscreen-test wallpaperxplugin wallpaperx-shaders)
            # Step 13 Stage F: what the item keeps imported, and that keeping it changes nothing it
            # draws or tells the daemon. One entry per phase, for item_release_ledger's reason. The
            # modifier phase forces a dma_buf export, the only kind with a layout to withdraw.
            add_executable(wallpaperx-item-import-cache-test tests/item_import_cache_test.cpp)
            target_link_libraries(wallpaperx-item-import-cache-test PRIVATE
                wallpaperx-renderer wallpaperx-frame-import wallpaperx-log-escape
                Qt6::Core Qt6::DBus Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickPrivate
                PkgConfig::EGL OpenGL::GL)
            target_include_directories(wallpaperx-item-import-cache-test PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../renderer/tests)
            target_compile_definitions(wallpaperx-item-import-cache-test PRIVATE
                WALLPAPERX_QML_MODULE_PATH="${CMAKE_CURRENT_BINARY_DIR}/qml"
                WALLPAPERX_SHADER_DIR="${WALLPAPERX_SHADER_OUTPUT_DIR}"
                WALLPAPERX_TEST_IMAGE_DIR="${WALLPAPERX_ITEM_ASSET_DIR}")
            add_dependencies(wallpaperx-item-import-cache-test wallpaperxplugin wallpaperx-shaders)
            # tests/gl_semaphore_fault.h defines eglGetProcAddress in the executable to fail a GL
            # semaphore wait on demand; exported, or the plugin -- loaded at runtime -- binds libEGL's.
            target_link_options(wallpaperx-item-import-cache-test PRIVATE
                "LINKER:--export-dynamic-symbol=eglGetProcAddress")
            # tests/bus_monitor.h watches the private bus through sd-bus, the one view of where a call
            # was addressed.
            pkg_check_modules(WALLPAPERX_TEST_LIBSYSTEMD REQUIRED IMPORTED_TARGET libsystemd)
            target_link_libraries(wallpaperx-item-import-cache-test PRIVATE PkgConfig::WALLPAPERX_TEST_LIBSYSTEMD)
            foreach(cache_phase counts single pixels no-acquire handover handover-reset handover-modifier
                                stale-daemon stale-monitor offscreen publish not-opengl descriptor-backoff
                                declare declare-refused declare-monitor declare-daemon unique-name lost-import
                                protocol restart-pending pending-monitor pending-offscreen no-owner no-item
                                asking-monitor startup-no-daemon late-acquire newer-reply newer-failure)
                set(cache_environment "QT_QPA_PLATFORM=wayland\;xcb\;offscreen;QT_NO_XDG_DESKTOP_PORTAL=1")
                if(cache_phase STREQUAL "handover-modifier")
                    set(cache_environment "${cache_environment};WALLPAPERX_FORCE_HANDLE_TYPE=dmabuf")
                endif()
                # A real window, so never on a real desktop's platform, and it needs no OpenGL.
                if(cache_phase STREQUAL "not-opengl")
                    set(cache_environment "QT_QPA_PLATFORM=offscreen;QT_NO_XDG_DESKTOP_PORTAL=1")
                    wallpaperx_add_test(NAME item_cache_${cache_phase}
                             COMMAND $<TARGET_FILE:wallpaperx-item-import-cache-test> ${cache_phase})
                else()
                    wallpaperx_add_display_test(NAME item_cache_${cache_phase}
                             COMMAND $<TARGET_FILE:wallpaperx-item-import-cache-test> ${cache_phase})
                endif()
                set_tests_properties(item_cache_${cache_phase} PROPERTIES
                    ENVIRONMENT "${cache_environment}"
                    RESOURCE_LOCK wallpaperx_bus_tests
                    SKIP_RETURN_CODE 77)
            endforeach()

            # The acquire timeline, from the plugin's side: one entry per phase, for
            # item_release_ledger's reason -- the plugin's timeline imports and its refused-import
            # flag are process-wide. Needs GL_NV_timeline_semaphore, so every phase but three
            # reports NOT VERIFIED on Mesa.
            add_executable(wallpaperx-item-timeline-test tests/item_timeline_test.cpp)
            target_link_libraries(wallpaperx-item-timeline-test PRIVATE
                wallpaperx-renderer wallpaperx-frame-import
                Qt6::Core Qt6::DBus Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickPrivate
                PkgConfig::EGL OpenGL::GL)
            target_include_directories(wallpaperx-item-timeline-test PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../renderer/tests)
            target_compile_definitions(wallpaperx-item-timeline-test PRIVATE
                WALLPAPERX_QML_MODULE_PATH="${CMAKE_CURRENT_BINARY_DIR}/qml"
                WALLPAPERX_SHADER_DIR="${WALLPAPERX_SHADER_OUTPUT_DIR}"
                WALLPAPERX_TEST_IMAGE_DIR="${WALLPAPERX_ITEM_ASSET_DIR}")
            target_link_options(wallpaperx-item-timeline-test PRIVATE
                "LINKER:--export-dynamic-symbol=eglGetProcAddress"
                "LINKER:--export-dynamic-symbol=glGetStringi")
            add_dependencies(wallpaperx-item-timeline-test wallpaperxplugin wallpaperx-shaders)
            foreach(timeline_phase steady old-daemon unsupported unknown-interface too-many-copies retry-busy retry-exportfailed
                                   lost-reply refused-timeline refused-binary refused-shared wait-no-trip setup-create setup-type
                                   stray-value restart restart-memory stale-value context-move late-value
                                   late-answer replaced-asking retry-replaced rebuild context-share shared-replaced
                                   shared-threads shared-stuck)
                wallpaperx_add_display_test(NAME item_timeline_${timeline_phase}
                         COMMAND $<TARGET_FILE:wallpaperx-item-timeline-test> ${timeline_phase})
                set_tests_properties(item_timeline_${timeline_phase} PROPERTIES
                    ENVIRONMENT "QT_QPA_PLATFORM=wayland\;xcb\;offscreen;QT_NO_XDG_DESKTOP_PORTAL=1"
                    RESOURCE_LOCK wallpaperx_bus_tests
                    SKIP_RETURN_CODE 77)
            endforeach()
            # The steady phase again on the dma_buf export, the other handle type the plugin waits on.
            wallpaperx_add_display_test(NAME item_timeline_steady-dmabuf
                     COMMAND $<TARGET_FILE:wallpaperx-item-timeline-test> steady)
            set_tests_properties(item_timeline_steady-dmabuf PROPERTIES
                ENVIRONMENT "QT_QPA_PLATFORM=wayland\;xcb\;offscreen;QT_NO_XDG_DESKTOP_PORTAL=1;WALLPAPERX_FORCE_HANDLE_TYPE=dmabuf"
                RESOURCE_LOCK wallpaperx_bus_tests
                SKIP_RETURN_CODE 77)

            foreach(offscreen_phase hidden painted)
                wallpaperx_add_display_test(NAME item_offscreen_${offscreen_phase}
                         COMMAND $<TARGET_FILE:wallpaperx-item-offscreen-test> ${offscreen_phase})
                set_tests_properties(item_offscreen_${offscreen_phase} PROPERTIES
                    ENVIRONMENT "QT_QPA_PLATFORM=wayland\;xcb\;offscreen;QT_NO_XDG_DESKTOP_PORTAL=1"
                    RESOURCE_LOCK wallpaperx_bus_tests
                    SKIP_RETURN_CODE 77)
            endforeach()
        else()
            # Registered anyway, as an entry that always skips. An unregistered test is one
            # nobody runs and nobody misses: ctest prints a full green run with no line for it,
            # which reads exactly like a passing check. The same reasoning registers
            # frame_import_probe unconditionally above.
            wallpaperx_add_test(NAME item_import_release
                     COMMAND ${CMAKE_COMMAND} -E echo
                     "item_import_release: NOT VERIFIED -- Qt6 QuickPrivate headers were not found at configure "
                     "time, so this check was not built. It is the only check that the GL objects imported from a "
                     "daemon frame are freed by the scene graph's own teardown, on its thread with its context "
                     "current; without it a build that leaks a screen's worth of GPU memory per unplugged monitor "
                     "is green. Install the Qt6 Quick private headers and re-configure.")
            set_tests_properties(item_import_release PROPERTIES SKIP_REGULAR_EXPRESSION "NOT VERIFIED")
            wallpaperx_add_test(NAME item_frame_shown
                     COMMAND ${CMAKE_COMMAND} -E echo
                     "item_frame_shown: NOT VERIFIED -- Qt6 QuickPrivate headers were not found at configure time, "
                     "so this check was not built. It is the only check that the plugin stops telling the desktop a "
                     "wallpaper is on screen once the scene graph has taken the node away; without it a build that "
                     "leaves a black desktop with no message on it is green. Install the Qt6 Quick private headers "
                     "and re-configure.")
            set_tests_properties(item_frame_shown PROPERTIES SKIP_REGULAR_EXPRESSION "NOT VERIFIED")
            foreach(release_phase repeat-import repeat-many dropped-frame in-flight-monitor-change daemon-restart
                    late-reply late-reply-alive late-reply-replaced)
                wallpaperx_add_test(NAME item_release_${release_phase}
                         COMMAND ${CMAKE_COMMAND} -E echo
                         "item_release_${release_phase}: NOT VERIFIED -- Qt6 QuickPrivate headers were not found at "
                         "configure time, so this check was not built. It is part of the only coverage that the "
                         "plugin tells the daemon when it stops reading an image; without it a build that never "
                         "releases -- which permanently costs every monitor two of its three images -- is green. "
                         "Install the Qt6 Quick private headers and re-configure.")
                set_tests_properties(item_release_${release_phase} PROPERTIES
                    SKIP_REGULAR_EXPRESSION "NOT VERIFIED")
            endforeach()
            message(WARNING
                "Qt6 QuickPrivate headers not found -- item_import_release and item_frame_shown are registered "
                "as permanent SKIPs rather than built. Those are the only checks that the GL objects imported "
                "from a daemon frame are freed by the scene graph's own teardown, and that the desktop says so "
                "when nothing is drawn rather than going black in silence.")
        endif()
    endif()

    # The cross-process half of the import path, and the only entry that has it: the real daemon
    # binary exports a frame in its own process, and THIS process imports it into a GL context and
    # reads the pixels back. frame_import_probe performs a genuine export and import but inside one
    # process, so no descriptor ever crosses a boundary; daemon_system_test crosses it but can only
    # read the pixels of a LINEAR dma_buf, which is precisely the path a real GPU here does not use.
    #
    # No LABELS, for the reason the renderer's GPU probes carry none: the hosted CI runner has no
    # GPU, and this needs a Vulkan device and a GL context that resolve to the same one. That also
    # makes the ThreadSanitizer block below DISABLE it, which is right -- the proprietary driver
    # SEGVs inside its own uninstrumented code there.
    if(WALLPAPERX_HAVE_FRAME_IMPORT)
        add_executable(wallpaperx-cross-process-import-test tests/cross_process_import_test.cpp)
        target_link_libraries(wallpaperx-cross-process-import-test PRIVATE
            wallpaperx-daemon-lib wallpaperx-frame-import)
        target_include_directories(wallpaperx-cross-process-import-test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../renderer/tests)
        # Its own asset directory. The fixture is written by this test and read by the daemon it
        # spawns, and a directory shared with another entry is one another entry can be mid-write in.
        set(WALLPAPERX_XPROC_ASSET_DIR ${CMAKE_CURRENT_BINARY_DIR}/cross_process_assets)
        file(MAKE_DIRECTORY ${WALLPAPERX_XPROC_ASSET_DIR})
        target_compile_definitions(wallpaperx-cross-process-import-test PRIVATE
            WALLPAPERX_TEST_IMAGE_DIR="${WALLPAPERX_XPROC_ASSET_DIR}"
            WALLPAPERX_DAEMON_BINARY="$<TARGET_FILE:wallpaperx-daemon>")
        # Baking the path in is not enough: without this the test can be built and run against a
        # stale daemon binary, and would report an old daemon's behaviour as the current one's.
        add_dependencies(wallpaperx-cross-process-import-test wallpaperx-daemon)
        if(WALLPAPERX_BUILD_KDE_PLUGIN)
            set_target_properties(wallpaperx-cross-process-import-test PROPERTIES
                AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
        endif()
        wallpaperx_add_test(NAME cross_process_import
                 COMMAND $<TARGET_FILE:wallpaperx-cross-process-import-test>)
        # SKIP_RETURN_CODE because a host whose GL and Vulkan resolve to different drivers, or whose
        # daemon exports a dma_buf, has not exercised this and must not report that it has.
        set_tests_properties(cross_process_import PROPERTIES
            RESOURCE_LOCK wallpaperx_bus_tests
            SKIP_RETURN_CODE 77)
    else()
        # Registered as a permanent skip rather than left absent, for the reason above: a missing
        # entry reads as a green run, and this is the only check that an fd exported by one
        # process imports as correct pixels in another.
        wallpaperx_add_test(NAME cross_process_import
                 COMMAND ${CMAKE_COMMAND} -E echo
                 "cross_process_import: NOT VERIFIED -- the frame importer was not built (EGL/OpenGL missing at "
                 "configure time), so nothing checked that an opaque fd exported by one process imports as "
                 "correct pixels in another.")
        set_tests_properties(cross_process_import PROPERTIES SKIP_REGULAR_EXPRESSION "NOT VERIFIED")
    endif()
else()
    wallpaperx_tests_not_registered(
        "dbus-run-session not found -- test_isolation's bus check reports not verified, and ALL bus-driven "
        "tests are unregistered: dbus_service, "
        "dbus_service_classic, dbus_service_lavapipe, dbus_service_classic_lavapipe, http_config, "
        "http_config_lavapipe, daemon_system, "
        "daemon_system_lavapipe, item_import_failure, item_import_release, item_frame_shown, "
        "cross_process_import. That "
        "removes the only coverage of the DBus wire contract, the DMA_BUF metadata, the HTTP config "
        "server, the Classic "
        "descriptor tier through the daemon, the release of GL objects imported from a daemon "
        "frame by the scene graph's own teardown, the only check that the plugin stops claiming a "
        "wallpaper is on screen once the scene graph has taken its node away, the only check that an opaque fd exported by "
        "one process imports as correct pixels in another, and the daemon's "
        "process-level behaviour (name acquisition, second-instance refusal, SIGTERM/SIGINT "
        "shutdown). A green ctest run without them means much less "
        "than it looks like. Install dbus.")
endif()

# The wrapper driven from outside with a made-up caller: a port bound inside an entry, TMPDIR in its
# home or runtime directory, home-rooted search paths and Vulkan layers, an inherited platform, a
# compositor that does not start, a test killed by a signal, a wrapper killed outright; and, on a
# private bus, what a run leaves in /tmp and the paths it accepts.
wallpaperx_add_isolation_wrapper_tests()

# Read from inside an entry registered like every other: the private HOME, XDG directories, TMPDIR
# and bus, and no display (test_isolation) or only the nested compositor's (test_isolation_display).
# Registered with or without dbus-run-session: everything but the bus check needs no bus, and with
# none the bus check reports not verified.
add_executable(wallpaperx-isolation-test tests/isolation_test.cpp)
target_link_libraries(wallpaperx-isolation-test PRIVATE SDBusCpp::sdbus-c++)
target_include_directories(wallpaperx-isolation-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(wallpaperx-isolation-test PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
wallpaperx_add_test(NAME test_isolation COMMAND wallpaperx-isolation-test bus)
wallpaperx_add_display_test(NAME test_isolation_display COMMAND wallpaperx-isolation-test display)
set_tests_properties(test_isolation test_isolation_display PROPERTIES LABELS "novulkan" SKIP_RETURN_CODE 77)

# That every entry in this tree, the renderer's included, runs through isolated_run.sh, and that every
# one asking Qt for a display platform gets the nested compositor -- read from ctest's own listing,
# so an entry registered with a bare add_test() is caught however it got there. Last: its floor is
# what this configure registered before it (isolated_test.cmake).
wallpaperx_add_isolation_registry()

# --------------------------------------------------------------------------- #
# ThreadSanitizer tier
# --------------------------------------------------------------------------- #
#
# Without this block a bare `ctest --test-dir backend/build-tsan` is worse than useless.
# It needs two environment variables that nothing set, so every run drowned in validation
# -layer noise and reported the loader artifact as a failure -- meaning a *correct* run
# was red. A gate that is red when correct is not a gate: a real race arrives as one more
# FAIL among expected FAILs and nobody looks twice. That is very nearly what happened with
# the NameWatcher use-after-destroy, which TSan had been reporting all along.
#
# So: when this tree is built with -DWALLPAPERX_SANITIZE=thread, the suppressions file and
# the validation waiver are attached to the tests themselves, and a clean run is GREEN.
#
# The waiver is not optional here. The Khronos validation layer is uninstrumented and
# keeps concurrent state behind rwlocks TSan cannot pair, so it is by far the loudest
# source of noise -- and the shipped daemon never loads it. Waiving it costs the GPU-side
# oracle for sanitizer runs only; the non-sanitized tiers still assert it, and the tests
# announce the waiver loudly rather than degrading in silence.
if(WALLPAPERX_SANITIZE STREQUAL "thread" AND CMAKE_VERSION VERSION_LESS 3.28)
    # DIRECTORY-scoped TEST properties landed in 3.28, and this project's floor is 3.25.
    # Refusing to configure would punish everyone for an opt-in developer tier, so say
    # plainly what is missing instead of half-applying it -- a partially-wired TSan tier
    # is the red-when-correct state this block exists to remove.
    message(WARNING
        "CMake ${CMAKE_VERSION} is too old to auto-wire the TSan tier (needs 3.28 for "
        "DIRECTORY-scoped TEST properties). The tests are registered but carry neither the "
        "suppressions file nor the validation waiver, so a bare ctest here will be red for "
        "reasons unrelated to your code. Run them by hand instead:\n"
        "  WALLPAPERX_ALLOW_NO_VALIDATION=1 \\\n"
        "  TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tsan.supp:report_mutex_bugs=0 \\\n"
        "  ctest --test-dir <build> -L 'lavapipe|novulkan'\n"
        "report_mutex_bugs=0 is not optional: without it every run reports the Vulkan "
        "loader's and llvmpipe's mutex misuse as failures. The lavapipe entries set "
        "VK_ICD_FILENAMES themselves, so it does not belong on this command line.")
elseif(WALLPAPERX_SANITIZE STREQUAL "thread")
    set(_tsan_env
        "TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tsan.supp:report_mutex_bugs=0")

    # The waiver goes on every test, because every test now reads it -- all of them
    # through the single wallpaperx::testing::validationWaived() in
    # renderer/tests/support/validation_grade.h: the renderer probes via ValidationGrade,
    # modifier_roundtrip_probe via its own gate, the bus tests directly.
    #
    # It used to be withheld from the renderer probes, on the stated grounds that they
    # made "a claim about warnings and not about the layer being present". Folding the
    # layer gate into the grader made that false -- a probe now fails outright when no
    # validator was listening, because a zero count from an unwatched run is
    # indistinguishable from a clean one. Withholding the waiver after that left every
    # renderer lavapipe probe red under TSan on a host without the layer package, which
    # is precisely the condition this waiver exists to accept.
    set(_tsan_waiver "WALLPAPERX_ALLOW_NO_VALIDATION=1")

    # Lavapipe only, and the NVIDIA entries are disabled rather than left to fail. Under
    # TSan the proprietary driver takes a SEGV inside its own uninstrumented code on the
    # first SetWallpaper ("nested bug in the same thread, aborting"), so the daemon dies
    # before the storm phase runs. Left enabled, those entries would be permanently red
    # for a reason that has nothing to do with this project's code -- exactly the
    # red-when-correct trap this block exists to remove.
    # Both directories, and the second one is the whole point: the renderer probes are
    # registered by renderer/CMakeLists.txt via add_subdirectory, and DIRECTORY PROPERTY
    # TESTS only ever lists the tests of the directory asked. Querying just this one left
    # every renderer probe without the suppressions file -- so the validation layer's own
    # uninstrumented races were reported as failures, which is the noise this block exists
    # to remove, and it looked like the block was working because the backend tests went
    # green.
    set(_tsan_dirs "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../renderer")
    set(_tsan_enabled 0)
    set(_tsan_lavapipe 0)
    foreach(_dir IN LISTS _tsan_dirs)
        get_property(_dir_tests DIRECTORY "${_dir}" PROPERTY TESTS)
        foreach(_t IN LISTS _dir_tests)
            # DIRECTORY-scoped, because a TEST property is only reachable from the
            # directory that registered the test (CMake >= 3.28 for this form).
            get_property(_labels TEST "${_t}" DIRECTORY "${_dir}" PROPERTY LABELS)
            # Two labels, not one, and the second is the correction: what disqualifies a
            # test here is *loading the proprietary ICD*, which the absence of a lavapipe
            # label is a bad proxy for. Keyed on that absence, this branch also disabled
            # the CPU-only tests, which never open a driver and cannot SEGV in one.
            #
            # no-tsan overrides both, and is carried by the entry rather than listed here so
            # the reason sits beside the test that has it -- what they share is a crash
            # *inside the sanitizer runtime*, reached from a third-party library, which no
            # suppression can touch. A renamed test loses the label and comes back red, which
            # is the safe direction to fail: an entry silently dropped from the tier is the
            # one nobody would notice.
            if(("lavapipe" IN_LIST _labels OR "novulkan" IN_LIST _labels)
               AND NOT "no-tsan" IN_LIST _labels)
                # APPEND rather than read-modify-write: CMake already handles the
                # empty-property case and the list quoting.
                set_property(TEST "${_t}" DIRECTORY "${_dir}"
                             APPEND PROPERTY ENVIRONMENT ${_tsan_env})
                set_property(TEST "${_t}" DIRECTORY "${_dir}"
                             APPEND PROPERTY ENVIRONMENT ${_tsan_waiver})
                math(EXPR _tsan_enabled "${_tsan_enabled} + 1")
                if("lavapipe" IN_LIST _labels)
                    math(EXPR _tsan_lavapipe "${_tsan_lavapipe} + 1")
                endif()
            else()
                set_property(TEST "${_t}" DIRECTORY "${_dir}" PROPERTY DISABLED TRUE)
            endif()
        endforeach()
    endforeach()

    # A tier that disabled everything is the failure this block is most likely to have,
    # and the one least likely to be noticed: ctest prints "0 tests failed out of 0" and
    # exits 0, which reads exactly like a clean gate. It happens whenever the lavapipe
    # ICD is missing, since every lavapipe entry is registered under find_file. Say so at
    # configure time -- by the time ctest is green it is too late to notice.
    # Counted on the lavapipe entries specifically, not on the total. The novulkan entries
    # are single-threaded and always survive, so a nonzero total would satisfy an "anything
    # left?" check while detecting no race whatever -- every threaded path in this project
    # reaches TSan through a lavapipe entry. Without the ICD that is what the tier degrades
    # to, and it still exits 0. The live counts are in the configure-time message below,
    # which cannot go stale because it is computed.
    if(_tsan_lavapipe EQUAL 0)
        message(WARNING
            "TSan tier: no lavapipe test survived, so this tree detects no races at all "
            "-- only the ${_tsan_enabled} single-threaded CPU-only test(s) will run, and "
            "ctest will still exit 0. Almost certainly the Lavapipe ICD was not found "
            "(install vulkan-swrast / mesa-lavapipe and re-configure). Do not read a "
            "green run from this tree as a passing sanitizer gate.")
    else()
        message(STATUS "  TSan tier: ${_tsan_enabled} test(s) carry suppressions "
                       "(${_tsan_lavapipe} lavapipe) and the validation waiver; "
                       "default-ICD tests and every no-tsan entry disabled (each crashes "
                       "the sanitizer runtime from a third-party library)")
    endif()
endif()

# --------------------------------------------------------------------------- #
# KDE plugin — wpxctl, the wallpaperxplugin QML module, KDE package install
# --------------------------------------------------------------------------- #

if(WALLPAPERX_BUILD_KDE_PLUGIN)
    add_executable(wpxctl
        wpxctl/main.cpp
    )

    target_link_libraries(wpxctl PRIVATE
        Qt6::Core
        Qt6::DBus
        SDBusCpp::sdbus-c++
    )

    plasma_install_package(kde/kpackage com.wallpaperx.wallpaper wallpapers wallpaper)

    # The QML module the package's own WallpaperSurface.qml imports. It goes INSIDE the installed
    # package rather than into Qt's global QML path, because a wallpaper package is what Plasma
    # copies around and a plugin left outside it is one the desktop cannot find.
    #
    # qt_add_qml_module rather than a hand-written qmldir beside a QQmlExtensionPlugin: it
    # generates the qmldir and, the part that matters, the type information. qmllint resolves a
    # module's types only from that -- with a hand-written qmldir it reported WallpaperXRenderer
    # as "not found" even with the module on its import path, which left the file that draws the
    # wallpaper unlintable and the lint suite's own completeness check red.
    if(WALLPAPERX_HAVE_FRAME_IMPORT)
        qt_add_qml_module(wallpaperxplugin
            URI com.wallpaperx
            VERSION 1.0
            PLUGIN_TARGET wallpaperxplugin
            # Without this the generated qmldir says `optional plugin`, and Qt then declines to
            # load it: the type information alone looks sufficient, so `import com.wallpaperx`
            # resolves but WallpaperXRenderer never gets registered and nothing instantiates.
            NO_PLUGIN_OPTIONAL
            SOURCES kde/wallpaperx_item.cpp kde/wallpaperx_item.h kde/buffer_claims.h
            OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/qml/com/wallpaperx)
        target_link_libraries(wallpaperxplugin PRIVATE
            wallpaperx-frame-import wallpaperx-log-escape Qt6::Core Qt6::DBus Qt6::Quick Qt6::Qml)

        # The QML engine looks for lib<plugin>.so, and this target builds as
        # wallpaperxplugin.so without it -- so the module resolves, the type information is read,
        # and the plugin is never loaded.
        set_target_properties(wallpaperxplugin PROPERTIES PREFIX "lib")

        # Qt's own import root, NOT inside the KPackage. Plasma does not add a package's internal
        # directories to the QML import path, so a module shipped there is invisible: plasmashell
        # reports `module "com.wallpaperx" is not installed` and the wallpaper never loads.
        # Measured in a real session -- the in-tree harnesses missed it because they pass the
        # package path with -I by hand.
        set(WALLPAPERX_QML_MODULE_DIR ${KDE_INSTALL_QMLDIR}/com/wallpaperx)
        install(TARGETS wallpaperxplugin DESTINATION ${WALLPAPERX_QML_MODULE_DIR})
        install(FILES
            ${CMAKE_CURRENT_BINARY_DIR}/qml/com/wallpaperx/qmldir
            ${CMAKE_CURRENT_BINARY_DIR}/qml/com/wallpaperx/wallpaperxplugin.qmltypes
            DESTINATION ${WALLPAPERX_QML_MODULE_DIR})
    else()
        message(WARNING
            "The frame importer is not built, so the QML module that draws wallpapers is not "
            "installed either -- the package will load and report itself, but show nothing.")
    endif()
endif()

message(STATUS "wallpaperx-backend configured")
if(WALLPAPERX_BUILD_KDE_PLUGIN)
    message(STATUS "  Qt6:     ${Qt6_VERSION}")
    message(STATUS "  ECM:     ${ECM_VERSION}")
else()
    message(STATUS "  KDE plugin: disabled (WALLPAPERX_BUILD_KDE_PLUGIN=OFF) -- daemon + tests only")
endif()
message(STATUS "  sdbus:   ${SDBusCpp_VERSION}")
