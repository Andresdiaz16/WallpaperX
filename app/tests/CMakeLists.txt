# The app's tests. Every entry is labelled `novulkan` because none opens a GPU driver, but hosted
# CI does not run them: .github/workflows/ci.yml builds only renderer/ and backend/, and the app
# needs Qt 6.8 or newer, which the hosted runner's packages do not provide. They run locally only.
#
# The sources under test are compiled into each test binary rather than linked from a library:
# the app has no library target, and adding one only for tests would reshape the production build.

# Every entry is registered through wallpaperx_add_test (isolated_run.sh): a private HOME, XDG
# directories and TMPDIR, a network of its own, no live display, and a private bus that cannot
# start services. test_isolation_registry at the end fails for any entry that is not.
include(${CMAKE_CURRENT_SOURCE_DIR}/../../renderer/tests/support/isolated_test.cmake)

set(WALLPAPERX_APP_KDE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../kde/kscreen_config.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../kde/kscreen_config.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../kde/kscreen_client.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../kde/kscreen_client.h
)

# Applying: the daemon and Plasma clients, and what drives them from the picker and the library.
set(WALLPAPERX_APP_APPLY_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../daemon/daemon_client.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../daemon/daemon_client.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../daemon/apply_controller.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../daemon/apply_controller.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../kde/plasma_controller.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../kde/plasma_controller.h
)

# The library: reading .wpx archives and watching the folder. The preview provider is separate
# because it needs Qt Quick.
set(WALLPAPERX_APP_LIBRARY_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../library/wpx_package.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../library/wpx_package.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../library/folder_watcher.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../library/folder_watcher.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../library/library_manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../library/library_manager.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../library/importer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../library/importer.h
)
set(WALLPAPERX_APP_PREVIEW_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../library/preview_image_provider.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../library/preview_image_provider.h
)

# This folder, for the functions below: CMAKE_CURRENT_SOURCE_DIR inside a function is the
# caller's, and library_view/ calls them too.
set(WALLPAPERX_APP_TESTS_DIR ${CMAKE_CURRENT_SOURCE_DIR})

function(wallpaperx_app_library_test_target target)
    target_include_directories(${target} PRIVATE ${WALLPAPERX_APP_TESTS_DIR}/../library)
    target_link_libraries(${target} PRIVATE Qt6::Concurrent Qt6::Gui nlohmann_json::nlohmann_json PkgConfig::LIBZIP)
endfunction()

function(wallpaperx_app_test_target target)
    target_include_directories(${target} PRIVATE
        ${WALLPAPERX_APP_TESTS_DIR}
        ${WALLPAPERX_APP_TESTS_DIR}/../kde
        ${WALLPAPERX_APP_TESTS_DIR}/../daemon
        # tests/test_report.h: the pass / fail / skip contract every test binary here shares.
        ${WALLPAPERX_APP_TESTS_DIR}/../../backend
        # support/forked_child.h: the children's preamble and core suppression, shared with the daemon's tests.
        ${WALLPAPERX_APP_TESTS_DIR}/../../renderer/tests)
    target_compile_definitions(${target} PRIVATE
        WALLPAPERX_APP_TEST_DATA="${WALLPAPERX_APP_TESTS_DIR}/data")
    target_link_libraries(${target} PRIVATE Qt6::Core Qt6::DBus Qt6::Qml)
endfunction()

# S15.1-S15.5, S15.21, S15.22, S15.25: the reply parser and the EDID reader. Uses no bus.
qt_add_executable(wallpaperx-kscreen-config-test kscreen_config_test.cpp ${WALLPAPERX_APP_KDE_SOURCES})
wallpaperx_app_test_target(wallpaperx-kscreen-config-test)
# S15.335: a double converted out of int's range ends the run with the sanitizer's report.
target_compile_options(wallpaperx-kscreen-config-test PRIVATE -fsanitize=float-cast-overflow -fno-sanitize-recover=float-cast-overflow)
target_link_options(wallpaperx-kscreen-config-test PRIVATE -fsanitize=float-cast-overflow)
wallpaperx_add_test(NAME app_kscreen_config COMMAND wallpaperx-kscreen-config-test)
set_tests_properties(app_kscreen_config PROPERTIES LABELS "novulkan" SKIP_RETURN_CODE 77 TIMEOUT 60)

