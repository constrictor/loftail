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

#include "DensityScrollBar.h"

#include "Document.h"
#include "LogModel.h"
#include "LogView.h"
#include "Palette.h"
#include "UiColors.h"

#include <QElapsedTimer>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QTimer>

namespace loftail {

namespace {

// How long one slice of the scan may take, and how often a slice runs. A quarter of a
// frame, four times a second's worth of frames apart: the bar fills in visibly while
// nothing else in the window is ever kept waiting for it. Wall clock rather than a row
// count because the two lanes cost wildly different amounts per row — the rule lane
// compares integers (invariant #4) and the find lane decodes every column of every
// record — and a row budget that suits one is either a stall or a crawl for the other.
constexpr int kSliceMs = 4;
constexpr int kSliceIntervalMs = 20;

// How many rows a slice checks its clock after. Reading the clock per row would be a
// measurable fraction of the cheap lane's per-row cost.
constexpr int kClockEvery = 2048;

// The thinnest a mark may be drawn. A bucket of a ten-million-record log is a fraction
// of a pixel tall, and the one FATAL in it is exactly what the bar exists to show, so
// a mark is never smaller than this however little of the file it stands for.
constexpr int kMinMarkPx = 2;

// The margin either side of the marks, and the hairline dividing the bar from the table.
// The margin is equal on both sides and it is measured from the DIVIDER on the left, not
// from the widget edge — the divider is the seam with the log, not part of the bar's own
// ground, so counting it as margin leaves the marks a pixel left of centre on a bar this
// narrow. Between the two margins the columns are given every remaining pixel,
// remainders included, so a rounding cannot pull them apart either.
constexpr int kDividerPx = 1;
constexpr int kEdgePx = 2;

// The narrowest a column of marks may be, and the gap between the rule columns and the
// find one. Below two pixels a mark reads as a rendering artefact rather than a mark.
constexpr int kMinColumnPx = 2;
constexpr int kLaneGapPx = 1;

// The shortest the thumb may be drawn. A log of a million lines gives the viewport a
// fraction of a pixel of the range, and a thumb nobody can grab is not a scrollbar.
// This is the one place the thumb stops being an exact image of the visible band, and
// it is why the mark comparison is stated as "the mark is inside the thumb" rather than
// as an equality.
constexpr int kMinThumbPx = 18;

// How much of the marks under it the thumb lets through. It is drawn translucent
// deliberately: on a log where every record matches the query, an opaque thumb would
// hide exactly the marks the reader is scrolling towards.
constexpr int kThumbAlpha = 52;
constexpr int kThumbAlphaActive = 92;
constexpr int kThumbEdgeAlpha = 130;

// How far the thumb is held off each edge of the bar's own ground — the divider is not
// part of it, so the left inset is measured past that hairline exactly as the marks'
// margin is. The same either side: the thumb is the largest thing drawn here, so a pixel
// of difference between its two margins is what the whole bar reads as leaning by.
constexpr qreal kThumbInsetPx = 1.5;

// How far a mark's colour must stand off the bar's own ground before it is drawn as
// itself. A rule may legitimately set Paper on a light theme or Ink on a dark one, and
// that colour lands on a table row against text that gives it away — but a two-pixel
// mark on a bar of the same colour is simply not there. Deliberately far below the 3:1
// WCAG bound for a non-text indicator: the soft palette bands are *meant* to be quiet,
// and substituting the rule's other colour for every one of them would draw the log in
// colours it is not wearing. This catches invisible, not quiet.
constexpr qreal kMinMarkContrast = 1.3;

} // namespace

DensityScrollBar::DensityScrollBar(LogView *view, const Document *document, LogModel *model)
    : QScrollBar(Qt::Vertical, view), m_view(view), m_document(document), m_model(model)
{
    setObjectName(QStringLiteral("densityScrollBar")); // test contract, never the visible text
    setToolTip(tr("Where the highlighted records and find matches are. Click to jump."));

    m_scanTimer = new QTimer(this);
    m_scanTimer->setInterval(kSliceIntervalMs);
    connect(m_scanTimer, &QTimer::timeout, this, &DensityScrollBar::scanSlice);

    // A value change needs no wire of its own: QAbstractSlider::sliderChange already
    // repaints, and this class draws the bar whole.
}

DensityScrollBar::~DensityScrollBar() = default;

QSize DensityScrollBar::sizeHint() const
{
    QSize s = QScrollBar::sizeHint();
    // Measured in characters of the view's own font, and floored: a platform with an
    // empty font database answers 0 to every advance (the Windows offscreen plugin ships
    // no fonts), and an unfloored width would collapse the bar to nothing.
    // Two characters plus the divider and the two margins: a column per highlight rule
    // that fires, and the find lane beside them, all of which have to be at least two
    // pixels each or they read as artefacts rather than marks.
    const int ch = qMax(4, fontMetrics().horizontalAdvance(QLatin1Char('0')));
    const int chrome = kDividerPx + 2 * kEdgePx;
    const int extent = style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, this);
    s.setWidth(qMax(extent, qBound(14, ch * 2 + chrome, 28)));
    return s;
}

