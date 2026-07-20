#include "LogView.h"

#include "Decoder.h"
#include "Document.h"
#include "LogFormat.h"
#include "LogModel.h"
#include "LogSource.h"
#include "RecordIndex.h"

#include <QApplication>
#include <QClipboard>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QScrollBar>

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
    case FieldRole::Message:    return 1600; // wide: wrap-off scrolls sideways (§5)
    }
    return 120;
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

LogView::LogView(const Document *document, LogModel *model, QWidget *parent)
    : QAbstractScrollArea(parent), m_document(document), m_model(model)
{
    QFont f(QStringLiteral("monospace"));
    f.setStyleHint(QFont::TypeWriter);
    setFont(f);
    setFocusPolicy(Qt::StrongFocus);
    viewport()->setFocusProxy(this);

    m_header = new QHeaderView(Qt::Horizontal, this);
    m_header->setModel(m_model);
    m_header->setSectionsMovable(true);
    m_header->setSectionsClickable(false);
    m_header->setSectionResizeMode(QHeaderView::Interactive);
    m_header->setStretchLastSection(false);
    m_header->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    // Seed sensible per-role widths; the user can resize and the layout is saved.
    for (int c = 0; c < m_document->format().fields.size(); ++c)
        m_header->resizeSection(c, defaultColumnWidth(m_document->format().fields.at(c).role));

    m_selection = new QItemSelectionModel(m_model, this);

    verticalScrollBar()->setSingleStep(1);

    connect(m_header, &QHeaderView::sectionResized, this, [this](int, int, int) { recomputeGeometry(); });
    connect(m_header, &QHeaderView::sectionMoved, this, [this](int, int, int) { viewport()->update(); });
    connect(m_model, &QAbstractItemModel::rowsInserted, this, &LogView::handleRowsInserted);
    connect(m_model, &QAbstractItemModel::modelReset, this, &LogView::handleModelReset);

    layoutHeader();
    recomputeGeometry();
}

LogView::~LogView() = default;

// ---------------------------------------------------------------------------
// Basic metrics
// ---------------------------------------------------------------------------

int LogView::lineHeight() const { return qMax(1, fontMetrics().height()); }
int LogView::visibleLines() const { return qMax(1, viewport()->height() / lineHeight()); }
int LogView::recordCount() const { return m_document->index().records.size(); }

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
    return RecordIndex::displayLines(m_document->index().records.at(sel));
}

int LogView::recordHeightLines(int r) const
{
    if (selRecordForGeometry() == r)
        return qMax(1, selWrapLines());
    return RecordIndex::displayLines(m_document->index().records.at(r));
}

// ---------------------------------------------------------------------------
// Geometry / scrollbars / header layout
// ---------------------------------------------------------------------------

void LogView::recomputeGeometry()
{
    const int sel = selRecordForGeometry();
    if (sel >= 0) {
        const int msgCol = messageColumn();
        if (msgCol >= 0) {
            const int msgX = m_header->sectionViewportPosition(msgCol);
            const int avail = qMax(50, viewport()->width() - msgX);
            m_selWrapCache = measureWrappedLines(m_model->cellText(sel, msgCol), avail);
        } else {
            m_selWrapCache = RecordIndex::displayLines(m_document->index().records.at(sel));
        }
    } else {
        m_selWrapCache = -1;
    }
    updateScrollBars();
    viewport()->update();
}

void LogView::updateScrollBars()
{
    const RecordIndex &idx = m_document->index();
    const int sel = selRecordForGeometry();
    const int selW = selWrapLines();

    const qint64 total = totalScrollLines(idx, sel, selW);
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
    const int h = m_header->sizeHint().height();
    setViewportMargins(0, h, 0, 0);
    m_header->setGeometry(viewport()->x(), viewport()->y() - h, viewport()->width(), h);
}

void LogView::resizeEvent(QResizeEvent *event)
{
    QAbstractScrollArea::resizeEvent(event);
    layoutHeader();
    recomputeGeometry();
}

