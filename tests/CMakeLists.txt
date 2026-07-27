# One trivial test wiring CTest to Qt Test. Core logic is testable without a
# QApplication, so tests link loftail_core only (never the UI).
qt_add_executable(tst_scaffold
    tst_scaffold.cpp
)

target_link_libraries(tst_scaffold PRIVATE
    loftail_core
    Qt6::Test
)

add_test(NAME tst_scaffold COMMAND tst_scaffold)

# M1 — PatternCompiler. Table-driven, links loftail_core only (no QApplication).
qt_add_executable(tst_patterncompiler
    tst_patterncompiler.cpp
)

target_link_libraries(tst_patterncompiler PRIVATE
    loftail_core
    Qt6::Test
)

add_test(NAME tst_patterncompiler COMMAND tst_patterncompiler)

# M2a — the spine. All link loftail_core only; none needs a QApplication.
foreach(unit decoder indexer logmodel)
    qt_add_executable(tst_${unit} tst_${unit}.cpp)
    target_link_libraries(tst_${unit} PRIVATE loftail_core Qt6::Test)
    add_test(NAME tst_${unit} COMMAND tst_${unit})
endforeach()

# M2b — worker-thread indexing controller. Core-only, GUILESS (QCoreApplication
# event loop drives the queued cross-thread signals).
qt_add_executable(tst_indexcontroller tst_indexcontroller.cpp)
target_link_libraries(tst_indexcontroller PRIVATE loftail_core Qt6::Test)
add_test(NAME tst_indexcontroller COMMAND tst_indexcontroller)

# M2c — estimated geometry math (wrap-always-on). Pure integer/vector logic, no
# decoding and no QApplication (QTEST_APPLESS_MAIN); links loftail_core only.
qt_add_executable(tst_estimatedgeometry tst_estimatedgeometry.cpp)
target_link_libraries(tst_estimatedgeometry PRIVATE loftail_core Qt6::Test)
add_test(NAME tst_estimatedgeometry COMMAND tst_estimatedgeometry)

# M3 — log-format seam and per-file cache: ManualFormatProvider round-trip,
# FormatCache save/restore (QSettings), the pure preview/field-breakdown builder,
# and the source-zone timestamp reparse. Core-only, no QApplication.
foreach(unit formatprovider formatpreview documentzone)
    qt_add_executable(tst_${unit} tst_${unit}.cpp)
    target_link_libraries(tst_${unit} PRIVATE loftail_core Qt6::Test)
    add_test(NAME tst_${unit} COMMAND tst_${unit})
endforeach()

# M8 — format autodetection: candidate-library match-rate scoring, the priority-
# anchored structural-inference fallback, and the DetectingFormatProvider behind the
# IFormatProvider seam. Core-only, no QApplication.
qt_add_executable(tst_formatdetector tst_formatdetector.cpp)
target_link_libraries(tst_formatdetector PRIVATE loftail_core Qt6::Test)
add_test(NAME tst_formatdetector COMMAND tst_formatdetector)

# M4 — filtering core: the predicate chain (integers before text), each axis, the
# FilteredIndex geometry over the visible subset, Document::applyFilters end to end,
# and the Find walk. Core-only, GUILESS (a temp file + QCoreApplication).
qt_add_executable(tst_filter tst_filter.cpp)
target_link_libraries(tst_filter PRIVATE loftail_core Qt6::Test)
add_test(NAME tst_filter COMMAND tst_filter)

# M5 — highlight core (palette-index persistence, first-match-wins roles with the
# default fallback), the preset store (schema-versioned JSON, atomic writes,
# export/import portability), and the session `documents`-array round-trip. All
# core-only; the highlight test is even QApplication-free (QColor is a value type).
foreach(unit highlight presetstore session)
    qt_add_executable(tst_${unit} tst_${unit}.cpp)
    target_link_libraries(tst_${unit} PRIVATE loftail_core Qt6::Test)
    add_test(NAME tst_${unit} COMMAND tst_${unit})