# S15.254: the app's pause-mode table and the daemon's stay two arrays in one program. Built twice,
# the objects linked in each order, since which of two merged definitions survives follows the order.
foreach(order app_first daemon_first)
    if(order STREQUAL "app_first")
        set(link_sources ${CMAKE_CURRENT_SOURCE_DIR}/../daemon/daemon_client.cpp pause_mode_link_daemon.cpp)
    else()
        set(link_sources pause_mode_link_daemon.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../daemon/daemon_client.cpp)
    endif()
    qt_add_executable(wallpaperx-pause-mode-link-${order} ${link_sources} pause_mode_link_test.cpp
                      ${CMAKE_CURRENT_SOURCE_DIR}/../daemon/daemon_client.h)
    wallpaperx_app_test_target(wallpaperx-pause-mode-link-${order})
    wallpaperx_add_test(NAME app_pause_mode_link_${order} COMMAND wallpaperx-pause-mode-link-${order})
    set_tests_properties(app_pause_mode_link_${order} PROPERTIES LABELS "novulkan" TIMEOUT 60)
endforeach()

find_package(Qt6 COMPONENTS Test Quick QuickTest)

# S15.31-S15.35, S15.45, S15.47, S15.48, S15.50, S15.51, S15.53, S15.55, S15.67, S15.68, S15.221-S15.225:
# one wallpaper.json, one .wpx, one wallpaper folder, one library folder, one preview address, with and
# without openat2 (openat2_shim.cpp stands in for a kernel that lacks it, or a sandbox that refuses it).
# daemon_rules.cpp and the daemon's wallpaper_properties.cpp grade the validator against the daemon.
# No bus and no event loop. TIMEOUT backs up the test's own bound on a scan blocked by a FIFO.
if(Qt6Quick_FOUND)
    qt_add_executable(wallpaperx-wpx-package-test wpx_package_test.cpp openat2_shim.cpp daemon_rules.cpp
                      ${WALLPAPERX_APP_TESTS_DIR}/../../backend/daemon/wallpaper_properties.cpp
                      ${WALLPAPERX_APP_LIBRARY_SOURCES} ${WALLPAPERX_APP_PREVIEW_SOURCES})
    wallpaperx_app_test_target(wallpaperx-wpx-package-test)
    wallpaperx_app_library_test_target(wallpaperx-wpx-package-test)
    target_link_libraries(wallpaperx-wpx-package-test PRIVATE Qt6::Quick)
    wallpaperx_add_test(NAME app_wpx_package COMMAND wallpaperx-wpx-package-test)
    set_tests_properties(app_wpx_package PROPERTIES LABELS "novulkan" SKIP_RETURN_CODE 77 TIMEOUT 60)
else()
    wallpaperx_tests_not_registered("Qt6 Quick not found -- app_wpx_package (S15.31-S15.35, S15.45, S15.47, S15.48, S15.50, S15.51, "
                    "S15.53, S15.55, S15.67, S15.68) is not registered")
endif()

# S15.36-S15.44, S15.48, S15.49, S15.57, S15.69: LibraryManager against real library folders of
# wallpaper folders, watched through inotify. No bus.
# openat2_shim.cpp counts the wallpaper files read (each through openat2), so an idle library is
# shown to read none.
qt_add_executable(wallpaperx-library-manager-test library_manager_test.cpp open_counter.cpp openat2_shim.cpp ${WALLPAPERX_APP_LIBRARY_SOURCES})
wallpaperx_app_test_target(wallpaperx-library-manager-test)
wallpaperx_app_library_test_target(wallpaperx-library-manager-test)
wallpaperx_add_test(NAME app_library_manager COMMAND wallpaperx-library-manager-test)
set_tests_properties(app_library_manager PROPERTIES LABELS "novulkan" SKIP_RETURN_CODE 77 TIMEOUT 90)

