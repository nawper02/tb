cmake_minimum_required(VERSION 3.10)
project(tb VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(NCURSES REQUIRED ncursesw)

add_executable(tb tb.cpp)

target_include_directories(tb PRIVATE ${NCURSES_INCLUDE_DIRS})
target_link_libraries(tb PRIVATE ${NCURSES_LIBRARIES})

include(GNUInstallDirs)
install(TARGETS tb DESTINATION ${CMAKE_INSTALL_BINDIR})
