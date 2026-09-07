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

# Fuzz targets, and the corpus-replay binaries built from the same sources.
#
# TWO BINARIES PER TARGET, from one .cpp:
#
#   fuzz_<name>          the libFuzzer binary. Built only with -DLOFTAIL_FUZZ=ON,
#                        which needs Clang; libFuzzer supplies main().
#   tst_fuzzreplay_<name> the same LLVMFuzzerTestOneInput linked with ReplayMain.cpp,
#                        which walks the checked-in corpus. Built ALWAYS, in the
#                        ordinary GCC build, and registered as a CTest case.
#
# The second is the half that pays every day. A fuzzer finds an input; the input
# is minimised and committed under corpus/<name>/; from then on it is an ordinary
# regression test that costs milliseconds and needs no Clang, no libFuzzer and no
# instrumented build. Nothing about it depends on the option being on.
#
# The fuzz binaries are deliberately NOT add_test()ed. A CTest case runs to
# completion; a fuzzer runs until it is told to stop, and how long is a decision
# for whoever is running it (the CI smoke gate says 60 seconds; a person hunting a
# bug says hours). `--target fuzz` builds them all; CLAUDE.md's Commands section
# has the recipes.

set(loftail_fuzz_targets
    patterncompiler
    timestampparser
    decoder
    address
    indexer
)

# The corpus is read from the SOURCE tree, not copied into the build tree: a
# crash file dropped in by hand must be picked up by the next ctest run without a
# reconfigure, and the corpus is what a person edits.
set(LOFTAIL_FUZZ_CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus")

foreach(name IN LISTS loftail_fuzz_targets)
    # The replay binary. Plain add_executable rather than qt_add_executable: there
    # is no moc, no ui and no resource here, and the WIN32_EXECUTABLE/MACOSX_BUNDLE
    # properties the tests/ directory sets on its tst_ targets are set below for
    # the same reason — a GUI-subsystem process writes nothing to a redirected
    # stdout, so on Windows a failure would arrive as an empty block.
    add_executable(tst_fuzzreplay_${name}
        fuzz_${name}.cpp
        ReplayMain.cpp
    )
    target_link_libraries(tst_fuzzreplay_${name} PRIVATE loftail_core Qt6::Core)
    # MemoryLogSource.h and the other test scaffolding live one directory up.
    target_include_directories(tst_fuzzreplay_${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    set_target_properties(tst_fuzzreplay_${name} PROPERTIES
        WIN32_EXECUTABLE FALSE
        MACOSX_BUNDLE FALSE
    )
    add_test(NAME fuzz_replay_${name}
             COMMAND tst_fuzzreplay_${name} "${LOFTAIL_FUZZ_CORPUS}/${name}")
endforeach()

# The ENABLED/DISABLED configure line is printed by the LOFTAIL_FUZZ block in the
# root CMakeLists.txt, beside the sanitizer and coverage ones, because that is
# where the option and its toolchain check live.
if(NOT LOFTAIL_FUZZ)
    return()
endif()

add_custom_target(fuzz COMMENT "Building every libFuzzer target")

foreach(name IN LISTS loftail_fuzz_targets)
    add_executable(fuzz_${name} fuzz_${name}.cpp)
    target_link_libraries(fuzz_${name} PRIVATE loftail_core Qt6::Core)
    target_include_directories(fuzz_${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    # -fsanitize=fuzzer on the LINK only. The instrumentation every translation
    # unit needs (-fsanitize=fuzzer-no-link, plus ASan and UBSan) is applied at the
    # top level by the LOFTAIL_FUZZ block in the root CMakeLists.txt, for the
    # sanitizer block's reason: coverage feedback is only sound when every TU
    # linked into the binary agrees. This line is what turns that instrumentation
    # into a driver with libFuzzer's own main().
    target_link_options(fuzz_${name} PRIVATE -fsanitize=fuzzer)
    set_target_properties(fuzz_${name} PROPERTIES
        WIN32_EXECUTABLE FALSE
        MACOSX_BUNDLE FALSE
    )
    add_dependencies(fuzz fuzz_${name})
endforeach()