# S15.52, S15.56, S15.59-S15.66, S15.70-S15.74, S15.86-S15.88: LibraryManager following a library as
# it changes -- folders read once no file in them is open for writing, rewrites and holds, polling,
# an overflowing event queue, the watch limit and a file system where a lease says nothing (each in
# a user namespace of its own, through unshare) -- and its list model under Qt's model tester. No bus.
if(Qt6Test_FOUND)
    qt_add_executable(wallpaperx-library-changes-test library_changes_test.cpp open_counter.cpp openat2_shim.cpp ${WALLPAPERX_APP_LIBRARY_SOURCES})
    wallpaperx_app_test_target(wallpaperx-library-changes-test)
    wallpaperx_app_library_test_target(wallpaperx-library-changes-test)
    target_link_libraries(wallpaperx-library-changes-test PRIVATE Qt6::Test)
    wallpaperx_add_test(NAME app_library_changes COMMAND wallpaperx-library-changes-test)
    # Six namespace children, each bounded at 10 s (kNamespaceChildMs), plus about 16 s for the
    # rest: every verdict prints before this TIMEOUT, even with all six hung.
    set_tests_properties(app_library_changes PROPERTIES LABELS "novulkan" SKIP_RETURN_CODE 77 TIMEOUT 120)
else()
    wallpaperx_tests_not_registered("Qt6 Test not found -- app_library_changes (S15.52, S15.56, S15.59-S15.66, S15.70-S15.74, S15.86-S15.88) is not registered")
endif()

# S15.58, S15.75-S15.81, S15.89-S15.93: unpacking a .wpx into a wallpaper folder, and the Importer's
# drop folder (links in it included) and explicit imports. The free-space case runs in a user and
# mount namespace of its own, through unshare. No bus.
qt_add_executable(wallpaperx-importer-test importer_test.cpp open_counter.cpp ${WALLPAPERX_APP_LIBRARY_SOURCES})
wallpaperx_app_test_target(wallpaperx-importer-test)
wallpaperx_app_library_test_target(wallpaperx-importer-test)
wallpaperx_add_test(NAME app_importer COMMAND wallpaperx-importer-test)
# One namespace child, bounded at 10 s (kNamespaceChildMs), plus about 30 s for the rest.
set_tests_properties(app_importer PROPERTIES LABELS "novulkan" SKIP_RETURN_CODE 77 TIMEOUT 120)

