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

#include <QTemporaryFile>

#include "Document.h"
#include "LogFormat.h"
#include "LogModel.h"
#include "Priority.h"

using namespace loftail;

// LogModel coverage: lazy data() returns the correct field for each column,
// pulling eager fields off the Record and decoding the rest from the mapped bytes
// (invariant #1). Runs through Document::open against a real temp file, so the
// LogSource + Decoder + Indexer path is exercised end to end without a
// QApplication (GUILESS: a QCoreApplication only).
class TestLogModel : public QObject
{
    Q_OBJECT

private:
    // A fresh temp file per call, kept alive by the caller so Document can map it.
    static bool writeLog(QTemporaryFile &file, const QByteArray &bytes)
    {
        if (!file.open())
            return false;
        file.write(bytes);
        file.flush();
        return true;
    }

private slots:
    void columnsAndRows();
    void lazyFieldsAreCorrect();
    void multiLineMessageJoined();
    void unparsedRecordShowsRawLine();

    // Timestamp display modes (SPEC.md §4). The Date column is the one cell whose
    // text depends on more than its own Record, so each mode gets its own case.
    void timeDisplayAsWrittenMatchesFile();
    void timeDisplayUtcAndLocal();
    void timeDisplayEpochSecondsWithAndWithoutMillis();
    void timeDisplayRunSecondsWithoutRunPattern();
    void timeDisplayRunSecondsRebasesPerRun();
    void timeDisplayRunSecondsSkipsUnparsedRunStart();
    void timeDisplayLeavesUnparsedCellEmpty();
    void timeDisplayRunSecondsSurvivesReparse();

    // The gap column (SPEC.md §4). The one mode whose cell is a function of ANOTHER
    // ROW, so each of the four things that can stand above a row gets a case.
    void timeDisplaySincePreviousMeasuresTheRowAbove();
    void timeDisplaySincePreviousCountsVisibleRecordsAndNotOrdinals();
    void timeDisplaySincePreviousLeavesAGapItCannotStateEmpty();
    void timeDisplaySincePreviousWithoutMillisAndBackwards();

    // M15 — the seam LogView dims through. Core has no palette, so the model's whole
    // contribution is this one question about a view row.
    void rowIsContextTracksTheFilteredSubset();
};

namespace {
// The pattern every timestamp-display case uses: ISO date WITH log4cplus %q, so
// DateFormat::hasMillis is set and the seconds modes render s.mmm.
constexpr auto kMsPattern = "%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n";
// The same without %q — the case that must render BARE integer seconds.
constexpr auto kNoMsPattern = "%d{%Y-%m-%d %H:%M:%S} [%t] %-5p %c - %m%n";
} // namespace

void TestLogModel::columnsAndRows()
{
    QTemporaryFile file;
    QVERIFY(writeLog(file,
        "2026-07-21 14:32:05,123 [main] INFO  net.socket - Connection opened\n"
        "2026-07-21 14:32:06,000 [worker] WARN  db.pool - Pool exhausted\n"));

    Document doc;
    QVERIFY2(doc.open(file.fileName(), QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"),
                      Encoding::Utf8, QTimeZone::utc()),
             qPrintable(doc.lastError()));

    LogModel model(&doc);
    QCOMPARE(model.rowCount(), 2);
    // Fields: Time, Thread, Priority, Subsystem, Message.
    QCOMPARE(model.columnCount(), 5);
    QCOMPARE(model.headerData(0, Qt::Horizontal, Qt::DisplayRole).toString(), QStringLiteral("Time"));
    QCOMPARE(model.headerData(4, Qt::Horizontal, Qt::DisplayRole).toString(), QStringLiteral("Message"));
}

void TestLogModel::lazyFieldsAreCorrect()
{
    QTemporaryFile file;
    QVERIFY(writeLog(file,
        "2026-07-21 14:32:05,123 [main] INFO  net.socket - Connection opened\n"));

    Document doc;
    QVERIFY2(doc.open(file.fileName(), QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"),
                      Encoding::Utf8, QTimeZone::utc()),
             qPrintable(doc.lastError()));

    LogModel model(&doc);
    auto cell = [&](int c) { return model.data(model.index(0, c)).toString(); };

    QCOMPARE(cell(0), QStringLiteral("2026-07-21 14:32:05,123")); // Time, display zone UTC
    QCOMPARE(cell(1), QStringLiteral("main"));                    // Thread
    QCOMPARE(cell(2), QStringLiteral("INFO"));                    // Priority
    QCOMPARE(cell(3), QStringLiteral("net.socket"));              // Subsystem
    QCOMPARE(cell(4), QStringLiteral("Connection opened"));       // Message (lazy decode)
}

