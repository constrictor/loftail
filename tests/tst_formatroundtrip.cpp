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
#include "Priority.h"
#include "Record.h"
#include "RecordIndex.h"
#include "TimestampParser.h"

#include "MemoryLogSource.h"

using namespace loftail;

// The round trip: render a record the way a log4cplus layout would, index it back
// through the pattern that describes it, and assert every field comes back — the
// timestamp as an instant, and the timestamp as it is DISPLAYED.
//
// This is the property half of tests/fuzz. A fuzzer asks "does any input break
// it"; this asks "does the answer mean what it says", which no crash-oriented
// target can see: a parser that reads a field correctly and a renderer that shows
// it as something else are each internally consistent.
//
// bugs.md 37 is exactly that shape and is what this exists for. `%d{}`, `%s` and
// the skip-only date codes reach translateDateFormat()'s no-Qt-spelling fallback,
// which assigned kDefaultDateFormat — the STRFTIME spelling `%Y-%m-%d %H:%M:%S` —
// where kDefaultQtDateFormat, `yyyy-MM-dd HH:mm:ss`, was meant. QDateTime reads
// `%m` as minutes and `%M` as the month, so the Time column rendered
// `%Y-%15-%27 %10:%8:%S` with parsing correct throughout. Nobody would think to
// write a case about %m against %M; a row that renders the compiled qtFormat and
// compares it with the text the record was built from catches it without anyone
// having to.
//
// DETERMINISTIC BY CONSTRUCTION: an enumerated table, never a seeded RNG, so a
// red build names one row and can be re-run. The one thing that is not fixed is
// the clock, and it is confined to the rows marked yearFromClock — a format
// carrying a month and a day but no year takes its year from the clock by design
// (SPEC.md §4), so those rows assert the fields the format actually carries.
class TestFormatRoundTrip : public QObject
{
    Q_OBJECT

private:
    // What every row's record says after its timestamp. Fixed, so that a row is
    // about its date format and nothing else.
    static constexpr auto kThread = "worker-3";
    static constexpr auto kLogger = "svc.core";
    static constexpr auto kMessage = "connection established";

    struct Indexed
    {
        RecordIndex index;
        LogFormat   format;
    };

    static Indexed indexOne(const QString &pattern, const QString &line)
    {
        auto compiled = PatternCompiler::compile(pattern);
        if (!compiled) {
            // Reported by the caller's QVERIFY; returning an empty index keeps the
            // failure on the row rather than aborting the whole binary.
            return {};
        }
        const QByteArray bytes = (line + QLatin1Char('\n')).toUtf8();
        MemoryLogSource source(bytes);
        const Decoder dec = Decoder::detect(source.bytesCopy(0, bytes.size()), Encoding::Utf8);
        const Indexer indexer(compiled.value(), dec, QTimeZone::utc());
        return {indexer.index(source), compiled.value()};
    }

private slots:
    void aRenderedRecordComesBackFieldForField_data();
    void aRenderedRecordComesBackFieldForField();

    void theCompiledDisplayFormatRendersTheTextItWasCompiledFrom_data();
    void theCompiledDisplayFormatRendersTheTextItWasCompiledFrom();

    void everyDateCodeThatCompilesReadsBackTheTextItAgreedToMatch_data();
    void everyDateCodeThatCompilesReadsBackTheTextItAgreedToMatch();
};

// --- The whole record ------------------------------------------------------

