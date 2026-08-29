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

#include "EstimatedGeometry.h"
#include "Filter.h"
#include "WrapMetrics.h"
#include "WrapMode.h"

#include <QAbstractScrollArea>
#include <QVector>

#include <functional>

QT_BEGIN_NAMESPACE
class QContextMenuEvent;
class QHeaderView;
class QItemSelectionModel;
class QTimer;
class QToolButton;
QT_END_NAMESPACE

namespace loftail {

class DensityScrollBar;
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
//     cached keyed by wrap width, the rest estimated, the scrollbar refining as
//     blocks are measured, and a debounced resize remeasuring once per drag. What
//     a record's height IS comes from WrapMetrics, this view's own font-keyed
//     greedy fill; EstimatedGeometry only caches and sums the answers.
//
// Reused: QItemSelectionModel for selection, a QHeaderView for column geometry
// (giving resize/reorder/hide and remembered layout for free), and LogModel for
// lazy cell text. Hand-rolled: hit-testing, range selection, keyboard navigation,
// and clipboard serialization.
class LogView : public QAbstractScrollArea
{
    Q_OBJECT

public:
    // Declared in core (WrapMode.h) since M20, because a log's settings node carries the
    // mode a new view of it starts in and nothing in core may see a widget. Aliased here
    // so `LogView::WrapMode::AlwaysOn` still means what it always did.
    using WrapMode = loftail::WrapMode;

    // What this view is FOR (M19, SPEC.md §7). Configuration, not a subclass: the
    // digest strip differs from the log table only in what it is allowed to do.
    //
    //   Main    the record table — scrolls, follows the tail, offers the record menu.
    //   Digest  the strip under it: at most one row per highlight rule, no header of
    //           its own (it borrows the table's column layout), no scrollbars, no
    //           follow control, sized to fit its rows rather than scrolled.
    enum class Role { Main, Digest };

    LogView(const Document *document, LogModel *model, QWidget *parent = nullptr,
            Role role = Role::Main);
    ~LogView() override;

    Role role() const { return m_role; }

    // The most display lines a digest strip will occupy however tall the window is.
    // Twelve is a handful of rules with a multi-line record among them; past that the
    // strip is competing with the log rather than annotating it.
    static constexpr int kDigestMaxLines = 12;

    // How tall this view wants to be. Meaningful in Role::Digest, where it is the
    // widget's whole contract: header (hidden, so zero) + one line per display line +
    // frame, CAPPED — a single digest record can legitimately be a hundred-line stack
    // trace, and the strip must never eat the log it sits under. Only when the cap
    // bites does a vertical scrollbar appear, so "not scrolled" is true in every
    // ordinary case. Role::Main falls through to QAbstractScrollArea's own hint.
    QSize sizeHint() const override;

    // Adopt another view's horizontal scroll position. The digest mirrors the main
    // view's, because "rendered exactly as it is in the log" is otherwise only true at
    // offset zero: mirroring the column STATE lines the columns up until the user
    // scrolls right, and then it does not.
    void setHorizontalOffset(int value);

    // Re-decide the height cap and the scrollbar policy, then updateGeometry(). Public
    // because the cap is a fraction of the PARENT's height, which the strip's own
    // resize cannot observe — DocumentView calls this from its resizeEvent. Deliberately
    // separate from sizeHint(), which must stay a pure query: setting a scrollbar policy
    // relayouts, and layout asks for the size hint.
    void refreshDigestCap();

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
    //
    // A restore speaks for EVERY column's width from then on — see seedColumnWidths().
    QByteArray saveColumnState() const;
    bool restoreColumnState(const QByteArray &state);

