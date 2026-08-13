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

# M3 — the log-format seam: ManualFormatProvider round-trip, the pure preview /
# field-breakdown builder, and the source-zone timestamp reparse. The per-file memory
# that used to be tested here is one level of the settings tree now — tst_logsettings.
# Core-only, no QApplication.
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

# M15 — filter with context (grep -B/-A): the pure emitter, Document::applyFilters
# over real bytes (including composition with a selected run), and the LIVE path,
# where the incremental emitter has to reach the same answer as a one-shot scan
# across arbitrary write boundaries. Core-only, GUILESS.
#
# UNGATED, unlike tst_tail beside which the live cases would otherwise sit: nothing
# here needs mmap-sees-appends, an unlink or a rename — only appends, which
# BufferedLogSource serves too — and the emission rule must hold on Windows.
qt_add_executable(tst_filtercontext tst_filtercontext.cpp)
target_link_libraries(tst_filtercontext PRIVATE loftail_core Qt6::Test)
add_test(NAME tst_filtercontext COMMAND tst_filtercontext)

# M5 — highlight core (palette-index persistence, first-match-wins roles with the
# default fallback) and the session `documents`-array round-trip. Both core-only; the
# highlight test is even QApplication-free (QColor is a value type). The preset store,
# the third member of this group until presets went behind a build option, is registered
# separately below.
#
# `matchcriteria` covers the portable form of the five match axes that the Filters
# pane and every highlight rule share: its JSON keys (unchanged from FilterPane's, so
# existing presets and sessions keep loading) and the two arguments where filtering
# and highlighting deliberately disagree — the absent-field exemption and whether a
# no-op axis collapses.
foreach(unit highlight matchcriteria session)
    qt_add_executable(tst_${unit} tst_${unit}.cpp)
    target_link_libraries(tst_${unit} PRIVATE loftail_core Qt6::Test)
    add_test(NAME tst_${unit} COMMAND tst_${unit})
endforeach()

# The preset store (schema-versioned JSON, atomic writes, export/import portability).
#
# GATED, unlike tst_secretstore and tst_archivelocation beside their own gates: those stay
# ungated because a seam survives in both configurations and has to behave identically in
# each, and a rule tested in one configuration is a rule that holds in one configuration.
# Presets leave no seam behind — PresetStore is simply not compiled — so there is nothing
# for this to assert about a build without them.
if(LOFTAIL_HAVE_PRESETS)
    qt_add_executable(tst_presetstore tst_presetstore.cpp)
    target_link_libraries(tst_presetstore PRIVATE loftail_core Qt6::Test)
    add_test(NAME tst_presetstore COMMAND tst_presetstore)
endif()

# M11 — remote sources, path layer. The ssh:// URL value type, the one-spelling
# normalization every entry point relies on, the display/availability helpers, and
# the settings key that used to mangle a URL into a working-directory-dependent
# string. No libssh2 and no network, so it runs in every build configuration.
qt_add_executable(tst_remotelocation tst_remotelocation.cpp)
target_link_libraries(tst_remotelocation PRIVATE loftail_core Qt6::Test)
add_test(NAME tst_remotelocation COMMAND tst_remotelocation)

# M12 — archived sources, path layer. The nested-path value type, the resolution rule
# (where the container ends) and the collapse rule (a bare compressed stream keeps its
# plain path), plus the same display/availability/settings-key obligations the remote
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

# M17 — THE test for the milestone: opening a remote log must not block the thread that
# opened it. Uses a transport that genuinely takes a second, because a fake that returns
# instantly satisfies the old contract just as well as the new one. Network-free.
qt_add_executable(tst_asyncconnect tst_asyncconnect.cpp)
target_link_libraries(tst_asyncconnect PRIVATE loftail_core Qt6::Test)
add_test(NAME tst_asyncconnect COMMAND tst_asyncconnect)

# M17 — a question asked on a fetcher's thread, answered on the application's. Pins
# where the answer runs, that only one question is in flight at a time, and that a
# cancel unblocks a waiter instead of stranding it. No network and no libssh2.
qt_add_executable(tst_promptrelay tst_promptrelay.cpp)
target_link_libraries(tst_promptrelay PRIVATE loftail_core Qt6::Test)
add_test(NAME tst_promptrelay COMMAND tst_promptrelay)

