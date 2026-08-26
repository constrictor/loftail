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
    const int ch = qMax(4, fontMetrics().horizontalAdvance(QLatin1Char('0')));
    const int extent = style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, this);
    s.setWidth(qMax(extent, qBound(12, ch * 2, 28)));
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

void DensityScrollBar::scanSlice()
{
    if (!m_model || !m_view) {
        m_scanTimer->stop();
        return;
    }
    syncRows();

    const bool rulesDone = m_map.complete(DensityMap::Lane::Rules);
    const bool findArmed = !m_view->findMatcher().isEmpty()
                           && m_view->findMatcher().isValid();
    const bool findDone = !findArmed || m_map.complete(DensityMap::Lane::Find);
    if (rulesDone && findDone) {
        m_scanTimer->stop();
        return;
    }

    QElapsedTimer clock;
    clock.start();
    bool moved = false;

    // The rule lane first: it is the cheap one and the one that is always there, so a
    // bar on a log with a Find running still shows its errors within a frame or two.
    const auto slice = [&](DensityMap::Lane lane, const std::function<qint16(int)> &probe) {
        while (!m_map.complete(lane) && clock.elapsed() < kSliceMs) {
            if (m_map.scan(lane, kClockEvery, probe) == 0)
                break;
            moved = true;
        }
    };

    if (!rulesDone) {
        slice(DensityMap::Lane::Rules, [this](int row) -> qint16 {
            const int rule = m_model->matchedRule(row);
            return rule < 0 ? DensityMap::kNothing : qint16(rule);
        });
    }
    if (findArmed && !m_map.complete(DensityMap::Lane::Find) && clock.elapsed() < kSliceMs) {
        const TextMatcher matcher = m_view->findMatcher();
        slice(DensityMap::Lane::Find, [this, matcher](int row) -> qint16 {
            return m_model->rowMatchesText(row, matcher) ? qint16(0) : DensityMap::kNothing;
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
    m_map.scan(DensityMap::Lane::Rules, m_map.rows(), [this](int row) -> qint16 {
        const int rule = m_model->matchedRule(row);
        return rule < 0 ? DensityMap::kNothing : qint16(rule);
    });
    const TextMatcher matcher = m_view->findMatcher();
    if (!matcher.isEmpty() && matcher.isValid()) {
        m_map.scan(DensityMap::Lane::Find, m_map.rows(), [this, matcher](int row) -> qint16 {
            return m_model->rowMatchesText(row, matcher) ? qint16(0) : DensityMap::kNothing;
        });
    }
    update();
}

QColor DensityScrollBar::markColour(int ruleIndex) const
{
    if (!m_document || ruleIndex < 0 || ruleIndex >= m_document->highlighters().rules.size())
        return palette().text().color();
    const HighlightRule &rule = m_document->highlighters().rules.at(ruleIndex);
    const bool dark = m_model->darkTheme();
    // The record's own background is what the reader is scanning the log for, so it is
    // what the bar shows. A rule that sets only a foreground still marks its records;
    // one that sets neither is a rule with some other action, and the theme's text
    // colour says "something is here" without claiming a colour the record does not have.
    if (const QColor bg = HighlightPalette::color(rule.background, dark); bg.isValid())
        return bg;
    if (const QColor fg = HighlightPalette::color(rule.foreground, dark); fg.isValid())
        return fg;
    return palette().text().color();
}

qint64 DensityScrollBar::spanLines() const
{
    // The quantity BOTH the thumb and the marks are fractions of. LogView::updateScrollBars
    // sets the range to 0..total-page with pageStep = page, so this is the view's total
    // line count — the same denominator LogView::scrollFractionOfRow divides by, which is
    // what makes a mark and the thumb that shows its record land in the same place.
    return qMax<qint64>(1, qint64(maximum()) - qint64(minimum()) + qint64(qMax(0, pageStep())));
}

QRect DensityScrollBar::thumbRect() const
{
    const int h = height();
    if (h <= 0)
        return {};
    const qint64 total = spanLines();
    int th = int(qreal(qMax(0, pageStep())) / qreal(total) * h);
    th = qBound(qMin(kMinThumbPx, h), th, h);
    const qint64 v = qint64(sliderPosition()) - qint64(minimum());
    int top = int(qreal(qMax<qint64>(0, v)) / qreal(total) * h);
    top = qBound(0, top, h - th);
    return {0, top, width(), th};
}

void DensityScrollBar::scrollToThumbTop(int topPx)
{
    const int h = qMax(1, height());
    const qint64 total = spanLines();
    const qint64 v = qint64(qreal(qBound(0, topPx, h)) / qreal(h) * qreal(total));
    setSliderPosition(int(qBound<qint64>(qint64(minimum()), qint64(minimum()) + v,
                                         qint64(maximum()))));
}

void DensityScrollBar::bandOf(int bucket, int &top, int &bottom) const
{
    const int h = height();
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
        // Two lanes, side by side, so a find match cannot hide under a rule's colour or
        // the other way round. Rules take the wider one: they are what is there all the
        // time.
        const int laneGap = 1;
        const int findWidth = qBound(2, (w - 2) / 3, 5);
        const int rulesLeft = 2;
        const int rulesWidth = qMax(2, w - rulesLeft - findWidth - laneGap);
        const int findLeft = rulesLeft + rulesWidth + laneGap;

        const int buckets = m_map.bucketCount();
        for (int b = 0; b < buckets; ++b) {
            const qint16 rule = m_map.at(DensityMap::Lane::Rules, b);
            const qint16 found = m_map.at(DensityMap::Lane::Find, b);
            if (rule == DensityMap::kNothing && found == DensityMap::kNothing)
                continue;
            int top = 0;
            int bottom = 0;
            bandOf(b, top, bottom);
            if (rule != DensityMap::kNothing)
                p.fillRect(QRect(rulesLeft, top, rulesWidth, bottom - top), markColour(rule));
            if (found != DensityMap::kNothing) {
                // Find marks wear the theme's own selection colour — what "the thing you
                // asked for" already looks like everywhere else in the window, and the
                // one colour guaranteed to be distinct from every palette slot on both
                // themes.
                p.fillRect(QRect(findLeft, top, findWidth, bottom - top),
                           palette().highlight().color());
            }
        }
    }

    // The thumb LAST and translucent, over the marks rather than instead of them.
    const QRect thumb = thumbRect();
    if (thumb.height() <= 0 || thumb.height() >= h)
        return; // nothing to scroll: a thumb spanning the whole bar says nothing
    QColor fill = palette().text().color();
    fill.setAlpha(m_dragging || underMouse() ? kThumbAlphaActive : kThumbAlpha);
    QColor edge = palette().text().color();
    edge.setAlpha(kThumbEdgeAlpha);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setBrush(fill);
    p.setPen(edge);
    p.drawRoundedRect(QRectF(thumb).adjusted(1.5, 0.5, -0.5, -0.5), 2.0, 2.0);
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
