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
#
# `matchcriteria` covers the portable form of the five match axes that the Filters
# pane and every highlight rule share: its JSON keys (unchanged from FilterPane's, so
# existing presets and sessions keep loading) and the two arguments where filtering
# and highlighting deliberately disagree — the absent-field exemption and whether a
# no-op axis collapses.
foreach(unit highlight matchcriteria presetstore session)
    qt_add_executable(tst_${unit} tst_${unit}.cpp)
    target_link_libraries(tst_${unit} PRIVATE loftail_core Qt6::Test)
    add_test(NAME tst_${unit} COMMAND tst_${unit})
endforeach()

# M11 — remote sources, path layer. The ssh:// URL value type, the one-spelling
# normalization every entry point relies on, the display/availability helpers, and
# the format-cache key that used to mangle a URL into a working-directory-dependent
# string. No libssh2 and no network, so it runs in every build configuration.
qt_add_executable(tst_remotelocation tst_remotelocation.cpp)
target_link_libraries(tst_remotelocation PRIVATE loftail_core Qt6::Test)
add_test(NAME tst_remotelocation COMMAND tst_remotelocation)

# M12 — archived sources, path layer. The nested-path value type, the resolution rule
# (where the container ends) and the collapse rule (a bare compressed stream keeps its
# plain path), plus the same display/availability/format-cache obligations the remote
# path layer carries. UNGATED: recognising, normalizing, persisting and displaying an
# archived path must be identical with and without libarchive, or the two builds would
# disagree about what a settings file means.
qt_add_executable(tst_archivelocation tst_archivelocation.cpp)
target_link_libraries(tst_archivelocation PRIVATE loftail_core Qt6::Test)
add_test(NAME tst_archivelocation COMMAND tst_archivelocation)

# M11 — the spooled source: the committed-size clamp that stands in for a lock
# between the GUI thread and the fetcher thread, and generation-based rotation. Uses
# tests/FakeFetcher.h, so no network and no libssh2 — this runs everywhere.
qt_add_executable(tst_spooledsource tst_spooledsource.cpp)
target_link_libraries(tst_spooledsource PRIVATE loftail_core Qt6::Test)
add_test(NAME tst_spooledsource COMMAND tst_spooledsource)

# M11 — `tail -f` over a remote log: tst_tail's matrix (append with a split
# multi-line record, truncate, rotate, filtered/highlighted append) run again over a
# SpooledLogSource, plus the two cases with no local counterpart — uncommitted bytes
# staying invisible, and a rescan mid-tail not reconnecting. Also network-free, and
# NOT gated on POSIX: the spool is an ordinary local file on every platform.
qt_add_executable(tst_remotetail tst_remotetail.cpp)
target_link_libraries(tst_remotetail PRIVATE loftail_core Qt6::Test)
add_test(NAME tst_remotetail COMMAND tst_remotetail)

