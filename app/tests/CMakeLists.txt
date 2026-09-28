# The app's tests. Every entry is labelled `novulkan` because none opens a GPU driver, but hosted
# CI does not run them: .github/workflows/ci.yml builds only renderer/ and backend/, and the app
# needs Qt 6.8 or newer, which the hosted runner's packages do not provide. They run locally only.
#
# The sources under test are compiled into each test binary rather than linked from a library:
# the app has no library target, and adding one only for tests would reshape the production build.

set(WALLPAPERX_APP_KDE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../kde/kscreen_config.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../kde/kscreen_config.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../kde/kscreen_client.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../kde/kscreen_client.h
)

function(wallpaperx_app_test_target target)
    target_include_directories(${target} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../kde
        # tests/test_report.h: the pass / fail / skip contract every test binary here shares.
        ${CMAKE_CURRENT_SOURCE_DIR}/../../backend)
    target_compile_definitions(${target} PRIVATE
        WALLPAPERX_APP_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
    target_link_libraries(${target} PRIVATE Qt6::Core Qt6::DBus Qt6::Qml)
endfunction()

# S15.1-S15.5, S15.21, S15.22, S15.25: the reply parser and the EDID reader. No bus, so no dbus-run-session.
qt_add_executable(wallpaperx-kscreen-config-test kscreen_config_test.cpp ${WALLPAPERX_APP_KDE_SOURCES})
wallpaperx_app_test_target(wallpaperx-kscreen-config-test)
add_test(NAME app_kscreen_config COMMAND wallpaperx-kscreen-config-test)
set_tests_properties(app_kscreen_config PROPERTIES LABELS "novulkan" SKIP_RETURN_CODE 77)

# The bus tests run on a private bus from a config with no service directories, so nothing can be
# auto-started on it (app_kscreen_launcher's can start only its own test stand-ins);
# dbus-run-session's default would let the real KScreen launcher start and read the live
# compositor. The binaries prove both (refuseUnlessPrivateBus in fake_kscreen.h).
find_program(DBUS_RUN_SESSION dbus-run-session)
find_package(Qt6 COMPONENTS Test Quick)
set(WALLPAPERX_APP_TEST_BUS_CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/private_session.conf)

if(DBUS_RUN_SESSION)
    # S15.6-S15.12, S15.14, S15.16, S15.18-S15.20, S15.23, S15.25, S15.26, S15.28: KScreenClient
    # against a fake KScreen.
    qt_add_executable(wallpaperx-kscreen-client-test kscreen_client_test.cpp ${WALLPAPERX_APP_KDE_SOURCES})
    wallpaperx_app_test_target(wallpaperx-kscreen-client-test)
    add_test(NAME app_kscreen_client
             COMMAND ${DBUS_RUN_SESSION} --config-file=${WALLPAPERX_APP_TEST_BUS_CONFIG}
                     -- $<TARGET_FILE:wallpaperx-kscreen-client-test>)
    set_tests_properties(app_kscreen_client PROPERTIES LABELS "novulkan" SKIP_RETURN_CODE 77
                         RESOURCE_LOCK wallpaperx_bus_tests)

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
        file(GENERATE OUTPUT ${WALLPAPERX_APP_TEST_SERVICE_DIR}/${standin_name}.service
             CONTENT "[D-BUS Service]\nName=${standin_name}\nExec=$<TARGET_FILE:wallpaperx-kscreen-launcher-test> --stand-in ${behaviour} ${standin_name}\n")
    endforeach()
    string(REPLACE ";" "\\;" standin_define "${standin_names}")
    target_compile_definitions(wallpaperx-kscreen-launcher-test PRIVATE "WALLPAPERX_STANDIN_NAMES=\"${standin_define}\"")
    configure_file(activating_session.conf.in ${CMAKE_CURRENT_BINARY_DIR}/activating_session.conf @ONLY)
    add_test(NAME app_kscreen_launcher
             COMMAND ${DBUS_RUN_SESSION} --config-file=${CMAKE_CURRENT_BINARY_DIR}/activating_session.conf
                     -- $<TARGET_FILE:wallpaperx-kscreen-launcher-test>)
    set_tests_properties(app_kscreen_launcher PROPERTIES LABELS "novulkan" SKIP_RETURN_CODE 77 TIMEOUT 120
                         ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"
                         RESOURCE_LOCK wallpaperx_bus_tests)

    # S15.15: the app installed to a temporary prefix starts from outside the build tree. It
    # installs whatever this tree last built, so it is only meaningful after a build.
    add_test(NAME app_installed_starts
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/installed_app_test.sh ${CMAKE_COMMAND}
                     ${PROJECT_BINARY_DIR} ${DBUS_RUN_SESSION} ${WALLPAPERX_APP_TEST_BUS_CONFIG})
    set_tests_properties(app_installed_starts PROPERTIES LABELS "novulkan" SKIP_RETURN_CODE 77
                         TIMEOUT 90 RESOURCE_LOCK wallpaperx_bus_tests)

    if(Qt6Test_FOUND AND Qt6Quick_FOUND)
        # S15.13, S15.17, S15.24, S15.26: the picker's tiles, offscreen. Its own QML module under the app's URI, built from
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
        add_test(NAME app_monitor_picker
                 COMMAND ${DBUS_RUN_SESSION} --config-file=${WALLPAPERX_APP_TEST_BUS_CONFIG}
                         -- $<TARGET_FILE:wallpaperx-monitor-picker-test>)
        set_tests_properties(app_monitor_picker PROPERTIES LABELS "novulkan" SKIP_RETURN_CODE 77
                             ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
                             RESOURCE_LOCK wallpaperx_bus_tests)
    else()
        message(WARNING "Qt6 Test or Quick not found -- app_monitor_picker (S15.13, S15.17, S15.24, S15.26) is not registered")
    endif()
else()
    message(WARNING "dbus-run-session not found -- app_kscreen_client (S15.6-S15.12, S15.14, S15.16, S15.18-S15.20, "
                    "S15.23, S15.25, S15.26, S15.28), app_kscreen_launcher (S15.27, S15.29, S15.30), app_installed_starts "
                    "(S15.15) and app_monitor_picker (S15.13, S15.17, S15.24, S15.26) are not registered")
endif()
