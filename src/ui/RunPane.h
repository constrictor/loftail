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

    // The list's two fixed rows, and the run index each carries. Neither is a run:
    // "All runs" lifts the restriction, and "Follow the last" is a standing instruction
    // to show whichever run is last — which is why it needs a sentinel of its own rather
    // than the ordinal it currently means, the whole point being that the ordinal
    // changes underneath it (SPEC.md §3a).
    //
    // The list reads in FILE order — "All runs" first, then the runs from oldest to
    // newest — and "Follow the last" sits at the BOTTOM, beside the newest run it
    // resolves to and where a reader watching a live log is already looking. So the two
    // fixed rows are the two ENDS of the list and only the top one has a constant row
    // number; the follow row is `count() - 1`, whatever the log turned out to hold, and
    // `followRow()` is what says so to a caller (tests included) rather than each of
    // them writing the arithmetic out.
    static constexpr int kAllRunsRow  = 0;
    static constexpr int kFirstRunRow = 1;   // row of runs().at(0)
    static constexpr int kLastRun     = -2;  // runSelected() payload for the follow row
    static constexpr int kAllRuns     = -1;  // ...and for row 0
    int followRow() const;

    // A run row is drawn as THREE lines — its name in bold, the span of instants it
    // covers, and what is outstanding in it — so its parts travel as item data and the
    // delegate composes them, rather than one label string being taken apart again at
    // paint time. The two mode rows at the ends of the list carry them too — they
    // resolve to a stretch of this log and report what they will show — so what the
    // absence of the title role now means is a row with no document behind it.
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

    // The user chose a run to view: an index into Document::runs(), kAllRuns for the
    // explicit "All runs" entry (no restriction), or kLastRun for "whichever run is
    // last", which keeps moving as the log grows.
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
    // Row 0 is "All runs" and the bottom row is "Follow the last"; row i + kFirstRunRow
    // is runs().at(i).
    QListWidget *m_runList = nullptr;
};

} // namespace loftail