QSize DensityScrollBar::minimumSizeHint() const
{
    QSize s = QScrollBar::minimumSizeHint();
    s.setWidth(sizeHint().width());
    return s;
}

void DensityScrollBar::rebind()
{
    m_map.rebind(m_model ? m_model->rowCount() : 0);
    startScanning();
}

void DensityScrollBar::rowsChanged()
{
    syncRows();
    startScanning();
}

void DensityScrollBar::invalidateRules()
{
    syncRows();
    m_map.clear(DensityMap::Lane::Rules);
    startScanning();
}

void DensityScrollBar::invalidateFind()
{
    syncRows();
    m_map.clear(DensityMap::Lane::Find);
    startScanning();
}

void DensityScrollBar::refresh()
{
    update();
}

void DensityScrollBar::syncRows()
{
    m_map.setRows(m_model ? m_model->rowCount() : 0);
}

void DensityScrollBar::startScanning()
{
    update();
    // Only while the bar is on screen. isVisible() is false for a widget in a tab that
    // is not current, which is exactly the case this is guarding: ten open logs must not
    // be ten scans.
    if (!isVisible())
        return;
    if (!m_scanTimer->isActive())
        m_scanTimer->start();
}

void DensityScrollBar::showEvent(QShowEvent *event)
{
    QScrollBar::showEvent(event);
    // A tab raised after its scan was suspended picks up where it left off — nothing
    // was thrown away, so returning to a log does not restart its scan.
    syncRows();
    startScanning();
}

void DensityScrollBar::hideEvent(QHideEvent *event)
{
    QScrollBar::hideEvent(event);
    m_scanTimer->stop();
}

bool DensityScrollBar::findArmed() const
{
    if (!m_view)
        return false;
    const TextMatcher &matcher = m_view->findMatcher();
    return !matcher.isEmpty() && matcher.isValid();
}

void DensityScrollBar::scanSlice()
{
    if (!m_model || !m_view) {
        m_scanTimer->stop();
        return;
    }
    syncRows();

    const bool rulesDone = m_map.complete(DensityMap::Lane::Rules);
    const bool armed = findArmed();
    const bool findDone = !armed || m_map.complete(DensityMap::Lane::Find);
    if (rulesDone && findDone) {
        m_scanTimer->stop();
        return;
    }

    QElapsedTimer clock;
    clock.start();
    bool moved = false;

    // The rule lane first: it is the cheap one and the one that is always there, so a
    // bar on a log with a Find running still shows its errors within a frame or two.
    const auto slice = [&](DensityMap::Lane lane,
                           const std::function<DensityMap::Marks(int)> &probe) {
        while (!m_map.complete(lane) && clock.elapsed() < kSliceMs) {
            if (m_map.scan(lane, kClockEvery, probe) == 0)
                break;
            moved = true;
        }
    };

    if (!rulesDone) {
        slice(DensityMap::Lane::Rules, [this](int row) -> DensityMap::Marks {
            const int rule = m_model->matchedRule(row);
            return rule < 0 ? DensityMap::kNone : DensityMap::classBit(rule);
        });
    }
    if (armed && !m_map.complete(DensityMap::Lane::Find) && clock.elapsed() < kSliceMs) {
        const TextMatcher matcher = m_view->findMatcher();
        slice(DensityMap::Lane::Find, [this, matcher](int row) -> DensityMap::Marks {
            return m_model->rowMatchesText(row, matcher) ? DensityMap::classBit(0)
                                                         : DensityMap::kNone;
        });
    }

    if (moved)
        update();
}