void TestLogModel::multiLineMessageJoined()
{
    QTemporaryFile file;
    QVERIFY(writeLog(file,
        "2026-07-21 14:32:05,123 [main] ERROR app.core - Exception:\n"
        "    at foo()\n"
        "    at bar()\n"));

    Document doc;
    QVERIFY2(doc.open(file.fileName(), QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"),
                      Encoding::Utf8, QTimeZone::utc()),
             qPrintable(doc.lastError()));

    LogModel model(&doc);
    QCOMPARE(model.rowCount(), 1);
    const QString msg = model.data(model.index(0, 4)).toString();
    QVERIFY(msg.startsWith(QStringLiteral("Exception:")));
    QVERIFY(msg.contains(QStringLiteral("at foo()")));
    QVERIFY(msg.contains(QStringLiteral("at bar()")));
}

// The plain-text fallback (SPEC.md §4): when the pattern does not match a line,
// the indexer emits one Unparsed record per line, and the Message column must
// carry the whole raw line. Otherwise the view is a table of blank rows — the
// record count (and the scrollbar) says there is content while nothing renders.
void TestLogModel::unparsedRecordShowsRawLine()
{
    QTemporaryFile file;
    QVERIFY(writeLog(file,
        "03/12/26 11:50:47 DEBUG Vms::App [] - log4cplus config:\n"
        "log4cplus.threadPoolSize=1\n"));

    Document doc;
    // A pattern that matches neither line (ISO date vs. the file's %m/%d/%y).
    QVERIFY2(doc.open(file.fileName(), QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"),
                      Encoding::Utf8, QTimeZone::utc()),
             qPrintable(doc.lastError()));

    LogModel model(&doc);
    QCOMPARE(model.rowCount(), 2);
    auto cell = [&](int r, int c) { return model.data(model.index(r, c)).toString(); };

    QCOMPARE(cell(0, 4), QStringLiteral("03/12/26 11:50:47 DEBUG Vms::App [] - log4cplus config:"));
    QCOMPARE(cell(1, 4), QStringLiteral("log4cplus.threadPoolSize=1"));
    // No field parsed out of an unmatched line: the other columns stay empty.
    QCOMPARE(cell(0, 0), QString());
    QCOMPARE(cell(0, 1), QString());
    QCOMPARE(cell(0, 3), QString());
}

// --- Timestamp display modes (SPEC.md §4) ----------------------------------

void TestLogModel::timeDisplayAsWrittenMatchesFile()
{
    QTemporaryFile file;
    QVERIFY(writeLog(file,
        "2026-07-21 14:32:05,123 [main] INFO  app - a\n"));

    Document doc;
    QVERIFY2(doc.open(file.fileName(), QString::fromLatin1(kMsPattern),
                      Encoding::Utf8, QTimeZone::utc()),
             qPrintable(doc.lastError()));
    QCOMPARE(int(doc.timeDisplay()), int(TimeDisplay::AsWritten)); // the default

    // The whole point of the default: the cell reads exactly as the file does, so
    // cross-checking against raw log text in an editor needs no mental arithmetic.
    LogModel model(&doc);
    QCOMPARE(model.data(model.index(0, 0)).toString(),
             QStringLiteral("2026-07-21 14:32:05,123"));
}

