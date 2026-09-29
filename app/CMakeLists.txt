cmake_minimum_required(VERSION 3.25)
project(wallpaperx VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --------------------------------------------------------------------------- #
# Dependencies
# --------------------------------------------------------------------------- #

find_package(Qt6 REQUIRED COMPONENTS Core Concurrent DBus Quick Qml QuickControls2)
# 6.8 sets QTP0001 and QTP0004 to NEW. QTP0001 puts the QML module under qrc:/qt/qml, which is on
# the engine's default import path; without it the installed binary finds no module, since only
# the build tree has a WallpaperX/qmldir beside it.
qt_standard_project_setup(REQUIRES 6.8)

find_package(sdbus-c++ REQUIRED CONFIG)
find_package(nlohmann_json REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBZIP REQUIRED IMPORTED_TARGET libzip)

# --------------------------------------------------------------------------- #
# wallpaperx GUI
# --------------------------------------------------------------------------- #

qt_add_executable(wallpaperx
    main.cpp
    single_instance.cpp
    single_instance.h
    kde/kscreen_config.cpp
    kde/kscreen_config.h
    kde/kscreen_client.cpp
    kde/kscreen_client.h
    library/current_exception.h
    library/unique_fd.h
    library/wpx_package.cpp
    library/wpx_package.h
    library/folder_watcher.cpp
    library/folder_watcher.h
    library/library_manager.cpp
    library/library_manager.h
    library/importer.cpp
    library/importer.h
    library/preview_image_provider.cpp
    library/preview_image_provider.h
)

# The generated QML type registration includes these headers by bare name.
target_include_directories(wallpaperx PRIVATE kde library)

target_link_libraries(wallpaperx PRIVATE
    Qt6::Core
    Qt6::Concurrent
    Qt6::DBus
    Qt6::Quick
    Qt6::Qml
    Qt6::QuickControls2
    SDBusCpp::sdbus-c++
    nlohmann_json::nlohmann_json
    PkgConfig::LIBZIP
)

qt_add_qml_module(wallpaperx
    URI WallpaperX
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/MonitorPicker.qml
        qml/LibraryView.qml
        qml/WallpaperDetail.qml
)

# Nothing installed this app until now -- it was only ever run out of the build directory. The
# plugin's settings pane launches it by desktop id, so both halves have to land: the binary
# where a PATH lookup finds it, and the entry where the desktop environment does. The id in the
# file name is what the pane passes to kstart, and what puts the app in the menu and KRunner.
include(GNUInstallDirs)
install(TARGETS wallpaperx RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES com.wallpaperx.app.desktop
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
# The desktop entry opens .wpx files by this type; nothing here runs update-mime-database.
install(FILES com.wallpaperx.app.xml
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/mime/packages)

enable_testing()
add_subdirectory(tests)

message(STATUS "wallpaperx configured")
message(STATUS "  Qt6: ${Qt6_VERSION}")
