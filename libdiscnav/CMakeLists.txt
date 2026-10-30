cmake_minimum_required(VERSION 3.21)
project(discnav VERSION 0.1 LANGUAGES C)

# libdiscnav: DVD menus for stream-fed players (LGPL-2.1-or-later). Built
# as a static library; the app uses it from the source tree, the Flatpak
# installs it as a module (discnav.pc).

find_package(PkgConfig REQUIRED)
pkg_check_modules(DVDNAV REQUIRED IMPORTED_TARGET dvdnav)
find_package(Threads REQUIRED)

add_library(discnav STATIC discnav.c spu.c)
set_target_properties(discnav PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON
                                         PUBLIC_HEADER discnav.h)
target_include_directories(discnav PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
target_compile_options(discnav PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(discnav PUBLIC PkgConfig::DVDNAV Threads::Threads)

include(GNUInstallDirs)
install(TARGETS discnav ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
                        PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/discnav)
configure_file(discnav.pc.in discnav.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/discnav.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
install(FILES COPYING DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/discnav)
