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
    # M3 — the Log Format dialog. The controls it edits live in FormatEditor, shared with
    # the Preferences dialog below so the per-file format and the default for new files
    # are entered through one widget and cannot drift apart.
    FormatEditor.cpp
    FormatEditor.h
    LogFormatDialog.cpp
    LogFormatDialog.h
    # M18 — application-wide settings: the default log format a never-seen file is tried
    # with, and the escape hatch for the per-file formats that outrank it.
    PreferencesDialog.cpp
    PreferencesDialog.h
    # M4 — the Filters side pane and the Find bar. The five axis controls themselves
    # live in AxisEditor, shared with the per-rule editor in the Highlighters pane so
    # filtering and highlighting offer the same criteria and cannot drift apart.
    AxisEditor.cpp
    AxisEditor.h
    FilterPane.cpp
    FilterPane.h
    FindBar.cpp
    FindBar.h
    # M5 — the Highlighters side pane. The Presets pane, the other half of that
    # milestone's pane work, is gated below.
    HighlighterPane.cpp
    HighlighterPane.h
    # Run selection side pane (a run-start regexp splits the file into runs)
    RunPane.cpp
    RunPane.h
    # A checkable group box that can draw a hairline along its own title row instead of a
    # frame round its body — used by the axes and actions of the Highlighters pane's rule
    # editor, which sit inside a framed section already
    SectionBox.cpp
    SectionBox.h
    # Chrome colours that must work on a light AND a dark theme: error red, caution
    # amber, muted secondary text, and the placeholder grey Qt's own role often lacks
    UiColors.cpp
    UiColors.h
    # How the side panes' title bars are painted: no name repeated under the tab that
    # already carries it, and title-bar buttons that can be seen and hit
    PaneTitleStyle.cpp
    PaneTitleStyle.h
    # The application-wide style: the desktop's own, minus the themed icons Qt puts on
    # standard dialog buttons
    AppStyle.cpp
    AppStyle.h
    # Which language the interface is in, and keeping Qt's own strings in the same one
    UiLanguage.cpp
    UiLanguage.h
    # A disclosure triangle over a block of controls, for options with a good default
    CollapsibleSection.cpp
    CollapsibleSection.h
)

# The Presets pane, gated as PresetStore is in loftail_core. No compile definition here:
# LOFTAIL_HAVE_PRESETS arrives PUBLIC from loftail_core, so the two halves of the feature
# cannot fall out of step. AUTOMOC picks the Q_OBJECT up from a conditional target_sources
# exactly as it does for the gated files in core.
if(LOFTAIL_HAVE_PRESETS)
    target_sources(loftail_ui PRIVATE
        PresetPane.cpp
        PresetPane.h
    )
endif()

target_link_libraries(loftail_ui PUBLIC
    Qt6::Widgets
    loftail_core
)

target_include_directories(loftail_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
