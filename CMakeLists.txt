cmake_minimum_required(VERSION 3.21)

project(loftail
    VERSION 0.1.0
    DESCRIPTION "A desktop GUI viewer for log4cplus logs"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Qt 6.4's headers (e.g. qvarlengtharray.h) use stdext::checked_array_iterator,
# a Microsoft STL extension that recent MSVC toolsets compile out by default
# (_HAS_DEPRECATED_STDEXT_ARR_ITERS now defaults to 0), causing "'stdext':
# identifier not found". Re-enable it so the reference Qt 6.4 builds with a
# modern MSVC. MSVC-only; harmless elsewhere. Remove once the Qt floor is >6.4.
if(MSVC)
    add_compile_definitions(
        _HAS_DEPRECATED_STDEXT_ARR_ITERS=1
        _SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING)
endif()

# Target Qt 6.4 (the version in Ubuntu 24.04's repos); dev machines may have newer.
# See ARCHITECTURE.md §1.
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Widgets Test)

qt_standard_project_setup()

enable_testing()

add_subdirectory(src)
add_subdirectory(tests)
