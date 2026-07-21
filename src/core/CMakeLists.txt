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
    # M2b — worker-thread indexing controller (QtCore only, no Widgets)
    IndexController.cpp
    IndexController.h
)

# MappedLogSource is POSIX-only (mmap). On Windows the BufferedLogSource is the
# read strategy; the mmap source is simply not compiled there.
if(NOT WIN32)
    target_sources(loftail_core PRIVATE MappedLogSource.cpp)
endif()

target_link_libraries(loftail_core PUBLIC Qt6::Core)

target_include_directories(loftail_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions(loftail_core PRIVATE
    LOFTAIL_VERSION="${PROJECT_VERSION}"
)
