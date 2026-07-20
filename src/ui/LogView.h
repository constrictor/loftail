#pragma once

#include <QAbstractScrollArea>
#include <QVector>

QT_BEGIN_NAMESPACE
class QHeaderView;
class QItemSelectionModel;
QT_END_NAMESPACE

namespace loftail {

class Document;
class LogModel;
class RecordIndex;

// The production record table (invariant #6, ARCHITECTURE.md §7.1). A custom
// QAbstractScrollArea, NOT a QTableView, because multi-line records render at full
// height and QTableView cannot do variable row heights lazily.
//
// Scrolling is in LINE units over the two-level prefix sums of Record::lineCount:
// the vertical scrollbar range is the total display-line count, recordAtScrollLine
// resolves a scroll position to a record in O(log n), and painting walks forward
// from the first visible record so only visible records are ever touched.
//
// Geometry has two EXACT modes (M2b); the estimated always-on mode (M2c) is not
// built here and is unreachable from this path (see WrapMode):
//   * Off               — every record is its unwrapped physical-line height.
//   * SelectedRecordOnly — the one selected record wraps to the available width;
//     its height is measured directly and patched into the line mapping, so the
//     geometry stays exact because exactly one record's height varies.
//
// Reused: QItemSelectionModel for selection, a QHeaderView for column geometry
// (giving resize/reorder/hide and remembered layout for free), and LogModel for
// lazy cell text. Hand-rolled: hit-testing, range selection, keyboard navigation,
// and clipboard serialization.
class LogView : public QAbstractScrollArea
{
    Q_OBJECT

public:
    enum class WrapMode {
        Off,                // long lines extend horizontally (SPEC.md §5)
        SelectedRecordOnly, // only the focused record wraps
        // AlwaysOn is deliberately absent — it needs estimated geometry (M2c).
    };

    LogView(const Document *document, LogModel *model, QWidget *parent = nullptr);
    ~LogView() override;

    QItemSelectionModel *selectionModel() const { return m_selection; }
    QHeaderView *header() const { return m_header; }

    WrapMode wrapMode() const { return m_wrapMode; }
    void setWrapMode(WrapMode mode);

    // Column layout persistence (SPEC.md §5): the QHeaderView's own state carries
    // section order, sizes, and hidden flags. M5 folds this into full session
    // restore; the round-trip itself lives here.
    QByteArray saveColumnState() const;
    bool restoreColumnState(const QByteArray &state);

    // Clipboard actions (SPEC.md §5). Raw yields the records' original bytes; the
    // columns form is tab-separated fields for spreadsheet paste.
    void copySelectionRaw() const;
    void copySelectionAsColumns() const;

    // Move to a record and scroll it into view. Used by the model-reset handling
    // and by callers that open at the file's end (SPEC.md §3).
    void setCurrentRecord(int record, bool extendSelection = false);
    int currentRecord() const { return m_current; }
    void scrollToEnd();

    // --- Pure geometry mapping (public for unit tests; no widget state) ---------
    // These express the exact-mode line<->record mapping with the selected record's
    // measured wrapped height folded in. `selRecord` is -1 when nothing wraps;
    // `selWrapLines` is that record's height in display lines when wrapping (equal
    // to its unwrapped display height otherwise).
    static qint64 totalScrollLines(const RecordIndex &idx, int selRecord, int selWrapLines);
    static qint64 scrollLineOfRecord(const RecordIndex &idx, int selRecord, int selWrapLines, int r);
    static int recordAtScrollLine(const RecordIndex &idx, int selRecord, int selWrapLines, qint64 line);

    // --- Pure clipboard helpers (public for unit tests) -------------------------
    // Flatten a cell so embedded newlines/tabs cannot break TSV row/column structure.
    static QString flattenCell(const QString &text);
    // Join a grid of already-flattened cells into tab/newline-separated text.
    static QString columnsToTsv(const QVector<QVector<QString>> &rows);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void scrollContentsBy(int dx, int dy) override;

private slots:
    void handleRowsInserted();
    void handleModelReset();

private:
    int lineHeight() const;
    int visibleLines() const;
    int recordCount() const;

    // The record whose height varies under the current wrap mode, or -1 when
    // nothing wraps (wrap off, or no selection). Drives the geometry statics.
    int selRecordForGeometry() const;
    QVector<int> selectedRecordsSorted() const;

    // Height, in display lines, of record r as currently rendered (folds in the
    // selected-record wrap when applicable).
    int recordHeightLines(int r) const;
    // The selected record's wrapped height in display lines, or its unwrapped
    // display height when it does not wrap. Drives the geometry statics above.
    int selWrapLines() const;
    // Measure how many visual lines the message text occupies at `width` px.
    int measureWrappedLines(const QString &text, int width) const;
    int messageColumn() const; // logical column of the Message field, or -1

    void recomputeGeometry();     // recompute selection-dependent wrap + scrollbars
    void updateScrollBars();
    void layoutHeader();
    void ensureRecordVisible(int record);
    int recordAtViewportY(int y) const; // hit-test, or -1
    void selectRange(int anchor, int current);

    const Document *m_document;
    LogModel       *m_model;
    QHeaderView    *m_header;
    QItemSelectionModel *m_selection;

    WrapMode m_wrapMode = WrapMode::Off;
    int      m_current = -1;   // focused record (drives keyboard nav + wrap)
    int      m_anchor = -1;    // range-selection anchor
    int      m_selWrapCache = -1; // memoized selWrapLines() for the current width
};

} // namespace loftail