void DensityScrollBar::scanNowForTests()
{
    if (!m_model || !m_view)
        return;
    syncRows();
    m_map.scan(DensityMap::Lane::Rules, m_map.rows(), [this](int row) -> DensityMap::Marks {
        const int rule = m_model->matchedRule(row);
        return rule < 0 ? DensityMap::kNone : DensityMap::classBit(rule);
    });
    const TextMatcher matcher = m_view->findMatcher();
    if (!matcher.isEmpty() && matcher.isValid()) {
        m_map.scan(DensityMap::Lane::Find, m_map.rows(), [this, matcher](int row) -> DensityMap::Marks {
            return m_model->rowMatchesText(row, matcher) ? DensityMap::classBit(0)
                                                         : DensityMap::kNone;
        });
    }
    update();
}

QColor DensityScrollBar::markColour(int ruleIndex) const
{
    const QColor ground = palette().base().color();
    const QColor fallback = palette().text().color();
    if (!m_document || ruleIndex < 0 || ruleIndex >= m_document->highlighters().rules.size())
        return fallback;
    const HighlightRule &rule = m_document->highlighters().rules.at(ruleIndex);
    const bool dark = m_model && m_model->darkTheme();
    // The record's own background is what the reader is scanning the log for, so it is
    // what the bar shows. A rule that sets only a foreground still marks its records;
    // one that sets neither is a rule with some other action, and the theme's text
    // colour says "something is here" without claiming a colour the record does not have.
    // A colour that cannot be told from the bar's own ground is passed over for the same
    // reason: a mark nobody can see is worse than one drawn in the rule's other colour.
    const QColor bg = HighlightPalette::color(rule.background, dark);
    if (bg.isValid() && contrastRatio(bg, ground) >= kMinMarkContrast)
        return bg;
    const QColor fg = HighlightPalette::color(rule.foreground, dark);
    if (fg.isValid() && contrastRatio(fg, ground) >= kMinMarkContrast)
        return fg;
    return fallback;
}

qint64 DensityScrollBar::spanLines() const
{
    // The quantity BOTH the thumb and the marks are fractions of. LogView::updateScrollBars
    // sets the range to 0..total-page with pageStep = page, so this is the view's total
    // line count — the same denominator LogView::scrollFractionOfRow divides by, which is
    // what makes a mark and the thumb that shows its record land in the same place.
    return qMax<qint64>(1, qint64(maximum()) - qint64(minimum()) + qint64(qMax(0, pageStep())));
}

QRect DensityScrollBar::trackRect() const
{
    const int h = height();
    if (h <= 0)
        return {};
    // The bar is laid out over the scroll area's whole frame contents, and the viewport
    // is not: LogView spends a top viewport margin on its header (§7.1), so the bar
    // stands a header's height taller than the rows it is describing. Placing a mark at
    // a fraction of the WIDGET therefore puts it that far above the record it points at
    // — 18 px at the reference face, at the top of the log, tapering to nothing at the
    // bottom, which reads exactly like a bar that is slightly out of step. The marks and
    // the thumb are placed in the viewport's own band instead, so a mark at the top of
    // the log is level with the first row of text.
    if (m_view && m_view->viewport() && m_view->viewport()->height() > 0) {
        const QWidget *vp = m_view->viewport();
        const int top = vp->mapTo(m_view, QPoint(0, 0)).y() - mapTo(m_view, QPoint(0, 0)).y();
        if (top >= 0 && top + vp->height() <= h)
            return {0, top, width(), vp->height()};
    }
    return {0, 0, width(), h};
}

