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

#include <QtTest>

#include "Decoder.h"
#include "Indexer.h"
#include "LogFormat.h"
#include "PatternCompiler.h"
#include "Record.h"
#include "RecordIndex.h"

#include "MemoryLogSource.h"

using namespace loftail;

// Indexer + Record + RecordIndex coverage: the invariants that make the spine
// correct — 32-byte Record (#1), record-vs-line rule (#2), interning (#4),
// single forward pass producing UTC timestamps (#9, #10), and the two-level
// prefix sums (#6).
class TestIndexer : public QObject
{
    Q_OBJECT

private:
    static constexpr auto kPattern = "%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n";

    static LogFormat fmt()
    {
        auto r = PatternCompiler::compile(QString::fromUtf8(kPattern));
        Q_ASSERT(r);
        return r.value();
    }

    static RecordIndex indexOf(const QByteArray &bytes, const QTimeZone &zone = QTimeZone::utc())
    {
        MemoryLogSource src(bytes);
        const LogFormat f = fmt();
        Decoder dec = Decoder::detect(src.bytesCopy(0, qMin<qint64>(src.size(), 64 * 1024)), Encoding::Utf8);
        Indexer indexer(f, dec, zone);
        return indexer.index(src);
    }

    // The same pass under a pattern the case supplies, for the shapes kPattern
    // cannot express — a field written after %m, above all.
    static RecordIndex indexWith(const char *pattern, const QByteArray &bytes,
                                 const QTimeZone &zone = QTimeZone::utc())
    {
        MemoryLogSource src(bytes);
        auto r = PatternCompiler::compile(QString::fromUtf8(pattern));
        Q_ASSERT(r);
        Decoder dec = Decoder::detect(src.bytesCopy(0, qMin<qint64>(src.size(), 64 * 1024)), Encoding::Utf8);
        Indexer indexer(r.value(), dec, zone);
        return indexer.index(src);
    }

private slots:
    void recordIsThirtyTwoBytes();
    void severityOrder();
    void singleLineRecords();
    void multiLineContinuation();
    void unparsedLeadingLines();
    void internsLoggersAndThreads();
    void fieldsWrittenAfterTheMessageAreStillParsed();
    void timestampIsUtcEpochMs();
    void timestampRespectsSourceZone();
    void crlfHandled();
    void lineLongerThanChunk();
    void prefixSumRoundTrip();
    void prefixSumLineCap();
};

void TestIndexer::recordIsThirtyTwoBytes()
{
    QCOMPARE(sizeof(Record), size_t(32)); // invariant #1
}

void TestIndexer::severityOrder()
{
    // Priority is declared in severity order and Unknown sorts below Trace (§7.2).
    QVERIFY(static_cast<quint8>(Priority::Unknown) < static_cast<quint8>(Priority::Trace));
    QVERIFY(static_cast<quint8>(Priority::Trace) < static_cast<quint8>(Priority::Debug));
    QVERIFY(static_cast<quint8>(Priority::Info) < static_cast<quint8>(Priority::Warn));
    QVERIFY(static_cast<quint8>(Priority::Error) < static_cast<quint8>(Priority::Fatal));
}

void TestIndexer::singleLineRecords()
{
    const QByteArray log =
        "2026-07-21 14:32:05,123 [main] INFO  net.socket - Connection opened\n"
        "2026-07-21 14:32:06,000 [worker] WARN  db.pool - Pool exhausted\n";
    const RecordIndex idx = indexOf(log);

    QCOMPARE(idx.records.size(), 2);
    QCOMPARE(idx.records.at(0).priorityEnum(), Priority::Info);
    QCOMPARE(idx.records.at(1).priorityEnum(), Priority::Warn);
    QCOMPARE(idx.records.at(0).lineCount, quint16(1));
    QCOMPARE(idx.records.at(0).offset, qint64(0));
}

void TestIndexer::multiLineContinuation()
{
    // A record whose message has embedded newlines (invariant #2): the following
    // non-matching lines are continuations, not new records.
    const QByteArray log =
        "2026-07-21 14:32:05,123 [main] ERROR app.core - Exception:\n"
        "    at foo()\n"
        "    at bar()\n"
        "2026-07-21 14:32:06,000 [main] INFO  app.core - recovered\n";
    const RecordIndex idx = indexOf(log);

    QCOMPARE(idx.records.size(), 2);
    QCOMPARE(idx.records.at(0).lineCount, quint16(3));
    QCOMPARE(idx.records.at(1).lineCount, quint16(1));
    // The first record's byte span covers its continuation lines.
    const qint64 firstStart = idx.records.at(0).offset;
    const qint64 secondStart = idx.records.at(1).offset;
    QCOMPARE(firstStart + qint64(idx.records.at(0).length), secondStart);
}

