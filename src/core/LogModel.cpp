#include "LogModel.h"

#include "Decoder.h"
#include "Document.h"
#include "Highlight.h"
#include "LogSource.h"
#include "Palette.h"
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
    // Rows are the VISIBLE records: identity over the index when no filter is
    // active, the filtered subset otherwise (M4, invariant #6).
    return m_document->filtered().recordCount();
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
    if (column < 0 || column >= format.fields.size())
        return QString();

    // `row` is a VIEW row: map it to the source record ordinal (identity when no
    // filter is active). The intern tables and byte reads all key off the source
    // index; only the row addressing goes through the filtered mapping (M4).
    const int srcRow = m_document->filtered().sourceRow(row);
    if (srcRow < 0 || srcRow >= idx.records.size())
        return QString();

    const Record &rec = idx.records.at(srcRow);
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

void LogModel::beginFilterReset()
{
    beginResetModel();
}

void LogModel::endFilterReset()
{
    endResetModel();
}

void LogModel::beginAppendRows(int count)
{
    const int first = rowCount();
    beginInsertRows(QModelIndex(), first, first + count - 1);
}

void LogModel::endAppendRows()
{
    endInsertRows();
}

QColor LogModel::highlightColor(int row, bool background) const
{
    if (!m_document)
        return QColor();

    const HighlighterSet &set = m_document->highlighters();
    if (set.rules.isEmpty())
        return QColor();

    // Map the view row to the source record (identity when no filter is active),
    // then run the ordered rules first-match-wins on integers (invariant #4).
    const RecordIndex &idx = m_document->index();
    const int srcRow = m_document->filtered().sourceRow(row);
    if (srcRow < 0 || srcRow >= idx.records.size())
        return QColor();

    const int ruleIndex = set.match(idx.records.at(srcRow));
    if (ruleIndex < 0)
        return QColor(); // no rule matched — the theme's normal color

    const HighlightRule &rule = set.rules.at(ruleIndex);
    const int slot = background ? rule.background : rule.foreground;
    // kDefault (or a corrupt slot) resolves to an invalid color, i.e. "leave this
    // role at the theme default" (SPEC.md §7). The view treats an invalid return as
    // no override.
    return HighlightPalette::color(slot, m_darkTheme);
}

QVariant LogModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return {};

    switch (role) {
    case Qt::DisplayRole:
    case Qt::ToolTipRole:
        return cellText(index.row(), index.column());
    case Qt::BackgroundRole: {
        // Highlighting (SPEC.md §7): the matched rule's background palette color, or
        // an empty variant for "theme default" so the view keeps its normal fill.
        const QColor c = highlightColor(index.row(), /*background=*/true);
        return c.isValid() ? QVariant(c) : QVariant();
    }
    case Qt::ForegroundRole: {
        const QColor c = highlightColor(index.row(), /*background=*/false);
        return c.isValid() ? QVariant(c) : QVariant();
    }
    default:
        return {};
    }
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
