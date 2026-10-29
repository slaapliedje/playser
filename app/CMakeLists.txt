cmake_minimum_required(VERSION 3.21)
project(discplayer VERSION 0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Quick DBus)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MPV REQUIRED IMPORTED_TARGET mpv)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
find_package(ZLIB REQUIRED)

qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(discplayer src/main.cpp)

# must precede qt_add_qml_module, which reads it when generating qmldir
set_source_files_properties(qml/Theme.qml qml/Resume.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(discplayer
    URI DiscPlayer
    VERSION 1.0
    SOURCES
        src/Mpv.h src/Mpv.cpp
        src/UiRenderer.h src/UiRenderer.cpp
        src/OverlayEncoder.h src/OverlayEncoder.cpp
        src/ScreenSaverInhibitor.h src/ScreenSaverInhibitor.cpp
        src/DiscMonitor.h src/DiscMonitor.cpp
        src/DiscIndex.h src/DiscIndex.cpp
        src/SystemCheck.h src/SystemCheck.cpp
        src/Gamepad.h src/Gamepad.cpp
    QML_FILES
        qml/Main.qml
        qml/HomeScreen.qml
        qml/PlayerScreen.qml
        qml/TrackPicker.qml
        qml/Theme.qml
        qml/Resume.qml
        qml/DiscScreen.qml
        qml/SetupScreen.qml
        qml/ButtonGlyph.qml
        qml/HintBar.qml
)

target_include_directories(discplayer PRIVATE src)

# offline disc catalogue from TheDiscDB (tools/build-discdb-index.py)
qt_add_resources(discplayer discdb
    PREFIX /
    BASE data
    FILES data/discdb-index.json.gz)

# UI font (Inter, SIL Open Font License)
qt_add_resources(discplayer fonts
    PREFIX /
    BASE data
    FILES data/fonts/InterVariable.ttf)

target_link_libraries(discplayer PRIVATE
    Qt6::Quick Qt6::DBus PkgConfig::MPV PkgConfig::SDL3 ZLIB::ZLIB)

include(GNUInstallDirs)
install(TARGETS discplayer RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/THEDISCDB-LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/discplayer)
install(FILES data/fonts/INTER-LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/discplayer)
