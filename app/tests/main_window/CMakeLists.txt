# S15.54, S15.55: app/qml/Main.qml offscreen, with the picker and both library views it holds, in
# a module of its own under the app's URI (a subdirectory, for the reason library_view/ gives). On
# the private bus, since it stands up the fake KScreen there.
set(qml_dir ${WALLPAPERX_APP_TESTS_DIR}/../qml)
qt_add_executable(wallpaperx-main-window-test ${WALLPAPERX_APP_TESTS_DIR}/main_window_test.cpp)
foreach(view Main MonitorPicker LibraryView WallpaperDetail)
    set_source_files_properties(${qml_dir}/${view}.qml PROPERTIES QT_RESOURCE_ALIAS ${view}.qml)
endforeach()
qt_add_qml_module(wallpaperx-main-window-test
    URI WallpaperX
    VERSION 1.0
    SOURCES ${WALLPAPERX_APP_KDE_SOURCES} ${WALLPAPERX_APP_LIBRARY_SOURCES} ${WALLPAPERX_APP_PREVIEW_SOURCES}
    QML_FILES ${qml_dir}/Main.qml ${qml_dir}/MonitorPicker.qml ${qml_dir}/LibraryView.qml ${qml_dir}/WallpaperDetail.qml
)
wallpaperx_app_test_target(wallpaperx-main-window-test)
wallpaperx_app_library_test_target(wallpaperx-main-window-test)
target_link_libraries(wallpaperx-main-window-test PRIVATE Qt6::Quick Qt6::Test Qt6::QuickTest)
add_test(NAME app_main_window
         COMMAND ${DBUS_RUN_SESSION} --config-file=${WALLPAPERX_APP_TEST_BUS_CONFIG}
                 -- $<TARGET_FILE:wallpaperx-main-window-test>)
set_tests_properties(app_main_window PROPERTIES LABELS "novulkan" SKIP_RETURN_CODE 77 TIMEOUT 60
                     ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
                     RESOURCE_LOCK wallpaperx_bus_tests)