    // --- Column widths that fit (SPEC.md §5, ARCHITECTURE.md §7.1) ----------------
    //
    // Seeded from the FONT — the header caption and a per-role allowance of characters
    // — never in raw pixels, since nothing knows how wide a pixel is until the font is
    // resolved. Applied ONLY to columns nobody has spoken for: a drag, a fit, or a
    // restored session claims its column, and no later seed may overwrite it.
    //
    // Called from the constructor, where the caption is all there is to measure; from
    // MainWindow::onIndexFinished, where the intern tables are complete and the
    // Subsystem/Thread columns can take the widest name in the file (invariant #4); on
    // a FONT CHANGE, where every character it measured in has a different advance —
    // which is how a zoom widens the columns nobody has spoken for while leaving a width
    // somebody dragged exactly where they put it; and from applyFormatChange() below,
    // where the columns did not exist at all until now. Never on the paint path, never
    // per ingest tick, and never anywhere a person did not just ask for something.
    void seedColumnWidths();

    // The compiled format changed, so THE SET OF COLUMNS changed — which is a thing no
    // model reset says out loud. The one caller that needs it is a document that opened
    // WAITING (SPEC.md §3): it has no format while it waits, so LogModel::columnCount()
    // is 0, and everything this view did at construction was a no-op over an empty
    // header — layoutChrome() reserved no top margin and gave the header no geometry,
    // and seedColumnWidths() had nothing to measure. Nothing revisits either when the
    // log turns up and the count goes 0 → n, so the tab renders with no header band at
    // all (no dividers, no column menu) and every section at Qt's default 100 px.
    //
    // Its caller must gate on the format having JUST settled and not on the resume: a
    // log that comes and goes resumes over and over, and a re-seed there would overwrite
    // widths the reader had dragged since. What protects an already-laid-out view from a
    // stray call is only seedColumnWidths()'s own m_userSizedColumns test, which speaks
    // for a column somebody sized and for no other.
    void applyFormatChange();

    // Forget every user width and seed the lot again — the header menu's "Reset Widths".
    void resetColumnWidths();

    // "Fit to Contents": the column's widest value rather than a typical one. BOUNDED —
    // the metadata columns read the intern table, everything else a few hundred sampled
    // records — because a log has ten million of them and a menu item may not walk them
    // all (ARCHITECTURE.md §7.1). Also what double-clicking a section divider does.
    // A fit is an explicit choice, so it claims its column exactly as a drag does.
    void fitColumnsToContents();
    void fitColumnToContents(int logical);

    // Select every record IN VIEW (SPEC.md §5) — the filtered subset while a filter is
    // active, the whole index otherwise (invariant #6), which is what makes the command
    // compose with the filters and what the two copy actions then act on.
    //
    // One QItemSelection range whatever the count, so a ten-million-record log costs one
    // object here rather than ten million; and it deliberately does NOT scroll, because
    // selecting everything says nothing about where the reader wants to be.
    void selectAllRecords();

    // --- Marking what Find matched (SPEC.md §5, ARCHITECTURE.md §7.1.4) -----------
    //
    // A search selects a RECORD, which on a 200-character message still leaves the
    // reader hunting for the words that matched. So the view keeps the QUERY — never a
    // list of positions — and re-runs it over the text of the cells it is painting
    // anyway. Two things fall out of that and both are the point: nothing is held for a
    // record that is not on screen (invariant #1), and every repaint remarks from
    // scratch, so a scroll, a resize under wrap, a filter re-apply, a tab switch and an
    // ingest tick all keep the marks with no invalidation to get wrong.
    //
    // It is the SAME TextMatcher MainWindow::runFind() searched with, handed over
    // rather than rebuilt, so a mark and a hit can never disagree about the regex or
    // the case option. An empty or invalid matcher — the default — marks nothing;
    // MainWindow clears it when the query empties, when the regex will not compile and
    // when nothing matched, and DocumentView clears it when the bar closes.
    //
    // Per VIEW (invariant #7): two views of one log may be searching for different
    // things. The digest strip is deliberately never armed — Find walks the table's
    // rows, and a mark in the strip would claim the search had landed there.
    void setFindMatcher(const TextMatcher &matcher);
    void clearFindMatcher();
    const TextMatcher &findMatcher() const { return m_findMatcher; }

