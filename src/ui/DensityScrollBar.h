#pragma once

#include "DensityMap.h"

#include <QList>
#include <QScrollBar>

QT_BEGIN_NAMESPACE
class QTimer;
QT_END_NAMESPACE

namespace loftail {

class Document;
class LogModel;
class LogView;

// The log view's vertical scrollbar, with the density marks drawn INSIDE it
// (SPEC.md §5, ARCHITECTURE.md §7.1.7): where in the WHOLE view the highlighted records
// and the current Find matches sit, in the colour the record itself wears.
//
// It IS the scrollbar rather than a strip beside one, and that is the whole of why the
// marks agree with the thumb. A separate widget cannot line up with a scrollbar: its
// marks are a fraction of its own height while the thumb is a fraction of the GROOVE,
// which a style shortens by an arrow button at each end and which spans the view's top
// viewport margin (the header) that the strip sat below. Both errors are silent, both
// move with the style and the font, and there is no arithmetic that removes them from
// outside the bar — so the bar is where this lives. It is Kate's minimap scrollbar, for
// the same reason Kate has one.
//
// The mapping is then exact by construction rather than by agreement. LogView's scroll
// range is in LINE units and spans 0..total-page with a page-sized thumb, so a record
// whose first line is L has thumb-top fraction L/total — which is precisely
// LogView::scrollFractionOfRow(), the one mapping, shared with the mark placement and
// with the click that jumps there. That fraction is taken of the TRACK (trackRect), not
// of the widget: the widget spans the frame contents, which includes the header row the
// viewport begins below, so a fraction of the widget's height puts every mark and the
// thumb with it a header's height above the row it is pointing at — the same class of
// error being the bar removed two of, arriving from the one direction being the bar
// does not answer by itself.
//
// A navigation aid, not a chart (SPEC.md §11 rules out charts and statistics, and this
// counts nothing and reports no quantity): it says "there is an ERROR two thirds of the
// way down" and lets the reader click on it.
//
// The scan behind it is the interesting part and it lives in DensityMap: bounded per
// slice, resumable, and driven from here by a timer that only runs while the bar is on
// screen — a background tab must not scan, or ten open logs are ten scans.
class DensityScrollBar : public QScrollBar
{
    Q_OBJECT

public:
    // `view` is the log table this bar scrolls. Never a digest strip: that view shows a
    // handful of rows and has no vertical scrollbar at all.
    DensityScrollBar(LogView *view, const Document *document, LogModel *model);
    ~DensityScrollBar() override;

    // Wider than a plain scrollbar, because it carries several lanes of marks — measured
    // in the log font's own characters, the unit everything else in the view is measured
    // in (ARCHITECTURE.md §7.1), so it tracks a zoom instead of staying a pixel constant
    // that is fat at 7 pt and a hairline at 30. Never NARROWER than the style's own
    // scrollbar extent: the thumb is still a thumb and has to be draggable.
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    // The row space was REPLACED — a filter re-apply, a rotation rescan, a run switch.
    // Everything is dropped: a view row now names a different record.
    void rebind();

    // The same row space, resized. Cheap by construction (see DensityMap::setRows), so
    // this is safe on the ingest path.
    void rowsChanged();

    // The highlight rules moved: re-scan the rule lane, keep the find lane.
    void invalidateRules();

    // The Find query moved: re-scan the find lane, keep the rule lane. Kept separate
    // from the rules lane precisely because this fires on every keystroke in the bar.
    void invalidateFind();

    // The line mapping moved (records appended, a wrap-mode change, a font change) so
    // the marks belong at different heights. Repaint only — nothing is rescanned.
    void refresh();

    // Test seam: how much of each lane has been scanned, and whether it finished.
    const DensityMap &map() const { return m_map; }
    // Run the scan to completion now, rather than over the next few frames. For tests
    // and for nothing else: it is exactly the unbounded pass the slicing exists to
    // avoid.
    void scanNowForTests();

