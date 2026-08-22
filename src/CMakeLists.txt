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
endif()
