#pragma once

#include "DensityMap.h"

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
// with the click that jumps there.
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

    // Wider than a plain scrollbar, because it carries two lanes of marks — measured in
    // the log font's own characters, the unit everything else in the view is measured
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
    // Where the thumb is drawn, in this widget's coordinates. Public because the one
    // claim the design turns on — a mark sits where the thumb that shows that record
    // goes — is only checkable against it.
    QRect thumbRect() const;

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
    void syncRows();
    void startScanning();
    void scanSlice();
    // The colour a rule's marks are drawn in: its background where it sets one, its
    // foreground where it sets only that, and the theme's text where it sets neither —
    // a rule that colours nothing can still be the reason a record matters.
    QColor markColour(int ruleIndex) const;
    // Where bucket `bucket` starts and ends down the bar, in widget pixels.
    void bandOf(int bucket, int &top, int &bottom) const;
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
