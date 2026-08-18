#include "LogView.h"

#include "UiColors.h"

#include "Decoder.h"
#include "Document.h"
#include "Fonts.h"
#include "LogFormat.h"
#include "LogModel.h"
#include "LogSource.h"
#include "RecordIndex.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QHeaderView>
#include <QHelpEvent>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyleOptionHeader>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>

namespace loftail {

namespace {
int defaultColumnWidth(FieldRole role)
{
    switch (role) {
    case FieldRole::Date:       return 190;
    case FieldRole::Thread:     return 110;
    case FieldRole::Priority:   return 60;
    case FieldRole::Logger:     return 160;
    case FieldRole::FileName:   return 140;
    case FieldRole::LineNumber: return 60;
    case FieldRole::Method:     return 140;
    case FieldRole::Location:   return 200;
    case FieldRole::ThreadName: return 120;
    case FieldRole::ProcessId:  return 70;
    case FieldRole::Hostname:   return 140;
    case FieldRole::Elapsed:    return 90;
    case FieldRole::Ndc:        return 140;
    case FieldRole::Mdc:        return 160;
    case FieldRole::EnvVar:     return 120;
    case FieldRole::Message:    return 1600; // wide: wrap-off scrolls sideways (§5)
    }
    return 120;
}

// One fixed-height cell, ELIDED at the column's right edge rather than left to clip
// mid-glyph (SPEC.md §5). The ellipsis is the only thing that tells a value too wide
// for its column from one that genuinely ends there — and it is what makes the tooltip
// honest, since both ask the same question of the same width.
void drawElidedCell(QPainter &p, const QRect &rect, const QString &text)
{
    p.drawText(rect, Qt::AlignVCenter | Qt::TextSingleLine,
               p.fontMetrics().elidedText(text, Qt::ElideRight, rect.width()));
}
} // namespace

// ---------------------------------------------------------------------------
// Pure geometry mapping (exact mode). The base RecordIndex prefix sums give the
// unwrapped line<->record mapping; here we fold in the one selected record whose
// wrapped height differs, patching a single delta rather than rebuilding (§7.1.1).
// ---------------------------------------------------------------------------

static qint64 selExtraLines(const RecordIndex &idx, int selRecord, int selWrapLines)
{
    if (selRecord < 0 || selRecord >= idx.records.size())
        return 0;
    // Wrapping never merges physical lines, so after the shared 100-line display
    // cap the wrapped height is >= the unwrapped height: the delta is non-negative.
    return qMax<qint64>(0, qint64(selWrapLines) - RecordIndex::displayLines(idx.records.at(selRecord)));
}

qint64 LogView::totalScrollLines(const RecordIndex &idx, int selRecord, int selWrapLines)
{
    return idx.totalLines() + selExtraLines(idx, selRecord, selWrapLines);
}

qint64 LogView::scrollLineOfRecord(const RecordIndex &idx, int selRecord, int selWrapLines, int r)
{
    qint64 line = idx.firstLineOfRecord(r);
    if (selRecord >= 0 && r > selRecord)
        line += selExtraLines(idx, selRecord, selWrapLines);
    return line;
}

int LogView::recordAtScrollLine(const RecordIndex &idx, int selRecord, int selWrapLines, qint64 line)
{
    const qint64 extra = selExtraLines(idx, selRecord, selWrapLines);
    if (extra == 0 || selRecord < 0)
        return idx.recordAtLine(line);

    const qint64 selStart = idx.firstLineOfRecord(selRecord);
    if (line < selStart)
        return idx.recordAtLine(line);
    if (line < selStart + selWrapLines)
        return selRecord; // inside the wrapped selected record
    return idx.recordAtLine(line - extra); // records after the selected one
}

// ---------------------------------------------------------------------------
// Pure clipboard helpers
// ---------------------------------------------------------------------------

QString LogView::flattenCell(const QString &text)
{
    QString out = text;
    out.replace(QLatin1Char('\t'), QLatin1Char(' '));
    out.replace(QLatin1Char('\r'), QLatin1Char(' '));
    out.replace(QLatin1Char('\n'), QLatin1Char(' '));
    return out;
}

QString LogView::columnsToTsv(const QVector<QVector<QString>> &rows)
{
    QStringList lines;
    lines.reserve(rows.size());
    for (const QVector<QString> &cells : rows) {
        QStringList joined;
        joined.reserve(cells.size());
        for (const QString &c : cells)
            joined << c;
        lines << joined.join(QLatin1Char('\t'));
    }
    return lines.join(QLatin1Char('\n'));
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

LogView::LogView(const Document *document, LogModel *model, QWidget *parent, Role role)
    : QAbstractScrollArea(parent), m_document(document), m_model(model), m_role(role)
{
    // Every column, and the header, render in the same fixed-pitch font: cells
    // line up vertically, and the estimated-geometry path's character-count model
    // stays valid (invariant #6, ARCHITECTURE.md §7.1.1).
    setFont(monospaceFont());
    setFocusPolicy(Qt::StrongFocus);
    viewport()->setFocusProxy(this);

    m_header = new QHeaderView(Qt::Horizontal, this);
    m_header->setModel(m_model);
    m_header->setSectionsMovable(true);
    m_header->setSectionsClickable(false);
    m_header->setSectionResizeMode(QHeaderView::Interactive);
    m_header->setStretchLastSection(false);
    m_header->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    // A caption reading "Priorit" says nothing about being a caption cut short, so the
    // header elides like the cells under it and names itself in full on hover. The
    // tooltip is filtered rather than answered through the model's ToolTipRole, which
    // QHeaderView would show for every section whether or not it fitted.
    m_header->setTextElideMode(Qt::ElideRight);
    m_header->viewport()->installEventFilter(this);
    // Seed sensible per-role widths; the user can resize and the layout is saved.
    for (int c = 0; c < m_document->format().fields.size(); ++c)
        m_header->resizeSection(c, defaultColumnWidth(m_document->format().fields.at(c).role));

    m_selection = new QItemSelectionModel(m_model, this);

    // Debounces width-change remeasurement in AlwaysOn (§7.1.1): a drag-resize
    // fires many events, so remeasure once when it settles rather than per frame.
    m_resizeTimer = new QTimer(this);
    m_resizeTimer->setSingleShot(true);
    m_resizeTimer->setInterval(120);
    connect(m_resizeTimer, &QTimer::timeout, this, &LogView::applyDebouncedResize);

    verticalScrollBar()->setSingleStep(1);

    connect(m_header, &QHeaderView::sectionResized, this, [this](int, int, int) {
        recomputeGeometry();
        emit columnLayoutChanged();
    });
    connect(m_header, &QHeaderView::sectionMoved, this, [this](int, int, int) {
        viewport()->update();
        emit columnLayoutChanged();
    });
    connect(m_model, &QAbstractItemModel::rowsInserted, this, &LogView::handleRowsInserted);
    connect(m_model, &QAbstractItemModel::rowsRemoved, this, &LogView::handleRowsRemoved);
    connect(m_model, &QAbstractItemModel::modelReset, this, &LogView::handleModelReset);
    // A trailing record that grew in place (M6 live append with no new rows) arrives
    // as a dataChanged, not an insert; it changes that record's height, so refresh
    // geometry and keep following if attached.
    connect(m_model, &QAbstractItemModel::dataChanged, this,
            [this](const QModelIndex &, const QModelIndex &, const QList<int> &) { handleTailChanged(); });

    // Return-to-bottom control (SPEC.md §3): a small overlay shown only when follow
    // has detached; clicking it re-attaches and jumps to the newest record. A digest
    // strip does not scroll, so it has nothing to detach from and no button to offer.
    if (m_role == Role::Main) {
        m_followButton = new QToolButton(viewport());
        m_followButton->setText(tr("Follow tail ↓"));
        m_followButton->setToolTip(tr("Jump to the newest record and follow new ones"));
        m_followButton->setCursor(Qt::PointingHandCursor);
        m_followButton->setAutoRaise(false);
        m_followButton->hide();
        connect(m_followButton, &QToolButton::clicked, this, &LogView::followTail);
    }

    if (m_role == Role::Digest) {
        // The strip borrows the table's column layout rather than carrying a second
        // set of captions, and it is sized to its rows rather than scrolled — so both
        // scrollbars go, and the vertical one comes back only when the height cap bites
        // (see sizeHint). Its height is its whole contract, so it must be able to ask
        // for one and must not be stretched past it.
        m_header->hide();
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        // Click-focus, not strong: Tab must not stop on a strip of at most a few rows
        // on the way from the table to the Find bar.
        setFocusPolicy(Qt::ClickFocus);
        // Its rows are a different ordinal space from the main view's, so the size hint
        // has to be recomputed whenever the content changes rather than only on resize.
        connect(m_model, &QAbstractItemModel::modelReset, this, [this] { refreshDigestCap(); });
        connect(m_model, &QAbstractItemModel::rowsInserted, this, [this] { refreshDigestCap(); });
        connect(m_model, &QAbstractItemModel::rowsRemoved, this, [this] { refreshDigestCap(); });
    }

    layoutHeader();
    recomputeGeometry();
}

LogView::~LogView() = default;

// ---------------------------------------------------------------------------
// Basic metrics
// ---------------------------------------------------------------------------

int LogView::lineHeight() const { return qMax(1, fontMetrics().height()); }
int LogView::visibleLines() const { return qMax(1, viewport()->height() / lineHeight()); }
// Both go through the MODEL, not the document (M19, ARCHITECTURE.md §7.5): which
// subset a view shows is the model's question now that a second model can point at a
// second FilteredIndex over the same document. With no view index set — every view but
// the digest strip — these are exactly what they were.
int LogView::recordCount() const { return m_model->rowCount(); }
const RecordIndex &LogView::geom() const { return m_model->viewGeometry(); }

int LogView::messageColumn() const
{
    const QVector<Field> &fields = m_document->format().fields;
    for (int c = 0; c < fields.size(); ++c)
        if (fields.at(c).role == FieldRole::Message)
            return c;
    return -1;
}

int LogView::selRecordForGeometry() const
{
    if (m_wrapMode == WrapMode::SelectedRecordOnly && m_current >= 0 && m_current < recordCount())
        return m_current;
    return -1;
}

int LogView::measureWrappedLines(const QString &text, int width) const
{
    const QRect br = fontMetrics().boundingRect(QRect(0, 0, qMax(10, width), 0),
                                                Qt::TextWordWrap | Qt::TextWrapAnywhere, text);
    const int lh = lineHeight();
    const int lines = qMax(1, (br.height() + lh - 1) / lh);
    return qMin<int>(RecordIndex::kDisplayLineCap, lines);
}

int LogView::selWrapLines() const
{
    const int sel = selRecordForGeometry();
    if (sel < 0)
        return 0;
    if (m_selWrapCache > 0)
        return m_selWrapCache;
    return RecordIndex::displayLines(geom().records.at(sel));
}

int LogView::recordHeightLines(int r) const
{
    if (selRecordForGeometry() == r)
        return qMax(1, selWrapLines());
    return RecordIndex::displayLines(geom().records.at(r));
}

// ---------------------------------------------------------------------------
// Mode-branching geometry wrappers (invariant #6). This is the ONLY seam where
// the exact and estimated paths meet: each wrapper branches on the mode once.
// When not estimating() the wrappers forward to the exact statics with the
// selected-record wrap folded in, so the M2b exact path is byte-identical; the
// estimated machinery (m_estimated) is reached from nowhere else.
// ---------------------------------------------------------------------------

bool LogView::estimating() const
{
    // AlwaysOn with a message column to wrap. Without a message field there is
    // nothing width-dependent to estimate, so we stay on the exact path.
    return m_wrapMode == WrapMode::AlwaysOn && messageColumn() >= 0;
}

qint64 LogView::mapTotalLines() const
{
    if (estimating())
        return m_estimated.totalLines();
    return totalScrollLines(geom(), selRecordForGeometry(), selWrapLines());
}

qint64 LogView::mapLineOfRecord(int r) const
{
    if (estimating())
        return m_estimated.firstLineOfRecord(r);
    return scrollLineOfRecord(geom(), selRecordForGeometry(), selWrapLines(), r);
}

int LogView::mapRecordAtLine(qint64 line) const
{
    if (estimating())
        return m_estimated.recordAtLine(line);
    return recordAtScrollLine(geom(), selRecordForGeometry(), selWrapLines(), line);
}

int LogView::mapRecordHeightLines(int r) const
{
    if (estimating())
        return qMax(1, m_estimated.recordHeightLines(r));
    return recordHeightLines(r);
}

// ---------------------------------------------------------------------------
// Estimated-mode support (AlwaysOn only)
// ---------------------------------------------------------------------------

int LogView::viewportCols() const
{
    // Characters that fit across the wrapped message column. Fixed-pitch font, so
    // this is a divide, not a shaping pass (§7.1.1). The message wraps within the
    // width from its column origin to the viewport's right edge.
    const int msgCol = messageColumn();
    const int x = msgCol >= 0 ? m_header->sectionViewportPosition(msgCol) : 0;
    const int avail = qMax(1, viewport()->width() - x);
    const int cw = qMax(1, fontMetrics().averageCharWidth());
    return qMax(1, avail / cw);
}

void LogView::ensureEstimatorBound()
{
    // Bind (or rebind) the estimator to the current index. Rebinds only when the
    // index identity or its block count changed (e.g. records appended during the
    // scan) — never on a plain width change, which the debounced resize owns.
    const RecordIndex &idx = geom();
    const int wantBlocks =
        (idx.records.size() + RecordIndex::kBlockSize - 1) / RecordIndex::kBlockSize;
    if (m_estimated.index() != &idx || m_estimated.blockCount() != wantBlocks)
        m_estimated.reset(&idx, viewportCols());
}

void LogView::measureBlock(int block)
{
    if (!estimating() || block < 0 || block >= m_estimated.blockCount()
        || m_estimated.isBlockMeasured(block))
        return;

    const int msgCol = messageColumn();
    const int cols = m_estimated.columns();
    const int cap = RecordIndex::kDisplayLineCap;
    const int start = block * RecordIndex::kBlockSize;
    const int n = recordCount();
    const int end = qMin(start + RecordIndex::kBlockSize, n);

    QVector<quint16> lines;
    lines.reserve(end - start);
    QVector<int> lineChars;
    for (int r = start; r < end; ++r) {
        // Decoded message text via the model (invariant #8: split the decoded
        // string on '\n', never scan raw bytes). Only char counts are kept — no
        // parsed text is retained per record (invariant #1); the block cache
        // stores heights, and only for visited blocks.
        const QString msg = m_model->cellText(r, msgCol);
        lineChars.clear();
        int from = 0;
        while (true) {
            const int nl = msg.indexOf(QLatin1Char('\n'), from);
            if (nl < 0) {
                lineChars.append(int(msg.size()) - from);
                break;
            }
            lineChars.append(nl - from);
            from = nl + 1;
        }
        lines.append(quint16(EstimatedGeometry::measuredRecordLines(lineChars, cols, cap)));
    }
    m_estimated.measureBlock(block, lines);
}

void LogView::measureBlockOfRecord(int record)
{
    if (!estimating())
        return;
    ensureEstimatorBound();
    if (record >= 0 && record < recordCount())
        measureBlock(m_estimated.blockOfRecord(record));
    updateScrollBars();
    viewport()->update();
}

void LogView::measureVisibleBlocks()
{
    // Measure every block the viewport touches, then re-anchor so a height
    // refinement does not make the content under the cursor jump. Called from
    // paint; the whole thing is cached, so subsequent frames measure nothing.
    ensureEstimatorBound();
    const int n = recordCount();
    if (n == 0)
        return;

    const qint64 topLine = verticalScrollBar()->value();
    const int topRec = m_estimated.recordAtLine(topLine);
    if (topRec < 0)
        return;
    // A viewport of V lines holds at most V records (each >= 1 line), so
    // [topRec, topRec+V] bounds everything paintable this frame.
    const int lastRec = qMin(n - 1, topRec + visibleLines());
    const int b0 = m_estimated.blockOfRecord(topRec);
    const int b1 = m_estimated.blockOfRecord(lastRec);

    bool measured = false;
    for (int b = b0; b <= b1; ++b) {
        if (!m_estimated.isBlockMeasured(b)) {
            measureBlock(b);
            measured = true;
        }
    }
    if (measured) {
        // Refined heights change the total and can shift records; keep topRec put
        // (snap to its top) and refresh the scrollbar range without recursing.
        const QSignalBlocker guard(verticalScrollBar());
        updateScrollBars();
        const qint64 anchor = m_estimated.firstLineOfRecord(topRec);
        verticalScrollBar()->setValue(int(qMin<qint64>(anchor, verticalScrollBar()->maximum())));
    }
}

// ---------------------------------------------------------------------------
// Geometry / scrollbars / header layout
// ---------------------------------------------------------------------------

void LogView::recomputeGeometry()
{
    if (estimating()) {
        // Estimated path: no per-record selection wrap to fold in. Ensure the
        // estimator tracks the current index, then let the scrollbar reflect the
        // (refining) estimated total. Measurement happens lazily on paint.
        ensureEstimatorBound();
        m_selWrapCache = -1;
        updateScrollBars();
        viewport()->update();
        // A column resize changes the message width (hence the wrap column count)
        // just like a viewport resize does, so re-sync the width-keyed cache on
        // the same debounce rather than remeasuring on every drag step.
        m_resizeTimer->start();
        return;
    }

    const int sel = selRecordForGeometry();
    if (sel >= 0) {
        const int msgCol = messageColumn();
        if (msgCol >= 0) {
            const int msgX = m_header->sectionViewportPosition(msgCol);
            const int avail = qMax(50, viewport()->width() - msgX);
            m_selWrapCache = measureWrappedLines(m_model->cellText(sel, msgCol), avail);
        } else {
            m_selWrapCache = RecordIndex::displayLines(geom().records.at(sel));
        }
    } else {
        m_selWrapCache = -1;
    }
    updateScrollBars();
    viewport()->update();
}

void LogView::updateScrollBars()
{
    const qint64 total = mapTotalLines(); // exact or estimated, per the mode
    const int page = visibleLines();
    verticalScrollBar()->setRange(0, int(qMax<qint64>(0, total - page)));
    verticalScrollBar()->setPageStep(page);
    verticalScrollBar()->setSingleStep(1);

    const int len = m_header->length();
    horizontalScrollBar()->setRange(0, qMax(0, len - viewport()->width()));
    horizontalScrollBar()->setPageStep(viewport()->width());
    horizontalScrollBar()->setSingleStep(qMax(1, fontMetrics().averageCharWidth() * 2));
    m_header->setOffset(horizontalScrollBar()->value());
}

void LogView::layoutHeader()
{
    // A hidden header reserves NOTHING. Asking unconditionally would leave a
    // header-tall blank band above the digest strip — which reads as a rendering fault
    // rather than a bug, in the one widget whose whole claim is that it is exactly as
    // tall as its rows.
    if (m_header->isHidden()) {
        setViewportMargins(0, 0, 0, 0);
        return;
    }
    const int h = m_header->sizeHint().height();
    setViewportMargins(0, h, 0, 0);
    m_header->setGeometry(viewport()->x(), viewport()->y() - h, viewport()->width(), h);
}

qint64 LogView::digestContentLines(bool *capped) const
{
    const qint64 lines = m_model->rowCount() > 0 ? mapTotalLines() : 0;

    // The cap. A single digest record can legitimately be a hundred-line stack trace,
    // and a strip that ate the log it sits under would be worse than no strip. A third
    // of the parent's height, or kDigestMaxLines, whichever is smaller.
    qint64 capLines = kDigestMaxLines;
    if (const QWidget *p = parentWidget(); p && p->height() > 0)
        capLines = qMin<qint64>(capLines, qMax(1, p->height() / (3 * lineHeight())));

    const qint64 shown = qMin(lines, capLines);
    if (capped)
        *capped = shown < lines;
    return shown;
}

void LogView::refreshDigestCap()
{
    if (m_role != Role::Digest)
        return;
    bool capped = false;
    digestContentLines(&capped);
    // The strip is "not scrolled" in every ordinary case; the scrollbar comes back only
    // where the cap bit, so the content past it stays reachable rather than truncated.
    //
    // Guarded on an actual change, and kept OUT of sizeHint(), which must stay a pure
    // query: QAbstractScrollArea::setVerticalScrollBarPolicy() calls layoutChildren()
    // whether or not the policy moved, and layout asks for the size hint — so setting
    // it from inside the hint is an infinite recursion. It hung tst_multidoc.
    const Qt::ScrollBarPolicy wanted =
        capped ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff;
    if (verticalScrollBarPolicy() != wanted)
        setVerticalScrollBarPolicy(wanted);
    updateGeometry();
}

QSize LogView::sizeHint() const
{
    if (m_role != Role::Digest)
        return QAbstractScrollArea::sizeHint();

    // Display lines, not rows: a digest record renders at full height exactly as it
    // does in the log, which is the point of showing it here rather than summarising it.
    return QSize(QAbstractScrollArea::sizeHint().width(),
                 frameWidth() * 2 + int(digestContentLines(nullptr)) * lineHeight());
}

void LogView::setHorizontalOffset(int value)
{
    if (horizontalScrollBar()->value() == value)
        return;
    // Past the strip's own range when the table is wider than it is; clamping keeps the
    // columns as aligned as they can be rather than snapping back to zero.
    horizontalScrollBar()->setValue(
        qBound(horizontalScrollBar()->minimum(), value, horizontalScrollBar()->maximum()));
    m_header->setOffset(horizontalScrollBar()->value());
    viewport()->update();
}

void LogView::resizeEvent(QResizeEvent *event)
{
    QAbstractScrollArea::resizeEvent(event);
    layoutHeader();
    positionFollowButton();
    if (estimating()) {
        // Debounce (§7.1.1): a drag-resize fires a burst of these. Keep the view
        // usable now from the existing (possibly stale-width) estimates, and
        // remeasure once when the drag settles.
        ensureEstimatorBound();
        updateScrollBars();
        viewport()->update();
        m_resizeTimer->start();
        return;
    }
    recomputeGeometry();
}

void LogView::applyDebouncedResize()
{
    if (!estimating())
        return;
    ensureEstimatorBound();
    const int topRec = m_estimated.recordAtLine(verticalScrollBar()->value());
    if (m_estimated.setColumns(viewportCols())) {
        // Width actually changed: every measurement was width-keyed and is now
        // dropped, so the total falls back to estimates until blocks are
        // remeasured on the next paint. Re-anchor on the top record so the
        // content does not jump under the new geometry.
        const QSignalBlocker guard(verticalScrollBar());
        updateScrollBars();
        if (topRec >= 0) {
            const qint64 anchor = m_estimated.firstLineOfRecord(topRec);
            verticalScrollBar()->setValue(int(qMin<qint64>(anchor, verticalScrollBar()->maximum())));
        }
    }
    viewport()->update();
}

void LogView::scrollContentsBy(int dx, int dy)
{
    if (dx != 0) {
        m_header->setOffset(horizontalScrollBar()->value());
        emit horizontalOffsetChanged(horizontalScrollBar()->value());
    }
    if (dy != 0)
        updateFollowFromScrollPosition(); // a vertical move may detach/re-attach follow
    viewport()->update();
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

void LogView::resolveRowColors(int row, bool selected, QColor &bg, QColor &fg) const
{
    // Selection always wins — a highlighted record is still clearly selectable.
    if (selected) {
        bg = palette().highlight().color();
        fg = palette().highlightedText().color();
        return;
    }

    // Highlight rules (M5, SPEC.md §7): first-match-wins is evaluated in
    // LogModel::data(), which hands back the matched rule's palette colors via the
    // Background/Foreground roles — or an empty variant meaning "leave this role at
    // the theme default". An invalid/absent background keeps the base fill with the
    // alternating-record band; an invalid foreground keeps the theme text color.
    // One call, not two data() lookups: highlighting is per record, and a rule may
    // now match on message text, so resolving the roles separately would run the rule
    // list — and potentially the decode — twice per painted record (SPEC.md §7).
    QColor ruleBg, ruleFg;
    m_model->rowColors(row, ruleBg, ruleFg);

    if (ruleBg.isValid()) {
        // A rule-coloured record wears its own colour, unbanded: the band is what a
        // record wears when nothing else has claimed it, and tinting every other one
        // of a rule's records would read as two rules.
        bg = ruleBg;
    } else {
        // `row` is a RECORD, not a line (invariant #2), and the caller fills the whole
        // record's rect with what comes back — so the band changes exactly at a record
        // boundary, which in wrap-always-on is the only thing marking one.
        bg = (row % 2) ? alternateRowColor(palette()) : palette().base().color();
    }

    fg = ruleFg.isValid() ? ruleFg : palette().text().color();

    // Filter context (M15, SPEC.md §6): a record shown only because a neighbour
    // matched recedes, so the matches stay findable by eye. Last, and below
    // selection: it modifies whatever the row would otherwise have been, including a
    // highlight rule's colours — which are softened rather than dropped, or a
    // rule-coloured context row would read as a match.
    if (m_model->rowIsContext(row)) {
        if (ruleBg.isValid())
            bg = contextFillColor(bg, palette().base().color());
        fg = contextTextColor(fg, bg);
    }
}

void LogView::paintEvent(QPaintEvent *event)
{
    QPainter p(viewport());
    p.fillRect(event->rect(), palette().base());

    const RecordIndex &idx = geom();
    const int n = idx.records.size();
    if (n == 0) {
        // An empty grid is indistinguishable from an empty log, so a view with nothing
        // in it says WHY when it has something to say — "app.log has not appeared yet"
        // (SPEC.md §3). Drawn here rather than as a swapped-in widget because the tab
        // is a real, live view throughout: it keeps its filters, its column layout and
        // its place in the session, and gains rows the moment the log turns up.
        if (!m_placeholderText.isEmpty()) {
            p.setPen(mutedColor(palette()));
            p.drawText(viewport()->rect(), Qt::AlignCenter | Qt::TextWordWrap,
                       m_placeholderText);
        }
        return;
    }

    const int lh = lineHeight();

    // Estimated (wrap-always-on) path — kept entirely separate from the exact
    // painting below so the exact path never routes through estimation (#6).
    if (estimating()) {
        measureVisibleBlocks(); // one-time per block; cached, re-anchors if refined

        const qint64 topLine = verticalScrollBar()->value();
        int r = m_estimated.recordAtLine(topLine);
        if (r < 0)
            return;
        int y = int((m_estimated.firstLineOfRecord(r) - topLine) * lh);

        const QVector<Field> &fields = m_document->format().fields;
        const int msgCol = messageColumn();
        const int vh = viewport()->height();
        const int vw = viewport()->width();

        while (r < n && y < vh) {
            const int hLines = qMax(1, m_estimated.recordHeightLines(r));
            const int rowH = hLines * lh;
            const bool selected = m_selection->isSelected(m_model->index(r, 0));

            QColor band, fg;
            resolveRowColors(r, selected, band, fg);
            p.fillRect(QRect(0, y, vw, rowH), band);
            p.setPen(fg);

            for (int vi = 0; vi < fields.size(); ++vi) {
                const int logical = m_header->logicalIndex(vi);
                if (logical < 0 || m_header->isSectionHidden(logical))
                    continue;
                const int x = m_header->sectionViewportPosition(logical);
                const int w = m_header->sectionSize(logical);
                if (logical == msgCol) {
                    const int availW = qMax(10, vw - x);
                    // Character wrapping (TextWrapAnywhere) so the painted height
                    // matches the ceil(chars/cols) measurement model exactly.
                    p.drawText(QRect(x, y, availW, rowH),
                               Qt::TextWrapAnywhere | Qt::AlignTop, m_model->cellText(r, logical));
                } else {
                    drawElidedCell(p, QRect(x, y, w, lh), m_model->cellText(r, logical));
                }
            }
            y += rowH;
            ++r;
        }
        return;
    }

    const int sel = selRecordForGeometry();
    const int selW = selWrapLines();
    const qint64 topLine = verticalScrollBar()->value();

    int r = recordAtScrollLine(idx, sel, selW, topLine);
    if (r < 0)
        return;
    const qint64 rTop = scrollLineOfRecord(idx, sel, selW, r);
    int y = int((rTop - topLine) * lh); // <= 0 when scrolled into a record

    const QVector<Field> &fields = m_document->format().fields;
    const int msgCol = messageColumn();
    const int vh = viewport()->height();
    const int vw = viewport()->width();

    while (r < n && y < vh) {
        const int hLines = recordHeightLines(r);
        const int rowH = hLines * lh;
        const bool selected = m_selection->isSelected(m_model->index(r, 0));

        QColor band, fg;
        resolveRowColors(r, selected, band, fg);
        p.fillRect(QRect(0, y, vw, rowH), band);
        p.setPen(fg);

        const bool wrapThis = (sel == r);
        for (int vi = 0; vi < fields.size(); ++vi) {
            const int logical = m_header->logicalIndex(vi);
            if (logical < 0 || m_header->isSectionHidden(logical))
                continue;
            const int x = m_header->sectionViewportPosition(logical);
            const int w = m_header->sectionSize(logical);

            if (logical == msgCol) {
                const QString msg = m_model->cellText(r, logical);
                if (wrapThis) {
                    const int availW = qMax(10, vw - x);
                    p.drawText(QRect(x, y, availW, rowH),
                               Qt::TextWordWrap | Qt::TextWrapAnywhere | Qt::AlignTop, msg);
                } else {
                    // Wrap off: each physical line of the record is a clipped line of
                    // its own (invariant #2), so each elides on its own too.
                    const QList<QStringView> segs = QStringView(msg).split(QLatin1Char('\n'));
                    for (int li = 0; li < segs.size() && li < hLines; ++li)
                        drawElidedCell(p, QRect(x, y + li * lh, w, lh), segs.at(li).toString());
                }
            } else {
                drawElidedCell(p, QRect(x, y, w, lh), m_model->cellText(r, logical));
            }
        }

        y += rowH;
        ++r;
    }
}

// ---------------------------------------------------------------------------
// Hit-testing, selection, navigation
// ---------------------------------------------------------------------------

int LogView::recordAtViewportY(int yPix) const
{
    const RecordIndex &idx = m_document->index();
    if (idx.records.isEmpty() || yPix < 0)
        return -1;
    const qint64 line = qint64(verticalScrollBar()->value()) + yPix / lineHeight();
    return mapRecordAtLine(line);
}

int LogView::recordUnderPoint(int y) const
{
    if (y < 0 || recordCount() == 0)
        return -1;
    const qint64 line = qint64(verticalScrollBar()->value()) + y / lineHeight();
    if (line >= mapTotalLines())
        return -1;
    const int record = mapRecordAtLine(line);
    return (record < 0 || record >= recordCount()) ? -1 : record;
}

void LogView::selectRange(int anchor, int current)
{
    const int cols = m_model->columnCount();
    if (cols == 0)
        return;
    const int lo = qMin(anchor, current);
    const int hi = qMax(anchor, current);
    const QItemSelection sel(m_model->index(lo, 0), m_model->index(hi, cols - 1));
    m_selection->select(sel, QItemSelectionModel::ClearAndSelect);
    m_selection->setCurrentIndex(m_model->index(current, 0), QItemSelectionModel::NoUpdate);
}

QVector<int> LogView::selectedRecordsSorted() const
{
    QVector<int> rows;
    const QModelIndexList sel = m_selection->selectedRows(0);
    rows.reserve(sel.size());
    for (const QModelIndex &i : sel)
        rows.push_back(i.row());
    if (rows.isEmpty() && m_current >= 0)
        rows.push_back(m_current);
    std::sort(rows.begin(), rows.end());
    return rows;
}

void LogView::selectRecordSilently(int record)
{
    const int n = recordCount();
    if (n == 0)
        return;
    record = qBound(0, record, n - 1);
    m_current = record;
    m_anchor = record;
    selectRange(record, record);
}

void LogView::setCurrentRecord(int record, bool extendSelection)
{
    const int n = recordCount();
    if (n == 0)
        return;
    record = qBound(0, record, n - 1);
    // An explicit pick forgets a selection a filter had hidden: the user has chosen a
    // different record, so re-selecting the old one on the next widening would fight them.
    m_stickySource = -1;
    if (extendSelection && m_anchor >= 0) {
        m_current = record;
        selectRange(m_anchor, record);
    } else {
        selectRecordSilently(record);
    }
    recomputeGeometry();     // selection can change the wrapped-record geometry
    ensureRecordVisible(record);
}

void LogView::ensureRecordVisible(int record)
{
    const qint64 recTop = mapLineOfRecord(record);
    const qint64 recBottom = recTop + mapRecordHeightLines(record);
    const qint64 top = verticalScrollBar()->value();
    const qint64 page = visibleLines();
    if (recTop < top)
        verticalScrollBar()->setValue(int(recTop));
    else if (recBottom > top + page)
        verticalScrollBar()->setValue(int(qMax<qint64>(0, recBottom - page)));
}

void LogView::mousePressEvent(QMouseEvent *event)
{
    const int record = recordAtViewportY(int(event->position().y()));
    if (record < 0) {
        QAbstractScrollArea::mousePressEvent(event);
        return;
    }
    setFocus();
    const bool shift = event->modifiers().testFlag(Qt::ShiftModifier);
    setCurrentRecord(record, shift);
}

void LogView::contextMenuEvent(QContextMenuEvent *event)
{
    // The viewport forwards this to the scroll area the same way it forwards a mouse
    // press, so the position is in VIEWPORT coordinates — the coordinates the hit test
    // and the header's own both expect.
    //
    // Resolved through recordUnderPoint rather than recordAtViewportY because the empty
    // space BELOW the last record has to answer "nothing", and that hit test deliberately
    // clamps to the last record instead (it backs a click, which selects the nearest
    // row). A menu for a record the cursor is not on would act on a record the user
    // cannot see themselves pointing at.
    const int record = recordUnderPoint(int(event->pos().y()));
    if (record < 0) {
        QAbstractScrollArea::contextMenuEvent(event);
        return;
    }

    // Right-clicking outside the selection moves it, as every list view does: the
    // menu's copy items act on the selection, so what is under the cursor and what
    // the menu acts on must not disagree. A right-click INSIDE a multi-record
    // selection leaves it alone — that is what makes "these five records" possible.
    if (!m_selection->isSelected(m_model->index(record, 0)))
        setCurrentRecord(record);
    setFocus();

    emit recordMenuRequested(record, m_header->logicalIndexAt(int(event->pos().x())),
                             event->globalPos());
    event->accept();
}

// ---------------------------------------------------------------------------
// Tooltips for what does not fit (SPEC.md §5)
//
// The columns elide; the tooltip does not — HighlighterPane's summary column set the
// precedent. Both halves ask the same question of the same width, the section's, so the
// tooltip appears exactly where an ellipsis was painted and nowhere else: a tooltip
// repeating a value already fully on screen is noise, and "there is more here" is the
// whole of what this feature says.
//
// Everything here runs when a tooltip is ASKED FOR, never on the paint path, and holds
// nothing: the text is decoded through the model on demand, exactly as painting does
// (invariant #1).
// ---------------------------------------------------------------------------

QString LogView::truncatedCellText(const QPoint &pos) const
{
    const int record = recordUnderPoint(pos.y());
    if (record < 0)
        return {};
    const int logical = m_header->logicalIndexAt(pos.x());
    if (logical < 0 || logical >= m_model->columnCount() || m_header->isSectionHidden(logical))
        return {};

    QString text;
    if (logical == messageColumn()) {
        // A wrapped message is not elided — every character of it is on screen, in as
        // many lines as it takes — so there is nothing for a tooltip to add.
        if (estimating() || selRecordForGeometry() == record)
            return {};
        // Wrap off: each physical line of the record is drawn and clipped on its own
        // (invariant #2), so the answer is about the line under the cursor rather than
        // about the whole record.
        const qint64 line = qint64(verticalScrollBar()->value()) + pos.y() / lineHeight();
        const int within = int(line - mapLineOfRecord(record));
        const QString message = m_model->cellText(record, logical);
        const QList<QStringView> segs = QStringView(message).split(QLatin1Char('\n'));
        if (within < 0 || within >= segs.size())
            return {};
        text = segs.at(within).toString();
    } else {
        text = m_model->cellText(record, logical);
    }

    if (text.isEmpty())
        return {};
    const int w = m_header->sectionSize(logical);
    return fontMetrics().elidedText(text, Qt::ElideRight, w) == text ? QString() : text;
}

QString LogView::truncatedHeaderText(int x) const
{
    const int logical = m_header->logicalIndexAt(x);
    if (logical < 0 || logical >= m_model->columnCount() || m_header->isSectionHidden(logical))
        return {};
    const QString text =
        m_model->headerData(logical, Qt::Horizontal, Qt::DisplayRole).toString();
    if (text.isEmpty())
        return {};

    // Measured against the rect the STYLE puts the label in, not the raw section: a
    // header section spends a few pixels either side on its margin, so a caption that
    // fits the section can still be the one painted as "Priorit".
    QStyleOptionHeader opt;
    opt.initFrom(m_header);
    opt.orientation = Qt::Horizontal;
    opt.section = logical;
    opt.text = text;
    opt.rect = QRect(m_header->sectionViewportPosition(logical), 0,
                     m_header->sectionSize(logical), m_header->height());
    const QRect label = m_header->style()->subElementRect(QStyle::SE_HeaderLabel, &opt, m_header);
    return m_header->fontMetrics().elidedText(text, Qt::ElideRight, label.width()) == text
        ? QString()
        : text;
}

bool LogView::viewportEvent(QEvent *event)
{
    if (event->type() == QEvent::ToolTip) {
        const auto *help = static_cast<QHelpEvent *>(event);
        const QString text = truncatedCellText(help->pos());
        // Answered either way — hiding is as much of an answer as showing, and letting
        // an unanswered help event travel up the parents would hand the cursor whatever
        // tooltip the surrounding widget carries.
        if (text.isEmpty())
            QToolTip::hideText();
        else
            QToolTip::showText(help->globalPos(), text, viewport());
        return true;
    }
    return QAbstractScrollArea::viewportEvent(event);
}

bool LogView::eventFilter(QObject *watched, QEvent *event)
{
    // The header is a QHeaderView of its own, so its tooltips arrive on ITS viewport and
    // never reach this one. QAbstractScrollArea filters its own scrollbars through this
    // same function, and does so from ITS constructor — which runs before m_header.
    if (m_header && watched == m_header->viewport() && event->type() == QEvent::ToolTip) {
        const auto *help = static_cast<QHelpEvent *>(event);
        const QString text = truncatedHeaderText(help->pos().x());
        if (text.isEmpty())
            QToolTip::hideText();
        else
            QToolTip::showText(help->globalPos(), text, m_header);
        return true;
    }
    return QAbstractScrollArea::eventFilter(watched, event);
}

void LogView::keyPressEvent(QKeyEvent *event)
{
    const int n = recordCount();
    if (n == 0) {
        QAbstractScrollArea::keyPressEvent(event);
        return;
    }
    const bool shift = event->modifiers().testFlag(Qt::ShiftModifier);
    const int cur = m_current < 0 ? mapRecordAtLine(verticalScrollBar()->value()) : m_current;

    if (event->matches(QKeySequence::Copy)) {
        copySelectionRaw();
        return;
    }
    if (event->modifiers().testFlag(Qt::ControlModifier)
        && event->modifiers().testFlag(Qt::ShiftModifier) && event->key() == Qt::Key_C) {
        copySelectionAsColumns();
        return;
    }

    switch (event->key()) {
    case Qt::Key_Up:   setCurrentRecord(cur - 1, shift); return;
    case Qt::Key_Down: setCurrentRecord(cur + 1, shift); return;
    case Qt::Key_Home: setCurrentRecord(0, shift); return;
    case Qt::Key_End:  setCurrentRecord(n - 1, shift); return;
    case Qt::Key_PageUp: {
        const qint64 target = mapLineOfRecord(cur) - visibleLines();
        setCurrentRecord(mapRecordAtLine(qMax<qint64>(0, target)), shift);
        return;
    }
    case Qt::Key_PageDown: {
        const qint64 target = mapLineOfRecord(cur) + visibleLines();
        setCurrentRecord(mapRecordAtLine(target), shift);
        return;
    }
    default:
        QAbstractScrollArea::keyPressEvent(event);
    }
}

// ---------------------------------------------------------------------------
// Clipboard
// ---------------------------------------------------------------------------

void LogView::copySelectionRaw() const
{
    const QVector<int> rows = selectedRecordsSorted();
    if (rows.isEmpty())
        return;
    const RecordIndex &idx = m_document->index();
    LogSource *src = m_document->source();
    const Decoder &dec = m_document->decoder();
    QStringList parts;
    parts.reserve(rows.size());
    for (int viewRow : rows) {
        // Selection rows are VIEW rows; copy must read the SOURCE record's true byte
        // range (invariant #6 mapping, and the full text regardless of display cap).
        // Through the model, so a digest strip copies the record its own row names.
        const int r = m_model->sourceRow(viewRow);
        if (r < 0 || r >= idx.records.size())
            continue;
        const Record &rec = idx.records.at(r);
        // The true byte range — copy yields full text regardless of display cap (§5).
        QString text = src ? dec.decode(src->bytes(rec.offset, rec.length)) : QString();
        while (text.endsWith(QLatin1Char('\n')) || text.endsWith(QLatin1Char('\r')))
            text.chop(1);
        parts << text;
    }
    QApplication::clipboard()->setText(parts.join(QLatin1Char('\n')));
}

void LogView::copySelectionAsColumns() const
{
    const QVector<int> rows = selectedRecordsSorted();
    if (rows.isEmpty())
        return;
    const int fieldCount = m_model->columnCount();
    QVector<QVector<QString>> grid;
    grid.reserve(rows.size());
    for (int r : rows) {
        QVector<QString> cells;
        cells.reserve(fieldCount);
        // Visual order, skipping hidden columns, so the copy matches what is shown.
        for (int vi = 0; vi < fieldCount; ++vi) {
            const int logical = m_header->logicalIndex(vi);
            if (logical < 0 || m_header->isSectionHidden(logical))
                continue;
            cells << flattenCell(m_model->cellText(r, logical));
        }
        grid << cells;
    }
    QApplication::clipboard()->setText(columnsToTsv(grid));
}

// ---------------------------------------------------------------------------
// Wrap mode, column state, model signals
// ---------------------------------------------------------------------------

void LogView::setPlaceholderText(const QString &text)
{
    if (m_placeholderText == text)
        return;
    m_placeholderText = text;
    viewport()->update();
}

void LogView::setWrapMode(WrapMode mode)
{
    if (m_wrapMode == mode)
        return;
    m_wrapMode = mode;
    if (mode == WrapMode::AlwaysOn) {
        // Bind the estimator and sync it to the current width. setColumns() is a
        // no-op (and preserves the cache) when the width is unchanged since the
        // last AlwaysOn stint, so a plain Off<->AlwaysOn toggle keeps measurements.
        ensureEstimatorBound();
        m_estimated.setColumns(viewportCols());
    }
    // Switching AWAY from AlwaysOn deliberately leaves m_estimated untouched: the
    // exact path never reads it, and keeping the cache means returning to AlwaysOn
    // does not re-measure from scratch.
    recomputeGeometry();
}

QByteArray LogView::saveColumnState() const
{
    return m_header->saveState();
}

bool LogView::restoreColumnState(const QByteArray &state)
{
    const bool ok = m_header->restoreState(state);
    recomputeGeometry();
    // Session restore moves every section at once and QHeaderView reports none of it,
    // so a digest strip mirroring only sectionResized/sectionMoved would sit under the
    // restored layout with the default one until the user touched a divider.
    emit columnLayoutChanged();
    return ok;
}

void LogView::scrollToEnd()
{
    // Programmatic jump to the end (follow, or an explicit End). Guard the follow
    // bookkeeping so this move is never misread as the user scrolling away.
    m_inFollowScroll = true;
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
    m_inFollowScroll = false;
}

void LogView::followTail()
{
    setFollowingState(true);
    scrollToEnd();
    viewport()->update();
}

void LogView::setFollowingState(bool following)
{
    if (m_followButton)
        m_followButton->setVisible(!following);
    if (following != m_following) {
        m_following = following;
        if (!following && m_followButton)
            positionFollowButton();
        emit followingChanged(m_following);
    }
}

void LogView::updateFollowFromScrollPosition()
{
    if (m_inFollowScroll)
        return; // our own scroll-to-end, not a user action
    if (m_filterAnchor.active) {
        // Inside a filter re-apply. endResetModel() narrows the scroll range and Qt
        // CLAMPS the old line value into it, which arrives here as an ordinary scroll —
        // and a clamp that lands at the bottom is exactly how a view the user had
        // detached used to start following again, in silence. endFilterUpdate() carries
        // the follow state over verbatim instead.
        return;
    }
    const bool atBottom = verticalScrollBar()->value() >= verticalScrollBar()->maximum();
    // Scrolling to the bottom re-attaches; scrolling away detaches (SPEC.md §3).
    setFollowingState(atBottom);
}

void LogView::positionFollowButton()
{
    if (!m_followButton)
        return;
    m_followButton->adjustSize();
    const int m = 12;
    const QSize s = m_followButton->size();
    m_followButton->move(viewport()->width() - s.width() - m,
                         viewport()->height() - s.height() - m);
}

void LogView::handleRowsInserted()
{
    // Stay pinned to the newest record while following (SPEC.md §3). Detached, the
    // rows still index and the view holds its position for reading history.
    if (estimating())
        ensureEstimatorBound(); // extend the estimator over the newly appended blocks
    updateScrollBars();
    if (m_following)
        scrollToEnd();
    if (m_followButton && !m_following)
        positionFollowButton();
    viewport()->update();
}

void LogView::handleRowsRemoved()
{
    // The trailing (provisional) record's view row was dropped for re-evaluation
    // under an active filter (M6). Refresh geometry; a following view re-pins after
    // the matching re-insert arrives.
    if (estimating())
        ensureEstimatorBound();
    updateScrollBars();
    if (m_following)
        scrollToEnd();
    viewport()->update();
}

void LogView::handleTailChanged()
{
    // A trailing record grew taller in place (continuation lines appended) with no
    // new rows: its height changed, so the scroll range must be recomputed.
    if (estimating())
        ensureEstimatorBound();
    updateScrollBars();
    if (m_following)
        scrollToEnd();
    viewport()->update();
}

void LogView::beginFilterUpdate()
{
    m_filterAnchor = FilterAnchor{};
    if (m_role != Role::Main)
        return; // the digest strip does not scroll and holds no selection
    m_filterAnchor.active = true;
    m_filterAnchor.following = m_following;

    const int n = recordCount();
    if (n == 0)
        return;

    const qint64 top = verticalScrollBar()->value();
    const int topRow = mapRecordAtLine(top);
    if (topRow >= 0 && topRow < n) {
        m_filterAnchor.topSource = m_model->sourceRow(topRow);
        // Lines scrolled INTO that record — which is why it carries over only where the
        // record itself survives; a different record has its own first line.
        m_filterAnchor.topOffset = qMax<qint64>(0, top - mapLineOfRecord(topRow));
    }
    if (m_current >= 0 && m_current < n) {
        m_filterAnchor.currentSource = m_model->sourceRow(m_current);
        const qint64 curTop = mapLineOfRecord(m_current);
        m_filterAnchor.currentOffset = curTop - top; // may be negative: partly above
        // Intersects the viewport, not "starts inside it": a tall record scrolled to
        // its second line is still what the reader is looking at.
        m_filterAnchor.currentOnScreen = curTop + mapRecordHeightLines(m_current) > top
                                      && curTop < top + visibleLines();
    }
}

void LogView::endFilterUpdate()
{
    if (!m_filterAnchor.active)
        return; // unpaired, or a digest strip. Must be a no-op, or the follow guard latches.
    const FilterAnchor a = m_filterAnchor;

    // (a) SELECTION FIRST. In WrapMode::SelectedRecordOnly the selection IS part of the
    //     line space (selRecordForGeometry -> the geometry statics), so a target line
    //     computed before the selection is restored is computed against a different
    //     mapping than the one it is then applied to.
    const int wanted = a.currentSource >= 0 ? a.currentSource : m_stickySource;
    const int currentRow = wanted >= 0 ? m_model->viewRowOf(wanted) : -1;
    if (currentRow >= 0) {
        selectRecordSilently(currentRow);
        m_stickySource = -1;
    } else {
        // Hidden by the new filter: nothing is selected, but the ordinal is kept so a
        // widening brings the selection back (SPEC.md §6). It is never moved to a
        // neighbour — that would silently select a record the user did not pick.
        m_stickySource = wanted;
    }

    // (b) geometry over the new subset, with that selection's wrapped height in it
    recomputeGeometry();

    // (c) the target top line
    const int n = recordCount();
    qint64 target = 0;
    if (n > 0) {
        if (currentRow >= 0 && a.currentOnScreen) {
            target = mapLineOfRecord(currentRow) - a.currentOffset;
        } else if (a.topSource >= 0) {
            const int topRow = m_model->viewRowAtOrAfter(a.topSource);
            target = topRow >= n
                   ? verticalScrollBar()->maximum() // every survivor is above the old top
                   : mapLineOfRecord(topRow)
                     + (m_model->sourceRow(topRow) == a.topSource ? a.topOffset : 0);
        }
    }

    // (d) apply, still inside the bracket so the move is not read as the user scrolling
    if (a.following) {
        scrollToEnd(); // following the tail outranks the anchor (SPEC.md §3)
    } else {
        const qint64 maxLine = verticalScrollBar()->maximum();
        verticalScrollBar()->setValue(int(qBound<qint64>(0, target, maxLine)));
    }

    m_filterAnchor = FilterAnchor{};
    setFollowingState(a.following); // carried over VERBATIM, never re-derived
    viewport()->update();
}

void LogView::handleModelReset()
{
    // Any reset that is NOT a filter re-apply replaces the record space itself, so a
    // remembered source ordinal means something different there, or nothing.
    if (!m_filterAnchor.active)
        m_stickySource = -1;
    m_current = -1;
    m_anchor = -1;
    m_selWrapCache = -1;
    // The index the estimator was bound to is gone; drop the cache so it rebinds
    // (with fresh measurements) on the next AlwaysOn geometry query.
    m_estimated.clear();
    recomputeGeometry();
    if (m_followButton)
        m_followButton->setVisible(!m_following);
}

} // namespace loftail