void TestIndexer::unparsedLeadingLines()
{
    // Lines before any record start are retained as Unparsed records (§4) so the
    // view is never empty.
    const QByteArray log =
        "### banner line, not a record ###\n"
        "another preamble line\n"
        "2026-07-21 14:32:05,123 [main] INFO  app - real record\n";
    const RecordIndex idx = indexOf(log);

    QCOMPARE(idx.records.size(), 3);
    QCOMPARE(idx.records.at(0).priorityEnum(), Priority::Unknown);
    QCOMPARE(idx.records.at(1).priorityEnum(), Priority::Unknown);
    QCOMPARE(idx.records.at(2).priorityEnum(), Priority::Info);
}

void TestIndexer::internsLoggersAndThreads()
{
    const QByteArray log =
        "2026-07-21 14:32:05,123 [main] INFO  net.socket - a\n"
        "2026-07-21 14:32:06,000 [worker] WARN  db.pool - b\n"
        "2026-07-21 14:32:07,000 [main] INFO  net.socket - c\n";
    const RecordIndex idx = indexOf(log);

    // Two distinct loggers, two distinct threads; repeats reuse the same id.
    QCOMPARE(idx.records.at(0).loggerId, idx.records.at(2).loggerId);
    QVERIFY(idx.records.at(0).loggerId != idx.records.at(1).loggerId);
    QCOMPARE(idx.records.at(0).threadId, idx.records.at(2).threadId);
    QVERIFY(idx.records.at(0).threadId != idx.records.at(1).threadId);

    QCOMPARE(idx.loggers.name(idx.records.at(0).loggerId), QStringLiteral("net.socket"));
    QCOMPARE(idx.threads.name(idx.records.at(1).threadId), QStringLiteral("worker"));
}

void TestIndexer::fieldsWrittenAfterTheMessageAreStillParsed()
{
    // `%d{...} [%t] %-5p %m (%c)%n` is an ordinary log4cplus pattern: the subsystem is
    // written AFTER the message. recordStartRe stops at the message, so it captures
    // three groups while %c is numbered 4 against recordRe — and Qt answers an
    // out-of-range group with a null string rather than an error, so every record used
    // to intern the empty logger name, silently, leaving the Subsystem column blank and
    // the Filters pane's list empty on a log that plainly carries subsystems.
    static constexpr auto kTailPattern = "%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %m (%c)%n";
    const QByteArray log =
        "2026-07-21 14:32:05,123 [main] INFO  Connection opened (net.socket)\n"
        "2026-07-21 14:32:06,000 [worker] WARN  Pool exhausted (db.pool)\n";
    const RecordIndex idx = indexWith(kTailPattern, log);

    QCOMPARE(idx.records.size(), 2);
    QCOMPARE(idx.loggers.name(idx.records.at(0).loggerId), QStringLiteral("net.socket"));
    QCOMPARE(idx.loggers.name(idx.records.at(1).loggerId), QStringLiteral("db.pool"));
    QVERIFY(idx.records.at(0).loggerId != idx.records.at(1).loggerId);

    // The fields recordStartRe does reach are unaffected — they are still read from the
    // start match, so nothing about an existing pattern moves.
    QCOMPARE(idx.records.at(0).priorityEnum(), Priority::Info);
    QCOMPARE(idx.records.at(1).priorityEnum(), Priority::Warn);
    QCOMPARE(idx.threads.name(idx.records.at(0).threadId), QStringLiteral("main"));
    QVERIFY(idx.records.at(0).timestamp != Record::kNoTimestamp);

    // The known remaining gap, pinned here so it is a decision rather than a surprise:
    // recordStartRe stays the SOLE record-boundary decider (invariant #2), and recordRe
    // is ^...$-anchored, so a MULTI-LINE record's first line — whose trailing `(%c)` sits
    // on its last line — does not match it. Such a record keeps the blank subsystem it
    // has always had; widening the boundary regex to reach it is what the invariant
    // forbids.
    const QByteArray multi =
        "2026-07-21 14:32:05,123 [main] ERROR Exception:\n"
        "    at foo() (app.core)\n";
    const RecordIndex midx = indexWith(kTailPattern, multi);
    QCOMPARE(midx.records.size(), 1);
    QCOMPARE(midx.records.at(0).lineCount, quint16(2));
    QCOMPARE(midx.records.at(0).priorityEnum(), Priority::Error);
    QCOMPARE(midx.loggers.name(midx.records.at(0).loggerId), QString());
}

