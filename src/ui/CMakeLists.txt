# loftail — a desktop viewer for log4cplus logs.
# Copyright (C) 2026 Valentyn Pavliuchenko
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
# SPDX-License-Identifier: GPL-3.0-or-later

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
    # M23 — what the restart script did. Modal, because this is the one thing loftail
    # does that changes something on a machine, and abortable for the same reason.
    RestartDialog.cpp
    RestartDialog.h
    # Which open log to take a highlight rule list from. Holds strings and ints only —
    # the window renders the candidates and this answers with an index — so it links
    # nothing of core's and is testable with no log anywhere.
    CopyHighlightersDialog.cpp
    CopyHighlightersDialog.h
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
    # M2b — the production custom record view, and the per-codepoint wrap model its
    # estimated geometry measures records through (a fallback face's advance and an
    # overhanging glyph's ink are both wider than a column count can express)
    LogView.cpp
    LogView.h
    # The density strip in that view's right viewport margin: where the highlighted
    # records and the current find matches are in the whole log, drawn in the colours the
    # highlighter already resolves, and clickable to jump there.
    DensityScrollBar.cpp
    DensityScrollBar.h
    WrapMetrics.cpp
    WrapMetrics.h
    # M3 — the format editor. Its controls live in FormatEditor, shared with
    # the Preferences dialog below so the per-file format and the default for new files
    # are entered through one widget and cannot drift apart.
    FormatEditor.cpp
    FormatEditor.h
    # M18 — application-wide settings: the default log format a never-seen file is tried
    # with, and the escape hatch for the per-file formats that outrank it.
    PreferencesDialog.cpp
    PreferencesDialog.h
    # M20 — every field one settings node carries, on one page: FormatEditor
    # plus run splitting, timestamp display and the wrap mode a new view starts in.
    LogProfileEditor.cpp
    LogProfileEditor.h
    # The config-file editor's syntax layer: which grammar a config file is coloured in
    # (extension first, contents only where the extension says nothing) and the one
    # highlighter that draws all three. A rule TABLE per grammar rather than a class per
    # grammar, because they differ only in their patterns and the role each maps to.
    ConfigSyntax.cpp
    ConfigSyntax.h
    # The config-file editor page itself — the PEER of DocumentView, and the second kind
    # of page the document well has ever held. Holds no Document and no DocumentContext:
    # a config file is not a log. What it shares with a log tab is the Find bar, verbatim,
    # and the log font.
    ConfigView.cpp
    ConfigView.h
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
    # A wrapping message label that reports its own wrapped height, because a layout
    # sizing a word-wrapped QLabel from its hint reserves a row the text does not fit in
    MessageLabel.h
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
    # The command line main() is handed: the options, the help text, and every file
    # named on it. Here rather than in main.cpp so a test can drive it without a process
    CommandLine.cpp
    CommandLine.h
    # What each open log is called on its tab, decided for the whole set at once so two
    # logs with one basename say which is which
    TabLabels.cpp
    TabLabels.h
    # A disclosure triangle over a block of controls, for options with a good default
    CollapsibleSection.cpp
    CollapsibleSection.h
    # What a window with no log open shows: the recent logs and the saved hosts, on the
    # page and openable from it, rather than one sentence naming two menus. Holds strings
    # only and answers with an address — the window owns both enumerations, so the two
    # stores are read on its schedule and once for the menu and this page together.
    WelcomeView.cpp
    WelcomeView.h
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
