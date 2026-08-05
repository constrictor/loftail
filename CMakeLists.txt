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
# Network is used only by the SSH transport (QTcpSocket, M11) and is optional for
# exactly that reason: a build without libssh2 must not start needing a new Qt module.
find_package(Qt6 6.4 QUIET COMPONENTS Network)

qt_standard_project_setup()

# ---------------------------------------------------------------------------
# Remote log sources over SSH (M11) — the project's only non-Qt dependency, and
# an OPTIONAL one. ARCHITECTURE.md §1 promises the reference build works with the
# stock Ubuntu toolchain and nothing separately installed, so libssh2 is detected,
# never REQUIRED: without it loftail builds, tests and runs exactly as before, and
# says plainly that remote logs are unavailable (RemoteSpool.cpp).
option(LOFTAIL_WITH_SSH "Build SSH remote log source support (needs libssh2)" ON)
# Off by default so ordinary and offline builds stay hermetic — no configure-time
# network. The Windows CI job turns it on, because that runner has no package manager.
option(LOFTAIL_SSH_FETCH "Build libssh2 from source if it is not installed" OFF)

set(LOFTAIL_HAVE_SSH OFF)
if(LOFTAIL_WITH_SSH)
    # libssh2 ships a CMake package config; Debian/Ubuntu's libssh2-1-dev installs it.
    find_package(Libssh2 CONFIG QUIET)
    if(TARGET Libssh2::libssh2_shared)
        set(LOFTAIL_SSH_TARGET Libssh2::libssh2_shared)
    elseif(TARGET Libssh2::libssh2_static)
        set(LOFTAIL_SSH_TARGET Libssh2::libssh2_static)
    elseif(TARGET Libssh2::libssh2)
        set(LOFTAIL_SSH_TARGET Libssh2::libssh2)
    endif()

    # Fall back to pkg-config, which is how libssh2 is found on distributions whose
    # package predates the CMake config.
    if(NOT LOFTAIL_SSH_TARGET)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(LIBSSH2 QUIET IMPORTED_TARGET libssh2)
            if(TARGET PkgConfig::LIBSSH2)
                set(LOFTAIL_SSH_TARGET PkgConfig::LIBSSH2)
            endif()
        endif()
    endif()

    if(NOT LOFTAIL_SSH_TARGET AND LOFTAIL_SSH_FETCH)
        # Static, with Windows' own CNG crypto: no OpenSSL anywhere, so the portable
        # zip needs no extra DLL and windeployqt stays untouched (PLAN.md M11 risk 2).
        include(FetchContent)
        set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
        set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
        set(ENABLE_ZLIB_COMPRESSION OFF CACHE BOOL "" FORCE)
        if(WIN32)
            set(CRYPTO_BACKEND "WinCNG" CACHE STRING "" FORCE)
        endif()
        FetchContent_Declare(libssh2
            GIT_REPOSITORY https://github.com/libssh2/libssh2.git
            GIT_TAG libssh2-1.11.1
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(libssh2)
        if(TARGET libssh2_static)
            set(LOFTAIL_SSH_TARGET libssh2_static)
        elseif(TARGET libssh2)
            set(LOFTAIL_SSH_TARGET libssh2)
        endif()
    endif()

    if(LOFTAIL_SSH_TARGET)
        set(LOFTAIL_HAVE_SSH ON)
    endif()
endif()

if(LOFTAIL_HAVE_SSH)
    message(STATUS "SSH remote sources: ENABLED (${LOFTAIL_SSH_TARGET})")
else()
    message(STATUS "SSH remote sources: DISABLED "
                   "(libssh2 not found; install libssh2-1-dev, or configure with "
                   "-DLOFTAIL_SSH_FETCH=ON to build it from source)")
endif()

# --- M12: compressed and archived logs, via libarchive ---------------------
# Auto-detected and never REQUIRED, for the reason §1 gives about the reference build:
# `apt install` nothing. One dependency covers .gz/.bz2/.xz/.zst, zip, tar and every
# compressed tar, which is why it beats zlib plus a hand-rolled container reader.
option(LOFTAIL_WITH_ARCHIVE "Build compressed/archived log support (needs libarchive)" ON)
# libarchive's own headers declare its API __declspec(dllimport) unless LIBARCHIVE_STATIC
# is defined, and libarchive sets that only for its own build — so a consumer of a
# STATIC libarchive must say so or link against import stubs that exist in no DLL.
# There is no reliable way to detect it (an imported .lib may be UNKNOWN_LIBRARY), so
# it is declared rather than guessed. Only the Windows CI job needs it, where a static
# libarchive is what keeps the portable zip free of extra DLLs.
option(LOFTAIL_ARCHIVE_STATIC "The libarchive being linked is a static library" OFF)

set(LOFTAIL_HAVE_ARCHIVE OFF)
if(LOFTAIL_WITH_ARCHIVE)
    # CMake ships FindLibArchive, so the common case needs nothing of our own. 3.2 is
    # the floor: archive_read_support_format_raw is what makes a bare .gz a one-member
    # archive, so the container and single-stream paths can be one piece of code.
    find_package(LibArchive 3.2 QUIET)
    if(TARGET LibArchive::LibArchive)
        set(LOFTAIL_ARCHIVE_TARGET LibArchive::LibArchive)
    endif()

    if(NOT LOFTAIL_ARCHIVE_TARGET)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(LIBARCHIVE QUIET IMPORTED_TARGET libarchive>=3.2)
            if(TARGET PkgConfig::LIBARCHIVE)
                set(LOFTAIL_ARCHIVE_TARGET PkgConfig::LIBARCHIVE)
            endif()
        endif()
    endif()

    # There is deliberately no build-from-source fallback here, unlike LOFTAIL_SSH_FETCH.
    # libarchive is only useful WITH its codecs, and it finds zlib and liblzma through
    # find_package in MODULE mode — which CMAKE_FIND_PACKAGE_REDIRECTS_DIR does not
    # reach, so a FetchContent-built codec is silently ignored and libarchive builds
    # with no compression at all while still reporting itself found. That failure is
    # invisible until a .gz refuses to open. A package manager (apt, brew, vcpkg —
    # the Windows CI job uses vcpkg) supplies the codecs as a matter of course.
    if(LOFTAIL_ARCHIVE_TARGET)
        set(LOFTAIL_HAVE_ARCHIVE ON)
    endif()
endif()

if(LOFTAIL_HAVE_ARCHIVE)
    message(STATUS "Archived log sources: ENABLED (${LOFTAIL_ARCHIVE_TARGET})")
else()
    message(STATUS "Archived log sources: DISABLED "
                   "(libarchive not found; install libarchive-dev, brew install "
                   "libarchive, or vcpkg install libarchive)")
endif()

enable_testing()

add_subdirectory(src)
add_subdirectory(tests)

# Distro packaging. Included last, because CPack packs the install() rules the
# subdirectories above declare. It is a no-op off Linux; see packaging/README.md.
include(packaging/linux/deb.cmake)