void TestFormatRoundTrip::aRenderedRecordComesBackFieldForField_data()
{
    QTest::addColumn<QString>("dateFormat"); // the %d{...} body, or empty for a bare %d
    QTest::addColumn<QString>("dateText");   // what a layout with that format writes
    QTest::addColumn<qint64>("expectedMs");  // -1: the format carries no absolute instant

    const qint64 base = QDateTime(QDate(2026, 8, 27), QTime(10, 15, 1), QTimeZone::utc())
                            .toMSecsSinceEpoch();

    QTest::newRow("bare %d, the default format")
        << QString() << QStringLiteral("2026-08-27 10:15:01") << base;
    // NOT `%d{}`: empty braces are an empty date FORMAT, not the default one, so
    // the field matches the empty string and a line carrying a date does not match
    // the record at all. Only the braceless `%d` takes kDefaultDateFormat.
    QTest::newRow("iso")
        << QStringLiteral("%Y-%m-%d %H:%M:%S") << QStringLiteral("2026-08-27 10:15:01") << base;
    QTest::newRow("iso with milliseconds")
        << QStringLiteral("%Y-%m-%d %H:%M:%S,%q") << QStringLiteral("2026-08-27 10:15:01,123")
        << base + 123;
    QTest::newRow("%F %T, both composites")
        << QStringLiteral("%F %T") << QStringLiteral("2026-08-27 10:15:01") << base;
    QTest::newRow("%T alone is %H:%M:%S")
        << QStringLiteral("%T") << QStringLiteral("10:15:01") << qint64(-1);
    QTest::newRow("day first")
        << QStringLiteral("%d/%m/%Y %H:%M:%S") << QStringLiteral("27/08/2026 10:15:01") << base;
    QTest::newRow("month first, two-digit year")
        << QStringLiteral("%m/%d/%y %H:%M:%S") << QStringLiteral("08/27/26 10:15:01") << base;
    QTest::newRow("twelve-hour with am/pm")
        << QStringLiteral("%Y-%m-%d %I:%M:%S %p") << QStringLiteral("2026-08-27 10:15:01 AM")
        << base;
    QTest::newRow("space-padded day, the syslog shape")
        << QStringLiteral("%b %e %H:%M:%S") << QStringLiteral("Aug 27 10:15:01") << qint64(-1);
    QTest::newRow("space-padded day, single digit")
        << QStringLiteral("%b %e %H:%M:%S") << QStringLiteral("Aug  5 10:15:01") << qint64(-1);
    QTest::newRow("epoch seconds ignore the zone")
        << QStringLiteral("%s") << QString::number(base / 1000) << base;
    QTest::newRow("rfc 3339 with a zone offset")
        << QStringLiteral("%Y-%m-%dT%H:%M:%S.%Q%z")
        << QStringLiteral("2026-08-27T12:15:01.123456+0200") << base + 123;
    QTest::newRow("rfc 3339, rsyslog's separator-less %Q")
        << QStringLiteral("%Y-%m-%dT%H:%M:%S.%Q%z")
        << QStringLiteral("2026-08-27T12:15:01.123456+0200") << base + 123;
    QTest::newRow("a skipped day-of-year does not move the instant")
        << QStringLiteral("%Y-%m-%d %H:%M:%S %j") << QStringLiteral("2026-08-27 10:15:01 239")
        << base;
    QTest::newRow("a skipped weekday name does not move the instant")
        << QStringLiteral("%a %Y-%m-%d %H:%M:%S") << QStringLiteral("Thu 2026-08-27 10:15:01")
        << base;
    QTest::newRow("a literal percent inside the date")
        << QStringLiteral("%Y-%m-%d%%%H:%M:%S") << QStringLiteral("2026-08-27%10:15:01") << base;
}

void TestFormatRoundTrip::aRenderedRecordComesBackFieldForField()
{
    QFETCH(QString, dateFormat);
    QFETCH(QString, dateText);
    QFETCH(qint64, expectedMs);

    // %d rather than %D throughout: %d implies UTC (§5.1), which is what makes an
    // expected instant a constant rather than a function of where this runs.
    const QString spec =
        dateFormat.isNull() ? QStringLiteral("%d")
                            : QStringLiteral("%d{") + dateFormat + QStringLiteral("}");
    const QString pattern = spec + QStringLiteral(" [%t] %-5p %c - %m%n");
    const QString line = dateText + QStringLiteral(" [") + QLatin1String(kThread)
                         + QStringLiteral("] INFO  ") + QLatin1String(kLogger)
                         + QStringLiteral(" - ") + QLatin1String(kMessage);

    const Indexed got = indexOne(pattern, line);
    QVERIFY2(got.format.recordRe.isValid(), qPrintable(QStringLiteral("pattern did not compile: ")
                                                       + pattern));
    QCOMPARE(got.index.recordCount(), 1);

    const Record &r = got.index.records.at(0);
    QCOMPARE(r.priorityEnum(), Priority::Info);
    QCOMPARE(got.index.loggers.name(r.loggerId), QString::fromLatin1(kLogger));
    QCOMPARE(got.index.threads.name(r.threadId), QString::fromLatin1(kThread));
    QCOMPARE(r.lineCount, quint16(1));
    QCOMPARE(r.offset, qint64(0));

    // The message is the tail of the record's own bytes, which is what data()
    // decodes lazily on the paint path (invariant #1) — asserted here so a row
    // proves the whole line was split and not merely its head.
    QVERIFY(r.length > 0);

    QVERIFY2(r.timestamp != Record::kNoTimestamp,
             qPrintable(QStringLiteral("the timestamp did not parse: ") + dateText));
    if (expectedMs >= 0) {
        QCOMPARE(r.timestamp, expectedMs);
    } else {
        // A format with no year takes one from the clock, so what is asserted is
        // everything the format actually carries.
        const QDateTime dt = QDateTime::fromMSecsSinceEpoch(r.timestamp, QTimeZone::utc());
        QCOMPARE(dt.time().hour(), 10);
        QCOMPARE(dt.time().minute(), 15);
        QCOMPARE(dt.time().second(), 1);
    }
}