QRect DensityScrollBar::thumbRect() const
{
    const QRect track = trackRect();
    const int h = track.height();
    if (h <= 0)
        return {};
    const qint64 total = spanLines();
    int th = int(qreal(qMax(0, pageStep())) / qreal(total) * h);
    th = qBound(qMin(kMinThumbPx, h), th, h);
    const qint64 v = qint64(sliderPosition()) - qint64(minimum());
    int top = int(qreal(qMax<qint64>(0, v)) / qreal(total) * h);
    top = qBound(0, top, h - th);
    return {0, track.top() + top, width(), th};
}

void DensityScrollBar::scrollToThumbTop(int topPx)
{
    const QRect track = trackRect();
    const int h = qMax(1, track.height());
    const qint64 total = spanLines();
    const qint64 v = qint64(qreal(qBound(0, topPx - track.top(), h)) / qreal(h) * qreal(total));
    setSliderPosition(int(qBound<qint64>(qint64(minimum()), qint64(minimum()) + v,
                                         qint64(maximum()))));
}

void DensityScrollBar::bandOf(int bucket, const QRect &track, int &top, int &bottom) const
{
    const int h = track.height();
    const int firstRow = m_map.firstRowOf(bucket);
    const int nextRow = qMin(m_map.rows(), firstRow + m_map.rowsPerBucket());
    const qreal a = m_view->scrollFractionOfRow(firstRow);
    // The END of the bucket is the start of the row after it, so two adjacent marked
    // buckets meet rather than leaving a hairline of unmarked bar between them.
    const qreal b = nextRow >= m_map.rows() ? 1.0 : m_view->scrollFractionOfRow(nextRow);
    top = qBound(0, int(a * h), qMax(0, h - 1));
    bottom = qBound(top + 1, int(b * h), h);
    if (bottom - top < kMinMarkPx)
        bottom = qMin(h, top + kMinMarkPx);
    if (bottom - top < kMinMarkPx)
        top = qMax(0, bottom - kMinMarkPx);
    top += track.top();
    bottom += track.top();
}

QList<DensityScrollBar::MarkColumn> DensityScrollBar::layoutColumns() const
{
    QList<MarkColumn> columns;
    const int left = kDividerPx + kEdgePx;
    const int inner = width() - left - kEdgePx;
    if (inner < kMinColumnPx || m_map.bucketCount() <= 0)
        return columns;

    const DensityMap::Marks rulesMask = m_map.unionMask(DensityMap::Lane::Rules);
    const bool wantFind = findArmed();

    // The find lane is allocated on the QUERY being armed and not on its having found
    // anything, so the columns do not shuffle sideways under the reader as a scan turns
    // up its first match.
    int findWidth = 0;
    int gap = 0;
    if (wantFind) {
        findWidth = rulesMask == DensityMap::kNone ? inner : qBound(2, inner / 3, 5);
        gap = rulesMask == DensityMap::kNone ? 0 : kLaneGapPx;
    }
    int rulesWidth = inner - findWidth - gap;
    if (rulesMask == DensityMap::kNone)
        rulesWidth = 0;
    else if (!wantFind)
        rulesWidth = inner;

    int x = left;
    if (rulesWidth >= kMinColumnPx) {
        QList<int> classes;
        for (int cls = 0; cls < DensityMap::kClassCount; ++cls)
            if (rulesMask & DensityMap::classBit(cls))
                classes.append(cls);
        // One column each while they fit. Where they do not, the tail of them share the
        // last column — the loudest rules keep a column of their own, since a rule index
        // is severity order by convention (§7.5.4).
        const int fit = qMax(1, rulesWidth / kMinColumnPx);
        const int count = qMin(int(classes.size()), fit);
        const int each = rulesWidth / count;
        const int spare = rulesWidth % count;
        for (int i = 0; i < count; ++i) {
            MarkColumn column;
            column.x = x;
            column.width = each + (i < spare ? 1 : 0); // the extra pixels to the loudest
            if (i < count - 1) {
                column.classes = DensityMap::classBit(classes.at(i));
            } else {
                for (int j = i; j < classes.size(); ++j)
                    column.classes |= DensityMap::classBit(classes.at(j));
            }
            columns.append(column);
            x += column.width;
        }
        x += gap;
    } else if (findWidth > 0) {
        findWidth = inner; // nothing in the rule lane: the find marks take the width
    }

    if (findWidth >= kMinColumnPx) {
        MarkColumn column;
        column.classes = DensityMap::classBit(0);
        column.find = true;
        column.x = x;
        column.width = qMin(findWidth, width() - kEdgePx - x);
        if (column.width >= kMinColumnPx)
            columns.append(column);
    }
    return columns;
}

