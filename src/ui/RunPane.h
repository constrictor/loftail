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

    // The user chose a run to view: an index into Document::runs(), or -1 for the
    // explicit "All runs" entry (no restriction).
    void runSelected(int runIndex);

private:
    void buildUi();
    void emitPattern();
    void rebuildRunList();

    Document  *m_document = nullptr;
    bool       m_populating = false; // guards signal storms while repopulating

    QLineEdit  *m_patternEdit = nullptr;
    QCheckBox  *m_regex = nullptr;
    QCheckBox  *m_case = nullptr;
    QPushButton *m_apply = nullptr;
    QLabel     *m_info = nullptr;   // "N runs" / invalid-regex notice
    // The run list is the one thing in this pane that GROWS: it is as long as the log
    // has runs, which is unknown when the pane is built and changes while it scans.
    // Row 0 is "All runs"; row i+1 is runs().at(i).
    QListWidget *m_runList = nullptr;
};

} // namespace loftail
