// loftail — a desktop viewer for log4cplus logs.
// Copyright (C) 2026 Valentyn Pavliuchenko
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
QT_END_NAMESPACE

namespace loftail {

class Document;

// Run selection side pane (SPEC.md §3a). A log file often concatenates several app
// runs; the user types a "run-start" regexp that splits the file into runs and picks
// ONE to view/tail. Like the other panes it binds to the ACTIVE document by signal
// (invariant #7): setDocument() rebinds, never a global "current file".
//
// The pane owns no per-file state of its own: the run-start PATTERN is part of the
// document's FormatSettings (persisted per-file like the format), and the run list +
// current selection live on the Document. The pane edits the pattern (emitting
// runStartChanged) and selects a run (emitting runSelected); MainWindow turns both
// into Document calls wrapped in a model reset, exactly like the Filters pane.
class RunPane : public QWidget
{
    Q_OBJECT

public:
    explicit RunPane(QWidget *parent = nullptr);

    // The list's two fixed rows, and the run index each carries. "All runs" is not a
    // run — it lifts the restriction — and the separator under it is not a row a reader
    // can reach at all; it exists because the runs below it are three-line entries and
    // the whole file above them is a different kind of thing from any of them.
    //
    // THERE IS NO "FOLLOW THE LAST" ROW. Following whichever run is last is what
    // PICKING THE LAST RUN means (SPEC.md §3a): the list reads in file order — the whole
    // file, then the runs oldest to newest — and the bottom row is both the newest run
    // and the standing instruction to keep showing whichever run is newest. A row that
    // says "the last run" beside the row that IS the last run is two names for one
    // answer, and the reader has to be told which of them they are on.
    //
    // The separator is present only where there are runs, so `kFirstRunRow` is a
    // constant whenever it means anything at all.
    static constexpr int kAllRunsRow   = 0;
    static constexpr int kSeparatorRow = 1;
    static constexpr int kFirstRunRow  = 2;  // row of runs().at(0)
    static constexpr int kAllRuns      = -1; // runSelected() payload for row 0

    // The separator carries this role and nothing else, which is how the delegate tells
    // it from a row it has to compose and from one it must hand to the base class.
    static constexpr int kSeparatorRole = Qt::UserRole + 6;

    // A run row is drawn as THREE lines — its name in bold, the span of instants it
    // covers, and what is outstanding in it — so its parts travel as item data and the
    // delegate composes them, rather than one label string being taken apart again at
    // paint time. The "All runs" row at the top carries them too — it resolves to a
    // stretch of this log and reports what it will show — so what the absence of the
    // title role now means is a row with no document behind it, or the separator.
    static constexpr int kRunTitleRole = Qt::UserRole + 1;  // "Run 3"
    static constexpr int kRunTimesRole = Qt::UserRole + 2;  // "10:04:11 - 10:41:57"
    static constexpr int kRunFatalRole = Qt::UserRole + 3;  // int, 0 == absent
    static constexpr int kRunErrorRole = Qt::UserRole + 4;
    static constexpr int kRunWarnRole  = Qt::UserRole + 5;

    // Rebind to a document (or nullptr to clear). Fills the pattern field from the
    // document's configured matcher and rebuilds the run list.
    void setDocument(Document *document);

    // Rebuild the run list + counts from the document. Called as indexing finishes
    // and on each live append/rescan so newly-detected runs appear (SPEC.md §3a).
    // Does NOT touch the pattern field, so it never clobbers in-progress typing.
    void refresh();

signals:
    // The user changed the run-start query. MainWindow stores it in the document's
    // FormatSettings (persisting it), reconfigures the Document, and re-applies.
    void runStartChanged(const QString &pattern, bool regex, bool caseSensitive);

    // The user chose a run to view: an index into Document::runs(), or kAllRuns for the
    // explicit "All runs" entry (no restriction). Picking the run that is LAST is how
    // the user asks to follow whichever run is last, and that is decided one layer down
    // (Document::selectRun) rather than by a sentinel on this signal — every route into
    // a run selection, this pane's and the record menu's alike, means the same thing by
    // it.
    void runSelected(int runIndex);

protected:
    // The zebra band below is a colour derived from the CURRENT theme's Base and Text,
    // written into the list's own palette — so it has to be re-derived when the theme
    // moves, exactly as HighlighterPane re-paints its swatches.
    void changeEvent(QEvent *event) override;

private:
    void buildUi();
    // Give the run list a visible alternating band (SPEC.md §3a). QPalette::AlternateBase
    // is the role the style reads and nothing obliges a theme to make it differ from
    // Base, which is the trap UiColors::alternateRowColor() exists for — the log table's
    // own band measured 1.00:1 on a white theme for eight milestones. Only that one role
    // is written, so Base, Text and Highlight keep tracking the theme.
    void applyZebraColour();
    void emitPattern();
    void rebuildRunList();
    // Re-word the note under the Apply button: quiet while the field agrees with the
    // pattern in force, active once it does not. Writes ONLY on a real change of state
    // — it runs on every live append (rebuildRunList) and a setStyleSheet() there is a
    // full style repolish per tick, which is the same "rewrite only on a real change"
    // rule the dock titles and tab labels keep.
    void updateApplyNote();

    Document  *m_document = nullptr;
    bool       m_populating = false; // guards signal storms while repopulating

    QLineEdit  *m_patternEdit = nullptr;
    QCheckBox  *m_regex = nullptr;
    QCheckBox  *m_case = nullptr;
    QPushButton *m_apply = nullptr;
    QLabel     *m_applyNote = nullptr; // why this pane has an Apply button at all
    QLabel     *m_info = nullptr;   // "N runs" / invalid-regex notice
    // Tri-state so the first write always happens: -1 is "nothing written yet", 0 quiet,
    // 1 active. See updateApplyNote().
    int         m_noteState = -1;
    // The run list is the one thing in this pane that GROWS: it is as long as the log
    // has runs, which is unknown when the pane is built and changes while it scans.
    // Row 0 is "All runs", row 1 is the separator where there are runs at all, and
    // row i + kFirstRunRow is runs().at(i) — the bottom one of which is also what
    // following the last run means.
    QListWidget *m_runList = nullptr;
};

} // namespace loftail
