cmake_minimum_required(VERSION 3.10)
project(td VERSION 1.0 LANGUAGES CXX)

# Use C++17 as required by <filesystem>
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find ncurses (Wide version preferred for TUI)
find_package(PkgConfig REQUIRED)
pkg_check_modules(NCURSES REQUIRED ncursesw)

# Define the executable
add_executable(td td.cpp)

# Include ncurses headers and link the library
target_include_directories(td PRIVATE ${NCURSES_INCLUDE_DIRS})
target_link_libraries(td PRIVATE ${NCURSES_LIBRARIES})

# Install rule: defaults to /usr/local/bin, but can be overridden
include(GNUInstallDirs)
install(TARGETS td DESTINATION ${CMAKE_INSTALL_BINDIR})
