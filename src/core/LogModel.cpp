#include "LogModel.h"

#include "Decoder.h"
#include "Document.h"
#include "LogSource.h"
#include "Priority.h"

#include <QDateTime>
#include <QRegularExpression>

namespace loftail {

LogModel::LogModel(const Document *document, QObject *parent)
    : QAbstractTableModel(parent), m_document(document)
{
}

int LogModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid() || !m_document)
        return 0;
    return m_document->index().records.size();
}

int LogModel::columnCount(const QModelIndex &parent) const
{
    if (parent.isValid() || !m_document)
        return 0;
    return m_document->format().fields.size();
}

QString LogModel::cellText(int row, int column) const
{
    if (!m_document)
        return QString();

    const RecordIndex &idx = m_document->index();
    const LogFormat &format = m_document->format();
    if (row < 0 || row >= idx.records.size() || column < 0 || column >= format.fields.size())
        return QString();

    const Record &rec = idx.records.at(row);
    const Field &field = format.fields.at(column);

    switch (field.role) {
    case FieldRole::Date:
        if (rec.timestamp == Record::kNoTimestamp)
            return QString();
        // The single "out" zone conversion (§5.1): interpret the UTC ms in the
        // display zone when formatting.
        return QDateTime::fromMSecsSinceEpoch(rec.timestamp, m_document->displayZone())
            .toString(format.impliedDateFormat.qtFormat);
    case FieldRole::Priority:
        return priorityName(rec.priorityEnum());
    case FieldRole::Logger:
        return idx.loggers.name(rec.loggerId);
    case FieldRole::Thread:
        return idx.threads.name(rec.threadId);
    case FieldRole::Message:
    case FieldRole::FileName:
    case FieldRole::LineNumber:
    case FieldRole::Method:
        break; // decoded lazily below
    }

    // Lazy decode from the mapped bytes for fields not carried on the Record.
    LogSource *src = m_document->source();
    if (!src)
        return QString();
    const Decoder &dec = m_document->decoder();

    QByteArrayView bytes = src->bytes(rec.offset, rec.length);
    if (bytes.isEmpty())
        return QString();

    bool hadNl = false;
    const qsizetype firstEnd = dec.lineEnd(bytes, 0, &hadNl);
    const qsizetype firstContentLen = firstEnd - (hadNl ? dec.unitSize() : 0);
    const QString firstLine = dec.decodeLine(bytes.sliced(0, qMax<qsizetype>(0, firstContentLen)));

    const QRegularExpressionMatch m = format.recordRe.match(firstLine);
    QString value = (field.group > 0 && m.hasMatch()) ? m.captured(field.group) : QString();

    // The message spans continuation lines (invariant #2): append the remaining
    // physical lines, if any, so multi-line records show their full text.
    if (field.role == FieldRole::Message && firstEnd < bytes.size()) {
        QString rest = dec.decode(bytes.sliced(firstEnd));
        while (rest.endsWith(QLatin1Char('\n')) || rest.endsWith(QLatin1Char('\r')))
            rest.chop(1);
        if (!rest.isEmpty())
            value += QLatin1Char('\n') + rest;
    }
    return value;
}

QVariant LogModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || (role != Qt::DisplayRole && role != Qt::ToolTipRole))
        return {};
    return cellText(index.row(), index.column());
}

QVariant LogModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole || !m_document)
        return {};
    if (orientation == Qt::Horizontal) {
        const LogFormat &format = m_document->format();
        if (section >= 0 && section < format.fields.size())
            return format.fields.at(section).name;
        return {};
    }
    return section + 1;
}

} // namespace loftail
