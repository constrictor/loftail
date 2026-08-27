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

# src/core holds all UI-free logic (unit-testable without a QApplication);
# src/ui holds the Qt Widgets layer.
add_subdirectory(core)
add_subdirectory(ui)

# M2a throwaway performance harness (console). The exact-geometry scrolling
# prototype (src/proto) has been retired now that the production LogView (src/ui)
# exists; the bench harness is kept because it still measures the §11 targets.
add_subdirectory(bench)

qt_add_executable(loftail
    main.cpp
)

target_link_libraries(loftail PRIVATE
    loftail_core
    loftail_ui
)

# -DLOFTAIL_CLANG_TIDY=ON puts clang-tidy on the compile line of these three and
# NOTHING else. Per target rather than the global CMAKE_CXX_CLANG_TIDY, and the
# top-level block that computes LOFTAIL_CLANG_TIDY_COMMAND says why: unlike a
# sanitizer, clang-tidy needs no agreement between the TUs of one binary, so the
# 79 test targets and bench_index stay out of it. `clang_tidy_all` is the target
# for sweeping tests/ as well.
if(LOFTAIL_CLANG_TIDY)
    set_property(TARGET loftail_core loftail_ui loftail
                 PROPERTY CXX_CLANG_TIDY "${LOFTAIL_CLANG_TIDY_COMMAND}")
endif()

# --- Packaging / install layout (PLAN.md M7) -------------------------------
#
# These rules give every platform a standard install tree that the deploy
# tools (linuxdeploy / windeployqt / macdeployqt) run against. See
# packaging/README.md for how each artifact is built and clean-machine-verified.
#
# File association is a soft/capability declaration only (the .desktop MimeType /
# Info.plist LSHandlerRank=Alternate carried by the packaging assets): loftail
# offers itself under "Open With" but never claims the DEFAULT handler. Making it
# the default stays an installer/packager/user action (PLAN.md M7). This install
# step just places the .desktop file; it registers nothing.

set(LOFTAIL_DESKTOP_FILE "${CMAKE_SOURCE_DIR}/packaging/linux/loftail.desktop")
set(LOFTAIL_ICON_FILE    "${CMAKE_SOURCE_DIR}/packaging/linux/loftail.svg")

if(APPLE)
    # macOS: build a relocatable .app bundle. Info.plist is generated from the
    # template in packaging/macos so macdeployqt has a proper bundle to sign.
    set_target_properties(loftail PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "loftail"
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.loftail.loftail"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        # Substituted into NSHumanReadableCopyright by the template below, which is
        # the only place a macOS bundle states its licence terms before it is opened.
        MACOSX_BUNDLE_COPYRIGHT "Copyright (C) 2026 Valentyn Pavliuchenko. GPL-3.0-or-later."
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/packaging/macos/Info.plist.in"
    )
    install(TARGETS loftail BUNDLE DESTINATION .)
elseif(WIN32)
    # Windows: install the bare executable; windeployqt copies the Qt runtime
    # next to it (see packaging/windows/build-portable.ps1).
    install(TARGETS loftail RUNTIME DESTINATION .)
else()
    # Linux / other Unix: FHS-style layout that AppImage tooling understands.
    include(GNUInstallDirs)
    install(TARGETS loftail RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES "${LOFTAIL_DESKTOP_FILE}"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
    install(FILES "${LOFTAIL_ICON_FILE}"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps")
    # The licence, in the two spellings the two consumers of this layout look for.
    # GPLv3 §4 requires the terms to travel with the binary, and there is exactly
    # one install layout: deb.cmake says these rules are what CPack packs, and
    # build-appimage.sh stages the AppDir through `cmake --install`, so both
    # artifacts are covered here and neither needs a copy step of its own.
    #
    # `copyright` and not just LICENSE, because that exact path is what lintian's
    # copyright-file-missing checks for and what a Debian user reads; it is DEP-5,
    # so it also names the licences of the libraries the package links.
    install(FILES "${CMAKE_SOURCE_DIR}/LICENSE"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/loftail")
    install(FILES "${CMAKE_SOURCE_DIR}/packaging/linux/copyright"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/loftail")
endif()