    // Clipboard actions (SPEC.md §5). Raw yields the records' original bytes; the
    // columns form is tab-separated fields for spreadsheet paste.
    //
    // Both are BOUNDED (ARCHITECTURE.md §7.1.6). The selection is walked as RANGES,
    // never as one QModelIndex per record; the text is built into ONE reserved QString
    // rather than a list that is then joined; and above copyProgressThreshold() records
    // the walk runs in chunks behind a cancellable progress dialog. A copy that is
    // cancelled — or abandoned because the row space was replaced under it — leaves the
    // clipboard exactly as it was, because the clipboard is written once at the end.
    void copySelectionRaw() const;
    void copySelectionAsColumns() const;

    // How many selected records a copy may take before it says so and offers to stop
    // (SPEC.md §5). BELOW it a copy is a plain synchronous loop that processes no
    // events at all — no dialog, no re-entrancy, byte-for-byte what it always
    // produced. Settable per view so the bounded path can be driven without a
    // four-million-record log; the default is what ships.
    static constexpr int kDefaultCopyProgressThreshold = 100000;
    void setCopyProgressThreshold(int records) { m_copyProgressThreshold = qMax(0, records); }
    int copyProgressThreshold() const { return m_copyProgressThreshold; }

    // Move to a record and scroll it into view. Used by the model-reset handling
    // and by callers that open at the file's end (SPEC.md §3).
    void setCurrentRecord(int record, bool extendSelection = false);
    int currentRecord() const { return m_current; }
    void scrollToEnd();

    // Keep this view where it is across a FILTER re-apply (SPEC.md §6). The scroll
    // position is a line index in the view's OWN line space, which a refilter remaps
    // wholesale, and handleModelReset() drops the selection with it — so these two
    // bracket the caller's model reset and put both back in SOURCE-record terms, the
    // one coordinate a refilter does not move.
    //
    // begin() goes BEFORE LogModel::beginFilterReset(), end() AFTER endFilterReset():
    // QItemSelectionModel clears itself from the same modelReset signal, so a restore
    // running inside the reset would be undone by it.
    //
    // Only around a filter re-apply (MainWindow::applyFiltersFor). Every other model
    // reset — a rotation rescan, a resume from waiting, a digest republish, a
    // setViewIndex — replaces the record space itself, where a source ordinal means
    // something different or nothing.
    void beginFilterUpdate();
    void endFilterUpdate();

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
    // The per-codepoint wrap model those heights are measured through, bound to this
    // view's current font. Exposed const for tests, which hold it against a real
    // QTextLayout at the same width.
    const WrapMetrics &wrapMetrics() const { return m_wrapMetrics; }
    // Force the block containing `record` to be measured now (decodes the block's
    // records via LogModel and folds exact heights into m_estimated). Normally
    // driven lazily from painting; exposed so tests can drive it deterministically
    // without a shown window. No-op outside AlwaysOn.
    void measureBlockOfRecord(int record);

    // The selected record's wrapped height in display lines, or its unwrapped
    // display height when it does not wrap. Drives the geometry statics below, and
    // public for the same reason lineHeight() is: it is the number a SelectedRecordOnly
    // record's rows are counted in, and the only thing the rendered ink can be held
    // against.
    int selWrapLines() const;

    // The pitch one display line is drawn at, in pixels. Public because it is the
    // number the whole height model is expressed in (ARCHITECTURE.md §7.1.1) and the
    // one thing a test can hold the rendered ink against: it must be what QT lays text
    // out at, not what QFontMetrics::height() rounds to.
    int lineHeight() const;

    // The width a wrapped message is laid out in, in pixels, floored at kMinWrapCols
    // characters (ARCHITECTURE.md §7.1.1). ONE expression, used by the estimated column
    // count, by the exact path's selected-record measurement and by both paint sites —
    // the measure and the paint must derive the width identically or a record is given
    // rows the text does not fill, or fewer than it needs and is silently clipped.
    //
    // WHICH width depends on where the column is: the message column's origin to the
    // right edge of the viewport while it is the last visible column, and its OWN
    // SECTION otherwise. A message is only entitled to the space after it while there
    // is nothing after it — the columns are movable and an ordinary log4cplus pattern
    // may put a field after %m, and to the right edge regardless the message is drawn
    // over every one of them on the record's first line (bugs.md 18). The paint clips
    // at the section in that case, which is visible only where the section is narrower
    // than the floor.
    //
    // The floor is in CHARACTERS because log text zooms across 27 point sizes: a
    // pixel constant would be 28 columns at 7 pt and 6 at 30 pt. Without it a message
    // column pushed to the viewport's right edge wraps at one character per line,
    // every record measures the 100-line display cap, and one record fills the screen.
    static constexpr int kMinWrapCols = 20;
    int messageWrapWidth() const;