    // The band the marks and the thumb are placed in: the rows of the bar that stand
    // level with the VIEWPORT, which begins below the header row and ends at the bar's
    // own bottom. Public for the same reason thumbRect() is — the claim is about where
    // something is drawn, and there is nowhere else to check it.
    QRect trackRect() const;
    // Where the thumb is drawn, in this widget's coordinates. Public because the one
    // claim the design turns on — a mark sits where the thumb that shows that record
    // goes — is only checkable against it.
    QRect thumbRect() const;
    // Where the marks of one highlight rule are drawn, and where a find match is. Null
    // when that rule has nothing in this log (a colour with no records gets no width) or
    // when Find is not armed. Public because the columns are the answer to "a lone mark
    // must not be covered by a commoner one", and a rendered pixel is where that shows.
    QRect ruleColumnRect(int ruleIndex) const;
    QRect findColumnRect() const;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    // The scan runs only while the bar can be seen. A tab in the background is not
    // being read, and its scan would be competing with the one that is.
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    // One column of marks: which classes it draws, and where it sits across the bar.
    //
    // A column per colour is what keeps a single ERROR line among ten thousand WARNs
    // visible. With one shared column the two fight for the same pixels twice over —
    // once in the map, where a bucket of a big log covers thousands of rows, and once in
    // the paint, where a mark is never thinner than kMinMarkPx and so spills over its
    // neighbours — and the loser disappears from the bar entirely. Given a column each
    // neither can cover the other, and the horizontal position says which rule it was.
    struct MarkColumn {
        DensityMap::Marks classes = DensityMap::kNone;
        int x = 0;
        int width = 0;
        // Which lane this column draws. A flag rather than "the last column is the find
        // one": the find column is not appended at all when there is no room for it, and
        // a positional rule would then read the quietest rules' column as the find lane.
        bool find = false;
    };

    void syncRows();
    void startScanning();
    void scanSlice();
    bool findArmed() const;
    // The columns across the bar: one per highlight rule that has anything in this log,
    // plus the find lane's when Find is armed, laid out with equal margins either side so
    // the marks sit CENTRED in the bar rather than packed against one edge. Where there
    // is not room for a column per rule the tail of them share the last column, resolved
    // per pixel by severity.
    QList<MarkColumn> layoutColumns() const;
    // Paint one column's marks, resolving per PIXEL rather than per bucket: several
    // buckets land on one pixel row on any large log, and each one's kMinMarkPx floor
    // spills onto its neighbours, so painting them in bucket order lets whichever comes
    // last win. The lowest class wins instead, which is the loudest rule.
    void paintColumn(QPainter &painter, const MarkColumn &column, DensityMap::Lane lane,
                     const QColor *fixed) const;
    // The colour a rule's marks are drawn in: its background where it sets one, its
    // foreground where it sets only that, and the theme's text where it sets neither —
    // a rule that colours nothing can still be the reason a record matters.
    QColor markColour(int ruleIndex) const;
    // Where bucket `bucket` starts and ends down the bar, in widget pixels. `track` is
    // passed in rather than asked for: it costs a walk up the parent chain and this runs
    // once per bucket per column, which is thousands of times per repaint.
    void bandOf(int bucket, const QRect &track, int &top, int &bottom) const;
    // The scroll range as the thumb geometry sees it: total lines, which is
    // (maximum - minimum) + pageStep, never less than 1.
    qint64 spanLines() const;
    // Scroll so that the thumb's TOP lands at `topPx`. Goes through setSliderPosition,
    // which is what makes this the user scrolling — follow detaches here exactly as it
    // does on any other scrollbar drag.
    void scrollToThumbTop(int topPx);

    LogView        *m_view = nullptr;
    const Document *m_document = nullptr;
    LogModel       *m_model = nullptr;
    DensityMap m_map;
    QTimer   *m_scanTimer = nullptr;
    // Set between a press and its release. QScrollBar's own press handling is bypassed
    // for the left button: on a minimap a click means "go there", not "page down".
    bool m_dragging = false;
    int  m_dragOffset = 0;
};

} // namespace loftail
