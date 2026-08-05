#pragma once

#include "EstimatedGeometry.h"

#include <QAbstractScrollArea>
#include <QVector>

QT_BEGIN_NAMESPACE
class QContextMenuEvent;
class QHeaderView;
class QItemSelectionModel;
class QTimer;
class QToolButton;
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

    // What an EMPTY view says instead of showing a blank grid — "app.log has not
    // appeared yet" for a log that is being waited for (SPEC.md §3, M13). Empty by
    // default, which is the right answer for an empty log: there is nothing wrong with
    // it and nothing to explain. Only drawn when there are no records, so it costs the
    // paint path nothing.
    void setPlaceholderText(const QString &text);
    const QString &placeholderText() const { return m_placeholderText; }

    // How many records are IN VIEW — the filtered subset when a filter is active, the
    // whole index otherwise (invariant #6). Public so a caller can tell an empty view
    // from a populated one without reaching through to the model.
    int recordCount() const;

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

    // Follow mode (SPEC.md §3, M6). Every open starts following: as records are
    // appended the view stays pinned to the newest. Scrolling away from the bottom
    // DETACHES follow (history stays put while the file keeps growing); returning to
    // the bottom — by the scrollbar or the return-to-bottom control — RE-ATTACHES it.
    bool following() const { return m_following; }
    void followTail(); // re-attach: jump to the end and resume following

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
    void contextMenuEvent(QContextMenuEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void scrollContentsBy(int dx, int dy) override;

signals:
    // Follow attached/detached, so the window can reflect it (menu check, control).
    void followingChanged(bool following);

    // A record was right-clicked (SPEC.md §5). The view reports WHERE — the view row,
    // the column under the cursor, and where to pop up — and nothing else: the menu
    // is assembled by the window, which is the only place that can reach both the
    // record's fields and the panes the items edit. `column` is the logical column,
    // or -1 past the last one; it ranks the menu without changing its contents.
    void recordMenuRequested(int viewRow, int column, const QPoint &globalPos);

private slots:
    void handleRowsInserted();
    void handleRowsRemoved();
    void handleTailChanged(); // a trailing record grew in place (M6 live update)
    void handleModelReset();
    void applyDebouncedResize();

private:
    int lineHeight() const;
    int visibleLines() const;

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

    void setFollowingState(bool following); // update state + control + emit signal
    void updateFollowFromScrollPosition();  // detach/re-attach from the scrollbar
    void positionFollowButton();            // place the return-to-bottom overlay

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
    QString  m_placeholderText; // drawn centred when there are no records at all
    int      m_current = -1;   // focused record (drives keyboard nav + wrap)
    int      m_anchor = -1;    // range-selection anchor
    int      m_selWrapCache = -1; // memoized selWrapLines() for the current width

    // Estimated geometry for AlwaysOn (M2c). Only ever constructed/consulted in
    // AlwaysOn; switching to an exact mode leaves it untouched so its cache
    // survives a round-trip through the exact modes.
    EstimatedGeometry m_estimated;
    QTimer  *m_resizeTimer = nullptr; // debounces width-change remeasurement

    // Follow state (M6). Every open follows the tail; scrolling away detaches.
    bool         m_following = true;
    bool         m_inFollowScroll = false; // guards the programmatic scroll-to-end
    QToolButton *m_followButton = nullptr; // return-to-bottom overlay, shown detached
};

} // namespace loftail