    // --- The density marks IN the scrollbar (SPEC.md §5, ARCHITECTURE.md §7.1.7) ------
    //
    // Where the highlighted records and the current Find matches sit in the WHOLE view,
    // drawn inside the view's own vertical scrollbar with the thumb translucent over
    // them. Role::Main only — the digest strip has no vertical scrollbar — and null when
    // the reader has switched the marks off, which every caller must therefore expect:
    // the view still has a scrollbar then, just a plain one.
    DensityScrollBar *densityBar() const { return m_density; }
    // Swap the marked scrollbar in or out. One application-wide preference (MainWindow
    // keeps it in QSettings, exactly as it keeps the log text size): it answers a
    // question about how somebody reads, which does not differ between two tabs. The
    // marked bar is WIDER than a plain one, so the viewport narrows and every wrapped
    // height is re-measured — this is not a repaint.
    void setDensityStripVisible(bool visible);
    bool densityStripVisible() const;
    // The highlight rules moved, so the strip's rule lane has to be scanned again. The
    // Find lane is untouched: it is invalidated by setFindMatcher/clearFindMatcher, and
    // one gesture must not throw away the other one's work.
    void invalidateDensityRules();

    // Where view row `r` sits in the vertical scroll range, 0..1. In LINE units, which
    // is what the scrollbar is in — placing a mark by record index instead would put it
    // somewhere the thumb never goes on any log carrying a multi-line record, which is
    // most of them. The scroll range is 0..total-page with a page-sized thumb, so this
    // fraction of the bar's height IS where the thumb showing that record starts, which
    // is what DensityScrollBar places its marks by.
    qreal scrollFractionOfRow(int r) const;

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
    // Tooltips for what does not fit (SPEC.md §5). Served from the view because whether
    // a value is cut short is a question about the COLUMN's current width, which nothing
    // below the view can see; viewportEvent() answers for the cells and eventFilter()
    // for the header, whose own tooltips arrive on ITS viewport and never reach this one.
    bool viewportEvent(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

    // Building a selection with the pointer (SPEC.md §5). A left press picks a record
    // and arms a drag; a move while the button is down extends the range from the anchor
    // that press set — the SAME anchor a Shift+click extends from, so the two gestures
    // can never disagree; the release disarms it, and so does a hide, which is the only
    // other way a view can stop seeing the pointer mid-drag.
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    // Double-clicking a cell asks for what that column is FOR (SPEC.md §5). Reported,
    // not acted on: which column means what is the window's business, exactly as it is
    // for the record menu.
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    // Ctrl+wheel asks for a bigger or smaller font (SPEC.md §5) and is reported rather
    // than acted on (zoomStepRequested); a plain wheel scrolls, as it always did.
    void wheelEvent(QWheelEvent *event) override;
    // The one place a font change is noticed, whoever made it (ARCHITECTURE.md §7.1.5).
    // Every cached quantity derived from the line height or the character advance is
    // invalidated here — the selected record's wrapped height, the estimator's
    // width-keyed measurements, the seeded column widths, the header band and the
    // digest cap — and the reader is put back on the record they were reading.
    void changeEvent(QEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void scrollContentsBy(int dx, int dy) override;

signals:
    // Follow attached/detached, so the window can reflect it (menu check, control).
    void followingChanged(bool following);

    // The column layout moved — a section resized, moved, or a whole state restored
    // (M19). The digest strip mirrors it so its cells sit under the table's own.
    void columnLayoutChanged();

    // The horizontal scroll position moved. Mirrored for the same reason.
    void horizontalOffsetChanged(int value);

    // A record was right-clicked (SPEC.md §5). The view reports WHERE — the view row,
    // the column under the cursor, and where to pop up — and nothing else: the menu
    // is assembled by the window, which is the only place that can reach both the
    // record's fields and the panes the items edit. `column` is the logical column,
    // or -1 past the last one; it ranks the menu without changing its contents.
    void recordMenuRequested(int viewRow, int column, const QPoint &globalPos);

    // A record was double-clicked (SPEC.md §5). Reported exactly as a right-click is —
    // the view row and the logical column under the cursor, nothing else — because what
    // a column's default action IS lives where the record menu's items live, and the
    // gesture must reach that same item rather than a second copy of it.
    void recordDoubleClicked(int viewRow, int column);

    // A cell was Ctrl+Alt+clicked or Alt+clicked (SPEC.md §5) — "show only this value"
    // and "hide this value", the two edits the Filters pane's own lists offer, reached
    // from the record. Reported exactly as the two above are, and for the same reason:
    // the view has the hit test and no access to the panes, so what these MEAN lives
    // where the record menu's items live and the gesture reaches that same item.
    void recordShowOnlyRequested(int viewRow, int column);
    void recordHideRequested(int viewRow, int column);

    // The reader asked to zoom with Ctrl+wheel (SPEC.md §5). Reported, never acted on:
    // the log text size is ONE application-wide setting, so a view that re-fonted itself
    // would be a second path to it and the other open views would not follow. `steps` is
    // wheel notches, positive to enlarge.
    void zoomStepRequested(int steps);

private slots:
    void handleRowsInserted();
    void handleRowsRemoved();
    // A trailing record grew in place (M6 live update). The rows are carried through
    // because the density map has to re-scan exactly them: the row's CONTENT moved
    // under an unchanged row space, and nothing else in the bar ever revisits a row.
    void handleTailChanged(int firstRow, int lastRow);
    void handleModelReset();
    void applyDebouncedResize();
    // One step of a drag that has left the viewport: scroll, then extend to whatever
    // record the pointer is now over. Never a busy loop — a timer, stopped the moment
    // the drag ends (autoScroll*).
    void autoScrollTick();

private:
    // The whole of what a font change costs. Separate from changeEvent() because the
    // ORDER matters (widths, then the header, then the estimator's column count, then
    // geometry) and because the re-anchor at the end is the point of it.
    void applyFontChange();

    int visibleLines() const;

    // Digest role only: how many display lines the strip will actually show, after the
    // cap, and (out) whether the cap bit. A pure query — sizeHint() calls it and must
    // stay pure, since setting a scrollbar policy relayouts and layout asks the hint.
    qint64 digestContentLines(bool *capped) const;

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

    // AlwaysOn support: block measurement from decoded text, and keeping the visible
    // blocks measured.
    //
    // One character's advance, and the pixel floor kMinWrapCols of them come to. The
    // floor alone is what these two are for now — a record's height is a greedy fill
    // over per-codepoint metrics (WrapMetrics) and divides nothing. The advance is
    // still FRACTIONAL and must stay so: every integer advance Qt offers rounds, and a
    // floor a pixel short of kMinWrapCols characters is one column of clipping
    // (ARCHITECTURE.md §7.1.1).
    qreal charAdvance() const;
    int minWrapWidth() const;
    void ensureEstimatorBound();
    void measureBlock(int block);
    void measureVisibleBlocks();

    // The record whose height varies under the current wrap mode, or -1 when
    // nothing wraps (wrap off, or no selection). Drives the geometry statics.
    int selRecordForGeometry() const;

    // --- What a copy walks (ARCHITECTURE.md §7.1.6) --------------------------------
    // The selection as merged, ascending, inclusive VIEW-row ranges. QItemSelectionModel
    // already holds ranges and Select All is exactly ONE of them, so this costs a pair of
    // ints per range where selectedRows(0) cost a QModelIndex per record. Falls back to
    // the focused record when nothing is selected, exactly as the per-row form did.
    QVector<QPair<int, int>> selectedRecordRanges() const;
    static qint64 rangeTotal(const QVector<QPair<int, int>> &ranges);
    // The selected records' byte lengths, straight off the 32-byte index entries — what
    // the one output string is reserved from, so nothing is decoded twice to measure it.
    qint64 selectedByteLength(const QVector<QPair<int, int>> &ranges) const;
    // Call `emitRow` for every selected view row in ascending order. Returns false when
    // the copy was abandoned, which is the caller's signal to leave the clipboard alone.
    bool walkSelection(const QVector<QPair<int, int>> &ranges, qint64 total,
                       const std::function<void(int viewRow)> &emitRow) const;

    // Height, in display lines, of record r as currently rendered (folds in the
    // selected-record wrap when applicable).
    int recordHeightLines(int r) const;
    // Measure how many visual lines the message text occupies at `width` px.
    int measureWrappedLines(const QString &text, int width) const;
    int messageColumn() const; // logical column of the Message field, or -1
    // Whether a column has anything on screen: not hidden, and not resized to nothing.
    bool columnIsOnScreen(int logical) const;
    // The same column when there is something to wrap in it: -1 for a format with no
    // %m AND for a message column the reader has switched off in the header menu, which
    // is the same question as far as every height is concerned. Without it AlwaysOn went
    // on measuring a hidden message and gave every record the rows it wanted, which the
    // paint then left blank.
    int wrappedMessageColumn() const;
    // Whether the message column is the last one ON SCREEN — walked back from the end of
    // the VISUAL order past hidden sections, since the columns are movable and any of
    // them may be switched off. It is what decides how wide a wrapped message is laid
    // out and how far it may be drawn.
    bool messageIsLastVisibleColumn() const;

    // Resolve one row's background and text color for painting (M5). Selection wins;
    // otherwise the highlight rules speak through the model's Background/Foreground
    // roles (SPEC.md §7), each falling back to the theme color — the background to an
    // alternating band per RECORD (UiColors::alternateRowColor) when no rule applies.
    void resolveRowColors(int row, bool selected, QColor &bg, QColor &fg) const;

    // Whether anything is being marked at all — an empty query marks everything, which
    // is not a mark, and a regex that will not compile matched nothing to begin with.
    bool marking() const;

    void setFollowingState(bool following); // update state + control + emit signal
    void updateFollowFromScrollPosition();  // detach/re-attach from the scrollbar
    void positionFollowButton();            // place the return-to-bottom overlay

    void recomputeGeometry();     // recompute selection-dependent wrap + scrollbars
    // Re-measure m_selWrapCache, the exact path's ONE measured height. Split out of
    // recomputeGeometry() because a live append has to re-measure without paying that
    // function's estimated branch (which restarts the resize debounce): the selected
    // record may be the trailing one, and a trailing record grows in place on every
    // ingest tick of a live log. Leave it stale and the paint lays the record's new
    // text into the rows it was given before the growth and drops the rest, with no
    // ellipsis and no tooltip — truncatedCellText() declines for the selected record.
    void measureSelectionWrap();
    void updateScrollBars();
    // The header band and the density strip, which are the two things the viewport's
    // margins are spent on — ONE setViewportMargins call, because two would each undo
    // the other's margin.
    void layoutChrome();
    void ensureRecordVisible(int record);
    // Focus a record and select it WITHOUT scrolling. setCurrentRecord() is the
    // interactive form and deliberately scrolls; a filter restore must not, because
    // the scroll position is restored separately and by a different rule.
    void selectRecordSilently(int record);
    int recordAtViewportY(int y) const; // hit-test, or -1
    // The record under a viewport point, or -1 where there is none — the empty space
    // BELOW the last record included. Deliberately not recordAtViewportY(), which clamps
    // to the nearest record because it backs a click that selects one: a menu, or a
    // tooltip, about a record the cursor is not on speaks for a row the user cannot see
    // themselves pointing at.
    int recordUnderPoint(int y) const;
    void selectRange(int anchor, int current);
    // Add or remove ONE record, leaving the rest of the selection alone — Ctrl+click
    // (Cmd on macOS, which Qt already maps to ControlModifier). Moves the focus and the
    // anchor onto that record, so a Shift+click after it extends from where the pointer
    // last was, exactly as it does after a plain click.
    void toggleRecordSelection(int record);

    // The two filter chords (SPEC.md §5), shared by the press and the double-click that
    // a second chord-click arrives as. Answers true when it TOOK the event: the modifier
    // is matched by EXACT equality, so Ctrl+Alt+Shift is left to the Shift branch below
    // rather than claimed by a chord nobody aimed, and a click below the last record is
    // refused for the reason the menu and the double-click refuse it.
    bool takeFilterChord(QMouseEvent *event);

    // --- Drag-select (SPEC.md §5) --------------------------------------------------
    // extendDragTo() is the whole of what a drag does: clamp the pointer into the
    // viewport, hit-test it, and extend through setCurrentRecord()'s existing Shift
    // path. A drag that leaves the viewport keeps going on a timer, a few lines a tick
    // and faster the further out the pointer is; the timer is armed only while a drag is
    // live, and stopped by the release, by a hide, and by a model reset — a drag whose
    // record space has been replaced is over.
    void extendDragTo(int viewportY);
    void updateAutoScroll(int viewportY);
    void stopAutoScroll();
    void endDrag();

    // The full text of the cell / header section at a viewport position, but ONLY when
    // the column is too narrow to show it — empty otherwise, because a tooltip that
    // repeats what is already on screen is noise and "there is more here" is the whole
    // of what this says. Built on demand, never per paint, and holding nothing
    // (invariant #1).
    QString truncatedCellText(const QPoint &pos) const;
    QString truncatedHeaderText(int x) const;

    // --- Column width measurement (see seedColumnWidths) ---------------------------
    // Everything here measures through the view's own QFontMetrics and survives a font
    // that resolves to nothing at all — the Windows offscreen plugin ships no fonts, so
    // horizontalAdvance() answers 0 there and a width computed from it would collapse
    // every column to its floor.
    int charWidth() const;
    int textWidth(const QString &text) const;
    // A run of `chars` characters of the fixed-pitch font, measured as ONE string. Not
    // `chars * charWidth()`: an int advance is rounded to the pixel, and a column's
    // worth of that rounding is what left the Time column narrower than the timestamp
    // it was seeded for.
    int charsWidth(int chars) const;
    // What the STYLE spends on a section before its caption starts, so a caption is
    // measured against the rect it is actually painted in (see truncatedHeaderText).
    int headerLabelInset(int logical) const;
    int seedWidthOf(int logical) const;    // a typical value + the caption
    // The narrowest a seeded column may be squeezed to: its caption and nothing else.
    int seedFloorOf(int logical) const;
    // Bounds the SUM of the seeded widths by what is on screen, so the columns before the
    // message cannot push it off the right edge when the scan completes (bugs.md 19).
    // Called only from seedColumnWidths(), which keeps the seed a three-callers pass.
    void boundSeedToViewport(QVector<int> &width) const;
    int contentWidthOf(int logical) const; // the widest value, bounded
    // The widest interned logger/thread name, optionally clamped to `maxChars` — the
    // clamp is for the SEED, so one pathological name cannot open a column half a
    // window wide before the user has asked for anything.
    int widestInternedWidth(int logical, int maxChars) const;
    int sampledContentWidth(int logical) const;
    // Resize without the resize being read back as the user's own choice.
    void applyColumnWidth(int logical, int width);
    void markUserSized(int logical);

    const Document *m_document;
    LogModel       *m_model;
    Role            m_role = Role::Main;
    // nullptr until the constructor builds it, and DELIBERATELY initialized:
    // QAbstractScrollArea installs the view itself as an event filter on its own
    // scrollbars, so eventFilter() is called during construction — before this exists.
    QHeaderView    *m_header = nullptr;
    // The view's vertical scrollbar while it is carrying the density marks (Role::Main
    // only, and null while the reader has them switched off — the view then has an
    // ordinary QScrollBar instead). Switching them off genuinely REPLACES the bar rather
    // than telling it to draw nothing, or a scan stays alive for marks nobody can see.
    DensityScrollBar *m_density = nullptr;
    QItemSelectionModel *m_selection;

    // Which columns the user has spoken for — dragged, fitted, or brought back by a
    // restored session — indexed by LOGICAL column. A seed skips every one of them, so
    // the scan-completion seed can never overwrite a width somebody chose.
    QVector<bool> m_userSizedColumns;
    // Set while this class is doing the resizing, so the QHeaderView::sectionResized it
    // provokes is not read back as the user dragging a divider.
    bool m_applyingColumnWidths = false;
    // Whether this view has ever been given a real geometry. The constructor seeds before
    // anything has been laid out, where the viewport is still QWidget's default and a
    // bound taken from it would squash every column on a window with room to spare.
    bool m_viewportLaidOut = false;

    // The query Find is marking, or empty. The QUERY, not its results: see
    // setFindMatcher(). Costs one small object per view and nothing per record.
    TextMatcher m_findMatcher;

    // Above this many selected records a copy shows progress and offers to cancel
    // (SPEC.md §5, ARCHITECTURE.md §7.1.6).
    int m_copyProgressThreshold = kDefaultCopyProgressThreshold;

    WrapMode m_wrapMode = WrapMode::Off;
    QString  m_placeholderText; // drawn centred when there are no records at all
    int      m_current = -1;   // focused record (drives keyboard nav + wrap)
    int      m_anchor = -1;    // range-selection anchor
    int      m_selWrapCache = -1; // memoized selWrapLines() for the current width

    // What beginFilterUpdate() captured, in source ordinals. `active` doubles as the
    // in-bracket guard: while it is set, the scrollbar clamp that endResetModel()
    // provokes is not the user scrolling and must not be read as such.
    struct FilterAnchor {
        bool   active = false;
        bool   following = false;
        int    topSource = -1;     // source ordinal of the record at the viewport top
        qint64 topOffset = 0;      // display lines scrolled INTO that record (>= 0)
        int    currentSource = -1; // source ordinal of the focused record, or -1
        qint64 currentOffset = 0;  // its first line minus the top line (may be < 0)
        bool   currentOnScreen = false;
    };
    FilterAnchor m_filterAnchor;
    // A selection a filter hid: nothing is selected while it is out of view, but
    // widening the filter again brings it back (SPEC.md §6). Any model reset that is
    // not a filter re-apply forgets it — the record space itself changed.
    int m_stickySource = -1;

    // Pointer-built selection (SPEC.md §5). Set by a left press that landed on a record,
    // cleared by the release, by a hide, and by any model reset; while it is set every
    // mouse move extends the range from m_anchor. m_autoScrollY is where the pointer was
    // last seen, in viewport pixels and deliberately unclamped — its distance OUTSIDE
    // the viewport is what sets the autoscroll speed.
    // Ctrl+wheel notches not yet worth a zoom step (see wheelEvent): a trackpad sends
    // many small deltas and a mouse one big one, and the remainder is what makes the two
    // add up to the same gesture.
    int     m_wheelZoomRemainder = 0;

    bool    m_dragging = false;
    QTimer *m_autoScrollTimer = nullptr;
    int     m_autoScrollY = 0;

    // Estimated geometry for AlwaysOn (M2c). Only ever constructed/consulted in
    // AlwaysOn; switching to an exact mode leaves it untouched so its cache
    // survives a round-trip through the exact modes.
    EstimatedGeometry m_estimated;
    // The font-keyed per-codepoint metrics m_estimated's heights are measured through.
    // Rebound (and dropped) on QEvent::FontChange and nowhere else.
    WrapMetrics m_wrapMetrics;
    QTimer  *m_resizeTimer = nullptr; // debounces width-change remeasurement

    // Follow state (M6). Every open follows the tail; scrolling away detaches.
    bool         m_following = true;
    bool         m_inFollowScroll = false; // guards the programmatic scroll-to-end
    QToolButton *m_followButton = nullptr; // return-to-bottom overlay, shown detached
};

} // namespace loftail
