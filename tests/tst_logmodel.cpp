#include <QtTest>

#include <QTemporaryFile>

#include "Document.h"
#include "LogFormat.h"
#include "LogModel.h"

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

QTEST_GUILESS_MAIN(TestLogModel)
#include "tst_logmodel.moc"
