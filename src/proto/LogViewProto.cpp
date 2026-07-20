#include "LogViewProto.h"

#include "Document.h"
#include "LogFormat.h"
#include "LogModel.h"
#include "RecordIndex.h"

#include <QPainter>
#include <QPaintEvent>
#include <QScrollBar>

namespace loftail {

namespace {
// Fixed column widths for the prototype (the production view computes these).
int columnWidth(FieldRole role)
{
    switch (role) {
    case FieldRole::Date:       return 190;
    case FieldRole::Thread:     return 110;
    case FieldRole::Priority:   return 60;
    case FieldRole::Logger:     return 150;
    case FieldRole::FileName:   return 140;
    case FieldRole::LineNumber: return 60;
    case FieldRole::Method:     return 140;
    case FieldRole::Message:    return 2000;
    }
    return 120;
}
} // namespace

LogViewProto::LogViewProto(const Document *document, LogModel *model, QWidget *parent)
    : QAbstractScrollArea(parent), m_document(document), m_model(model)
{
    QFont f(QStringLiteral("monospace"));
    f.setStyleHint(QFont::TypeWriter);
    setFont(f);
    verticalScrollBar()->setSingleStep(1);
    horizontalScrollBar()->setSingleStep(20);
    updateScrollRange();
}

int LogViewProto::lineHeight() const
{
    return fontMetrics().height();
}

int LogViewProto::visibleLines() const
{
    return qMax(1, viewport()->height() / lineHeight());
}

void LogViewProto::updateScrollRange()
{
    const qint64 total = m_document->index().totalLines();
    // Scroll in LINE units (§7.1): the range is the total display line count.
    const int page = visibleLines();
    verticalScrollBar()->setRange(0, int(qMax<qint64>(0, total - page)));
    verticalScrollBar()->setPageStep(page);

    int widest = 0;
    for (const Field &field : m_document->format().fields)
        widest += columnWidth(field.role) + 12;
    horizontalScrollBar()->setRange(0, qMax(0, widest - viewport()->width()));
    horizontalScrollBar()->setPageStep(viewport()->width());
}

void LogViewProto::resizeEvent(QResizeEvent *event)
{
    QAbstractScrollArea::resizeEvent(event);
    updateScrollRange();
}

void LogViewProto::paintEvent(QPaintEvent *event)
{
    QPainter p(viewport());
    p.fillRect(event->rect(), palette().base());

    const RecordIndex &idx = m_document->index();
    const int records = idx.records.size();
    if (records == 0)
        return;

    const int lh = lineHeight();
    const qint64 topLine = verticalScrollBar()->value();
    const int xBase = -horizontalScrollBar()->value();

    // Resolve the scroll position to a record in O(log n) — the whole point of
    // the prefix-sum scheme. Only visible records are ever touched.
    int r = idx.recordAtLine(topLine);
    const qint64 recFirstLine = idx.firstLineOfRecord(r);
    int y = int((recFirstLine - topLine) * lh); // <= 0 when scrolled into a record

    const QVector<Field> &fields = m_document->format().fields;
    const int vh = viewport()->height();

    while (r < records && y < vh) {
        const Record &rec = idx.records.at(r);
        const int lines = RecordIndex::displayLines(rec);

        // Priority-tinted background band for the record's rows.
        QColor band = palette().base().color();
        switch (rec.priorityEnum()) {
        case Priority::Warn:  band = QColor(70, 60, 20); break;
        case Priority::Error:
        case Priority::Fatal: band = QColor(80, 30, 30); break;
        default: break;
        }
        if (r % 2)
            band = band.lighter(108);
        p.fillRect(QRect(0, y, viewport()->width(), lines * lh), band);

        // Non-message columns on the record's first row.
        int x = xBase;
        p.setPen(palette().text().color());
        for (int c = 0; c < fields.size(); ++c) {
            const Field &field = fields.at(c);
            const int w = columnWidth(field.role);
            if (field.role == FieldRole::Message) {
                // Message may span continuation lines: draw each physical line on
                // successive rows of this record.
                const QString msg = m_model->cellText(r, c);
                const QList<QStringView> segs = QStringView(msg).split(QLatin1Char('\n'));
                for (int li = 0; li < segs.size() && li < lines; ++li) {
                    p.drawText(QRect(x, y + li * lh, w, lh),
                               Qt::AlignVCenter | Qt::TextSingleLine,
                               segs.at(li).toString());
                }
            } else {
                p.drawText(QRect(x, y, w, lh), Qt::AlignVCenter | Qt::TextSingleLine,
                           m_model->cellText(r, c));
            }
            x += w + 12;
        }

        y += lines * lh;
        ++r;
    }
}

} // namespace loftail
