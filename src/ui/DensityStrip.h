#pragma once

#include "DensityMap.h"

#include <QWidget>

QT_BEGIN_NAMESPACE
class QTimer;
QT_END_NAMESPACE

namespace loftail {

class Document;
class LogModel;
class LogView;

// The density strip beside the vertical scrollbar (SPEC.md §5, ARCHITECTURE.md §7.1.7):
// where in the WHOLE view the highlighted records and the current Find matches sit.
//
// A navigation aid, not a chart (SPEC.md §11 rules out charts and statistics, and this
// counts nothing and reports no quantity): it says "there is an ERROR two thirds of the
// way down" and lets the reader click on it. It reuses the colours the highlighter
// already resolves, so a mark is the colour the record itself wears — nothing new to
// choose and nothing to keep in step with the rules.
//
// Positions are in LINE units, not record units, because the thing it sits beside is
// in line units: a strip that placed its marks by record index would disagree with the
// scrollbar thumb the moment a log carried multi-line records, which is most logs and
// every one with a stack trace in it. LogView::scrollFractionOfRow is the one mapping,
// shared with the click that jumps there.
//
// The scan behind it is the interesting part and it lives in DensityMap: bounded per
// slice, resumable, and driven from here by a timer that only runs while the strip is
// on screen — a background tab must not scan, or ten open logs are ten scans.
class DensityStrip : public QWidget
{
    Q_OBJECT

public:
    // `view` is the log table this strip annotates, and its parent. Never a digest
    // strip: that view shows a handful of rows with no scrollbar to sit beside.
    DensityStrip(LogView *view, const Document *document, LogModel *model);
    ~DensityStrip() override;

    // How wide the strip asks to be, in the log font's own characters — the unit
    // everything else in the view is measured in (ARCHITECTURE.md §7.1), so it tracks a
    // zoom instead of staying a pixel constant that is a fat bar at 7 pt and a hairline
    // at 30.
    int stripWidth() const;

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

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    // The scan runs only while the strip can be seen. A tab in the background is not
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
    // Where bucket `bucket` starts and ends down the strip, in widget pixels.
    void bandOf(int bucket, int &top, int &bottom) const;

    LogView        *m_view = nullptr;
    const Document *m_document = nullptr;
    LogModel       *m_model = nullptr;
    DensityMap m_map;
    QTimer   *m_scanTimer = nullptr;
};

} // namespace loftail
