cmake_minimum_required(VERSION 3.21)
project(playser VERSION 0.3.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Quick DBus Network)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MPV REQUIRED IMPORTED_TARGET mpv)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
find_package(ZLIB REQUIRED)

# libdiscnav (DVD menus, LGPL): installed (the Flatpak builds it as a
# module), else from the source tree
pkg_check_modules(DISCNAV IMPORTED_TARGET discnav)
if(DISCNAV_FOUND)
    add_library(discnav ALIAS PkgConfig::DISCNAV)
else()
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../libdiscnav ${CMAKE_CURRENT_BINARY_DIR}/libdiscnav EXCLUDE_FROM_ALL)
endif()

qt_standard_project_setup(REQUIRES 6.5)

# UDF reader (vendored, LGPL): disc files without mounting the disc
add_library(udfread STATIC
    third_party/libudfread/src/udfread.c
    third_party/libudfread/src/ecma167.c
    third_party/libudfread/src/default_blockinput.c)
target_compile_definitions(udfread PRIVATE
    _ISOC99_SOURCE _POSIX_C_SOURCE=200809L HAVE_UNISTD_H HAVE_FCNTL_H HAVE_PTHREAD_H)
target_include_directories(udfread PUBLIC third_party/libudfread/src)
set_target_properties(udfread PROPERTIES POSITION_INDEPENDENT_CODE ON)

qt_add_executable(playser src/main.cpp)

# must precede qt_add_qml_module, which reads it when generating qmldir
set_source_files_properties(qml/Theme.qml qml/Resume.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(playser
    URI Playser
    VERSION 1.0
    SOURCES
        src/Mpv.h src/Mpv.cpp
        src/UiRenderer.h src/UiRenderer.cpp
        src/OverlayEncoder.h src/OverlayEncoder.cpp
        src/ScreenSaverInhibitor.h src/ScreenSaverInhibitor.cpp
        src/DiscMonitor.h src/DiscMonitor.cpp
        src/DiscIndex.h src/DiscIndex.cpp
        src/DiscFs.h src/DiscFs.cpp
        src/DiscNav.h src/DiscNav.cpp
        src/SoundEffects.h src/SoundEffects.cpp
        src/StereoBadge.h src/StereoBadge.cpp
        src/SystemCheck.h src/SystemCheck.cpp
        src/Gamepad.h src/Gamepad.cpp
        src/MediaFiles.h src/MediaFiles.cpp
        src/MediaServer.h src/MediaServer.cpp
        src/PlexClient.h src/PlexClient.cpp
        src/JellyfinClient.h src/JellyfinClient.cpp
    QML_FILES
        qml/Main.qml
        qml/HomeScreen.qml
        qml/PlayerScreen.qml
        qml/TrackPicker.qml
        qml/Theme.qml
        qml/Resume.qml
        qml/DiscScreen.qml
        qml/FilesScreen.qml
        qml/LibraryScreen.qml
        qml/JellyfinSignIn.qml
        qml/OnScreenKeyboard.qml
        qml/SetupScreen.qml
        qml/ButtonGlyph.qml
        qml/HintBar.qml
        qml/ConfirmDialog.qml
)

target_include_directories(playser PRIVATE src)
target_compile_definitions(playser PRIVATE PLAYSER_VERSION="${PROJECT_VERSION}")

# offline disc catalogue from TheDiscDB (tools/build-discdb-index.py)
qt_add_resources(playser discdb
    PREFIX /
    BASE data
    FILES data/discdb-index.json.gz)

# UI font (Inter, SIL Open Font License)
qt_add_resources(playser fonts
    PREFIX /
    BASE data
    FILES data/fonts/InterVariable.ttf)

target_link_libraries(playser PRIVATE
    Qt6::Quick Qt6::DBus Qt6::Network PkgConfig::MPV PkgConfig::SDL3 ZLIB::ZLIB udfread discnav)

include(GNUInstallDirs)
install(TARGETS playser RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# GPL-3.0-or-later: the repo's COPYING (the Flatpak copies it next to app/'s files)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/COPYING)
    install(FILES COPYING DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/playser)
else()
    install(FILES ../COPYING DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/playser)
endif()
install(FILES data/THEDISCDB-LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/playser)
install(FILES data/fonts/INTER-LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/playser)
install(FILES third_party/libudfread/COPYING DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/playser RENAME LIBUDFREAD-LICENSE)
install(FILES data/io.github.slaapliedje.Playser.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/io.github.slaapliedje.Playser.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(FILES data/icons/io.github.slaapliedje.Playser.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
