cmake_minimum_required(VERSION 3.25)
project(wallpaperx VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --------------------------------------------------------------------------- #
# Dependencies
# --------------------------------------------------------------------------- #

find_package(Qt6 REQUIRED COMPONENTS Core DBus Quick Qml QuickControls2)
qt_standard_project_setup()

find_package(sdbus-c++ REQUIRED CONFIG)
find_package(nlohmann_json REQUIRED)

# --------------------------------------------------------------------------- #
# wallpaperx GUI (placeholder — sources added in later steps)
# --------------------------------------------------------------------------- #

qt_add_executable(wallpaperx
    main.cpp
)

target_link_libraries(wallpaperx PRIVATE
    Qt6::Core
    Qt6::DBus
    Qt6::Quick
    Qt6::Qml
    Qt6::QuickControls2
    SDBusCpp::sdbus-c++
    nlohmann_json::nlohmann_json
)

qt_add_qml_module(wallpaperx
    URI WallpaperX
    VERSION 1.0
    QML_FILES
        qml/main.qml
)

# Nothing installed this app until now -- it was only ever run out of the build directory. The
# plugin's settings pane launches it by desktop id, so both halves have to land: the binary
# where a PATH lookup finds it, and the entry where the desktop environment does. The id in the
# file name is what the pane passes to kstart, and what puts the app in the menu and KRunner.
include(GNUInstallDirs)
install(TARGETS wallpaperx RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES com.wallpaperx.app.desktop
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)

message(STATUS "wallpaperx configured")
message(STATUS "  Qt6: ${Qt6_VERSION}")
