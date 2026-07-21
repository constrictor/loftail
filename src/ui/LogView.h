#pragma once

#include "EstimatedGeometry.h"

#include <QAbstractScrollArea>
#include <QVector>

QT_BEGIN_NAMESPACE
class QHeaderView;
class QItemSelectionModel;
class QTimer;
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
// Geometry has two EXACT modes (M2b) plus one ESTIMATED mode (M2c). The exact
// path never touches the estimation machinery, and the estimated path never
// touches the exact statics/selWrap machinery — every geometry query routes
// through the map* wrappers, which branch on the mode once (see WrapMode):
//   * Off               — every record is its unwrapped physical-line height.
//   * SelectedRecordOnly — the one selected record wraps to the available width;
//     its height is measured directly and patched into the line mapping, so the
//     geometry stays exact because exactly one record's height varies.
//   * AlwaysOn          — EVERY record wraps to the viewport width, so heights
//     depend on width and cannot be known without measuring. Geometry is handled
//     by EstimatedGeometry (ARCHITECTURE.md §7.1.1): visited blocks measured and
//     cached keyed by column count, the rest estimated, the scrollbar refining as
//     blocks are measured, and a debounced resize remeasuring once per drag.
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
        AlwaysOn,           // every record wraps; estimated geometry (M2c, §7.1.1)
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

    // The estimated-geometry cache backing AlwaysOn (M2c). Exposed const for
    // tests (measurement refinement, width-keyed invalidation, and that switching
    // to an exact mode leaves the cache untouched); meaningful only in AlwaysOn.
    const EstimatedGeometry &estimatedGeometry() const { return m_estimated; }
    // Force the block containing `record` to be measured now (decodes the block's
    // records via LogModel and folds exact heights into m_estimated). Normally
    // driven lazily from painting; exposed so tests can drive it deterministically
    // without a shown window. No-op outside AlwaysOn.
    void measureBlockOfRecord(int record);

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
    void applyDebouncedResize();

private:
    int lineHeight() const;
    int visibleLines() const;
    int recordCount() const;

    // The RecordIndex the view scrolls over: the filtered subset when a filter is
    // active, the full index (identity) otherwise (M4, invariant #6). Every
    // geometry query and the paint loop address records in THIS index's row space
    // (view rows); LogModel maps those back to source records for cell text.
    const RecordIndex &geom() const;

    // --- Mode-branching geometry wrappers --------------------------------------
    // Every geometry query goes through these; they branch on the wrap mode ONCE.
    // In AlwaysOn (with a message column) they consult m_estimated; otherwise they
    // forward to the exact statics with the selected-record wrap folded in. This
    // is the single seam that keeps the estimation machinery unreachable from the
    // exact path and vice versa (invariant #6).
    bool estimating() const;
    qint64 mapTotalLines() const;
    qint64 mapLineOfRecord(int r) const;
    int    mapRecordAtLine(qint64 line) const;
    int    mapRecordHeightLines(int r) const;

    // AlwaysOn support: characters that fit across the message column, block
    // measurement from decoded text, and keeping the visible blocks measured.
    int viewportCols() const;
    void ensureEstimatorBound();
    void measureBlock(int block);
    void measureVisibleBlocks();

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

    // Resolve one row's background and text color for painting (M5). Selection wins;
    // otherwise the highlight rules speak through the model's Background/Foreground
    // roles (SPEC.md §7), each falling back to the theme color (with a subtle zebra
    // on the background when no rule applies).
    void resolveRowColors(int row, bool selected, QColor &bg, QColor &fg) const;

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

    // Estimated geometry for AlwaysOn (M2c). Only ever constructed/consulted in
    // AlwaysOn; switching to an exact mode leaves it untouched so its cache
    // survives a round-trip through the exact modes.
    EstimatedGeometry m_estimated;
    QTimer  *m_resizeTimer = nullptr; // debounces width-change remeasurement
};

} // namespace loftail