void TestLogModel::timeDisplayUtcAndLocal()
{
    QTemporaryFile file;
    QVERIFY(writeLog(file,
        "2026-07-21 14:32:05,123 [main] INFO  app - a\n"));

    Document doc;
    // Source zone UTC+2: the text names 14:32:05 local-to-the-file, i.e. 12:32:05Z.
    QVERIFY2(doc.open(file.fileName(), QString::fromLatin1(kMsPattern),
                      Encoding::Utf8, QTimeZone(2 * 3600)),
             qPrintable(doc.lastError()));

    LogModel model(&doc);
    auto cell = [&]() { return model.data(model.index(0, 0)).toString(); };
    const qint64 stored = doc.index().records.at(0).timestamp;

    doc.setTimeDisplay(TimeDisplay::AsWritten);
    QCOMPARE(cell(), QStringLiteral("2026-07-21 14:32:05,123"));

    doc.setTimeDisplay(TimeDisplay::Utc);
    QCOMPARE(cell(), QStringLiteral("2026-07-21 12:32:05,123"));

    doc.setTimeDisplay(TimeDisplay::LocalTime);
    QCOMPARE(cell(),
             QDateTime::fromMSecsSinceEpoch(stored, QTimeZone::systemTimeZone())
                 .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss,zzz")));

    // Storage never moved through any of that (invariant #10).
    QCOMPARE(doc.index().records.at(0).timestamp, stored);
}

void TestLogModel::timeDisplayEpochSecondsWithAndWithoutMillis()
{
    const QDateTime t(QDate(2026, 7, 21), QTime(14, 32, 5, 123), QTimeZone::utc());
    const qint64 epochMs = t.toMSecsSinceEpoch();

    {
        QTemporaryFile file;
        QVERIFY(writeLog(file, "2026-07-21 14:32:05,123 [main] INFO  app - a\n"));
        Document doc;
        QVERIFY2(doc.open(file.fileName(), QString::fromLatin1(kMsPattern),
                          Encoding::Utf8, QTimeZone::utc()),
                 qPrintable(doc.lastError()));
        doc.setTimeDisplay(TimeDisplay::EpochSeconds);

        LogModel model(&doc);
        QCOMPARE(model.data(model.index(0, 0)).toString(),
                 QStringLiteral("%1.123").arg(epochMs / 1000));
    }

    {
        // No %q in the pattern: the file carries no sub-second precision, so the
        // cell must NOT read ".000" — that would invent precision the log lacks.
        QTemporaryFile file;
        QVERIFY(writeLog(file, "2026-07-21 14:32:05 [main] INFO  app - a\n"));
        Document doc;
        QVERIFY2(doc.open(file.fileName(), QString::fromLatin1(kNoMsPattern),
                          Encoding::Utf8, QTimeZone::utc()),
                 qPrintable(doc.lastError()));
        QVERIFY(!doc.format().impliedDateFormat.hasMillis);
        doc.setTimeDisplay(TimeDisplay::EpochSeconds);

        LogModel model(&doc);
        QCOMPARE(model.data(model.index(0, 0)).toString(),
                 QString::number(epochMs / 1000));
    }
}

void TestLogModel::timeDisplayRunSecondsWithoutRunPattern()
{
    QTemporaryFile file;
    QVERIFY(writeLog(file,
        "2026-07-21 14:32:05,000 [main] INFO  app - a\n"
        "2026-07-21 14:32:06,250 [main] INFO  app - b\n"
        "2026-07-21 14:32:07,000 [main] INFO  app - c\n"));

    Document doc;
    QVERIFY2(doc.open(file.fileName(), QString::fromLatin1(kMsPattern),
                      Encoding::Utf8, QTimeZone::utc()),
             qPrintable(doc.lastError()));
    QVERIFY(doc.runs().isEmpty()); // no run-start pattern configured
    doc.setTimeDisplay(TimeDisplay::RunSeconds);

    // With no run splitting the whole file counts as one run, so the mode still
    // works and reads as elapsed time from the log's first record.
    LogModel model(&doc);
    auto cell = [&](int r) { return model.data(model.index(r, 0)).toString(); };
    QCOMPARE(cell(0), QStringLiteral("0.000"));
    QCOMPARE(cell(1), QStringLiteral("1.250"));
    QCOMPARE(cell(2), QStringLiteral("2.000"));
}

void TestLogModel::timeDisplayRunSecondsRebasesPerRun()
{
    QTemporaryFile file;
    QVERIFY(writeLog(file,
        "2026-07-21 14:32:05,000 [main] INFO  app - starting up\n"
        "2026-07-21 14:32:06,500 [main] INFO  app - work\n"
        "2026-07-21 15:00:00,000 [main] INFO  app - starting up\n"
        "2026-07-21 15:00:02,250 [main] INFO  app - work\n"));

    Document doc;
    QVERIFY2(doc.open(file.fileName(), QString::fromLatin1(kMsPattern),
                      Encoding::Utf8, QTimeZone::utc()),
             qPrintable(doc.lastError()));
    doc.setRunStart(QStringLiteral("starting up"), /*regex=*/false, Qt::CaseSensitive);
    doc.selectRun(-1); // show all runs, so every record is a row
    QCOMPARE(doc.runs().size(), 2);
    doc.setTimeDisplay(TimeDisplay::RunSeconds);

    // The counter restarts at each run boundary — not a whole-file delta, which for
    // the third record would have read 1675.000.
    LogModel model(&doc);
    auto cell = [&](int r) { return model.data(model.index(r, 0)).toString(); };
    QCOMPARE(cell(0), QStringLiteral("0.000"));
    QCOMPARE(cell(1), QStringLiteral("1.500"));
    QCOMPARE(cell(2), QStringLiteral("0.000"));
    QCOMPARE(cell(3), QStringLiteral("2.250"));
}

void TestLogModel::timeDisplayRunSecondsSkipsUnparsedRunStart()
{
    // The run marker's own record carries no timestamp. Note it must still START a
    // record to be seen by run detection at all (invariant #2 makes a non-matching
    // line a continuation of the record above it, invisible to detectRuns) — so the
    // reachable shape is a line that is structurally well-formed but whose date is
    // not a real one: month 13, a corrupt or clock-glitched line.
    QTemporaryFile file;
    QVERIFY(writeLog(file,
        "2026-07-21 14:32:05,000 [main] INFO  app - tail of the previous run\n"
        "2026-13-45 00:00:00,000 [main] INFO  app - Application starting\n"
        "2026-07-21 15:00:00,000 [main] INFO  app - first real line\n"
        "2026-07-21 15:00:01,750 [main] INFO  app - second\n"));

    Document doc;
    QVERIFY2(doc.open(file.fileName(), QString::fromLatin1(kMsPattern),
                      Encoding::Utf8, QTimeZone::utc()),
             qPrintable(doc.lastError()));
    doc.setRunStart(QStringLiteral("Application starting"), /*regex=*/false, Qt::CaseSensitive);
    doc.selectRun(-1);
    QCOMPARE(doc.runs().size(), 2); // a preamble plus the marked run
    QCOMPARE(doc.runs().at(1).startTimestamp, qint64(Record::kNoTimestamp));
    doc.setTimeDisplay(TimeDisplay::RunSeconds);

    LogModel model(&doc);
    auto cell = [&](int r) { return model.data(model.index(r, 0)).toString(); };
    QCOMPARE(cell(0), QStringLiteral("0.000")); // the preamble bases on itself
    QCOMPARE(cell(1), QString());               // the marker has no timestamp at all
    // The baseline falls FORWARD to the run's first timestamped record, so the first
    // logged line reads 0.000 rather than a delta against the previous run.
    QCOMPARE(cell(2), QStringLiteral("0.000"));
    QCOMPARE(cell(3), QStringLiteral("1.750"));
}

void TestLogModel::timeDisplayLeavesUnparsedCellEmpty()
{
    // A second RECORD with no timestamp: structurally well-formed, but month 13 is
    // not a date. (A wholly non-matching line would be folded into record 0 as a
    // continuation instead — invariant #2.)
    QTemporaryFile file;
    QVERIFY(writeLog(file,
        "2026-07-21 14:32:05,000 [main] INFO  app - parsed\n"
        "2026-13-45 00:00:00,000 [main] INFO  app - not a real date\n"));

    Document doc;
    QVERIFY2(doc.open(file.fileName(), QString::fromLatin1(kMsPattern),
                      Encoding::Utf8, QTimeZone::utc()),
             qPrintable(doc.lastError()));
    QCOMPARE(doc.index().records.at(1).timestamp, qint64(Record::kNoTimestamp));

    // The kNoTimestamp guard comes before the mode switch, so no mode can turn a
    // missing timestamp into a number (0, the epoch, or otherwise).
    LogModel model(&doc);
    for (TimeDisplay mode : {TimeDisplay::AsWritten, TimeDisplay::LocalTime,
                             TimeDisplay::Utc, TimeDisplay::EpochSeconds,
                             TimeDisplay::RunSeconds}) {
        doc.setTimeDisplay(mode);
        QCOMPARE(model.data(model.index(1, 0)).toString(), QString());
    }
}

void TestLogModel::timeDisplayRunSecondsSurvivesReparse()
{
    QTemporaryFile file;
    QVERIFY(writeLog(file,
        "2026-07-21 14:32:05,000 [main] INFO  app - a\n"
        "2026-07-21 14:32:06,250 [main] INFO  app - b\n"));

    Document doc;
    QVERIFY2(doc.open(file.fileName(), QString::fromLatin1(kMsPattern),
                      Encoding::Utf8, QTimeZone::utc()),
             qPrintable(doc.lastError()));
    doc.setTimeDisplay(TimeDisplay::RunSeconds);

    LogModel model(&doc);
    auto cell = [&](int r) { return model.data(model.index(r, 0)).toString(); };
    QCOMPARE(cell(1), QStringLiteral("1.250"));

    // A source-zone change shifts every timestamp uniformly, so the DELTAS are
    // unchanged — but only if the memoised baselines were invalidated. Without that
    // the second row would be measured against a baseline from the old zone.
    doc.reparseTimestamps(QTimeZone(2 * 3600));
    QCOMPARE(cell(0), QStringLiteral("0.000"));
    QCOMPARE(cell(1), QStringLiteral("1.250"));
}

void TestLogModel::rowIsContextTracksTheFilteredSubset()
{
    QTemporaryFile file;
    QVERIFY(writeLog(file,
        "2026-07-21 10:00:00,000 [main] INFO  svc - a\n"
        "2026-07-21 10:00:01,000 [main] INFO  svc - b\n"
        "2026-07-21 10:00:02,000 [main] ERROR svc - c\n"
        "2026-07-21 10:00:03,000 [main] INFO  svc - d\n"));

    Document doc;
    QVERIFY2(doc.open(file.fileName(),
                      QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"),
                      Encoding::Utf8, QTimeZone::utc()),
             qPrintable(doc.lastError()));
    LogModel model(&doc);

    // No filter: the identity view, where nothing can be context.
    QCOMPARE(model.rowCount(), 4);
    for (int row = 0; row < 4; ++row)
        QVERIFY(!model.rowIsContext(row));

    // A MESSAGE filter, because that is the only axis context widens (SPEC.md §6):
    // only record "c" matches, and one record of lead-up and follow-up comes with it.
    doc.filters().text.enabled = true;
    doc.filters().text.matcher.set(QStringLiteral("c"), /*regex=*/false, Qt::CaseSensitive);
    doc.setContext(1, 1);
    doc.applyFilters();

    // Rows 1, 2, 3 of the file are now view rows 0, 1, 2: context, match, context.
    QCOMPARE(model.rowCount(), 3);
    QVERIFY(model.rowIsContext(0));
    QVERIFY(!model.rowIsContext(1));
    QVERIFY(model.rowIsContext(2));
    QVERIFY(!model.rowIsContext(3)); // out of range, not a crash
}

// The gap is to the record on the row above, and the first row has no gap to state
// (SPEC.md §4). Millisecond precision comes from the file's own %d, exactly as the
// other two numeric modes take it.
void TestLogModel::timeDisplaySincePreviousMeasuresTheRowAbove()
{
    QTemporaryFile file;
    QVERIFY(writeLog(file,
        "2026-07-21 14:32:05,000 [main] INFO  app - a\n"
        "2026-07-21 14:32:06,250 [main] INFO  app - b\n"
        "2026-07-21 14:32:07,000 [main] INFO  app - c\n"));

    Document doc;
    QVERIFY2(doc.open(file.fileName(), QString::fromLatin1(kMsPattern),
                      Encoding::Utf8, QTimeZone::utc()),
             qPrintable(doc.lastError()));
    doc.setTimeDisplay(TimeDisplay::SincePrevious);

    LogModel model(&doc);
    auto cell = [&](int r) { return model.data(model.index(r, 0)).toString(); };
    // Nothing above row 0: blank, NOT "0.000", which would claim an interval of zero
    // where there is no interval at all.
    QCOMPARE(cell(0), QString());
    QCOMPARE(cell(1), QStringLiteral("1.250"));
    QCOMPARE(cell(2), QStringLiteral("0.750"));

    // Storage never moved (invariant #10), and the mode is a repaint: switching away
    // gives the file's own digits straight back.
    doc.setTimeDisplay(TimeDisplay::AsWritten);
    QCOMPARE(cell(1), QStringLiteral("2026-07-21 14:32:06,250"));
}

// "Previous" means the previous VISIBLE record — the row above in this table — which
// is the whole reason the mode composes with filters: filter to one subsystem and the
// column reads that subsystem's cadence rather than the file's.
void TestLogModel::timeDisplaySincePreviousCountsVisibleRecordsAndNotOrdinals()
{
    QTemporaryFile file;
    QVERIFY(writeLog(file,
        "2026-07-21 14:32:00,000 [main] INFO  net - a\n"
        "2026-07-21 14:32:01,000 [main] INFO  db  - noise\n"
        "2026-07-21 14:32:02,000 [main] INFO  db  - noise\n"
        "2026-07-21 14:32:10,000 [main] INFO  net - b\n"));

    Document doc;
    QVERIFY2(doc.open(file.fileName(), QString::fromLatin1(kMsPattern),
                      Encoding::Utf8, QTimeZone::utc()),
             qPrintable(doc.lastError()));
    doc.setTimeDisplay(TimeDisplay::SincePrevious);

    LogModel model(&doc);
    auto cell = [&](int r) { return model.data(model.index(r, 0)).toString(); };
    QCOMPARE(cell(3), QStringLiteral("8.000")); // unfiltered: the gap to the db record

    doc.filters().loggerEnabled = true;
    doc.filters().loggerIds = {doc.index().loggers.idOf(QStringLiteral("net"))};
    doc.applyFilters();
    QCOMPARE(model.rowCount(), 2);

    // The two `net` records are 10 s apart; the hidden records between them are not
    // what the column is being asked about.
    QCOMPARE(cell(0), QString());
    QCOMPARE(cell(1), QStringLiteral("10.000"));
}

// Two rows have no interval to state, and both must come out EMPTY rather than zero:
// the first visible row, and one whose predecessor's own date did not parse. There is
// deliberately no walk further back — that is unbounded on the paint path.
void TestLogModel::timeDisplaySincePreviousLeavesAGapItCannotStateEmpty()
{
    QTemporaryFile file;
    QVERIFY(writeLog(file,
        "not a log line at all\n"
        "2026-07-21 14:32:05,000 [main] INFO  app - a\n"
        "2026-07-21 14:32:06,000 [main] INFO  app - b\n"));

    Document doc;
    QVERIFY2(doc.open(file.fileName(), QString::fromLatin1(kMsPattern),
                      Encoding::Utf8, QTimeZone::utc()),
             qPrintable(doc.lastError()));
    QCOMPARE(doc.index().records.size(), 3);
    QCOMPARE(doc.index().records.at(0).timestamp, Record::kNoTimestamp);
    doc.setTimeDisplay(TimeDisplay::SincePrevious);

    LogModel model(&doc);
    auto cell = [&](int r) { return model.data(model.index(r, 0)).toString(); };
    QCOMPARE(cell(0), QString()); // the unparsed line itself has no time of its own
    QCOMPARE(cell(1), QString()); // nothing above it that a gap could be measured from
    QCOMPARE(cell(2), QStringLiteral("1.000"));
}

// The precision rule is the file's, not the mode's — a log written without sub-second
// precision must not grow a fabricated ".000" — and the gap is SIGNED, because
// log4cplus appends in the order threads reach the appender rather than the order they
// stamped, so a later row can carry an earlier instant.
void TestLogModel::timeDisplaySincePreviousWithoutMillisAndBackwards()
{
    QTemporaryFile file;
    QVERIFY(writeLog(file,
        "2026-07-21 14:32:05 [main] INFO  app - a\n"
        "2026-07-21 14:32:09 [main] INFO  app - b\n"
        "2026-07-21 14:32:07 [work] INFO  app - late\n"));

    Document doc;
    QVERIFY2(doc.open(file.fileName(), QString::fromLatin1(kNoMsPattern),
                      Encoding::Utf8, QTimeZone::utc()),
             qPrintable(doc.lastError()));
    QVERIFY(!doc.format().impliedDateFormat.hasMillis);
    doc.setTimeDisplay(TimeDisplay::SincePrevious);

    LogModel model(&doc);
    auto cell = [&](int r) { return model.data(model.index(r, 0)).toString(); };
    QCOMPARE(cell(1), QStringLiteral("4"));
    QCOMPARE(cell(2), QStringLiteral("-2"));
}

QTEST_GUILESS_MAIN(TestLogModel)
#include "tst_logmodel.moc"
