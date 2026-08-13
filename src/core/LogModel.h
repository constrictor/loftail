#pragma once

#include "Highlight.h"

#include <QAbstractTableModel>
#include <QColor>

namespace loftail {

class Document;
class FilteredIndex;
class RecordIndex;

// Rows are records, columns come from LogFormat::fields (ARCHITECTURE.md §7.2).
// data() parses LAZILY from the mapped bytes and stores nothing (invariant #1):
// the cheap eager fields (priority, interned logger/thread, timestamp) come
// straight from the 32-byte Record; the message and any post-message fields are
// decoded on demand on the paint path. It stays a QAbstractTableModel even though
// the view is the custom LogView, so the later proxy-filter machinery (M4) and
// the model/view separation stay intact.
//
// The custom LogView (M2b) drives painting; a QTableView also works for the
// throwaway M2a prototype since single-line records render fine there.
class LogModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    explicit LogModel(const Document *document, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

    // Convenience for the view: the display text of one cell without a
    // QModelIndex round-trip.
    QString cellText(int row, int column) const;

    // --- Which subset of the document this model shows (M19, ARCHITECTURE.md §7.5) ---
    //
    // A model reads ONE FilteredIndex to turn view rows into source records; everything
    // else it touches — the intern tables, the format, the decoder, the bytes — is the
    // document's source index and is shared. Pointing a second model at a second
    // FilteredIndex over the same document is therefore the whole of what a digest strip
    // needs, and it is why the digest is not a second model class duplicating cellText().
    //
    // nullptr, the default, means the document's own filtered view — so every existing
    // caller behaves exactly as it did, which is why tst_logmodel, tst_logview,
    // tst_filtercontext and tst_tail pass unaltered. The index must outlive the model.
    void setViewIndex(const FilteredIndex *index);
    const FilteredIndex *viewIndex() const { return m_view; }

    // The RecordIndex the view scrolls over — the compact one when this model's view
    // index is active, the document's source index otherwise. LogView's geometry reads
    // this rather than the document, so there is ONE seam and not two.
    const RecordIndex &viewGeometry() const;

    // View row -> source record ordinal in THIS model's view index, or -1.
    int sourceRow(int viewRow) const;

    // The inverse, and its nearest-survivor form: a source ordinal is the one coordinate
    // a filter change does not move, so it is what a view anchors its selection and its
    // scroll position to across a re-apply (SPEC.md §6, ARCHITECTURE.md §7.1.2).
    // viewRowAtOrAfter() is meaningful only over an ascending subset — a filter's, never
    // the digest's, whose ordinals publishDigest() reorders by timestamp.
    int viewRowOf(int sourceRow) const;
    int viewRowAtOrAfter(int sourceRow) const;

    // Which highlight action decides this model's row colours (M19, SPEC.md §7).
    // Color for the log itself; Digest for the digest strip, so a digest row wears the
    // colours of the rule that put it there whether or not that rule also colours the
    // log — which is what tells the user which rule a row is for.
    void setHighlightAction(HighlightAction action) { m_action = action; }
    HighlightAction highlightAction() const { return m_action; }

    // Highlighting (M5): the active theme decides which of each palette slot's two
    // colors data() resolves for the Background/Foreground roles (SPEC.md §7). The
    // UI sets this from the real widget palette and updates it on a theme change; it
    // defaults to light so core tests are deterministic without a QApplication.
    void setDarkTheme(bool dark) { m_darkTheme = dark; }
    bool darkTheme() const { return m_darkTheme; }

    // Filter reset hooks (M4). The caller wraps Document::applyFilters() between
    // these so the view, header, and selection model refresh over the new visible
    // set (SPEC.md §6): the filtered set is a wholesale row remap, so a model reset
    // is the correct, cheap signal — LogView::handleModelReset already rebuilds its
    // geometry and drops any stale estimation cache over the filtered subset.
    void beginFilterReset();
    void endFilterReset();

    // Batched-append hooks for the worker-thread indexer (M2b, §7.2). The caller
    // appends `count` records to the Document's index BETWEEN these two calls, so
    // rowCount() reads the old size at beginAppendRows() and the new size after
    // endAppendRows(). This is the only way rows enter the model — records are
    // never removed while indexing, so plain insert/endInsert semantics are safe.
    void beginAppendRows(int count);
    void endAppendRows();

    // Live-update hooks (M6). The trailing (provisional) record can be re-evaluated
    // as the file grows: when a filter is active its visible view row may need to be
    // dropped and re-added (removeTail), and in the unfiltered case a trailing record
    // that grew taller with continuation lines is signalled in place (rowChanged) so
    // the view refreshes its line geometry. The caller mutates the Document's
    // index/FilteredIndex to match, exactly as with the append hooks above.
    void beginRemoveTail(int count);
    void endRemoveTail();
    void notifyRowChanged(int row);

    // Both highlight colors for one view row in a SINGLE first-match-wins pass, each
    // invalid where no rule matched or the matched rule leaves that role at the theme
    // default (SPEC.md §7). This is what LogView paints with.
    //
    // Resolving both roles together matters now that a rule can match on message text
    // (SPEC.md §7): asking for the roles separately — as two data() calls do — runs
    // the rule list twice per record and so can cost two decodes where one suffices.
    void rowColors(int row, QColor &background, QColor &foreground) const;

    // One role of the above. Retained for callers and tests that want a single role;
    // prefer rowColors() on the paint path.
    QColor highlightColor(int row, bool background) const;

    // True when this view row is present only because a nearby record matched the
    // filter (M15, SPEC.md §6). The VIEW decides what that looks like: dimming needs
    // the resolved base and text colours, which live in the widget palette, and core
    // links QtCore only. data()'s Background/Foreground roles are deliberately left
    // undimmed — they are the QTableView prototype path, not the paint path.
    bool rowIsContext(int row) const;

private:
    // The matched rule for a view row, or -1. Supplies HighlighterSet::match with the
    // lazy message decode so the text axis costs nothing until a rule's integer axes
    // have admitted the record (invariant #4).
    int matchedRule(int row) const;

    // The subset this model shows: m_view when set, the document's own otherwise. Every
    // place that maps between view rows and source records goes through here and
    // nowhere else.
    const FilteredIndex &view() const;

    const Document      *m_document;
    const FilteredIndex *m_view = nullptr;
    HighlightAction      m_action = HighlightAction::Color;
    bool                 m_darkTheme = false;
};

} // namespace loftail