// --- The display half: bugs.md 37 ------------------------------------------

void TestFormatRoundTrip::theCompiledDisplayFormatRendersTheTextItWasCompiledFrom_data()
{
    QTest::addColumn<QString>("dateFormat");
    QTest::addColumn<QString>("dateText");
    QTest::addColumn<QString>("rendered"); // what the As Written column must show

    // Only the formats a Qt display string can spell exactly. The others (%s, %Q,
    // %z, the skip-only codes) have no Qt spelling at all, which is the whole
    // reason DateFormat carries a token list beside qtFormat — for those the
    // fallback is asserted below rather than here.
    const QString iso = QStringLiteral("2026-08-27 10:15:01");

    QTest::newRow("bare %d") << QString() << iso << iso;
    QTest::newRow("iso") << QStringLiteral("%Y-%m-%d %H:%M:%S") << iso << iso;
    QTest::newRow("iso with milliseconds")
        << QStringLiteral("%Y-%m-%d %H:%M:%S,%q") << QStringLiteral("2026-08-27 10:15:01,123")
        << QStringLiteral("2026-08-27 10:15:01,123");
    QTest::newRow("%F %T") << QStringLiteral("%F %T") << iso << iso;
    QTest::newRow("%T") << QStringLiteral("%T") << QStringLiteral("10:15:01")
                        << QStringLiteral("10:15:01");
    QTest::newRow("day first") << QStringLiteral("%d/%m/%Y %H:%M:%S")
                               << QStringLiteral("27/08/2026 10:15:01")
                               << QStringLiteral("27/08/2026 10:15:01");
    QTest::newRow("month first, two-digit year")
        << QStringLiteral("%m/%d/%y %H:%M:%S") << QStringLiteral("08/27/26 10:15:01")
        << QStringLiteral("08/27/26 10:15:01");

    // THE ROW bugs.md 37 LIVED IN, and the only one that reaches the branch it was
    // in: a format with NO Qt spelling at all — epoch seconds — falls back to
    // kDefaultQtDateFormat, and the column then renders the instant in the default
    // ISO form. Assign kDefaultDateFormat there instead (the strftime spelling,
    // which is what shipped) and QDateTime reads %m as minutes and %M as the
    // month, so this row renders `%Y-%15-%27 %10:%8:%S` while every other row here
    // — and every parse in the file — stays correct. Deleting it takes the whole
    // point of this case with it.
    QTest::newRow("epoch seconds have no Qt spelling and fall back to the default")
        << QStringLiteral("%s") << QStringLiteral("1787825701") << iso;
}

