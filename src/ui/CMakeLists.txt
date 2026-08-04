# loftail_ui: the Qt Widgets layer. Links QtWidgets and loftail_core.
add_library(loftail_ui STATIC
    # M12 — archived logs: which log to read out of an archive holding several. Always
    # compiled, like the dialog below, so both build configurations look the same.
    OpenArchiveDialog.cpp
    OpenArchiveDialog.h
    # M11 — remote logs: the Open Remote dialog (which also manages saved hosts, since
    # a global list has no active Document to bind a pane to — invariant #7) and the
    # widget-side answers to the host-key and password questions.
    OpenRemoteDialog.cpp
    OpenRemoteDialog.h
    SshPromptDialogs.cpp
    SshPromptDialogs.h
    MainWindow.cpp
    MainWindow.h
    # Per-file machinery (model, controllers, format choice) and one view onto it
    DocumentContext.cpp
    DocumentContext.h
    DocumentView.cpp
    DocumentView.h
    # The fixed-pitch font shared by the record view and the format preview
    Fonts.cpp
    Fonts.h
    # M2b — the production custom record view
    LogView.cpp
    LogView.h
    # M3 — the Log Format dialog
    LogFormatDialog.cpp
    LogFormatDialog.h
    # M4 — the Filters side pane and the Find bar. The five axis controls themselves
    # live in AxisEditor, shared with the per-rule editor in the Highlighters pane so
    # filtering and highlighting offer the same criteria and cannot drift apart.
    AxisEditor.cpp
    AxisEditor.h
    FilterPane.cpp
    FilterPane.h
    FindBar.cpp
    FindBar.h
    # M5 — the Highlighters and Presets side panes
    HighlighterPane.cpp
    HighlighterPane.h
    PresetPane.cpp
    PresetPane.h
    # Run selection side pane (a run-start regexp splits the file into runs)
    RunPane.cpp
    RunPane.h
)

target_link_libraries(loftail_ui PUBLIC
    Qt6::Widgets
    loftail_core
)

target_include_directories(loftail_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
