// loftail — a desktop viewer for log4cplus logs.
// Copyright (C) 2026 Valentyn Pavliuchenko
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Indexer.h"

#include "LogSource.h"
#include "TimestampParser.h"

#include <QRegularExpression>

#include <optional>
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
    const QRegularExpression &fullRe = m_format.recordRe;
    const TimestampParser tsParser(m_format.impliedDateFormat, m_sourceZone);

    // Every group number in LogFormat is numbered against recordRe, which carries one
    // capturing group per field in pattern order; recordStartRe is assembled from the
    // pieces BEFORE %m only, so it holds strictly fewer groups whenever anything follows
    // the message. Qt answers an out-of-range group with a null string rather than an
    // error, so under an ordinary pattern like `%d{...} [%t] %-5p %m (%c)%n` the record's
    // subsystem was interned as "" on every row — blank column, empty Filters list, no
    // integer axis (invariant #4) — and the same silently for a %p, %t or %d written past
    // the message. Where that can happen, the first line is matched a SECOND time against
    // recordRe and the trailing groups are read out of that match instead.
    //
    // Three things about this are easy to undo. It is gated on the pattern actually
    // needing it, because this is the index path and the extra match is a whole regex per
    // record — a pattern ending %m%n (every seeded one, and every autodetected one) pays
    // nothing. A group that recordStartRe DOES capture keeps being read from the start
    // match, so nothing about an existing pattern moves. And recordStartRe stays the sole
    // record-boundary decider (invariant #2): recordRe is ^…$-anchored, so under such a
    // pattern a MULTI-LINE record's first line — whose trailing fields sit on its last
    // line — does not match it, and those records keep the blank trailing fields they have
    // always had rather than the boundary regex being widened to reach them.
    const int startCaptures = haveFormat ? startRe.captureCount() : 0;
    const bool needsTailMatch = haveFormat && fullRe.isValid() && !fullRe.pattern().isEmpty()
                                && (m_format.dateGroup > startCaptures
                                    || m_format.prioGroup > startCaptures
                                    || m_format.loggerGroup > startCaptures
                                    || m_format.threadGroup > startCaptures);

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

                // The second match, for the fields recordStartRe does not reach. Absent
                // for a single-line pattern that needs none; present but without a match
                // for a multi-line record whose trailing fields are not on this line.
                //
                // It is a std::optional and not a plain QRegularExpressionMatch, which is
                // what makes the gate above mean what it claims — that a pattern ending
                // %m%n pays NOTHING. That class's default constructor is out-of-line in
                // libQt6Core and allocates TWICE (a match private, which default-
                // constructs a QRegularExpression of its own), measured at ~30 ns per
                // construction; declared unconditionally it ran per record on every log,
                // including the ones the gate exists to spare. Two cautions against
                // reopening this on either side. It does NOT show up end to end — against
                // a 110 MB / 1M-record log the two spellings are indistinguishable inside
                // bench_index's run-to-run noise, because 30 ns sits against ~1.3 us of
                // per-record regex work and the malloc/free pair is same-size-class and
                // tcache-served — so do not restore the plain declaration on the grounds
                // that the benchmark cannot tell, and do not quote a percentage of
                // indexing time for it either. And hoisting it to forwardScan() scope
                // removes the allocation just as well and must still NOT be done: a match
                // outliving the iteration holds a stale QStringView into a decoded block
                // that has since been replaced, which is a worse trap than the cost.
                std::optional<QRegularExpressionMatch> tail;
                if (needsTailMatch) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
                    tail.emplace(fullRe.matchView(text));
#else
                    tail.emplace(fullRe.match(text));
#endif
                }
                // Which of the two matches owns a group: recordRe's numbering is the
                // canonical one, so anything past recordStartRe's capture count can only
                // be read from the full match.
                const auto matchFor = [&](int group) -> const QRegularExpressionMatch & {
                    return (group > startCaptures && tail && tail->hasMatch()) ? *tail : m;
                };

                r.priority = static_cast<quint8>(
                    m_format.prioGroup > 0
                        ? parsePriority(matchFor(m_format.prioGroup).capturedView(m_format.prioGroup))
                        : Priority::Unknown);

                r.loggerId = (m_format.loggerGroup > 0)
                                 ? loggers.intern(matchFor(m_format.loggerGroup)
                                                      .captured(m_format.loggerGroup))
                                 : 0;
                r.threadId = (m_format.threadGroup > 0)
                                 ? threads.intern(matchFor(m_format.threadGroup)
                                                      .captured(m_format.threadGroup))
                                 : 0;

                r.timestamp = (m_format.dateGroup > 0)
                                  ? tsParser.parse(matchFor(m_format.dateGroup)
                                                       .capturedView(m_format.dateGroup))
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
