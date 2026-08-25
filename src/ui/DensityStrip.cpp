#include "DensityStrip.h"

#include "Document.h"
#include "LogModel.h"
#include "LogView.h"
#include "Palette.h"
#include "UiColors.h"

#include <QElapsedTimer>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>

namespace loftail {

namespace {

// How long one slice of the scan may take, and how often a slice runs. A quarter of a
// frame, four times a second's worth of frames apart: the strip fills in visibly while
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
// of a pixel tall, and the one FATAL in it is exactly what the strip exists to show, so
// a mark is never smaller than this however little of the file it stands for.
constexpr int kMinMarkPx = 2;

} // namespace

DensityStrip::DensityStrip(LogView *view, const Document *document, LogModel *model)
    : QWidget(view), m_view(view), m_document(document), m_model(model)
{
    setObjectName(QStringLiteral("densityStrip")); // test contract, never the visible text
    setToolTip(tr("Where the highlighted records and find matches are. Click to jump."));
    setCursor(Qt::PointingHandCursor);
    // The strip is drawn, never composed of children, and it must not take the focus
    // off the table on a click that only means "scroll there".
    setFocusPolicy(Qt::NoFocus);

    m_scanTimer = new QTimer(this);
    m_scanTimer->setInterval(kSliceIntervalMs);
    connect(m_scanTimer, &QTimer::timeout, this, &DensityStrip::scanSlice);
}

DensityStrip::~DensityStrip() = default;

int DensityStrip::stripWidth() const
{
    // Measured in characters of the view's own font, and floored: a platform with an
    // empty font database answers 0 to every advance (the Windows offscreen plugin ships
    // no fonts), and an unfloored width would collapse the strip to nothing.
    const int ch = qMax(4, fontMetrics().horizontalAdvance(QLatin1Char('0')));
    return qBound(10, ch * 2, 28);
}

void DensityStrip::rebind()
{
    m_map.rebind(m_model ? m_model->rowCount() : 0);
    startScanning();
}

void DensityStrip::rowsChanged()
{
    syncRows();
    startScanning();
}

void DensityStrip::invalidateRules()
{
    syncRows();
    m_map.clear(DensityMap::Lane::Rules);
    startScanning();
}

void DensityStrip::invalidateFind()
{
    syncRows();
    m_map.clear(DensityMap::Lane::Find);
    startScanning();
}

void DensityStrip::refresh()
{
    update();
}

void DensityStrip::syncRows()
{
    m_map.setRows(m_model ? m_model->rowCount() : 0);
}

void DensityStrip::startScanning()
{
    update();
    // Only while the strip is on screen. isVisible() is false for a widget in a tab
    // that is not current, which is exactly the case this is guarding: ten open logs
    // must not be ten scans.
    if (!isVisible())
        return;
    if (!m_scanTimer->isActive())
        m_scanTimer->start();
}

void DensityStrip::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // A tab raised after its scan was suspended picks up where it left off — nothing
    // was thrown away, so returning to a log does not restart its strip.
    syncRows();
    startScanning();
}

void DensityStrip::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    m_scanTimer->stop();
}

void DensityStrip::scanSlice()
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
    // strip on a log with a Find running still shows its errors within a frame or two.
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

void DensityStrip::scanNowForTests()
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

QColor DensityStrip::markColour(int ruleIndex) const
{
    if (!m_document || ruleIndex < 0 || ruleIndex >= m_document->highlighters().rules.size())
        return palette().text().color();
    const HighlightRule &rule = m_document->highlighters().rules.at(ruleIndex);
    const bool dark = m_model->darkTheme();
    // The record's own background is what the reader is scanning the log for, so it is
    // what the strip shows. A rule that sets only a foreground still marks its records;
    // one that sets neither is a rule with some other action, and the theme's text
    // colour says "something is here" without claiming a colour the record does not have.
    if (const QColor bg = HighlightPalette::color(rule.background, dark); bg.isValid())
        return bg;
    if (const QColor fg = HighlightPalette::color(rule.foreground, dark); fg.isValid())
        return fg;
    return palette().text().color();
}

void DensityStrip::bandOf(int bucket, int &top, int &bottom) const
{
    const int h = height();
    const int firstRow = m_map.firstRowOf(bucket);
    const int nextRow = qMin(m_map.rows(), firstRow + m_map.rowsPerBucket());
    const qreal a = m_view->scrollFractionOfRow(firstRow);
    // The END of the bucket is the start of the row after it, so two adjacent marked
    // buckets meet rather than leaving a hairline of unmarked strip between them.
    const qreal b = nextRow >= m_map.rows() ? 1.0 : m_view->scrollFractionOfRow(nextRow);
    top = qBound(0, int(a * h), qMax(0, h - 1));
    bottom = qBound(top + 1, int(b * h), h);
    if (bottom - top < kMinMarkPx)
        bottom = qMin(h, top + kMinMarkPx);
    if (bottom - top < kMinMarkPx)
        top = qMax(0, bottom - kMinMarkPx);
}

void DensityStrip::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter p(this);
    const int w = width();
    const int h = height();
    if (w <= 0 || h <= 0)
        return;

    // The strip's own ground is the table's, so it reads as part of the view rather than
    // as chrome, with one hairline separating it from the text — the same divider a
    // pane's section heading is drawn with, for the same reason (UiColors::dividerColor).
    p.fillRect(rect(), palette().base().color());
    p.setPen(dividerColor(palette()));
    p.drawLine(0, 0, 0, h - 1);

    if (m_map.bucketCount() <= 0 || !m_view)
        return;

    // Two lanes, side by side, so a find match cannot hide under a rule's colour or the
    // other way round. Rules take the wider one: they are what is there all the time.
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
            // asked for" already looks like everywhere else in the window, and the one
            // colour guaranteed to be distinct from every palette slot on both themes.
            p.fillRect(QRect(findLeft, top, findWidth, bottom - top),
                       palette().highlight().color());
        }
    }
}

void DensityStrip::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    m_view->scrollToFraction(event->position().y() / qMax(1, height()));
    event->accept();
}

void DensityStrip::mouseMoveEvent(QMouseEvent *event)
{
    // A drag down the strip scrolls continuously, which is how anybody uses a minimap
    // once they have found out a click works.
    if (!(event->buttons() & Qt::LeftButton)) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    m_view->scrollToFraction(event->position().y() / qMax(1, height()));
    event->accept();
}

} // namespace loftail
