# loftail_core: UI-free logic. Links QtCore only — never QtWidgets — so that
# parsing/indexing code stays unit-testable without a QApplication instance
# (see CLAUDE.md conventions). LogModel is a QAbstractTableModel (QtCore), so the
# whole model/index/parse stack lives here without pulling in Widgets.
add_library(loftail_core STATIC
    Version.cpp
    Version.h
    # loftail's own log about itself: connection attempts and their outcomes, and logs
    # that have not turned up yet. Always compiled and always on — a diagnostic log that
    # has to be enabled first is never on when it is needed (see the header).
    DiagnosticLog.cpp
    DiagnosticLog.h
    Expected.h
    CompileError.h
    LogFormat.h
    PatternCompiler.cpp
    PatternCompiler.h
    # M2a — the spine
    Priority.h
    Encoding.h
    TimeDisplay.h
    Record.h
    Decoder.cpp
    Decoder.h
    LogSource.h
    LogSourceFactory.cpp
    # The data behind the density strip beside the scrollbar: a coarse, bucketed map of
    # where the highlighted records and find matches are in the whole view. Pure and
    # timer-free — the slice budget comes from the owner — so the one thing that has to
    # be right about it (a bounded scan that survives a tailed log growing under it) is
    # testable with no QApplication.
    DensityMap.cpp
    DensityMap.h
    # M11 — remote sources. RemoteLocation is the ssh:// URL value type and the
    # path-shaped helpers built on it; it never touches libssh2 and is ALWAYS
    # compiled, so a build without SSH support still recognises, persists and
    # displays a remote path identically (see RemoteLocation.h).
    RemoteLocation.cpp
    RemoteLocation.h
    # M12 — archived sources. ArchiveLocation is the nested-path value type; like
    # RemoteLocation it never touches its optional dependency and is ALWAYS compiled,
    # so a build without archive support still recognises, persists and displays an
    # archived path identically (see ArchiveLocation.h).
    ArchiveLocation.cpp
    ArchiveLocation.h
    # The config-file editor: where a log's config file is. Derives one address from
    # another — the configured path placed on the filesystem the log is on, resolved
    # against the log's own directory — so it sits beside the two location types above
    # and, like them, is pure value work that never touches an optional dependency.
    ConfigLocation.cpp
    ConfigLocation.h
    # Reading and writing that config file — THE ONLY PLACE loftail writes a file the
    # user named, as against the several it writes under directories of its own. Its
    # three rules (never create a directory, never widen permissions, rename over the
    # target) are stated in one place because they are what makes writing somebody
    # else's file safe.
    ConfigFileIO.cpp
    ConfigFileIO.h
    # Where a log physically is — the filesystem it sits on and its path there, with an
    # archived log peeled to its container. Shared by the two things that have to place
    # something against a log: its config file, and the script that restarts the
    # application writing it. One copy because the archive-peel rule has a trap in it.
    LogAnchor.cpp
    LogAnchor.h
    # M23 — the restart script. Which machine it runs on and what LOGFILE/ARCHIVE/MEMBER
    # are, derived from the log's address. Pure value work like the location types above,
    # ALWAYS compiled, so a build without SSH derives the same answer and tests it.
    RestartTarget.cpp
    RestartTarget.h
    # Running it: a QProcess locally, an exec channel on a worker thread remotely.
    RestartRunner.cpp
    RestartRunner.h
    # The lifetime rules for running SSH work off the application thread — abandon and
    # never join, the relay the worker owns, the bounded shutdown drain. One copy, shared
    # by the config transfer and the restart runner, because every rule in it was learned
    # from a crash.
    SshWorkerPool.cpp
    SshWorkerPool.h
    SshWorkerPool.inl
    ArchiveFetcher.h
    # Declares the member listing and the reader; free of libarchive types, so the
    # member-picker dialog compiles either way and both builds' UI looks the same.
    ArchiveReader.h
    # The spool: a log that is not already a readable local file is fetched forward
    # into a local cache file and read back through an ordinary local source, so the
    # paint path stays zero-copy and `tail -f` is just the fetcher appending. How the
    # bytes are obtained sits behind SourceFetcher, keyed by the normalized path
    # string — which is what lets all of this be tested with no network.
    SourceFetcher.h
    SourceSpool.cpp
    SourceSpool.h
    SpooledLogSource.cpp
    SpooledLogSource.h
    # The prompt seam and the per-host credential cache are compiled even without
    # libssh2: the UI installs a prompter unconditionally, and the "SSH is not built
    # in" path must behave identically to the built-in one everywhere else.
    SshPrompter.cpp
    SshPrompter.h
    SshFetcher.h
    SshFetchOptions.cpp
    # M17 — carrying a question from a fetcher's thread to the application's and the
    # answer back. Always compiled for the same reason the prompt seam is, and for one
    # more: the threading discipline it documents is the only copy of that reasoning, so
    # it must not be something a build configuration can omit.
    GuiCallGate.cpp
    GuiCallGate.h
    PromptRelay.cpp
    PromptRelay.h
    # M14 — where a remembered password lives. The seam and the null implementation are
    # compiled in EVERY configuration for the same reason SshPrompter is: the checkbox
    # wording, the auth chain's ordering and the consent rule must not differ between
    # builds, and only the answer to available() may.
    SecretStore.cpp
    SecretStore.h
    # Saved hosts. A file rather than QSettings because it may hold a password the
    # user chose to keep, and a file can be restricted to its owner.
    HostBookmarkStore.cpp
    HostBookmarkStore.h
    MappedLogSource.h
    # Reading a file its writer is still rolling. The one place a share mode is chosen,
    # and the only file of ours that includes <windows.h> on purpose (see the header).
    SharedReadFile.cpp
    SharedReadFile.h
    BufferedLogSource.cpp
    BufferedLogSource.h
    # The content check both local sources need and neither the inode nor the size can
    # stand in for: an in-place rewrite (see the header).
    HeadWitness.h
    InternTable.h
    TimestampParser.cpp
    TimestampParser.h
    RecordIndex.h
    # M2c — estimated geometry for wrap-always-on (pure math, no UI/decoding)
    EstimatedGeometry.cpp
    EstimatedGeometry.h
    Indexer.cpp
    Indexer.h
    Document.cpp
    Document.h
    LogModel.cpp
    LogModel.h
    # M4 — filtering: the filter set + predicate chain (integers first, text last),
    # the visible-subset FilteredIndex, and the shared text matcher / Find helper.
    Filter.cpp
    Filter.h
    FilteredIndex.h
    # M15 — filter context (grep -B/-A): the one forward pass that widens the visible
    # subset to each match's neighbours, shared by the one-shot and the live path.
    ContextEmitter.h
    # The portable (name/level/wall-clock) form of those same five axes, shared by
    # the Filters pane and by every highlight rule so the two cannot drift apart.
    MatchCriteria.cpp
    MatchCriteria.h
    # M2b — worker-thread indexing controller (QtCore only, no Widgets)
    IndexController.cpp
    IndexController.h
    # M6 — live updates: the file watcher (QFileSystemWatcher + size poll) and the
    # incremental-append / rotation-rescan controller (QtCore only, no Widgets).
    LiveController.cpp
    LiveController.h
    # M3 — log-format seam, per-file cache, and the pure preview builder (no UI)
    FormatSettings.h
    IFormatProvider.h
    ManualFormatProvider.cpp
    ManualFormatProvider.h
    # M20 — the settings TREE: the defaults, an ordered list of file patterns, and
    # per-log nodes, in one JSON file (SPEC.md §4, ARCHITECTURE.md §8). It supersedes
    # M3's per-file FormatCache and M18's DefaultFormatStore, whose contents it migrates
    # in once and whose QSettings groups it then removes.
    WrapMode.h
    LogFileSettings.cpp
    LogFileSettings.h
    LogFileStore.cpp
    LogFileStore.h
    LogProfile.cpp
    LogProfile.h
    LogSettings.cpp
    LogSettings.h
    LogSettingsStore.cpp
    LogSettingsStore.h
    FormatPreview.cpp
    FormatPreview.h
    # M8 — format autodetection: candidate-library scoring + priority-anchored
    # structural inference, behind the same IFormatProvider seam (no UI, §9).
    FormatDetector.cpp
    FormatDetector.h
    DetectingFormatProvider.cpp
    DetectingFormatProvider.h
    # M5 — highlighting and session persistence (all UI-free, name/index based so
    # they are theme- and reindex-portable, ARCHITECTURE.md §8/§12.4). The third
    # member of that milestone, the preset store, is gated below.
    Palette.cpp
    Palette.h
    Highlight.cpp
    Highlight.h
    # M19 — how often a rule carrying the Notify action may interrupt. QObject-free and
    # clock-injected on purpose: the rate limit is the part with no honest CI coverage
    # of its delivery, so the part that DECIDES has to be testable without a desktop.
    AlertPolicy.cpp
    AlertPolicy.h
    # AtomicJson stays here whatever the preset gate says: HostBookmarkStore writes
    # through it too.
    AtomicJson.cpp
    AtomicJson.h
    SessionStore.cpp
    SessionStore.h
)