QRect DensityScrollBar::ruleColumnRect(int ruleIndex) const
{
    if (ruleIndex < 0)
        return {};
    const DensityMap::Marks bit = DensityMap::classBit(ruleIndex);
    if (!(m_map.unionMask(DensityMap::Lane::Rules) & bit))
        return {};
    const QRect track = trackRect();
    for (const MarkColumn &column : layoutColumns())
        if (!column.find && (column.classes & bit))
            return {column.x, track.top(), column.width, track.height()};
    return {};
}

QRect DensityScrollBar::findColumnRect() const
{
    if (!findArmed())
        return {};
    const QRect track = trackRect();
    for (const MarkColumn &column : layoutColumns())
        if (column.find)
            return {column.x, track.top(), column.width, track.height()};
    return {};
}

void DensityScrollBar::paintColumn(QPainter &painter, const MarkColumn &column,
                                   DensityMap::Lane lane, const QColor *fixed) const
{
    const QRect track = trackRect();
    if (track.height() <= 0 || column.width <= 0)
        return;

    // Resolved per PIXEL, not per bucket. On any large log several buckets land on one
    // pixel row, and each one's kMinMarkPx floor spills onto its neighbours' rows, so
    // filling them in bucket order lets whichever happens to come last win — which is
    // how a lone FATAL disappeared under the WARN two records below it. The lowest class
    // wins the pixel instead, which is the loudest rule, and the answer no longer depends
    // on the order the buckets were drawn in.
    QList<qint16> pixel(track.height(), -1);
    const int buckets = m_map.bucketCount();
    for (int b = 0; b < buckets; ++b) {
        const DensityMap::Marks marks = m_map.at(lane, b) & column.classes;
        if (marks == DensityMap::kNone)
            continue;
        const int cls = DensityMap::lowestClass(marks);
        int top = 0;
        int bottom = 0;
        bandOf(b, track, top, bottom);
        for (int y = qMax(top, track.top()); y < qMin(bottom, track.bottom() + 1); ++y) {
            qint16 &slot = pixel[y - track.top()];
            if (slot < 0 || cls < slot)
                slot = qint16(cls);
        }
    }

    // One fill per RUN of like pixels rather than one per pixel row.
    int runStart = -1;
    qint16 runClass = -1;
    for (int i = 0; i <= pixel.size(); ++i) {
        const qint16 here = i < pixel.size() ? pixel.at(i) : qint16(-1);
        if (here == runClass)
            continue;
        if (runClass >= 0 && runStart >= 0) {
            painter.fillRect(QRect(column.x, track.top() + runStart, column.width, i - runStart),
                             fixed ? *fixed : markColour(runClass));
        }
        runClass = here;
        runStart = here >= 0 ? i : -1;
    }
}

