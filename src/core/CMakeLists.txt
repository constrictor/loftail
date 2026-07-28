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

# QtGui (not QtWidgets) for QColor: the highlight palette stores concrete colors and
# LogModel::data() returns them via the Background/Foreground roles. QColor is a
# plain value type needing no QApplication, so the "unit-testable without a
# QApplication" rule (CLAUDE.md) still holds — the ban is on QtWidgets, not QtGui.
target_link_libraries(loftail_core PUBLIC Qt6::Core Qt6::Gui)

target_include_directories(loftail_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions(loftail_core PRIVATE
    LOFTAIL_VERSION="${PROJECT_VERSION}"
)