void TestFormatRoundTrip::theCompiledDisplayFormatRendersTheTextItWasCompiledFrom()
{
    QFETCH(QString, dateFormat);
    QFETCH(QString, dateText);
    QFETCH(QString, rendered);

    const QString spec =
        dateFormat.isNull() ? QStringLiteral("%d")
                            : QStringLiteral("%d{") + dateFormat + QStringLiteral("}");
    const QString pattern = spec + QStringLiteral(" [%t] %-5p %c - %m%n");
    const QString line = dateText + QStringLiteral(" [") + QLatin1String(kThread)
                         + QStringLiteral("] INFO  ") + QLatin1String(kLogger)
                         + QStringLiteral(" - ") + QLatin1String(kMessage);

    const Indexed got = indexOne(pattern, line);
    QCOMPARE(got.index.recordCount(), 1);
    const qint64 ms = got.index.records.at(0).timestamp;
    QVERIFY(ms != Record::kNoTimestamp);

    // The As Written column: the instant the parser produced, rendered through the
    // display string the SAME compile emitted. It must reproduce the text the
    // record was built from, character for character. This is the assertion that
    // fails when the two default constants are swapped — kDefaultDateFormat is the
    // strftime spelling and kDefaultQtDateFormat is what translating it produces —
    // and it fails without anyone having had to think about %m against %M.
    const QString qtFormat = got.format.impliedDateFormat.qtFormat;
    QVERIFY2(!qtFormat.isEmpty(), "a displayable date format has a Qt spelling");
    const QString shown = QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::utc()).toString(qtFormat);
    QCOMPARE(shown, rendered);
}

// --- Every date code, compiled and read back -------------------------------

void TestFormatRoundTrip::everyDateCodeThatCompilesReadsBackTheTextItAgreedToMatch_data()
{
    QTest::addColumn<QString>("dateFormat");
    QTest::addColumn<QString>("dateText");

    // One row per strftime code the compiler accepts, each wrapped in a format
    // that also carries a full date — so what is being asserted is that adding the
    // code neither breaks the compile nor moves the instant. The whole vocabulary,
    // because it is the newest grammar in the tree: the supported set went from
    // ten codes to the C/POSIX strftime set, and a code that compiles but whose
    // regex cannot match the text it agreed to is a record that silently loses its
    // timestamp (Record::kNoTimestamp) and with it its place in every time filter.
    struct Row { const char *code; const char *text; };
    static const Row rows[] = {
        {"%a", "Thu"},          {"%A", "Thursday"},   {"%b", "Aug"},
        {"%B", "August"},       {"%h", "Aug"},        {"%C", "20"},
        {"%g", "26"},           {"%G", "2026"},       {"%j", "239"},
        {"%u", "4"},            {"%U", "34"},         {"%V", "35"},
        {"%w", "4"},            {"%W", "34"},         {"%Z", "UTC"},
        {"%k", "10"},           {"%l", "10"},         {"%P", "am"},
        {"%q", "123"},
    };

    for (const Row &row : rows) {
        const QString format =
            QStringLiteral("%Y-%m-%d %H:%M:%S ") + QLatin1String(row.code);
        const QString text = QStringLiteral("2026-08-27 10:15:01 ") + QLatin1String(row.text);
        QTest::newRow(row.code) << format << text;
    }
}

void TestFormatRoundTrip::everyDateCodeThatCompilesReadsBackTheTextItAgreedToMatch()
{
    QFETCH(QString, dateFormat);
    QFETCH(QString, dateText);

    const QString pattern =
        QStringLiteral("%d{") + dateFormat + QStringLiteral("} [%t] %-5p %c - %m%n");
    const QString line = dateText + QStringLiteral(" [") + QLatin1String(kThread)
                         + QStringLiteral("] INFO  ") + QLatin1String(kLogger)
                         + QStringLiteral(" - ") + QLatin1String(kMessage);

    const Indexed got = indexOne(pattern, line);
    QVERIFY2(got.format.recordRe.isValid(), qPrintable(QStringLiteral("did not compile: ") + pattern));
    QCOMPARE(got.index.recordCount(), 1);

    const Record &r = got.index.records.at(0);
    QCOMPARE(got.index.loggers.name(r.loggerId), QString::fromLatin1(kLogger));
    QCOMPARE(r.priorityEnum(), Priority::Info);

    const qint64 base = QDateTime(QDate(2026, 8, 27), QTime(10, 15, 1), QTimeZone::utc())
                            .toMSecsSinceEpoch();
    // %q is the one code in the table that carries information; everything else is
    // matched and dropped, so the instant must be exactly the one the full date
    // spells.
    const qint64 expected = dateFormat.endsWith(QLatin1String("%q")) ? base + 123 : base;
    QCOMPARE(r.timestamp, expected);
}

QTEST_APPLESS_MAIN(TestFormatRoundTrip)
#include "tst_formatroundtrip.moc"