void DensityScrollBar::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter p(this);
    const int w = width();
    const int h = height();
    if (w <= 0 || h <= 0)
        return;

    // The bar's own ground is the table's, so it reads as part of the view rather than
    // as chrome, with one hairline separating it from the text — the same divider a
    // pane's section heading is drawn with, for the same reason (UiColors::dividerColor).
    // Nothing of the style's own groove is drawn: an arrow button at each end would take
    // pixels off the range the marks are placed in, which is the whole error this class
    // exists to remove.
    p.fillRect(rect(), palette().base().color());
    p.setPen(dividerColor(palette()));
    p.drawLine(0, 0, 0, h - 1);

    if (m_view && m_map.bucketCount() > 0) {
        // Find marks wear the theme's own selection colour — what "the thing you asked
        // for" already looks like everywhere else in the window, and the one colour
        // guaranteed to be distinct from every palette slot on both themes.
        const QColor findColour = palette().highlight().color();
        for (const MarkColumn &column : layoutColumns()) {
            paintColumn(p, column,
                        column.find ? DensityMap::Lane::Find : DensityMap::Lane::Rules,
                        column.find ? &findColour : nullptr);
        }
    }

    // The thumb LAST and translucent, over the marks rather than instead of them.
    const QRect thumb = thumbRect();
    if (thumb.height() <= 0 || thumb.height() >= trackRect().height())
        return; // nothing to scroll: a thumb spanning the whole track says nothing
    QColor fill = palette().text().color();
    fill.setAlpha(m_dragging || underMouse() ? kThumbAlphaActive : kThumbAlpha);
    QColor edge = palette().text().color();
    edge.setAlpha(kThumbEdgeAlpha);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setBrush(fill);
    p.setPen(edge);
    // Inset EQUALLY either side. It used to be 1.5 px on the left against 0.5 on the
    // right — a pixel of asymmetry on a fourteen-pixel bar, which is the widest thing
    // drawn here reading as if it were leaning against the log.
    p.drawRoundedRect(QRectF(thumb).adjusted(kDividerPx + kThumbInsetPx, 0.5,
                                             -kThumbInsetPx, -0.5),
                      2.0, 2.0);
}

void DensityScrollBar::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QScrollBar::mousePressEvent(event);
        return;
    }
    // A click on a minimap means "go there", never "page down" — so QScrollBar's own
    // press handling is bypassed entirely for the left button rather than being tuned
    // through SH_ScrollBar_LeftClickAbsolutePosition, which is a style's answer and not
    // this widget's to give.
    const int y = int(event->position().y());
    const QRect thumb = thumbRect();
    if (y >= thumb.top() && y <= thumb.bottom()) {
        m_dragOffset = y - thumb.top(); // grabbed where it was grabbed
    } else {
        // CENTRED, not put at the top: the click names a place to READ, and a mark
        // landed exactly on the first visible line is one the reader has to scroll
        // back for.
        m_dragOffset = thumb.height() / 2;
        scrollToThumbTop(y - m_dragOffset);
    }
    m_dragging = true;
    setSliderDown(true);
    update();
    event->accept();
}

void DensityScrollBar::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging) {
        QScrollBar::mouseMoveEvent(event);
        return;
    }
    scrollToThumbTop(int(event->position().y()) - m_dragOffset);
    event->accept();
}

void DensityScrollBar::enterEvent(QEnterEvent *event)
{
    // The thumb darkens under the pointer, so the widget has to hear it arrive and
    // leave: a scrollbar is not otherwise repainted for either.
    QScrollBar::enterEvent(event);
    update();
}

void DensityScrollBar::leaveEvent(QEvent *event)
{
    QScrollBar::leaveEvent(event);
    update();
}

void DensityScrollBar::mouseReleaseEvent(QMouseEvent *event)
{
    if (!m_dragging) {
        QScrollBar::mouseReleaseEvent(event);
        return;
    }
    m_dragging = false;
    setSliderDown(false);
    update();
    event->accept();
}

} // namespace loftail
