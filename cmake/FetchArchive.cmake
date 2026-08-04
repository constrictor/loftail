# Build libarchive and the two codecs loftail needs, from source, statically.
#
# Only reached when LOFTAIL_ARCHIVE_FETCH=ON, which is the Windows CI job and nothing
# else: that runner has no package manager, and every other build should either find a
# system libarchive or do without (the reference build installs nothing — §1).
#
# THE AWKWARD PART, and the reason this is a file of its own: libarchive is an
# old-style CMake project that calls find_package(ZLIB) / find_package(LibLZMA) and
# consumes the *_INCLUDE_DIR / *_LIBRARY variables those set. A FetchContent-built
# codec defines targets, not those variables, so each one is advertised back through
# CMAKE_FIND_PACKAGE_REDIRECTS_DIR — the documented hook for exactly this, and the
# reason the floor here is CMake 3.24 rather than the project's 3.21. Without it
# libarchive configures cleanly and silently builds with no compression at all.
#
# Codec set: zlib (.gz, and zip's deflate) and liblzma (.xz). NOT bzip2 or zstd — they
# would be two more trees for formats a rotated log is rarely written in. So a Windows
# build opens .gz .zip .tar .tar.gz .tgz .tar.xz .txz .xz, and SPEC.md says so.

include(FetchContent)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# These are third-party trees whose own cmake_minimum_required() predates CMake 4,
# which removed compatibility with < 3.5 outright. Without this the configure fails on
# the dependency rather than on anything of ours, and it fails only on machines with a
# new CMake — so it would have gone unnoticed until a runner image was updated.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# A fetched dependency's own test suite must not join ours. Without this, `ctest` in a
# LOFTAIL_ARCHIVE_FETCH build runs xz's 19 tests alongside loftail's — slower, confusing
# to read, and able to fail the pipeline over something that is not our code. Our own
# tests call add_test() directly and are unaffected.
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(XZ_TESTS OFF CACHE BOOL "" FORCE)

# --- zlib ------------------------------------------------------------------
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zlib
    GIT_REPOSITORY https://github.com/madler/zlib.git
    GIT_TAG        v1.3.1
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(zlib)

file(WRITE "${CMAKE_FIND_PACKAGE_REDIRECTS_DIR}/zlib-extra.cmake" "
if(NOT TARGET ZLIB::ZLIB)
    add_library(ZLIB::ZLIB ALIAS zlibstatic)
endif()
set(ZLIB_INCLUDE_DIR  \"${zlib_SOURCE_DIR};${zlib_BINARY_DIR}\")
set(ZLIB_INCLUDE_DIRS \"\${ZLIB_INCLUDE_DIR}\")
set(ZLIB_LIBRARY      zlibstatic)
set(ZLIB_LIBRARIES    zlibstatic)
set(ZLIB_FOUND        TRUE)
")

# --- liblzma ---------------------------------------------------------------
set(XZ_TOOL_XZ OFF CACHE BOOL "" FORCE)      # the library only; no CLI tools
set(XZ_TOOL_XZDEC OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_LZMADEC OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_LZMAINFO OFF CACHE BOOL "" FORCE)
set(XZ_DOC OFF CACHE BOOL "" FORCE)
FetchContent_Declare(liblzma
    GIT_REPOSITORY https://github.com/tukaani-project/xz.git
    GIT_TAG        v5.6.3
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(liblzma)

file(WRITE "${CMAKE_FIND_PACKAGE_REDIRECTS_DIR}/liblzma-extra.cmake" "
set(LIBLZMA_INCLUDE_DIR       \"${liblzma_SOURCE_DIR}/src/liblzma/api\")
set(LIBLZMA_INCLUDE_DIRS      \"\${LIBLZMA_INCLUDE_DIR}\")
set(LIBLZMA_LIBRARY           liblzma::liblzma)
set(LIBLZMA_LIBRARIES         liblzma::liblzma)
set(LIBLZMA_HAS_AUTO_DECODER  TRUE)
set(LIBLZMA_HAS_EASY_ENCODER  TRUE)
set(LIBLZMA_HAS_LZMA_PRESET   TRUE)
set(LIBLZMA_FOUND             TRUE)
")

# --- libarchive ------------------------------------------------------------
# Everything off but the two codecs above and the formats we read, for the reason
# ENABLE_ZLIB_COMPRESSION=OFF is forced on libssh2: nothing new for windeployqt to
# carry, and nothing built that is never called.
# WERROR among them, deliberately: this is somebody else's source tree, and a compiler
# newer than the one they release-tested against must not be able to break OUR build.
# (GCC 15 flags several -Wdiscarded-qualifiers in libarchive 3.8.1 that its own CI does
# not see.) The same reasoning as the feature switches — build only what we call, and
# never inherit a third party's strictness.
foreach(feature BZip2 LZ4 ZSTD LZO LZMADEC OPENSSL LIBB2 LIBXML2 EXPAT ICONV
                MBEDTLS NETTLE PCREPOSIX CNG TAR CPIO CAT UNZIP RPM2CPIO TEST
                ACL XATTR COVERAGE INSTALL WERROR)
    set(ENABLE_${feature} OFF CACHE BOOL "" FORCE)
endforeach()
set(ENABLE_ZLIB ON CACHE BOOL "" FORCE)
set(ENABLE_LZMA ON CACHE BOOL "" FORCE)

FetchContent_Declare(libarchive
    GIT_REPOSITORY https://github.com/libarchive/libarchive.git
    GIT_TAG        v3.8.1
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(libarchive)

if(TARGET archive_static)
    set(LOFTAIL_ARCHIVE_TARGET archive_static)
    # libarchive's public header declares its whole API __declspec(dllimport) unless
    # LIBARCHIVE_STATIC is defined. libarchive sets that PRIVATE for its own build and
    # never exports it, so a consumer linking the STATIC library compiles calls to
    # __imp_archive_* import stubs that exist in no DLL — and gets a wall of LNK2019
    # only on Windows, only in the from-source configuration. Put it on the target's
    # INTERFACE so every consumer inherits it, including the test binaries that call
    # libarchive's write side directly.
    target_compile_definitions(archive_static INTERFACE LIBARCHIVE_STATIC)
elseif(TARGET archive)
    set(LOFTAIL_ARCHIVE_TARGET archive)
endif()