# MappedLogSource is POSIX-only (mmap). On Windows the BufferedLogSource is the
# read strategy; the mmap source is simply not compiled there.
if(NOT WIN32)
    target_sources(loftail_core PRIVATE MappedLogSource.cpp)
endif()

# Taking a connected socket away from Qt so a third-party library can be its only
# reader. ALWAYS compiled although only the SSH transport uses it: the behaviour it
# works around is Qt's, not libssh2's, so tst_socketdetach pins it in every build.
target_sources(loftail_core PRIVATE
    SocketDetach.cpp
    SocketDetach.h
    # The shell commands used on a server that will not do SFTP, and the parsing of what
    # they print. Always compiled, like RemoteLocation: the quoting in it is a security
    # boundary, and one that exists in only one build configuration is one that gets
    # tested in only one build configuration.
    SshExecCommands.cpp
    SshExecCommands.h
    # M16 — which of those commands can measure the file on THIS server, and the proof
    # that the answer is usable before anything relies on it. Always compiled for the
    # same reason and one more: it is the only decision in the exec transport, and the
    # transport itself is reachable in one build configuration on one kind of server.
    ExecSizeProbe.cpp
    ExecSizeProbe.h
)
target_link_libraries(loftail_core PRIVATE Qt6::Network)

# M11 — the only libssh2-touching translation units. Same shape as the POSIX-only
# source above: the seam is always compiled, one implementation is conditional.
# PUBLIC on the definition so loftail_ui and the tests see the same feature state.
if(LOFTAIL_HAVE_SSH)
    target_sources(loftail_core PRIVATE
        SshSession.cpp
        SshSession.h
        SshFetcher.cpp
    )
    target_compile_definitions(loftail_core PUBLIC LOFTAIL_HAVE_SSH=1)
    target_link_libraries(loftail_core PRIVATE ${LOFTAIL_SSH_TARGET})
    # Qt6::Network for QTcpSocket: the SSH transport hands libssh2 a connected socket
    # rather than reimplementing name resolution and connect timeouts.
    target_link_libraries(loftail_core PRIVATE Qt6::Network)