# The bus tests use the wrapper's private bus, which has no service directories, so nothing can be
# auto-started on it (app_kscreen_launcher adds a bus of its own that can start only its test
# stand-ins); dbus-run-session's default would let the real KScreen launcher start and read the
# live compositor. The binaries prove both (refuseUnlessPrivateBus in fake_kscreen.h). Registered
# on the same lookup of dbus-run-session the wrapper's --bus comes from (isolated_test.cmake), so
# the two cannot disagree. Each has a bus, HOME and network of its own, so they run in parallel.
if(WALLPAPERX_DBUS_RUN_SESSION)
    # S15.6-S15.12, S15.14, S15.16, S15.18-S15.20, S15.23, S15.25, S15.26, S15.28: KScreenClient
    # against a fake KScreen.
    qt_add_executable(wallpaperx-kscreen-client-test kscreen_client_test.cpp ${WALLPAPERX_APP_KDE_SOURCES})
    wallpaperx_app_test_target(wallpaperx-kscreen-client-test)
    wallpaperx_add_test(NAME app_kscreen_client COMMAND wallpaperx-kscreen-client-test)
    set_tests_properties(app_kscreen_client PROPERTIES LABELS "novulkan" SKIP_RETURN_CODE 77 TIMEOUT 60)

    # S15.27, S15.29, S15.30: KScreenClient when the launcher leaves and D-Bus activation starts it
    # again. Its bus can start exactly the stand-ins below (this binary run with --stand-in), from a
    # service directory generated here and listed by no other config. AddressSanitizer, so a
    # callback that outlives a destroyed client aborts the run (S15.29).
    set(WALLPAPERX_STANDINS ColdStart:healthy Crashy:crashy ExitOnce:healthy ExitEvery:healthy
                            StaysUp:healthy LaterReady:healthy Destroyed:healthy Probe:healthy)
    set(WALLPAPERX_APP_TEST_SERVICE_DIR ${CMAKE_CURRENT_BINARY_DIR}/launcher_services)
    file(REMOVE_RECURSE ${WALLPAPERX_APP_TEST_SERVICE_DIR})
    qt_add_executable(wallpaperx-kscreen-launcher-test kscreen_launcher_test.cpp ${WALLPAPERX_APP_KDE_SOURCES})
    wallpaperx_app_test_target(wallpaperx-kscreen-launcher-test)
    target_compile_options(wallpaperx-kscreen-launcher-test PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(wallpaperx-kscreen-launcher-test PRIVATE -fsanitize=address)
    set(standin_names "")
    foreach(standin IN LISTS WALLPAPERX_STANDINS)
        string(REPLACE ":" ";" parts ${standin})
        list(GET parts 0 suffix)
        list(GET parts 1 behaviour)
        set(standin_name io.wallpaperx.test.Launcher.${suffix})
        list(APPEND standin_names ${standin_name})
        # Exec= is split like a shell line, so the path is single-quoted, which leaves only a quote
        # in it to refuse.
        file(GENERATE OUTPUT ${WALLPAPERX_APP_TEST_SERVICE_DIR}/${standin_name}.service
             CONTENT "[D-BUS Service]\nName=${standin_name}\nExec='$<TARGET_FILE:wallpaperx-kscreen-launcher-test>' --stand-in ${behaviour} ${standin_name}\n")
    endforeach()
    if(CMAKE_CURRENT_BINARY_DIR MATCHES "'")
        message(FATAL_ERROR "app_kscreen_launcher's stand-ins cannot be started from a build directory whose path holds a "
                            "single quote: ${CMAKE_CURRENT_BINARY_DIR}")
    endif()
    string(REPLACE ";" "\\;" standin_define "${standin_names}")
    target_compile_definitions(wallpaperx-kscreen-launcher-test PRIVATE "WALLPAPERX_STANDIN_NAMES=\"${standin_define}\"")
    wallpaperx_xml_text(WALLPAPERX_APP_TEST_SERVICE_DIR_XML "${WALLPAPERX_APP_TEST_SERVICE_DIR}")
    wallpaperx_xml_text(WALLPAPERX_ISOLATED_SESSION_CONF_XML "${WALLPAPERX_ISOLATED_SESSION_CONF}")
    configure_file(activating_session.conf.in ${CMAKE_CURRENT_BINARY_DIR}/activating_session.conf @ONLY)
    # A second private bus inside the wrapper's: this one must activate the stand-ins, which the
    # wrapper's bus never does. Started through private_bus.sh, which puts its socket in the
    # entry's private runtime directory.
    wallpaperx_add_test(NAME app_kscreen_launcher
                        COMMAND /bin/sh ${WALLPAPERX_ISOLATION_SUPPORT_DIR}/private_bus.sh ${WALLPAPERX_DBUS_RUN_SESSION}
                                ${CMAKE_CURRENT_BINARY_DIR}/activating_session.conf
                                -- $<TARGET_FILE:wallpaperx-kscreen-launcher-test>)
    set_tests_properties(app_kscreen_launcher PROPERTIES LABELS "novulkan" SKIP_RETURN_CODE 77 TIMEOUT 120
                         ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")

    # S15.15: the app installed to a temporary prefix starts from outside the build tree. It
    # installs whatever this tree last built, so it is only meaningful after a build. Its bus is
    # the wrapper's configuration, through the same private_bus.sh.
    # `cmake --install` writes install_manifest.txt into the build tree, as app_desktop_integration's
    # does: the lock keeps the two from writing it at once.
    wallpaperx_add_test(NAME app_installed_starts
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/installed_app_test.sh ${CMAKE_COMMAND}
                     ${PROJECT_BINARY_DIR} ${WALLPAPERX_DBUS_RUN_SESSION} ${WALLPAPERX_ISOLATED_SESSION_CONF}
                     ${WALLPAPERX_ISOLATION_SUPPORT_DIR}/private_bus.sh)
    set_tests_properties(app_installed_starts PROPERTIES LABELS "novulkan" SKIP_RETURN_CODE 77
                         TIMEOUT 90 RESOURCE_LOCK wallpaperx_app_install_manifest)

    # S15.83, S15.84, S15.93-S15.95: the app this tree built, run as separate processes on the private bus -- a
    # second launch hands its files to the first over org.freedesktop.Application and exits.
    qt_add_executable(wallpaperx-single-instance-test single_instance_test.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../single_instance.cpp
                      ${CMAKE_CURRENT_SOURCE_DIR}/../single_instance.h ${WALLPAPERX_APP_LIBRARY_SOURCES})
    wallpaperx_app_test_target(wallpaperx-single-instance-test)
    wallpaperx_app_library_test_target(wallpaperx-single-instance-test)
    target_include_directories(wallpaperx-single-instance-test PRIVATE ${WALLPAPERX_APP_TESTS_DIR}/..)
    target_compile_definitions(wallpaperx-single-instance-test PRIVATE WALLPAPERX_APP_BINARY="$<TARGET_FILE:wallpaperx>")
    add_dependencies(wallpaperx-single-instance-test wallpaperx)
    wallpaperx_add_test(NAME app_single_instance COMMAND wallpaperx-single-instance-test)
    set_tests_properties(app_single_instance PROPERTIES LABELS "novulkan" SKIP_RETURN_CODE 77 TIMEOUT 120)

    # S15.226-S15.253, S15.256-S15.264: Apply, live edits and the pause setting, against stand-ins for the daemon and
    # for plasmashell (which runs the activation script against a model of Plasma 6.7.5's scripting
    # API), with a fake KScreen and a real library. daemon_rules.cpp grades against the daemon's own
    # pause_mode.h. AddressSanitizer, so a reply reaching a destroyed caller aborts the run (S15.247).
    qt_add_executable(wallpaperx-apply-test apply_test.cpp daemon_rules.cpp
                      ${WALLPAPERX_APP_TESTS_DIR}/../../backend/daemon/wallpaper_properties.cpp
                      ${WALLPAPERX_APP_KDE_SOURCES} ${WALLPAPERX_APP_APPLY_SOURCES} ${WALLPAPERX_APP_LIBRARY_SOURCES})
    wallpaperx_app_test_target(wallpaperx-apply-test)
    wallpaperx_app_library_test_target(wallpaperx-apply-test)
    target_compile_definitions(wallpaperx-apply-test PRIVATE
        WALLPAPERX_PLUGIN_METADATA="${WALLPAPERX_APP_TESTS_DIR}/../../backend/kde/kpackage/metadata.json")
    target_compile_options(wallpaperx-apply-test PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(wallpaperx-apply-test PRIVATE -fsanitize=address)
    wallpaperx_add_test(NAME app_apply COMMAND wallpaperx-apply-test)
    set_tests_properties(app_apply PROPERTIES LABELS "novulkan" SKIP_RETURN_CODE 77 TIMEOUT 120
                         ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")

    if(Qt6Test_FOUND AND Qt6Quick_FOUND)
        # S15.13, S15.17, S15.24, S15.26, S15.55: the picker's tiles, offscreen. Its own QML module under the app's URI, built from
        # the app's own MonitorPicker.qml and C++ types, because the app's module lives inside the
        # app executable and cannot be loaded from outside it.
        qt_add_executable(wallpaperx-monitor-picker-test monitor_picker_test.cpp)
        set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/../qml/MonitorPicker.qml
                                    PROPERTIES QT_RESOURCE_ALIAS MonitorPicker.qml)
        qt_add_qml_module(wallpaperx-monitor-picker-test
            URI WallpaperX
            VERSION 1.0
            SOURCES ${WALLPAPERX_APP_KDE_SOURCES}
            QML_FILES ${CMAKE_CURRENT_SOURCE_DIR}/../qml/MonitorPicker.qml
        )
        wallpaperx_app_test_target(wallpaperx-monitor-picker-test)
        target_link_libraries(wallpaperx-monitor-picker-test PRIVATE Qt6::Quick Qt6::Test)
        wallpaperx_add_test(NAME app_monitor_picker COMMAND wallpaperx-monitor-picker-test)
        set_tests_properties(app_monitor_picker PROPERTIES LABELS "novulkan" SKIP_RETURN_CODE 77 TIMEOUT 60
                             ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

        # S15.45, S15.46, S15.52, S15.55, S15.82: LibraryView.qml and WallpaperDetail.qml; S15.54,
        # S15.55, S15.82: Main.qml. In subdirectories because two modules with one URI in one build
        # folder share its generated qmake_WallpaperX.qrc (the qmldir), so the last one generated
        # would replace the picker's.
        if(Qt6QuickTest_FOUND)
            add_subdirectory(library_view)
            add_subdirectory(main_window)
        else()
            wallpaperx_tests_not_registered("Qt6 QuickTest not found -- app_library_view (S15.45, S15.46, S15.52, S15.55, S15.82) and "
                            "app_main_window (S15.54, S15.55, S15.82) are not registered")
        endif()
    else()
        wallpaperx_tests_not_registered("Qt6 Test or Quick not found -- app_monitor_picker (S15.13, S15.17, S15.24, S15.26, S15.55), "
                        "app_library_view (S15.45, S15.46, S15.52, S15.55, S15.82) and app_main_window (S15.54, S15.55, "
                        "S15.82) are not registered")
    endif()
else()
    wallpaperx_tests_not_registered("dbus-run-session not found -- app_kscreen_client (S15.6-S15.12, S15.14, S15.16, S15.18-S15.20, "
                    "S15.23, S15.25, S15.26, S15.28), app_kscreen_launcher (S15.27, S15.29, S15.30), app_installed_starts "
                    "(S15.15), app_single_instance (S15.83, S15.84, S15.93-S15.95), app_apply (S15.226-S15.253, S15.256-S15.264), "
                    "app_monitor_picker (S15.13, S15.17, S15.24, S15.26, "
                    "S15.55), app_library_view (S15.45, S15.46, S15.52, S15.55, S15.82) and app_main_window (S15.54, S15.55, "
                    "S15.82) are not registered")
endif()

# S15.85: the MIME type and desktop entry, installed with the app's own rules into a scratch prefix
# and their databases built there, never on this system. No bus.
qt_add_executable(wallpaperx-desktop-integration-test desktop_integration_test.cpp)
wallpaperx_app_test_target(wallpaperx-desktop-integration-test)
wallpaperx_app_library_test_target(wallpaperx-desktop-integration-test)
add_dependencies(wallpaperx-desktop-integration-test wallpaperx)
wallpaperx_add_test(NAME app_desktop_integration COMMAND wallpaperx-desktop-integration-test ${CMAKE_COMMAND} ${PROJECT_BINARY_DIR})
set_tests_properties(app_desktop_integration PROPERTIES LABELS "novulkan" SKIP_RETURN_CODE 77 TIMEOUT 90
                     RESOURCE_LOCK wallpaperx_app_install_manifest)

# That every entry in this tree runs through isolated_run.sh, read from ctest's own listing, so an
# entry registered with a bare add_test() -- here or in a subdirectory -- is caught. The floor is
# what this configure registered, so a host lacking an optional dependency is not failed for the
# entries it never had; WALLPAPERX_REQUIRE_ALL_TESTS turns such a host into a failed configure.
# CONTROL: test-side, control_batch, 2026-10-04: `add_test(NAME control_bare_entry COMMAND true)` in
#       main_window/ -> "FAIL: control_bare_entry does not run through .../isolated_run.sh ...", and
#       the same just before the call below. In wallpaperx_add_isolation_registry
#       (renderer/tests/support/isolated_test.cmake), -DBUILD_DIR made ${CMAKE_BINARY_DIR}/CMakeFiles
#       (an empty listing) -> "FAIL: ctest's listing of .../CMakeFiles does not include
#       test_isolation_registry itself" and "FAIL: only 0 entries were listed, fewer than the 17 this
#       tree registers at the least" (control_batch, 2026-10-04, here and in the backend and renderer
#       trees). By hand, 2026-10-04: WALLPAPERX_DBUS_RUN_SESSION=OFF registers 9 entries and the
#       registry passes; with WALLPAPERX_REQUIRE_ALL_TESTS=ON the same configure fails.
wallpaperx_add_isolation_registry()
