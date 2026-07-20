cmake_minimum_required(VERSION 3.21)

project(loftail
    VERSION 0.1.0
    DESCRIPTION "A desktop GUI viewer for log4cplus logs"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Target Qt 6.4 (the version in Ubuntu 24.04's repos); dev machines may have newer.
# See ARCHITECTURE.md §1.
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Widgets Test)

qt_standard_project_setup()

enable_testing()

add_subdirectory(src)
add_subdirectory(tests)
