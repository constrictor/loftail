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

namespace {

// Seconds rendering for the two numeric display modes (SPEC.md §4). Integer
// seconds when the file's own %d carries no milliseconds, s.mmm when it does —
// see DateFormat::hasMillis.
//
// Signed throughout: RunSeconds goes negative for a record that precedes its run's
// baseline (an out-of-order or back-dated line), and epoch seconds are negative for
// a pre-1970 log.
QString formatSeconds(qint64 ms, bool withMillis)
{
    if (!withMillis) {
        // Floor, not C++ truncate-toward-zero, so the column stays monotonic across
        // zero: -0.5 s and +0.5 s must not both render as "0".
        qint64 s = ms / 1000;
        if (ms % 1000 != 0 && ms < 0)
            --s;
        return QString::number(s);
    }
    const bool neg = ms < 0;
    // Negate through unsigned so INT64_MIN cannot overflow. kNoTimestamp is guarded
    // by the caller, but a corrupt delta must not be UB on the paint path.
    const quint64 mag = neg ? (quint64(-(ms + 1)) + 1) : quint64(ms);
    return (neg ? QStringLiteral("-") : QString())
         + QString::number(mag / 1000) + QLatin1Char('.')
         + QStringLiteral("%1").arg(mag % 1000, 3, 10, QLatin1Char('0'));
}

} // namespace

LogModel::LogModel(const Document *document, QObject *parent)
    : QAbstractTableModel(parent), m_document(document)
{
}

void LogModel::setViewIndex(const FilteredIndex *index)
{
    if (m_view == index)
        return;
    beginResetModel();
    m_view = index;
    endResetModel();
}

const FilteredIndex &LogModel::view() const
{
    return m_view ? *m_view : m_document->filtered();
}

const RecordIndex &LogModel::viewGeometry() const
{
    return view().geometry();
}

int LogModel::sourceRow(int viewRow) const
{
    return m_document ? view().sourceRow(viewRow) : -1;
}

int LogModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid() || !m_document)
        return 0;
    // Rows are the VISIBLE records: identity over the index when no filter is
    // active, the filtered subset otherwise (M4, invariant #6).
    return view().recordCount();
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
    const int srcRow = view().sourceRow(row);
    if (srcRow < 0 || srcRow >= idx.records.size())
        return QString();

    const Record &rec = idx.records.at(srcRow);
    const Field &field = format.fields.at(column);

    switch (field.role) {
    case FieldRole::Date: {
        if (rec.timestamp == Record::kNoTimestamp)
            return QString(); // an unparsed line, or a format with no %d
        const TimeDisplay mode = m_document->timeDisplay();
        if (mode == TimeDisplay::EpochSeconds || mode == TimeDisplay::RunSeconds) {
            // Zone-free (§5.1): Record::timestamp is already UTC epoch ms
            // (invariant #10), so seconds need no "out" conversion at all.
            qint64 base = 0;
            if (mode == TimeDisplay::RunSeconds) {
                base = m_document->runBaseTimestamp(srcRow);
                if (base == Record::kNoTimestamp)
                    base = rec.timestamp; // unreachable: this record IS timestamped,
                                          // so its run has at least one such record
            }
            return formatSeconds(rec.timestamp - base, format.impliedDateFormat.hasMillis);
        }
        // The single "out" zone conversion (§5.1): interpret the UTC ms in the
        // display zone, rendered in the file's OWN date format.
        return QDateTime::fromMSecsSinceEpoch(rec.timestamp, m_document->displayZone())
            .toString(format.impliedDateFormat.qtFormat);
    }
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
    case FieldRole::Location:
    case FieldRole::ThreadName:
    case FieldRole::ProcessId:
    case FieldRole::Hostname:
    case FieldRole::Elapsed:
    case FieldRole::Ndc:
    case FieldRole::Mdc:
    case FieldRole::EnvVar:
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
    QString value;
    if (m.hasMatch()) {
        value = field.group > 0 ? m.captured(field.group) : QString();
    } else if (field.role == FieldRole::Message) {
        // Unparsed record (the plain-text fallback, SPEC.md §4): the pattern
        // matched nothing, so no field was extracted and the raw line IS the
        // message. Without this the row renders entirely blank, and a wholly
        // non-matching pattern yields a table of empty rows whose scrollbar
        // still says the file has content.
        value = firstLine;
    }

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

void LogModel::beginRemoveTail(int count)
{
    const int last = rowCount() - 1;
    beginRemoveRows(QModelIndex(), last - count + 1, last);
}

void LogModel::endRemoveTail()
{
    endRemoveRows();
}

void LogModel::notifyRowChanged(int row)
{
    const int cols = columnCount();
    if (row < 0 || cols <= 0)
        return;
    emit dataChanged(index(row, 0), index(row, cols - 1));
}

int LogModel::matchedRule(int row) const
{
    if (!m_document)
        return -1;

    // Early-out before touching the index: with no enabled rule carrying THIS model's
    // action there is nothing to evaluate and, in particular, no message decode to risk.
    const HighlighterSet &set = m_document->highlighters();
    if (!set.anyEnabled(m_action))
        return -1;

    // Map the view row to the source record (identity when no filter is active),
    // then run the ordered rules first-match-wins (invariant #4).
    const RecordIndex &idx = m_document->index();
    const int srcRow = view().sourceRow(row);
    if (srcRow < 0 || srcRow >= idx.records.size())
        return -1;

    const Record &rec = idx.records.at(srcRow);
    // The decode is passed as a callable, not a value: match() invokes it only once a
    // rule's integer axes have passed AND that rule has an active text axis, and
    // memoizes the result across rules. Document::messageText is the filter path's
    // decode (capture-free recordStartRe, whole-line fallback for unparsed records) —
    // deliberately not cellText(), which runs the full-capture recordRe per column.
    return set.match(rec, m_action, [this, &rec] { return m_document->messageText(rec); });
}

void LogModel::rowColors(int row, QColor &background, QColor &foreground) const
{
    background = QColor();
    foreground = QColor();

    const int ruleIndex = matchedRule(row);
    if (ruleIndex < 0)
        return; // no rule matched — the theme's normal colors

    const HighlightRule &rule = m_document->highlighters().rules.at(ruleIndex);
    // kDefault (or a corrupt slot) resolves to an invalid color, i.e. "leave this
    // role at the theme default" (SPEC.md §7). The view treats an invalid color as no
    // override. First-match-wins is per RULE, not per role: a rule that sets only the
    // background does not let a lower rule supply the foreground.
    background = HighlightPalette::color(rule.background, m_darkTheme);
    foreground = HighlightPalette::color(rule.foreground, m_darkTheme);
}

bool LogModel::rowIsContext(int row) const
{
    return m_document && view().isContext(row);
}

QColor LogModel::highlightColor(int row, bool background) const
{
    QColor bg, fg;
    rowColors(row, bg, fg);
    return background ? bg : fg;
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
