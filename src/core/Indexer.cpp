#include "Indexer.h"

#include "LogSource.h"
#include "TimestampParser.h"

#include <QRegularExpression>

namespace loftail {

Indexer::Indexer(const LogFormat &format, const Decoder &decoder, const QTimeZone &sourceZone)
    : m_format(format), m_decoder(decoder), m_sourceZone(sourceZone)
{
}

RecordIndex Indexer::index(LogSource &source, const Progress &progress, bool *cancelled) const
{
    RecordIndex idx;
    if (cancelled)
        *cancelled = false;

    const qint64 size = source.size();
    const int unit = m_decoder.unitSize();
    const bool haveFormat = !m_format.recordStartRe.pattern().isEmpty()
                            && m_format.recordStartRe.isValid();

    // A reusable QRegularExpressionMatch avoids per-line allocation churn.
    const QRegularExpression &startRe = m_format.recordStartRe;
    const TimestampParser tsParser(m_format.impliedDateFormat.qtFormat, m_sourceZone);

    // Index of the record currently open for continuations, or -1 for none, and
    // whether it is a parsed (matched) record. Continuations attach only to a
    // parsed record: a run of non-matching leading lines (or a wholly wrong
    // pattern) becomes one Unparsed record PER LINE so the plain-text fallback
    // shows real rows rather than a single giant row (§4).
    int currentRecord = -1;
    bool currentIsParsed = false;

    auto processLine = [&](qint64 fileOffset, qint64 byteLen, QStringView text) {
        bool isStart = false;
        if (haveFormat) {
            const QRegularExpressionMatch m = startRe.match(text);
            // A leading '^' anchor means a match necessarily begins at 0.
            if (m.hasMatch()) {
                isStart = true;
                Record r{};
                r.offset = fileOffset;
                r.length = static_cast<quint32>(qMin<qint64>(byteLen, std::numeric_limits<quint32>::max()));
                r.lineCount = 1;
                r.reserved = 0;

                r.priority = static_cast<quint8>(
                    m_format.prioGroup > 0 ? parsePriority(m.capturedView(m_format.prioGroup))
                                           : Priority::Unknown);

                r.loggerId = (m_format.loggerGroup > 0)
                                 ? idx.loggers.intern(m.captured(m_format.loggerGroup))
                                 : 0;
                r.threadId = (m_format.threadGroup > 0)
                                 ? idx.threads.intern(m.captured(m_format.threadGroup))
                                 : 0;

                r.timestamp = (m_format.dateGroup > 0)
                                  ? tsParser.parse(m.capturedView(m_format.dateGroup))
                                  : Record::kNoTimestamp;

                idx.records.append(r);
                currentRecord = idx.records.size() - 1;
                currentIsParsed = true;
            }
        }

        if (!isStart) {
            if (currentRecord >= 0 && currentIsParsed) {
                // Continuation of a parsed record: extend its byte span and lines.
                Record &r = idx.records[currentRecord];
                const qint64 newLen = static_cast<qint64>(r.length) + byteLen;
                r.length = static_cast<quint32>(qMin<qint64>(newLen, std::numeric_limits<quint32>::max()));
                if (r.lineCount < Record::kMaxLineCount)
                    ++r.lineCount;
            } else {
                // Leading unparsed line (or a wrong pattern): one plain-text record
                // per line so the view is never empty (§4).
                Record r{};
                r.offset = fileOffset;
                r.length = static_cast<quint32>(qMin<qint64>(byteLen, std::numeric_limits<quint32>::max()));
                r.lineCount = 1;
                r.loggerId = 0;
                r.threadId = 0;
                r.timestamp = Record::kNoTimestamp;
                r.priority = static_cast<quint8>(Priority::Unknown);
                r.reserved = 0;
                idx.records.append(r);
                currentRecord = idx.records.size() - 1;
                currentIsParsed = false;
            }
        }
    };

    qint64 pos = m_decoder.bomLength();
    while (pos < size) {
        // Read a window at `pos`, growing it (rare) only when the very first line
        // does not terminate within it — a single line longer than the chunk. No
        // complete line is processed until it terminates, so growing and
        // re-scanning double-processes nothing. `pos` only moves forward (#9).
        qint64 want = qMin(kChunkBytes, size - pos);
        QByteArrayView buf;
        bool atEof = false;
        qsizetype scan = 0;

        for (;;) {
            buf = source.bytes(pos, want);
            if (buf.isEmpty())
                break;
            atEof = (pos + buf.size() >= size);

            scan = 0;
            while (scan < buf.size()) {
                bool hadNl = false;
                const qsizetype end = m_decoder.lineEnd(buf, scan, &hadNl);
                if (!hadNl && !atEof) {
                    // Incomplete trailing line: stop and re-read it whole next round.
                    break;
                }
                const qsizetype contentLen = (end - scan) - (hadNl ? unit : 0);
                const QByteArrayView content = buf.sliced(scan, qMax<qsizetype>(0, contentLen));
                const QString text = m_decoder.decodeLine(content);
                processLine(pos + scan, end - scan, text);
                scan = end;
            }

            // At least one line completed, or we are at EOF, or we have already
            // asked for the whole remaining extent: done reading this window.
            if (scan > 0 || atEof || want >= size - pos)
                break;
            want = qMin(size - pos, want * 2); // giant line: grow and retry
        }

        if (buf.isEmpty() || scan == 0)
            break; // nothing left to make progress on

        pos += scan;

        if (progress) {
            if (!progress(pos, size)) {
                if (cancelled)
                    *cancelled = true;
                idx.rebuildBlockSums();
                return idx;
            }
        }
    }

    idx.rebuildBlockSums();
    return idx;
}

} // namespace loftail
