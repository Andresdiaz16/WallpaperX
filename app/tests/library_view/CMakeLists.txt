# S15.45, S15.46, S15.52, S15.55, S15.82: app/qml/LibraryView.qml and WallpaperDetail.qml offscreen, in their own module
# under the app's URI like the picker. On the private bus because a QGuiApplication's platform
# theme may connect to the session bus. QuickTest for qWaitForPolish: the views use Layouts, which
# place nothing until the window is polished.
set(qml_dir ${WALLPAPERX_APP_TESTS_DIR}/../qml)
qt_add_executable(wallpaperx-library-view-test ${WALLPAPERX_APP_TESTS_DIR}/library_view_test.cpp)
set_source_files_properties(${qml_dir}/LibraryView.qml PROPERTIES QT_RESOURCE_ALIAS LibraryView.qml)
set_source_files_properties(${qml_dir}/WallpaperDetail.qml PROPERTIES QT_RESOURCE_ALIAS WallpaperDetail.qml)
qt_add_qml_module(wallpaperx-library-view-test
    URI WallpaperX
    VERSION 1.0
    SOURCES ${WALLPAPERX_APP_LIBRARY_SOURCES} ${WALLPAPERX_APP_PREVIEW_SOURCES} ${WALLPAPERX_APP_KDE_SOURCES}
            ${WALLPAPERX_APP_APPLY_SOURCES}
    QML_FILES ${qml_dir}/LibraryView.qml ${qml_dir}/WallpaperDetail.qml
)
wallpaperx_app_test_target(wallpaperx-library-view-test)
wallpaperx_app_library_test_target(wallpaperx-library-view-test)
target_link_libraries(wallpaperx-library-view-test PRIVATE Qt6::Quick Qt6::Test Qt6::QuickTest)
wallpaperx_add_test(NAME app_library_view COMMAND wallpaperx-library-view-test)
set_tests_properties(app_library_view PROPERTIES LABELS "novulkan" SKIP_RETURN_CODE 77 TIMEOUT 60
                     ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
