# loftail_core: UI-free logic. Links QtCore only — never QtWidgets — so that
# parsing/indexing code stays unit-testable without a QApplication instance
# (see CLAUDE.md conventions). LogModel is a QAbstractTableModel (QtCore), so the
# whole model/index/parse stack lives here without pulling in Widgets.
add_library(loftail_core STATIC
    Version.cpp
    Version.h
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
    # Saved hosts. A file rather than QSettings because it may hold a password the
    # user chose to keep, and a file can be restricted to its owner.
    HostBookmarkStore.cpp
    HostBookmarkStore.h
    MappedLogSource.h
    BufferedLogSource.cpp
    BufferedLogSource.h
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
    FormatCache.cpp
    FormatCache.h
    FormatPreview.cpp
    FormatPreview.h
    # M8 — format autodetection: candidate-library scoring + priority-anchored
    # structural inference, behind the same IFormatProvider seam (no UI, §9).
    FormatDetector.cpp
    FormatDetector.h
    DetectingFormatProvider.cpp
    DetectingFormatProvider.h
    # M5 — highlighting, presets, session persistence (all UI-free, name/index
    # based so they are theme- and reindex-portable, ARCHITECTURE.md §8/§12.4).
    Palette.cpp
    Palette.h
    Highlight.cpp
    Highlight.h
    AtomicJson.cpp
    AtomicJson.h
    PresetStore.cpp
    PresetStore.h
    SessionStore.cpp
    SessionStore.h
)

# MappedLogSource is POSIX-only (mmap). On Windows the BufferedLogSource is the
# read strategy; the mmap source is simply not compiled there.
if(NOT WIN32)
    target_sources(loftail_core PRIVATE MappedLogSource.cpp)
endif()

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

# QtGui (not QtWidgets) for QColor: the highlight palette stores concrete colors and
# LogModel::data() returns them via the Background/Foreground roles. QColor is a
# plain value type needing no QApplication, so the "unit-testable without a
# QApplication" rule (CLAUDE.md) still holds — the ban is on QtWidgets, not QtGui.
target_link_libraries(loftail_core PUBLIC Qt6::Core Qt6::Gui)

target_include_directories(loftail_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions(loftail_core PRIVATE
    LOFTAIL_VERSION="${PROJECT_VERSION}"
)
