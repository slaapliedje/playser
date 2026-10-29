cmake_minimum_required(VERSION 3.21)
project(discplayer VERSION 0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Quick DBus OpenGL)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MPV REQUIRED IMPORTED_TARGET mpv)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)

qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(discplayer src/main.cpp)

# must precede qt_add_qml_module, which reads it when generating qmldir
set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(discplayer
    URI DiscPlayer
    VERSION 1.0
    SOURCES
        src/MpvItem.h src/MpvItem.cpp
        src/DiscMonitor.h src/DiscMonitor.cpp
        src/Gamepad.h src/Gamepad.cpp
    QML_FILES
        qml/Main.qml
        qml/HomeScreen.qml
        qml/PlayerScreen.qml
        qml/TrackPicker.qml
        qml/Theme.qml
)

target_include_directories(discplayer PRIVATE src)

target_link_libraries(discplayer PRIVATE
    Qt6::Quick Qt6::DBus Qt6::OpenGL PkgConfig::MPV PkgConfig::SDL3)
