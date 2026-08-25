#include "Indexer.h"

#include "LogSource.h"
#include "TimestampParser.h"

#include <QRegularExpression>

#include <utility>

namespace loftail {

// By const reference and NOT by value + std::move, which is what modernize-pass-by-value
// asks for: QTimeZone gained a move constructor after Qt 6.4, the version floor, so on the
// reference toolchain the move silently binds to the copy constructor and the by-value
// parameter is a second copy for nothing. clang-tidy 18 says so; 21 against Qt 6.10 does not.
Indexer::Indexer(const LogFormat &format, const Decoder &decoder, const QTimeZone &sourceZone)
    : m_format(format), m_decoder(decoder), m_sourceZone(sourceZone)
{
}

bool Indexer::forwardScan(LogSource &source, qint64 startPos, QVector<Record> &records,
                          InternTable &loggers, InternTable &threads,
                          const std::function<bool(qint64)> &onChunk) const
{
    const qint64 size = source.size();
    const int unit = m_decoder.unitSize();
    const bool haveFormat = !m_format.recordStartRe.pattern().isEmpty()
                            && m_format.recordStartRe.isValid();

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
            // matchView() rather than match(QStringView), which Qt 6.8 deprecates for
            // taking a view it does not own — the same non-owning semantics this call
            // has always had, since `text` is a view into the decoded block and the
            // match is consumed before it goes anywhere. matchView() arrived in Qt 6.5,
            // above the 6.4 floor (ARCHITECTURE.md §1), so the old spelling stays for it.
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
            const QRegularExpressionMatch m = startRe.matchView(text);
#else
            const QRegularExpressionMatch m = startRe.match(text);
#endif
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
                                 ? loggers.intern(m.captured(m_format.loggerGroup))
                                 : 0;
                r.threadId = (m_format.threadGroup > 0)
                                 ? threads.intern(m.captured(m_format.threadGroup))
                                 : 0;

                r.timestamp = (m_format.dateGroup > 0)
                                  ? tsParser.parse(m.capturedView(m_format.dateGroup))
                                  : Record::kNoTimestamp;

                records.append(r);
                currentRecord = int(records.size()) - 1;
                currentIsParsed = true;
            }
        }

        if (!isStart) {
            if (currentRecord >= 0 && currentIsParsed) {
                // Continuation of a parsed record: extend its byte span and lines.
                Record &r = records[currentRecord];
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
                records.append(r);
                currentRecord = int(records.size()) - 1;
                currentIsParsed = false;
            }
        }
    };

    qint64 pos = startPos;
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

        if (onChunk && !onChunk(pos))
            return false; // cancelled
    }
    return true;
}

RecordIndex Indexer::index(LogSource &source, const Progress &progress, bool *cancelled,
                           const Batch &batch) const
{
    RecordIndex idx;
    if (cancelled)
        *cancelled = false;

    const qint64 size = source.size();
    bool wasCancelled = false;

    // Bridge the shared forward pass to the streaming progress/batch seam: after
    // each chunk, report progress (which may cancel) and flush a non-final batch —
    // the last record stays open for continuations still to arrive (§4).
    const bool completed = forwardScan(
        source, m_decoder.bomLength(), idx.records, idx.loggers, idx.threads,
        [&](qint64 pos) -> bool {
            if (progress && !progress(pos, size)) {
                wasCancelled = true;
                return false;
            }
            if (batch)
                batch(idx, /*final=*/false);
            return true;
        });
    Q_UNUSED(completed);

    if (wasCancelled && cancelled)
        *cancelled = true;

    if (batch)
        batch(idx, /*final=*/true); // flush the remainder (completion or cancel)
    idx.rebuildBlockSums();
    return idx;
}

QVector<Record> Indexer::scanAppendedTail(LogSource &source, qint64 startOffset,
                                          InternTable &loggers, InternTable &threads) const
{
    // A self-contained forward pass over the appended tail: currentRecord seeds to
    // -1, so the line at `startOffset` opens a fresh record (a record boundary by
    // contract). No progress/batch streaming — the appended delta is small and the
    // caller applies it synchronously (invariant #9: still forward-only).
    QVector<Record> tail;
    forwardScan(source, startOffset, tail, loggers, threads, {});
    return tail;
}

} // namespace loftail