void LogView::scrollContentsBy(int dx, int dy)
{
    if (dx != 0)
        m_header->setOffset(horizontalScrollBar()->value());
    Q_UNUSED(dy);
    viewport()->update();
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

void LogView::paintEvent(QPaintEvent *event)
{
    QPainter p(viewport());
    p.fillRect(event->rect(), palette().base());

    const RecordIndex &idx = m_document->index();
    const int n = idx.records.size();
    if (n == 0)
        return;

    const int lh = lineHeight();
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
        const Record &rec = idx.records.at(r);
        const int hLines = recordHeightLines(r);
        const int rowH = hLines * lh;
        const bool selected = m_selection->isSelected(m_model->index(r, 0));

        QColor band = palette().base().color();
        if (selected) {
            band = palette().highlight().color();
        } else {
            switch (rec.priorityEnum()) {
            case Priority::Warn:  band = QColor(70, 60, 20); break;
            case Priority::Error:
            case Priority::Fatal: band = QColor(80, 30, 30); break;
            default: if (r % 2) band = band.lighter(108); break;
            }
        }
        p.fillRect(QRect(0, y, vw, rowH), band);
        p.setPen(selected ? palette().highlightedText().color() : palette().text().color());

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
                    const QList<QStringView> segs = QStringView(msg).split(QLatin1Char('\n'));
                    for (int li = 0; li < segs.size() && li < hLines; ++li)
                        p.drawText(QRect(x, y + li * lh, w, lh),
                                   Qt::AlignVCenter | Qt::TextSingleLine, segs.at(li).toString());
                }
            } else {
                p.drawText(QRect(x, y, w, lh), Qt::AlignVCenter | Qt::TextSingleLine,
                           m_model->cellText(r, logical));
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
    return recordAtScrollLine(idx, selRecordForGeometry(), selWrapLines(), line);
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

void LogView::setCurrentRecord(int record, bool extendSelection)
{
    const int n = recordCount();
    if (n == 0)
        return;
    record = qBound(0, record, n - 1);
    m_current = record;
    if (extendSelection && m_anchor >= 0)
        selectRange(m_anchor, record);
    else {
        m_anchor = record;
        selectRange(record, record);
    }
    recomputeGeometry();     // selection can change the wrapped-record geometry
    ensureRecordVisible(record);
}

void LogView::ensureRecordVisible(int record)
{
    const RecordIndex &idx = m_document->index();
    const int sel = selRecordForGeometry();
    const int selW = selWrapLines();
    const qint64 recTop = scrollLineOfRecord(idx, sel, selW, record);
    const qint64 recBottom = recTop + recordHeightLines(record);
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

void LogView::keyPressEvent(QKeyEvent *event)
{
    const int n = recordCount();
    if (n == 0) {
        QAbstractScrollArea::keyPressEvent(event);
        return;
    }
    const bool shift = event->modifiers().testFlag(Qt::ShiftModifier);
    const int cur = m_current < 0 ? int(recordAtScrollLine(m_document->index(), selRecordForGeometry(),
                                                           selWrapLines(), verticalScrollBar()->value()))
                                  : m_current;

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
        const qint64 target = scrollLineOfRecord(m_document->index(), selRecordForGeometry(),
                                                 selWrapLines(), cur) - visibleLines();
        setCurrentRecord(recordAtScrollLine(m_document->index(), selRecordForGeometry(),
                                            selWrapLines(), qMax<qint64>(0, target)), shift);
        return;
    }
    case Qt::Key_PageDown: {
        const qint64 target = scrollLineOfRecord(m_document->index(), selRecordForGeometry(),
                                                 selWrapLines(), cur) + visibleLines();
        setCurrentRecord(recordAtScrollLine(m_document->index(), selRecordForGeometry(),
                                            selWrapLines(), target), shift);
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
    for (int r : rows) {
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

void LogView::setWrapMode(WrapMode mode)
{
    if (m_wrapMode == mode)
        return;
    m_wrapMode = mode;
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
    return ok;
}

void LogView::scrollToEnd()
{
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
}

void LogView::handleRowsInserted()
{
    // Keep following the tail during a scan if the view is already at the bottom
    // (SPEC.md §3: files open at their end, following). Full follow/detach is M6.
    const bool wasAtBottom = verticalScrollBar()->value() >= verticalScrollBar()->maximum();
    updateScrollBars();
    if (wasAtBottom)
        scrollToEnd();
    viewport()->update();
}

void LogView::handleModelReset()
{
    m_current = -1;
    m_anchor = -1;
    m_selWrapCache = -1;
    recomputeGeometry();
}

} // namespace loftail