# M11 — a remote log through the REAL MainWindow, offscreen and network-free (the
# transport is FakeFetcher.h). Covers the promise that an ssh:// URL works wherever a
# path does: one tab however it is spelled, the tab/title/status wording, recent
# files, the session round trip, and the menu's behavior in a build without SSH.
qt_add_executable(tst_remoteopen tst_remoteopen.cpp)
target_link_libraries(tst_remoteopen PRIVATE loftail_ui Qt6::Test Qt6::Widgets)
add_test(NAME tst_remoteopen COMMAND tst_remoteopen)
set_tests_properties(tst_remoteopen PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# M11 — saved hosts and, more to the point, the containment rules around a password
# the user chose to keep in clear text: written only when asked for, erased when
# unasked, owner-only on disk, and never present in any of the path strings that get
# written to the session, the recent-files list or a window title.
qt_add_executable(tst_hostbookmarks tst_hostbookmarks.cpp)
target_link_libraries(tst_hostbookmarks PRIVATE loftail_core Qt6::Test)
add_test(NAME tst_hostbookmarks COMMAND tst_hostbookmarks)

# M11 — the live SSH harness, and the ONLY place libssh2 is exercised beyond linking.
# Gated twice: it is not built without SSH support, and it QSKIPs unless
# LOFTAIL_TEST_SSH_URL names a disposable remote path. CI never runs it, so the
# handshake, host-key and authentication paths are verified by hand — say so plainly
# rather than letting a green pipeline imply otherwise (PLAN.md M11 risk).
if(LOFTAIL_HAVE_SSH)
    qt_add_executable(tst_sshlive tst_sshlive.cpp)
    target_link_libraries(tst_sshlive PRIVATE loftail_core Qt6::Test)
    add_test(NAME tst_sshlive COMMAND tst_sshlive)
endif()

# M12 — a source that can prove its stream is finished, and the live seam's response.
# UNGATED: isComplete() and the take-before-refresh / act-after-ingest ordering are a
# contract of LiveController, not of libarchive, so they are driven through the fake
# fetcher and covered in a build with no codec linked at all.
qt_add_executable(tst_complete tst_complete.cpp)
target_link_libraries(tst_complete PRIVATE loftail_core Qt6::Test)
add_test(NAME tst_complete COMMAND tst_complete)

# M12 — the archive layer, exercised for real. THE CONTRAST WITH tst_sshlive ABOVE IS
# THE POINT: this needs no network, no credentials and no disposable remote path, so
# unlike M11's transport it runs in CI on every push. The fixtures are built at runtime
# by libarchive's own write side (tests/ArchiveFixtures.h) — nothing binary is
# committed — which is why these link the dependency directly as well: loftail_core
# links it PRIVATE, and the fixtures call it.
if(LOFTAIL_HAVE_ARCHIVE)
    foreach(unit archivemembers archivefetcher archivetail)
        qt_add_executable(tst_${unit} tst_${unit}.cpp)
        target_link_libraries(tst_${unit} PRIVATE loftail_core Qt6::Test ${LOFTAIL_ARCHIVE_TARGET})
        add_test(NAME tst_${unit} COMMAND tst_${unit})
    endforeach()

    # The same, through the real MainWindow: one tab per member however it is spelled,
    # the picker shown only when there is a genuine choice, and a remembered format.
    qt_add_executable(tst_archiveopen tst_archiveopen.cpp)
    target_link_libraries(tst_archiveopen
        PRIVATE loftail_ui Qt6::Test Qt6::Widgets ${LOFTAIL_ARCHIVE_TARGET})
    add_test(NAME tst_archiveopen COMMAND tst_archiveopen)
    set_tests_properties(tst_archiveopen PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

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

    # M13 — a log that is not there: opening a path before it exists, a log deleted
    # while open, and the grace period that keeps a rotation from flashing an empty
    # view. Beside tst_tail and NOT WIN32 for the same reason — it leans on POSIX
    # mmap + unlink semantics, and Windows must be exercised separately.
    qt_add_executable(tst_waiting tst_waiting.cpp)
    target_link_libraries(tst_waiting PRIVATE loftail_core Qt6::Test)
    add_test(NAME tst_waiting COMMAND tst_waiting)
endif()

# The side panes' title-bar chrome: no name repeated under the tab that already carries
# it, and the measured Qt constraints that made repainting the bar the only option
# (a dock drags only from its own title bar). Needs a QApplication — offscreen.
qt_add_executable(tst_panechrome tst_panechrome.cpp)
target_link_libraries(tst_panechrome PRIVATE loftail_ui Qt6::Test Qt6::Widgets)
add_test(NAME tst_panechrome COMMAND tst_panechrome)
set_tests_properties(tst_panechrome PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# M13 — the same waiting contract for a SPOOLED log: a host that is down, and one that
# goes away mid-tail. Ungated and network-free, over the fake fetcher — the transport
# is the only fake part, exactly as tst_remotetail arranges it.
qt_add_executable(tst_waitingremote tst_waitingremote.cpp)
target_link_libraries(tst_waitingremote PRIVATE loftail_core Qt6::Test)
add_test(NAME tst_waitingremote COMMAND tst_waitingremote)

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

# The Highlighters pane, where the same axis editor drives one rule among several
# rather than the whole file: a typed regex must reach the rule, selecting another
# rule must show THAT rule's subsystems (the Filters pane's "newly discovered arrives
# checked" rule is wrong here), and an invalid regex must be visible rather than
# silently coloring nothing. Links the UI layer, runs offscreen.
qt_add_executable(tst_highlighterpane tst_highlighterpane.cpp)
target_link_libraries(tst_highlighterpane PRIVATE loftail_ui Qt6::Test Qt6::Widgets)
add_test(NAME tst_highlighterpane COMMAND tst_highlighterpane)
set_tests_properties(tst_highlighterpane PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

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

# M9 — several logs open at once as tabs, and several views onto one log
# (SPEC.md §3, §5a). Covers what multi-document changes: an open adds a tab rather
# than replacing, pane rebinding on a FILE switch but not a view switch, and a file
# closing with its last view. Drives the real MainWindow; UI layer, offscreen.
qt_add_executable(tst_multidoc tst_multidoc.cpp)
target_link_libraries(tst_multidoc PRIVATE loftail_ui Qt6::Test Qt6::Widgets)
add_test(NAME tst_multidoc COMMAND tst_multidoc)
set_tests_properties(tst_multidoc PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# M9 — the tab arrangement itself round-trips: the same tabs come back in the same
# order, each view keeping its own column layout and wrap mode.
# Two MainWindow instances over one isolated settings store, offscreen.
qt_add_executable(tst_tabsession tst_tabsession.cpp)
target_link_libraries(tst_tabsession PRIVATE loftail_ui Qt6::Test Qt6::Widgets)
add_test(NAME tst_tabsession COMMAND tst_tabsession)
set_tests_properties(tst_tabsession PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# --- Console subsystem on Windows ------------------------------------------
#
# Qt 6's qt_add_executable() sets WIN32_EXECUTABLE on Windows, which makes the
# target a GUI-subsystem binary with NO console attached. A GUI-subsystem process
# writes nothing to a redirected stdout, so on Windows every one of these test
# binaries ran silently: `ctest --output-on-failure` reported a failure with an
# empty output block, and there was no way to tell an assertion from a crash.
#
# Test binaries are console programs. Say so explicitly, for the same reason
# src/bench is described as console-only. Harmless everywhere else — the property
# is ignored off Windows, and MACOSX_BUNDLE would likewise hide the output in a
# .app rather than printing it.
get_property(loftail_test_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(target IN LISTS loftail_test_targets)
    if(target MATCHES "^tst_")
        set_target_properties(${target} PROPERTIES
            WIN32_EXECUTABLE FALSE
            MACOSX_BUNDLE FALSE
        )
    endif()
endforeach()