endif()

# M12 — the only libarchive-touching translation units, gated the same way. ArchiveReader
# is the sole place archive.h is included; ArchiveFetcher rides on it and would compile
# anywhere, but has nothing to do without it.
if(LOFTAIL_HAVE_ARCHIVE)
    target_sources(loftail_core PRIVATE
        ArchiveReader.cpp
        ArchiveFetcher.cpp
    )
    target_compile_definitions(loftail_core PUBLIC LOFTAIL_HAVE_ARCHIVE=1)
    target_link_libraries(loftail_core PRIVATE ${LOFTAIL_ARCHIVE_TARGET})
    # PUBLIC: the test binaries call libarchive's write side directly to build their
    # fixtures, so they need the same declaration or they link against import stubs
    # that a static build never produces (see the option in the root CMakeLists).
    if(LOFTAIL_ARCHIVE_STATIC)
        target_compile_definitions(loftail_core PUBLIC LIBARCHIVE_STATIC)
    endif()
endif()

# M14 — the only QtKeychain-touching translation unit, gated exactly as SshSession.cpp and
# ArchiveReader.cpp are. PUBLIC on the definition for the same reason again: the dialog
# wording in loftail_ui and the assertions in the tests must see the same feature state.
#
# PRIVATE linkage is the point of the second line. On Linux this pulls a transitive
# Qt6::DBus in behind loftail_core, but nothing of loftail's own includes a DBus header and
# the rule here is "never QtWidgets", not "QtCore only in the linker" — QtGui is already
# linked below for QColor.
if(LOFTAIL_HAVE_KEYCHAIN)
    target_sources(loftail_core PRIVATE
        KeychainSecretStore.cpp
        KeychainSecretStore.h
    )
    target_compile_definitions(loftail_core PUBLIC LOFTAIL_HAVE_KEYCHAIN=1)
    target_link_libraries(loftail_core PRIVATE ${LOFTAIL_KEYCHAIN_TARGET})