endforeach()

# M6 — the tail harness: append (incl. a multi-line record split across writes),
# truncate, rotate, and filtered/highlighted append, all driven synchronously via
# LiveController::checkNow() against a real temp file. Core-only, GUILESS.
#
# POSIX-only: the harness relies on mmap seeing appended bytes and on renaming an
# open file (rotation), plus stat-based path identity. Windows uses BufferedLogSource
# and pathIdentity() there is still a stub (LogSourceFactory.cpp) — its rotation/
# file-sharing semantics differ and must be exercised separately (not done yet).
if(NOT WIN32)
    qt_add_executable(tst_tail tst_tail.cpp)
    target_link_libraries(tst_tail PRIVATE loftail_core Qt6::Test)
    add_test(NAME tst_tail COMMAND tst_tail)
endif()

# Run selection — a run-start regexp splits a file into runs; detection, subset
# restriction + sourceRow mapping, composition with filters, and the live cases
# (append inside the last run, a new run freezing the watched run, an earlier run
# untouched, rescan re-detect). Core-only, GUILESS (LiveController::checkNow()).
qt_add_executable(tst_runselect tst_runselect.cpp)
target_link_libraries(tst_runselect PRIVATE loftail_core Qt6::Test)
add_test(NAME tst_runselect COMMAND tst_runselect)

# M6 — live smoke: the REAL QFileSystemWatcher + poll path under a running event
# loop (a background timer appends "like another process"; the model must grow with
# no manual poke). Core-only, GUILESS.
qt_add_executable(tst_livegui tst_livegui.cpp)
target_link_libraries(tst_livegui PRIVATE loftail_core Qt6::Test)
add_test(NAME tst_livegui COMMAND tst_livegui)

# M2b — the production LogView. Links the UI layer; the geometry mapping and
# clipboard builders are pure, and the header-state round-trip needs QWidget, so
# the test runs under an offscreen QApplication (forced in its own main()).
qt_add_executable(tst_logview tst_logview.cpp)
target_link_libraries(tst_logview PRIVATE loftail_ui Qt6::Test Qt6::Widgets)
add_test(NAME tst_logview COMMAND tst_logview)
set_tests_properties(tst_logview PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# The Filters pane's defaults: the subsystem and priority axes ship enabled
# (SPEC.md §6), which is only safe if an all-inclusive axis stays inactive, later
# discoveries arrive checked, and a rebind forgets the previous file's names.
# Links the UI layer, runs offscreen.
qt_add_executable(tst_filterpane tst_filterpane.cpp)
target_link_libraries(tst_filterpane PRIVATE loftail_ui Qt6::Test Qt6::Widgets)
add_test(NAME tst_filterpane COMMAND tst_filterpane)
set_tests_properties(tst_filterpane PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# M5 — full session round-trip through the real MainWindow (open a file, set
# filters + highlighters, quit, relaunch, confirm restored) and graceful handling
# of a missing last file. Drives two MainWindow instances under an isolated config
# home; links the UI layer and runs offscreen.
qt_add_executable(tst_sessiongui tst_sessiongui.cpp)
target_link_libraries(tst_sessiongui PRIVATE loftail_ui Qt6::Test Qt6::Widgets)
add_test(NAME tst_sessiongui COMMAND tst_sessiongui)
set_tests_properties(tst_sessiongui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# M3 — the open flow around the Log Format dialog: Esc cancels the open (SPEC.md
# §4) and an accepted pattern opens the file. Drives the real MainWindow and
# dismisses the modal dialog from a timer; UI layer, offscreen, isolated config.
qt_add_executable(tst_openflow tst_openflow.cpp)
target_link_libraries(tst_openflow PRIVATE loftail_ui Qt6::Test Qt6::Widgets)
add_test(NAME tst_openflow COMMAND tst_openflow)
set_tests_properties(tst_openflow PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
