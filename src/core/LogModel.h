#pragma once

#include <QAbstractTableModel>

namespace loftail {

class Document;

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

private:
    const Document *m_document;
};

} // namespace loftail