# M17 — the same gate under concurrent pressure, aimed at the window tst_promptrelay
# does not open: a cancel landing while calls are queued and running on other threads.
# Checks rules 2 and 3 from OUTSIDE the gate (was any caller released while its work was
# still in flight? did two ever run at once?), so a lifetime bug fails as an assertion
# rather than as a use-after-free that needs the right 1-in-28 timing to crash.
# Ungated and network-free.
qt_add_executable(tst_gatestress tst_gatestress.cpp)
target_link_libraries(tst_gatestress PRIVATE loftail_core Qt6::Test)
add_test(NAME tst_gatestress COMMAND tst_gatestress)

# M17 — a dead spool is retired, not joined. Pins the promise the async open rests on:
# dropping the last handle returns at once even while the fetcher's thread is still
# winding up, and the directory is removed only once it has. Also network-free.
qt_add_executable(tst_spoolretire tst_spoolretire.cpp)
target_link_libraries(tst_spoolretire PRIVATE loftail_core Qt6::Test)
add_test(NAME tst_spoolretire COMMAND tst_spoolretire)

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

# The same dialog as a piece of user interface rather than as a route into MainWindow:
# which controls are live in which state (Open refusing an incomplete address, Save and
# Remove refusing an empty form), what the sign-in section discloses about where a
# password would go, and that a host's remembered paths are visible and prunable
# instead of accumulating out of sight. Ungated — the backend is FakeSecretStore.h.
qt_add_executable(tst_remotedialog tst_remotedialog.cpp)
target_link_libraries(tst_remotedialog PRIVATE loftail_ui Qt6::Test Qt6::Widgets)
add_test(NAME tst_remotedialog COMMAND tst_remotedialog)
set_tests_properties(tst_remotedialog PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Which language the interface is in, and specifically that QT'S OWN strings end up in
# the same one. loftail ships no catalogue, so its own strings need no help; the
# standard dialog buttons do, because qtbase_<lang> is loaded for the system locale by
# a layer above loftail. Asserts the override ordering against a stand-in catalogue.
qt_add_executable(tst_uilanguage tst_uilanguage.cpp)
target_link_libraries(tst_uilanguage PRIVATE loftail_ui Qt6::Test Qt6::Widgets)
add_test(NAME tst_uilanguage COMMAND tst_uilanguage)
set_tests_properties(tst_uilanguage PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# M11 — saved hosts and, more to the point, the containment rules around a password
# the user chose to keep in clear text: written only when asked for, erased when
# unasked, owner-only on disk, and never present in any of the path strings that get
# written to the session, the recent-files list or a window title.
qt_add_executable(tst_hostbookmarks tst_hostbookmarks.cpp)
target_link_libraries(tst_hostbookmarks PRIVATE loftail_core Qt6::Test)
add_test(NAME tst_hostbookmarks COMMAND tst_hostbookmarks)

# M20 — the settings TREE: defaults, an ordered list of file patterns, per-file nodes,
# and the one JSON file behind them. The matcher's semantics are pinned here rather
# than left to Qt: the wildcard dialect is written out because the option that would
# have bought it from QRegularExpression is Qt 6.6, above this project's floor. Core
# only — no QApplication, no widgets.
qt_add_executable(tst_logsettings tst_logsettings.cpp)
target_link_libraries(tst_logsettings PRIVATE loftail_core Qt6::Test)
add_test(NAME tst_logsettings COMMAND tst_logsettings)

# M14 — the seam a remembered password goes through. UNGATED, and for the same reason
# tst_sshexec is: what it pins is a rule about CONSENT — a keychain that refuses is
# reported, never quietly downgraded to a plain-text file — and a rule tested in one build
# configuration is a rule that holds in one build configuration. Nothing here needs
# QtKeychain; the backend is FakeSecretStore.h.
qt_add_executable(tst_secretstore tst_secretstore.cpp)
target_link_libraries(tst_secretstore PRIVATE loftail_core Qt6::Test)
add_test(NAME tst_secretstore COMMAND tst_secretstore)

# M14 — the password prompt's three destinations and where a ticked box actually writes.
# UNGATED for the same reason as the above: the backend is FakeSecretStore.h, so none of
# this needs QtKeychain, and the wording that tells a user where their secret is going has
# to read the same in every build.
qt_add_executable(tst_sshcredentials tst_sshcredentials.cpp)
target_link_libraries(tst_sshcredentials PRIVATE loftail_ui Qt6::Test Qt6::Widgets)
add_test(NAME tst_sshcredentials COMMAND tst_sshcredentials)
set_tests_properties(tst_sshcredentials PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# M14 — the live keychain harness, and the ONLY place QtKeychain is exercised beyond
# linking. Gated twice, exactly like tst_sshlive: not built without keychain support, and
# it QSKIPs unless LOFTAIL_TEST_KEYCHAIN is set — it writes to the developer's real
# keyring. CI never runs it, and could not: the runners are headless with no session bus,
# so nothing there would answer. Say so plainly rather than letting a green pipeline imply
# the backends were tested (PLAN.md M14 risk).
if(LOFTAIL_HAVE_KEYCHAIN)
    qt_add_executable(tst_keychainlive tst_keychainlive.cpp)
    target_link_libraries(tst_keychainlive PRIVATE loftail_core Qt6::Test)
    add_test(NAME tst_keychainlive COMMAND tst_keychainlive)
endif()

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

# The exec transport's commands and parsing, for a server that will not do SFTP.
# UNGATED: the quoting is a security boundary and must be tested in every build, and the
# injection cases run the generated commands through a real /bin/sh rather than merely
# comparing strings. Needs Qt6::Test only — no libssh2, no network.
qt_add_executable(tst_sshexec tst_sshexec.cpp)
target_link_libraries(tst_sshexec PRIVATE loftail_core Qt6::Test)
add_test(NAME tst_sshexec COMMAND tst_sshexec)

# M16 — which of those commands can measure the file on THIS server, and the proof that
# the answer is usable. UNGATED for the same reason and one stronger: this is the only
# decision the exec transport makes, and the transport itself needs libssh2, a server
# with no working SFTP, and — for the rungs below `stat` — a server missing a utility
# every developer machine has. The two injected seams stand in for all of that, running
# the real commands through a real /bin/sh against real files.
qt_add_executable(tst_execsizeprobe tst_execsizeprobe.cpp)
target_link_libraries(tst_execsizeprobe PRIVATE loftail_core Qt6::Test)
add_test(NAME tst_execsizeprobe COMMAND tst_execsizeprobe)

# Why the SSH transport takes its socket away from Qt. UNGATED and network-free
# (loopback only): the behaviour worked around is Qt's own, so it must be pinned in a
# build with no SSH support too — and a Qt that stopped doing it would make the whole
# workaround unnecessary, which is worth being told by a failing test.
qt_add_executable(tst_socketdetach tst_socketdetach.cpp)
target_link_libraries(tst_socketdetach PRIVATE loftail_core Qt6::Test Qt6::Network)
add_test(NAME tst_socketdetach COMMAND tst_socketdetach)

# Chrome colours on a dark theme: the placeholder role Qt leaves unset (which rendered
# every hint black on a dark field), and the error/warning/muted hues that used to be
# hardcoded against a light theme. Needs a QApplication — offscreen.
qt_add_executable(tst_uicolors tst_uicolors.cpp)
target_link_libraries(tst_uicolors PRIVATE loftail_ui Qt6::Test Qt6::Widgets)
add_test(NAME tst_uicolors COMMAND tst_uicolors)
set_tests_properties(tst_uicolors PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# The side panes' title-bar chrome: no name repeated under the tab that already carries
# it, and the measured Qt constraints that made repainting the bar the only option
# (a dock drags only from its own title bar). Needs a QApplication — offscreen.
qt_add_executable(tst_panechrome tst_panechrome.cpp)
target_link_libraries(tst_panechrome PRIVATE loftail_ui Qt6::Test Qt6::Widgets)
add_test(NAME tst_panechrome COMMAND tst_panechrome)
set_tests_properties(tst_panechrome PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Help ▸ About: the running binary's own answer to "which build is this", which until
# now lived only in --version. Needs a QApplication for the menu bar — offscreen.
qt_add_executable(tst_about tst_about.cpp)
target_link_libraries(tst_about PRIVATE loftail_ui Qt6::Test Qt6::Widgets)
add_test(NAME tst_about COMMAND tst_about)
set_tests_properties(tst_about PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

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

# The Runs pane: the detected runs are a LIST, it takes the pane's spare height, the
# selection travels on the current row (so the arrow keys work), and a repopulation
# during live tail is not a user choice. Links the UI layer, runs offscreen.
qt_add_executable(tst_runpane tst_runpane.cpp)
target_link_libraries(tst_runpane PRIVATE loftail_ui Qt6::Test Qt6::Widgets)
add_test(NAME tst_runpane COMMAND tst_runpane)
set_tests_properties(tst_runpane PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# The Highlighters pane, where the same axis editor drives one rule among several
# rather than the whole file: a typed regex must reach the rule, selecting another
# rule must show THAT rule's subsystems (the Filters pane's "newly discovered arrives
# checked" rule is wrong here), and an invalid regex must be visible rather than
# silently coloring nothing. Links the UI layer, runs offscreen.
qt_add_executable(tst_highlighterpane tst_highlighterpane.cpp)
target_link_libraries(tst_highlighterpane PRIVATE loftail_ui Qt6::Test Qt6::Widgets)
add_test(NAME tst_highlighterpane COMMAND tst_highlighterpane)
set_tests_properties(tst_highlighterpane PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# M19 — a highlight rule's effect is a SET of actions, and colour is one of them.
#
# The digest is a second visible subset of one document: last match per opted-in rule,
# bounded by the run and deliberately not by the filters, fenced at a fixed look-back.
# Core-only and guiless like tst_filtercontext, because the derivation is per FILE — the
# view that draws it is the only per-view part. Its live cases are the ones that earn
# their keep: the incremental update has to agree with a one-shot scan while the
# trailing record is still being re-read.
qt_add_executable(tst_digest tst_digest.cpp)
target_link_libraries(tst_digest PRIVATE loftail_core Qt6::Test)
add_test(NAME tst_digest COMMAND tst_digest)

# The rate limit behind the Notify action. UNGATED and clock-injected, because the
# DELIVERY path has no honest CI coverage — the runners are headless with no tray and no
# notification server, so isSystemTrayAvailable() is false and showMessage() is
# unreachable (the tst_sshlive / tst_keychainlive shape). What decides whether a
# notification happens, and what it says, is testable without a desktop and is here.
qt_add_executable(tst_alertpolicy tst_alertpolicy.cpp)
target_link_libraries(tst_alertpolicy PRIVATE loftail_core Qt6::Test)
add_test(NAME tst_alertpolicy COMMAND tst_alertpolicy)

# The digest STRIP: that it is exactly as tall as its rows and not scrolled, that it
# appears and vanishes with its content, that it tracks the table's columns and
# horizontal scroll, and that a digest row wears its own rule's colours while the log
# above stays uncoloured. Links the UI layer, runs offscreen.
qt_add_executable(tst_digestview tst_digestview.cpp)
target_link_libraries(tst_digestview PRIVATE loftail_ui Qt6::Test Qt6::Widgets)
add_test(NAME tst_digestview COMMAND tst_digestview)
set_tests_properties(tst_digestview PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# M5 — full session round-trip through the real MainWindow (open a file, set
# filters + highlighters, quit, relaunch, confirm restored) and graceful handling
# of a missing last file. Drives two MainWindow instances under an isolated config
# home; links the UI layer and runs offscreen.
qt_add_executable(tst_sessiongui tst_sessiongui.cpp)
target_link_libraries(tst_sessiongui PRIVATE loftail_ui Qt6::Test Qt6::Widgets)
add_test(NAME tst_sessiongui COMMAND tst_sessiongui)
set_tests_properties(tst_sessiongui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# M3 — the open flow around the Preferences dialog: Esc cancels the open (SPEC.md
# §4) and an accepted pattern opens the file. Drives the real MainWindow and
# dismisses the modal dialog from a timer; UI layer, offscreen, isolated config.
# M18 added the two cases that pin the DEFAULT format end to end — a saved default
# opening a never-seen log with no dialog, and a default that cannot parse one still
# asking. Both need the real open path, because "no dialog appeared" is only
# observable there.
qt_add_executable(tst_openflow tst_openflow.cpp)
target_link_libraries(tst_openflow PRIVATE loftail_ui Qt6::Test Qt6::Widgets)
add_test(NAME tst_openflow COMMAND tst_openflow)
set_tests_properties(tst_openflow PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# M18 — the default log format: the store that decides what a default IS, and the
# Preferences dialog where one is entered. What it DOES on an open is tst_openflow's.
qt_add_executable(tst_preferences tst_preferences.cpp)
target_link_libraries(tst_preferences PRIVATE loftail_ui Qt6::Test Qt6::Widgets)
add_test(NAME tst_preferences COMMAND tst_preferences)
set_tests_properties(tst_preferences PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# The scan indicator and its stop button (SPEC.md §3). Cancelling a scan moved out of
# the File menu and onto the progress bar, so what needs pinning is that the pair
# appears exactly while a scan runs and that the button reaches the cancel seam — the
# partial index a cancel leaves behind is tst_indexcontroller's. Needs the real window,
# since the two are shown by updateStatus() and hidden by onIndexFinished().
qt_add_executable(tst_scanprogress tst_scanprogress.cpp)
target_link_libraries(tst_scanprogress PRIVATE loftail_ui Qt6::Test Qt6::Widgets)
add_test(NAME tst_scanprogress COMMAND tst_scanprogress)
set_tests_properties(tst_scanprogress PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# M9 — several logs open at once as tabs, and several views onto one log
# (SPEC.md §3, §5a). Covers what multi-document changes: an open adds a tab rather
# than replacing, pane rebinding on a FILE switch but not a view switch, and a file
# closing with its last view. Drives the real MainWindow; UI layer, offscreen.
qt_add_executable(tst_multidoc tst_multidoc.cpp)
target_link_libraries(tst_multidoc PRIVATE loftail_ui Qt6::Test Qt6::Widgets)
add_test(NAME tst_multidoc COMMAND tst_multidoc)
set_tests_properties(tst_multidoc PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# The record context menu (SPEC.md §5): what a record offers, and that triggering an
# item reaches the same per-file state a click in the pane would. The pane-level half
# — what each edit MEANS, including that "show only" must not widen as the scan finds
# more — is in tst_filterpane; this covers the assembly, which is the part that needs
# a real window, a real format and a real record. Offscreen, isolated config.
qt_add_executable(tst_recordmenu tst_recordmenu.cpp)
target_link_libraries(tst_recordmenu PRIVATE loftail_ui Qt6::Test Qt6::Widgets)
add_test(NAME tst_recordmenu COMMAND tst_recordmenu)
set_tests_properties(tst_recordmenu PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Changing a filter must not move the reader (SPEC.md §6): the selected record keeps its
# place on screen if it survives, the viewport still shows the same part of the file if it
# does not, and follow is neither attached nor detached by the change. The mechanism is
# pinned view-side in tst_logview; this drives the REAL routes into a re-apply — the
# Filters pane's own controls and View ▸ Clear Filters — plus the two things only a window
# can show: that each of two views of one file keeps its own place (invariant #7), and
# that the one caller which opts out of the anchor still lands where it means to.
# Offscreen, isolated config.
qt_add_executable(tst_filteranchor tst_filteranchor.cpp)
target_link_libraries(tst_filteranchor PRIVATE loftail_ui Qt6::Test Qt6::Widgets)
add_test(NAME tst_filteranchor COMMAND tst_filteranchor)
set_tests_properties(tst_filteranchor PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# M9 — the tab arrangement itself round-trips: the same tabs come back in the same
# order, each view keeping its own column layout and wrap mode.
# Two MainWindow instances over one isolated settings store, offscreen.
qt_add_executable(tst_tabsession tst_tabsession.cpp)
target_link_libraries(tst_tabsession PRIVATE loftail_ui Qt6::Test Qt6::Widgets)
add_test(NAME tst_tabsession COMMAND tst_tabsession)
set_tests_properties(tst_tabsession PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# --- The `threading` label: what ThreadSanitizer is run over ----------------
#
# ARCHITECTURE.md §13. TSan is a supported build (-DLOFTAIL_SANITIZE=thread) but it is
# gated in CI over THIS SUBSET, not the whole suite, and the reason is a property of the
# system Qt rather than of the tests: Qt is not built with TSan, so its internal locking
# is invisible and anything driving widgets and a real event loop reports Qt's own
# queued-connection and glib internals as races. Measured on the UI tests: 11-23 reports
# each, none of them loftail's. Gating on those would mean a suppression file broad
# enough to hide real findings too.
#
# These are the tests that exercise loftail's OWN threads — the indexer, the fetchers,
# the spool registry, the live seam and the call gate — which is where every concurrency
# bug in this project has been. They run at ZERO reports with tests/tsan.supp, so a new
# one means a new bug rather than more noise. Keep that true: a test added here that
# cannot reach zero belongs outside the label, with a note saying why.
set(loftail_threading_tests
    tst_indexcontroller
    tst_spooledsource
    tst_asyncconnect
    tst_promptrelay
    tst_gatestress
    tst_spoolretire
    tst_remotetail
    tst_remoteopen
    tst_waitingremote
    tst_complete
    tst_livegui
    tst_runselect
    tst_tail            # POSIX-only
    tst_waiting         # POSIX-only
    tst_archivemembers  # libarchive-only
    tst_archivefetcher  # libarchive-only
    tst_archivetail     # libarchive-only
)
# Label only what this configuration actually defined — several above are conditional,
# and set_tests_properties on a test that does not exist is a configure error.
get_property(loftail_defined_tests DIRECTORY PROPERTY TESTS)
foreach(t IN LISTS loftail_threading_tests)
    if(t IN_LIST loftail_defined_tests)
        set_property(TEST ${t} APPEND PROPERTY LABELS threading)
    endif()
endforeach()

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