void TestIndexer::timestampIsUtcEpochMs()
{
    // %d is local per the pattern, but we index with an explicit UTC source zone,
    // so "2026-07-21 00:00:00,000" is exactly that instant in UTC epoch ms.
    const QByteArray log =
        "2026-07-21 00:00:00,000 [main] INFO  app - x\n";
    const RecordIndex idx = indexOf(log, QTimeZone::utc());

    const QDateTime expected(QDate(2026, 7, 21), QTime(0, 0, 0, 0), QTimeZone::utc());
    QCOMPARE(idx.records.at(0).timestamp, expected.toMSecsSinceEpoch());
    // Millisecond field (%q) must be captured.
    const QByteArray log2 =
        "2026-07-21 00:00:00,250 [main] INFO  app - x\n";
    const RecordIndex idx2 = indexOf(log2, QTimeZone::utc());
    QCOMPARE(idx2.records.at(0).timestamp - idx.records.at(0).timestamp, qint64(250));
}

void TestIndexer::timestampRespectsSourceZone()
{
    // The same wall-clock text in a +02:00 source zone is 2 hours earlier in UTC.
    const QByteArray log =
        "2026-07-21 12:00:00,000 [main] INFO  app - x\n";
    const qint64 utc = indexOf(log, QTimeZone::utc()).records.at(0).timestamp;
    const qint64 plus2 = indexOf(log, QTimeZone(2 * 3600)).records.at(0).timestamp;
    QCOMPARE(utc - plus2, qint64(2 * 3600 * 1000));
}

void TestIndexer::crlfHandled()
{
    const QByteArray log =
        "2026-07-21 14:32:05,123 [main] INFO  app - a\r\n"
        "2026-07-21 14:32:06,000 [main] INFO  app - b\r\n";
    const RecordIndex idx = indexOf(log);
    QCOMPARE(idx.records.size(), 2);
    QCOMPARE(idx.records.at(0).priorityEnum(), Priority::Info);
}

void TestIndexer::lineLongerThanChunk()
{
    // A single record whose message is far longer than the read chunk must still
    // be indexed as one record (the giant-line grow path, single forward pass).
    const QByteArray huge(Indexer::kChunkBytes + 4096, 'x');
    QByteArray log = "2026-07-21 14:32:05,123 [main] INFO  app - ";
    log += huge;
    log += '\n';
    log += "2026-07-21 14:32:06,000 [main] INFO  app - next\n";

    const RecordIndex idx = indexOf(log);
    QCOMPARE(idx.records.size(), 2);
    QCOMPARE(idx.records.at(0).lineCount, quint16(1));
    QCOMPARE(idx.records.at(1).priorityEnum(), Priority::Info);
}

void TestIndexer::prefixSumRoundTrip()
{
    // Build a synthetic index spanning several blocks with known line counts and
    // assert line->record and record->line invert each other (invariant #6).
    RecordIndex idx;
    const int n = RecordIndex::kBlockSize * 3 + 17; // spans 4 blocks
    idx.records.reserve(n);
    qint64 expectedLines = 0;
    for (int i = 0; i < n; ++i) {
        Record r{};
        r.lineCount = quint16((i % 5) + 1); // 1..5 lines, all under the display cap
        idx.records.append(r);
        expectedLines += r.lineCount;
    }
    idx.rebuildBlockSums();

    QCOMPARE(idx.totalLines(), expectedLines);

    // record -> firstLine is monotonic and matches a running sum.
    qint64 acc = 0;
    for (int i = 0; i < n; ++i) {
        QCOMPARE(idx.firstLineOfRecord(i), acc);
        // The first line of record i must map back to record i.
        QCOMPARE(idx.recordAtLine(acc), i);
        // A line in the middle of the record also maps back to i.
        const qint64 mid = acc + idx.records.at(i).lineCount - 1;
        QCOMPARE(idx.recordAtLine(mid), i);
        acc += idx.records.at(i).lineCount;
    }
    // Out-of-range line clamps to the last record.
    QCOMPARE(idx.recordAtLine(expectedLines + 100), n - 1);
}

void TestIndexer::prefixSumLineCap()
{
    // Display height caps at 100 lines per record (§7.1): the prefix sums use the
    // clamped value even though the true lineCount is larger.
    RecordIndex idx;
    Record tall{};
    tall.lineCount = 5000;
    idx.records.append(tall);
    Record small{};
    small.lineCount = 3;
    idx.records.append(small);
    idx.rebuildBlockSums();

    QCOMPARE(idx.totalLines(), qint64(RecordIndex::kDisplayLineCap) + 3);
    QCOMPARE(idx.firstLineOfRecord(1), qint64(RecordIndex::kDisplayLineCap));
    // Any display line within the tall record maps to record 0.
    QCOMPARE(idx.recordAtLine(RecordIndex::kDisplayLineCap - 1), 0);
    QCOMPARE(idx.recordAtLine(RecordIndex::kDisplayLineCap), 1);
}

QTEST_APPLESS_MAIN(TestIndexer)
#include "tst_indexer.moc"
