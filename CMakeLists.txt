# loftail — a desktop viewer for log4cplus logs.
# Copyright (C) 2026 Valentyn Pavliuchenko
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.21)

project(loftail
    VERSION 0.1.0
    DESCRIPTION "A desktop GUI viewer for log4cplus logs"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# clang-tidy, clangd and every other libtooling consumer read compile_commands.json.
# On unconditionally because it is free with Ninja and Makefiles, ignored by the
# Visual Studio generator, and the file is already gitignored. See the clang-tidy
# block at the bottom of this file.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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

# --- M14: the OS keychain for remembered passwords, via QtKeychain ---------
# Auto-detected and never REQUIRED, like the two above. Without it loftail builds, tests
# and runs exactly as before, and a password the user asks to keep falls back to the
# plain-text file it has always used — named in the dialog, so the fallback is something
# the user is told about rather than something chosen behind their back (SPEC.md §3).
#
# One dependency covers all four backends — KWallet and GNOME Keyring through the
# freedesktop Secret Service, the Windows Credential Manager, the macOS Keychain — which
# is why it beats writing three platform implementations against libsecret, wincred and
# Security.framework, on two platforms this project cannot currently test.
option(LOFTAIL_WITH_KEYCHAIN "Store remembered SSH passwords in the OS keychain (needs QtKeychain)" ON)
# Off by default so ordinary and offline builds stay hermetic, exactly like
# LOFTAIL_SSH_FETCH. The Windows CI job turns it on; see the argument below.
option(LOFTAIL_KEYCHAIN_FETCH "Build QtKeychain from source if it is not installed" OFF)

set(LOFTAIL_HAVE_KEYCHAIN OFF)
if(LOFTAIL_WITH_KEYCHAIN)
    # 0.14 is the floor, and a real one: QKeychain::isAvailable() arrived there, and it is
    # the cheap negative the availability probe starts from. Both Ubuntu legs clear it
    # (24.04 ships 0.14.2, 26.04 ships 0.15.0).
    find_package(Qt6Keychain 0.14 CONFIG QUIET)
    if(TARGET Qt6Keychain::Qt6Keychain)
        set(LOFTAIL_KEYCHAIN_TARGET Qt6Keychain::Qt6Keychain)
    endif()

    if(NOT LOFTAIL_KEYCHAIN_TARGET)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(QT6KEYCHAIN QUIET IMPORTED_TARGET qt6keychain)
            if(TARGET PkgConfig::QT6KEYCHAIN)
                set(LOFTAIL_KEYCHAIN_TARGET PkgConfig::QT6KEYCHAIN)
            endif()
        endif()
    endif()

    # A build-from-source fallback IS warranted here, unlike libarchive, and the reason is
    # the mirror image of libarchive's. libarchive has none because it is only useful WITH
    # codecs it locates in MODULE mode, so a source build silently produces a library that
    # reports itself found and cannot decompress anything. QtKeychain has no codecs and no
    # equivalent trap: on Windows it is a thin wrapper over the Credential Store with no
    # external dependency at all, so a source build there is complete by construction —
    # the libssh2/WinCNG situation exactly.
    #
    # And the Windows CI job NEEDS it: that runner has no package manager, and vcpkg's
    # qtkeychain-qt6 port depends on vcpkg's OWN qtbase and qttools. Installing it would
    # build Qt from source for hours and then sit beside the install-qt-action Qt the rest
    # of the job uses — two Qt6Cores, one ABI.
    if(NOT LOFTAIL_KEYCHAIN_TARGET AND LOFTAIL_KEYCHAIN_FETCH)
        include(FetchContent)
        # NOT the default. QtKeychain still builds Qt5 unless told otherwise, and the
        # else branch is a `find_package(Qt5 COMPONENTS Core REQUIRED)` — on a runner
        # with only Qt6 that is a hard configure failure, not a fallback.
        set(BUILD_WITH_QT6 ON CACHE BOOL "" FORCE)
        set(BUILD_TEST_APPLICATION OFF CACHE BOOL "" FORCE)
        set(BUILD_QTQUICK_DEMO OFF CACHE BOOL "" FORCE)
        # Translations need Qt LinguistTools, which the reference Qt install may not carry
        # and which loftail does not ship anyway.
        set(BUILD_TRANSLATIONS OFF CACHE BOOL "" FORCE)
        set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE) # no extra DLL in the portable zip
        # The tag is `0.15.0`, NOT `v0.15.0`: upstream dropped the `v` prefix at 0.14.1,
        # so the older tags in that repository carry one and every tag this build could
        # want does not. A wrong tag fails at the clone with `invalid reference`, which
        # is a configure error long before any of the options above are read — which is
        # how this whole block shipped in M14 without ever executing once.
        FetchContent_Declare(qtkeychain
            GIT_REPOSITORY https://github.com/frankosterfeld/qtkeychain.git
            GIT_TAG 0.15.0
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(qtkeychain)
        # Both spellings are probed because an installed QtKeychain exports the namespaced
        # alias while its own build tree defines the lowercase target, and which one a
        # given tag provides is not worth depending on.
        if(TARGET Qt6Keychain::Qt6Keychain)
            set(LOFTAIL_KEYCHAIN_TARGET Qt6Keychain::Qt6Keychain)
        elseif(TARGET qt6keychain)
            set(LOFTAIL_KEYCHAIN_TARGET qt6keychain)
        endif()
    endif()

    if(LOFTAIL_KEYCHAIN_TARGET)
        set(LOFTAIL_HAVE_KEYCHAIN ON)
    endif()
endif()

if(LOFTAIL_HAVE_KEYCHAIN)
    message(STATUS "OS keychain for passwords: ENABLED (${LOFTAIL_KEYCHAIN_TARGET})")
else()
    message(STATUS "OS keychain for passwords: DISABLED "
                   "(QtKeychain not found; install qtkeychain-qt6-dev, brew install "
                   "qtkeychain, or configure with -DLOFTAIL_KEYCHAIN_FETCH=ON — a "
                   "remembered password then falls back to plain text in hosts.json)")
endif()

# --- Presets (M5) -------------------------------------------------------------
#
# Unlike LOFTAIL_WITH_SSH/_ARCHIVE/_KEYCHAIN, this gates a FEATURE rather than a
# dependency, which is why it is the one that defaults OFF. Those three mean "auto-detect
# this library if it is installed", so ON is the honest default and LOFTAIL_HAVE_* is the
# probe's ANSWER. Here there is nothing to probe: the option IS the answer, and HAVE only
# mirrors it so that every sub-CMakeLists keeps reading `if(LOFTAIL_HAVE_...)`.
#
# Off by default means a stock build has no Presets pane, none of the preset code compiled
# and no preset files read or written (SPEC.md §9).
option(LOFTAIL_WITH_PRESETS "Build the filter and highlighter presets pane" OFF)
set(LOFTAIL_HAVE_PRESETS ${LOFTAIL_WITH_PRESETS})

if(LOFTAIL_HAVE_PRESETS)
    message(STATUS "Presets: ENABLED")
else()
    message(STATUS "Presets: DISABLED "
                   "(configure with -DLOFTAIL_WITH_PRESETS=ON to build the presets pane)")
endif()

# --- Build identity -----------------------------------------------------------
#
# Which BUILD a binary is, as distinct from which RELEASE it belongs to. The two are
# deliberately separate: PROJECT_VERSION is typed at the top of this file and names the
# release, so it is what the .deb, the AppImage and the macOS bundle carry in their
# filenames and metadata — where a suffix would be actively harmful, since the deb job
# asserts on the shape of that filename and two releases' packages must not collide.
# The build id below names the one CI run that produced this particular binary, and is
# appended to --version and nowhere else.
#
# Both are EMPTY BY DEFAULT, so a developer build prints a bare "loftail 0.1.0" exactly
# as it always has. That is the point rather than a concession: a downloaded artifact
# says which run built it, and a local build says it is a local build.
#
# Nothing is read from git here. A configure-time `git describe` goes stale the moment
# the tree moves under an already-configured build directory, and it would make git a
# build dependency for a source tarball that has no .git in it at all.
#
# The default comes from the ENVIRONMENT so that every configure in the packaging
# workflow is stamped by one job-level `env:` block, including the two that run inside
# packaging scripts (build-appimage.sh, build-appbundle.sh) whose cmake command lines
# no caller can reach. -D still overrides. Being a cache variable, the value is fixed at
# first configure and a reconfigure keeps it; CI configures a fresh build directory every
# run and caches none, so it cannot go stale there.
set(LOFTAIL_BUILD_NUMBER "$ENV{LOFTAIL_BUILD_NUMBER}" CACHE STRING
    "CI run number stamped into --version; empty for a local build")
set(LOFTAIL_BUILD_COMMIT "$ENV{LOFTAIL_BUILD_COMMIT}" CACHE STRING
    "Commit the build was made from, stamped into --version; empty for a local build")

set(loftail_build_parts "")
if(LOFTAIL_BUILD_NUMBER)
    list(APPEND loftail_build_parts "${LOFTAIL_BUILD_NUMBER}")
endif()
if(LOFTAIL_BUILD_COMMIT)
    # Abbreviated HERE rather than by the caller, because the caller is a GitHub
    # expression: `github.sha` is 40 characters and that syntax has no substring
    # function, so doing it in CMake keeps one rule in one place instead of the same
    # incantation written once for bash and once for PowerShell. Guarded rather than
    # trusting SUBSTRING to clamp, since a caller may reasonably pass a short sha.
    string(LENGTH "${LOFTAIL_BUILD_COMMIT}" loftail_commit_length)
    if(loftail_commit_length GREATER 7)
        string(SUBSTRING "${LOFTAIL_BUILD_COMMIT}" 0 7 loftail_short_commit)
    else()
        set(loftail_short_commit "${LOFTAIL_BUILD_COMMIT}")
    endif()
    list(APPEND loftail_build_parts "g${loftail_short_commit}")
endif()
# Joined with '.' and consumed as SemVer build metadata — everything after the first
# '+' is defined to be ignored when versions are compared, so "0.1.0+1234.g1a2b3c4"
# still names release 0.1.0 to anything that parses it. One token, no spaces: on
# Windows --version is printed into a MessageBox when there is no console to attach
# to (src/main.cpp), and every smoke test in the workflow greps the line.
list(JOIN loftail_build_parts "." LOFTAIL_BUILD_ID)

if(LOFTAIL_BUILD_ID)
    message(STATUS "Build identity: ${PROJECT_VERSION}+${LOFTAIL_BUILD_ID}")
else()
    message(STATUS "Build identity: ${PROJECT_VERSION} (local build; set "
                   "LOFTAIL_BUILD_NUMBER / LOFTAIL_BUILD_COMMIT to stamp one)")
endif()

# --- Translation tooling ------------------------------------------------------
#
# loftail ships NO translations. Every user-facing string goes through tr() so that one
# can be added later without touching the code (CLAUDE.md, "Localization"), and this
# target is how the .ts files would be produced when somebody does.
#
# Optional and gated like every other extra, and for a sharper version of the same
# reason: the reference build is a stock Ubuntu 24.04 toolchain with nothing extra
# installed, and Qt6::LinguistTools comes from a separate package (qt6-tools-dev-tools)
# that has no business being required to build a viewer that is English-only anyway.
# Nothing depends on this target; it is run by hand.
find_package(Qt6 6.4 QUIET COMPONENTS LinguistTools)
if(Qt6LinguistTools_FOUND)
    message(STATUS "Translation tooling: ENABLED (lupdate available as target loftail_lupdate)")
    file(GLOB_RECURSE loftail_tr_sources CONFIGURE_DEPENDS
         "${CMAKE_SOURCE_DIR}/src/*.cpp" "${CMAKE_SOURCE_DIR}/src/*.h")
    add_custom_target(loftail_lupdate
        COMMAND Qt6::lupdate -locations relative -no-obsolete
                ${loftail_tr_sources}
                -ts "${CMAKE_SOURCE_DIR}/translations/loftail_en.ts"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Extracting translatable strings into translations/loftail_en.ts"
        VERBATIM
    )
else()
    message(STATUS "Translation tooling: DISABLED "
                   "(Qt6 LinguistTools not found; install qt6-tools-dev-tools to get the "
                   "loftail_lupdate target — no translations ship, so nothing else needs it)")
endif()

# ---------------------------------------------------------------------------
# Sanitizers (ARCHITECTURE.md §13). Off by default; a separate build directory,
# never a flag you flip on the one you develop in.
#
#   -DLOFTAIL_SANITIZE=address   ASan + UBSan + LeakSanitizer
#   -DLOFTAIL_SANITIZE=thread    ThreadSanitizer
#
# Applied at the top level with add_compile_options/add_link_options rather than on
# a target, because a sanitizer is only sound when EVERY translation unit linked into
# a binary agrees — core, ui and all 55 test binaries. Mixing instrumented and
# uninstrumented objects is the one way to get results that are worse than none.
#
# address and thread are mutually exclusive: ASan and TSan both want the shadow
# memory mapping and refuse to share it.
set(LOFTAIL_SANITIZE "" CACHE STRING
    "Build with a sanitizer: address (ASan+UBSan+LSan), thread (TSan), or empty")
set_property(CACHE LOFTAIL_SANITIZE PROPERTY STRINGS "" address thread)

if(LOFTAIL_SANITIZE STREQUAL "address")
    # UBSan rides along with ASan because it is nearly free and catches a different
    # class entirely. -fno-sanitize-recover makes UBSan ABORT rather than print and
    # carry on: a report ctest does not turn into a failure is a report nobody reads.
    set(loftail_sanitize_flags
        -fsanitize=address,undefined
        -fno-sanitize-recover=undefined
        -fno-omit-frame-pointer)
elseif(LOFTAIL_SANITIZE STREQUAL "thread")
    set(loftail_sanitize_flags -fsanitize=thread -fno-omit-frame-pointer)
elseif(NOT LOFTAIL_SANITIZE STREQUAL "")
    message(FATAL_ERROR
        "LOFTAIL_SANITIZE must be 'address', 'thread' or empty; got '${LOFTAIL_SANITIZE}'")
endif()

if(loftail_sanitize_flags)
    if(MSVC)
        # MSVC has /fsanitize=address only, with different spelling and no UBSan or
        # TSan at all. Say so rather than emitting flags cl.exe will reject.
        message(FATAL_ERROR
            "LOFTAIL_SANITIZE is not supported with MSVC; use the Linux or macOS build")
    endif()
    add_compile_options(${loftail_sanitize_flags})
    add_link_options(${loftail_sanitize_flags})
    message(STATUS "Sanitizer: ${LOFTAIL_SANITIZE} (${loftail_sanitize_flags})")
else()
    message(STATUS "Sanitizer: none (-DLOFTAIL_SANITIZE=address or =thread to enable)")
endif()

# ---------------------------------------------------------------------------
# clang-tidy. Off by default; like the sanitizers, a build directory of its own
# rather than a flag you flip on the one you develop in.
#
#   -DLOFTAIL_CLANG_TIDY=ON       lint src/ as part of the build
#   -DLOFTAIL_CLANG_TIDY_EXE=...  name the binary (versions disagree; see below)
#
# The checks live in .clang-tidy at the repo root, which BOTH routes read: this
# option, and the standalone clang_tidy / clang_tidy_all targets below.
#
# Two things here are easy to undo.
#
# The property is set PER TARGET, on loftail_core/loftail_ui/loftail only (see
# src/CMakeLists.txt), and NOT through the global CMAKE_CXX_CLANG_TIDY. That is a
# deliberate divergence from the sanitizer block directly above, whose comment says
# the opposite: a sanitizer is only sound when every TU linked into a binary agrees,
# so it must be global. clang-tidy has no such requirement, and going global would
# put 79 test binaries and bench_index through the static analyzer as well, roughly
# doubling the cost for a scope neither the CI job nor the default target covers.
#
# And a `-*` config is written into the BUILD tree. Qt's AUTOMOC generates a
# mocs_compilation.cpp per target under <binary>/<target>_autogen/, which
# CXX_CLANG_TIDY lints like any other source — findings in a file nobody wrote and
# nobody can fix. clang-tidy resolves its config by walking UP from the input file's
# own directory, so a config at the root of the build tree switches the generated
# TUs off while src/ still reaches the real one at the repo root. (SKIP_LINTING on
# the generated source is the other route, and it is CMake 3.27 — above this
# project's 3.21 floor.)
option(LOFTAIL_CLANG_TIDY "Run clang-tidy over src/ as part of the build" OFF)

# Named rather than searched-for because the versions genuinely disagree: Ubuntu
# 24.04 (the reference environment, ARCHITECTURE.md §1) ships clang-tidy 18, and a
# dev machine may have anything. A finding present under one and absent under the
# other is expected; pinning is how two machines get the same answer.
set(LOFTAIL_CLANG_TIDY_EXE "" CACHE STRING
    "clang-tidy executable to use; empty searches the usual names")

if(LOFTAIL_CLANG_TIDY_EXE)
    set(loftail_tidy_names ${LOFTAIL_CLANG_TIDY_EXE})
else()
    set(loftail_tidy_names
        clang-tidy
        clang-tidy-21 clang-tidy-20 clang-tidy-19 clang-tidy-18)
endif()
# NO_CACHE, which is exactly the 3.21 floor: a cached result would not be re-searched
# when LOFTAIL_CLANG_TIDY_EXE is added to an existing build directory, so naming a
# version would silently keep using whichever one the first configure happened to find.
find_program(LOFTAIL_CLANG_TIDY_BIN NAMES ${loftail_tidy_names} NO_CACHE)

if(LOFTAIL_CLANG_TIDY)
    if(NOT LOFTAIL_CLANG_TIDY_BIN)
        # Refuse loudly rather than build without it: an option that silently did
        # nothing would go green and prove nothing, which is the sanitizer block's
        # rule one section up.
        message(FATAL_ERROR
            "-DLOFTAIL_CLANG_TIDY=ON but no clang-tidy was found (looked for "
            "${loftail_tidy_names}); install clang-tidy, or name one with "
            "-DLOFTAIL_CLANG_TIDY_EXE=/path/to/clang-tidy")
    endif()
    # --quiet drops the "N warnings generated" line per TU, which is one line of
    # noise per file in a build log that already has one line per file.
    set(LOFTAIL_CLANG_TIDY_COMMAND "${LOFTAIL_CLANG_TIDY_BIN}" --quiet)
    file(WRITE "${CMAKE_BINARY_DIR}/.clang-tidy" "Checks: '-*'\n")
    message(STATUS "clang-tidy: ENABLED (${LOFTAIL_CLANG_TIDY_BIN}, src/ only)")
else()
    message(STATUS "clang-tidy: DISABLED "
                   "(-DLOFTAIL_CLANG_TIDY=ON to lint src/ during the build, or build "
                   "the clang_tidy target to sweep compile_commands.json instead)")
endif()

# The standalone sweep. Declared whether or not the option is on, because it costs
# nothing unbuilt and it is the cheaper of the two routes: it reads the compilation
# database an ordinary build already wrote, so it needs no second compile.
#
# The path regex is what keeps _deps/ and the *_autogen TUs out here — the build-tree
# config above is only needed by the compile-line route.
find_program(LOFTAIL_RUN_CLANG_TIDY_BIN
    NAMES run-clang-tidy run-clang-tidy-21 run-clang-tidy-20 run-clang-tidy-19
          run-clang-tidy-18 run-clang-tidy.py
    NO_CACHE)
if(LOFTAIL_RUN_CLANG_TIDY_BIN AND LOFTAIL_CLANG_TIDY_BIN)
    add_custom_target(clang_tidy
        COMMAND "${LOFTAIL_RUN_CLANG_TIDY_BIN}"
                -p "${CMAKE_BINARY_DIR}"
                -clang-tidy-binary "${LOFTAIL_CLANG_TIDY_BIN}"
                -quiet
                "^${CMAKE_SOURCE_DIR}/src/"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "clang-tidy over src/"
        USES_TERMINAL
        VERBATIM)
    add_custom_target(clang_tidy_all
        COMMAND "${LOFTAIL_RUN_CLANG_TIDY_BIN}"
                -p "${CMAKE_BINARY_DIR}"
                -clang-tidy-binary "${LOFTAIL_CLANG_TIDY_BIN}"
                -quiet
                "^${CMAKE_SOURCE_DIR}/(src|tests)/"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "clang-tidy over src/ and tests/"
        USES_TERMINAL
        VERBATIM)
else()
    # Say so rather than leaving `--target clang_tidy` to fail as "no rule to make
    # target", which reads as a broken build rather than a missing tool.
    message(STATUS "clang-tidy sweep targets: unavailable "
                   "(run-clang-tidy and clang-tidy must both be on PATH for the "
                   "clang_tidy / clang_tidy_all targets)")
endif()

# ---------------------------------------------------------------------------
# Warnings, and warnings as errors.
#
# Three things here are easy to undo.
#
# The block sits BELOW every FetchContent block above it, and must stay there.
# add_compile_options only reaches directories added AFTER the call, which is the one
# thing keeping libssh2 and QtKeychain — third-party sources this project neither
# writes nor can fix — out of a -Werror build. Hoist it up beside the other options
# and the Windows CI job (the one leg that configures with LOFTAIL_SSH_FETCH and
# LOFTAIL_KEYCHAIN_FETCH) starts failing on somebody else's code.
#
# It IS global, unlike the clang-tidy property directly above and like the sanitizer
# block: tests/ is where a warning most often lands first (every -Wextra finding this
# was introduced with but three was in a test), so a per-target list covering
# loftail_core/loftail_ui/loftail would gate the third of the tree that matters least.
# The exclusion above is by ORDERING, not by target, and that is the whole mechanism.
#
# And MSVC is deliberately left out of the default. The reference Qt is 6.4 with a
# modern MSVC — a combination that already needs the _HAS_DEPRECATED_STDEXT_ARR_ITERS
# workaround at the top of this file — and Windows CI is the only cross-platform check
# this project has, so a warning nobody on a Linux box can reproduce would gate every
# Windows build with no way to see it first. -DLOFTAIL_WERROR=ON there turns /WX on for
# anyone who wants it.
if(MSVC)
    set(loftail_werror_default OFF)
else()
    set(loftail_werror_default ON)
endif()
option(LOFTAIL_WERROR "Treat compiler warnings as errors"
       ${loftail_werror_default})

if(MSVC)
    # No /W4: MSVC's level 4 over Qt headers is untested here, and raising a level
    # nobody reads is noise. /WX applies to whatever level the generator sets.
    if(LOFTAIL_WERROR)
        add_compile_options(/WX)
    endif()
else()
    add_compile_options(-Wall -Wextra)
    if(LOFTAIL_WERROR)
        add_compile_options(-Werror)
    endif()
endif()

if(LOFTAIL_WERROR)
    # Named in the message because the gate CAN fire on a compiler or Qt this tree has
    # never been built against: the dev machine runs GCC 15 and Qt 6.10, the reference
    # build GCC 13 and Qt 6.4 (ARCHITECTURE.md §1), and the two disagree about what is
    # worth warning about — a deprecation added in Qt 6.8 does not exist on the floor.
    message(STATUS "Warnings as errors: ENABLED "
                   "(-DLOFTAIL_WERROR=OFF to build through a warning)")
else()
    message(STATUS "Warnings as errors: DISABLED "
                   "(-DLOFTAIL_WERROR=ON to fail the build on a warning)")
endif()

enable_testing()

add_subdirectory(src)
add_subdirectory(tests)

# Distro packaging. Included last, because CPack packs the install() rules the
# subdirectories above declare. It is a no-op off Linux; see packaging/README.md.
include(packaging/linux/deb.cmake)