endif()

# The preset store, gated in the shape of the three blocks above — with no
# target_link_libraries, because unlike them this gates a feature and not a dependency
# (the option itself is the answer; see the top-level CMakeLists). PUBLIC on the
# definition for the same reason they are: loftail_ui and every test must see the same
# feature state, and it is what MainWindow's presets pane and tst_panechrome's expected
# dock list are compiled against.
if(LOFTAIL_HAVE_PRESETS)
    target_sources(loftail_core PRIVATE
        PresetStore.cpp
        PresetStore.h
    )
    target_compile_definitions(loftail_core PUBLIC LOFTAIL_HAVE_PRESETS=1)
endif()

# QtGui (not QtWidgets) for QColor: the highlight palette stores concrete colors and
# LogModel::data() returns them via the Background/Foreground roles. QColor is a
# plain value type needing no QApplication, so the "unit-testable without a
# QApplication" rule (CLAUDE.md) still holds — the ban is on QtWidgets, not QtGui.
target_link_libraries(loftail_core PUBLIC Qt6::Core Qt6::Gui)

# Windows headers define min() and max() as MACROS, which turn
# `std::numeric_limits<qint64>::min()` — Record::kNoTimestamp, among others — into a
# syntax error in any translation unit that happens to include <windows.h> FIRST.
# Nothing of ours includes it directly; libarchive's public header does, so the
# breakage lands in whichever file includes <archive.h> before a loftail header, which
# is a rule about include ORDER that no one can be expected to remember.
#
# PUBLIC, so loftail_ui and every test inherit it: the failure is not a property of
# this library but of anything downstream that pulls in a Windows header.
if(WIN32)
    target_compile_definitions(loftail_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

target_include_directories(loftail_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions(loftail_core PRIVATE
    LOFTAIL_VERSION="${PROJECT_VERSION}"
)

# The CI stamp (top-level CMakeLists.txt, "Build identity"), and why it is a separate
# definition from LOFTAIL_VERSION rather than part of it: see there. Added only when
# there IS one, rather than defined to the empty string, because an empty string
# definition has to survive the generator's quoting on every toolchain and MSVC is the
# one this project cannot try locally. Version.cpp's #ifndef supplies the empty
# default, so a local build takes a path that is exercised on every dev machine.
if(LOFTAIL_BUILD_ID)
    target_compile_definitions(loftail_core PRIVATE LOFTAIL_BUILD_ID="${LOFTAIL_BUILD_ID}")
endif()
