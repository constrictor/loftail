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

    // Convenience for the prototype view: the display text of one cell without a
    // QModelIndex round-trip.
    QString cellText(int row, int column) const;

private:
    const Document *m_document;
};

} // namespace loftail
