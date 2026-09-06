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

#include <QLocale>

#include "PatternCompiler.h"
#include "Record.h"
#include "TimestampParser.h"

using namespace loftail;

// TimestampParser over the whole strftime vocabulary PatternCompiler now
// translates. Every case goes through the compiler rather than hand-building a
// token list, because the two halves have to agree: a regex that matches text the
// parser cannot read (or the reverse) is exactly the failure this pins — the
// %d field is captured by recordRe and then handed to parse() verbatim.
class TestTimestampParser : public QObject
{
    Q_OBJECT

private slots:
    void theRegexAndTheParserAgree_data();
    void theRegexAndTheParserAgree();

    void aYearlessTimestampTakesTheYearThatPutsItInThePast();
    void aYearlessTimestampJustAheadOfTheClockStaysInThisYear();
    void aYearlessTimestampWellAheadOfTheClockIsReadAsLastYears();
    void aSpacePaddedFieldIsReadWhereNoLiteralSpacePrecedesIt();
    void aMonthAbbreviationCarryingItsLocalesFullStopIsStillThatMonth();
    void aFormatWithNoTokensOfItsOwnFallsBackToQtsParser();
    void aFieldThatIsNotThereStopsTheParseRatherThanGuessing_data();
    void aFieldThatIsNotThereStopsTheParseRatherThanGuessing();
    void epochSecondsNameAnInstantThatNoZoneMoves();
    void aUtcOffsetInTheTextOverridesTheSourceZone();
    void textThatDoesNotFitTheFormatHasNoTimestamp();
};

namespace {

// Compile "%d{<inner>} %m%n" and hand back the date half of it.
LogFormat compileDate(const QString &inner)
{
    const QString pattern = QStringLiteral("%d{") + inner + QStringLiteral("} %m%n");
    auto compiled = PatternCompiler::compile(pattern);
    if (!compiled) {
        qWarning("compile failed for %s: %s", qPrintable(inner),
                 qPrintable(compiled.error().message));
        return {};
    }
    return compiled.value();
}

// Parse `sample` as the date of a record, asserting on the way through that the
// generated regex captured exactly the text handed to the parser.
qint64 roundTrip(const QString &inner, const QString &sample, const QTimeZone &zone,
                 QString *capturedOut = nullptr)
{
    const LogFormat fmt = compileDate(inner);
    if (fmt.dateGroup < 0)
        return Record::kNoTimestamp;
    const auto m = fmt.recordRe.match(sample + QStringLiteral(" the message"));
    if (!m.hasMatch()) {
        qWarning("regex %s did not match %s", qPrintable(fmt.recordRe.pattern()),
                 qPrintable(sample));
        return Record::kNoTimestamp;
    }
    if (capturedOut)
        *capturedOut = m.captured(fmt.dateGroup);
    return TimestampParser(fmt.impliedDateFormat, zone).parse(m.capturedView(fmt.dateGroup));
}

qint64 utcMs(int y, int mo, int d, int h, int mi, int s, int ms = 0)
{
    return QDateTime(QDate(y, mo, d), QTime(h, mi, s, ms), QTimeZone::utc()).toMSecsSinceEpoch();
}

// "%b %e %H:%M:%S" as strftime writes it in the C locale: the day is SPACE-padded
// to two columns, which is the half of the syslog stamp Qt's own "d" cannot spell
// and the reason these are built by hand rather than through QDateTime::toString.
QString syslogStamp(const QDateTime &when)
{
    return QStringLiteral("%1 %2 %3")
        .arg(QLocale::c().monthName(when.date().month(), QLocale::ShortFormat))
        .arg(when.date().day(), 2, 10, QLatin1Char(' '))
        .arg(when.toString(QStringLiteral("HH:mm:ss")));
}

} // namespace

void TestTimestampParser::theRegexAndTheParserAgree_data()
{
    QTest::addColumn<QString>("inner");     // the strftime format inside %d{...}
    QTest::addColumn<QString>("sample");    // text that format produces
    QTest::addColumn<qint64>("expected");   // UTC epoch ms, reading the sample as UTC

    QTest::newRow("iso")
        << "%Y-%m-%d %H:%M:%S" << "2026-08-27 10:15:01" << utcMs(2026, 8, 27, 10, 15, 1);
    QTest::newRow("iso with log4cplus millis")
        << "%Y-%m-%d %H:%M:%S,%q" << "2026-08-27 10:15:01,123"
        << utcMs(2026, 8, 27, 10, 15, 1, 123);
    QTest::newRow("%F and %T expand to the same thing")
        << "%F %T" << "2026-08-27 10:15:01" << utcMs(2026, 8, 27, 10, 15, 1);
    QTest::newRow("%D is the C locale's slash date")
        << "%D %T" << "08/27/26 10:15:01" << utcMs(2026, 8, 27, 10, 15, 1);
    QTest::newRow("abbreviated month name")
        << "%b %d %Y %H:%M:%S" << "Aug 27 2026 10:15:01" << utcMs(2026, 8, 27, 10, 15, 1);
    QTest::newRow("full month name")
        << "%B %d %Y" << "August 27 2026" << utcMs(2026, 8, 27, 0, 0, 0);
    QTest::newRow("month name in another case")
        << "%b %d %Y" << "AUG 27 2026" << utcMs(2026, 8, 27, 0, 0, 0);
    QTest::newRow("weekday name is matched and dropped")
        << "%a %b %d %Y" << "Thu Aug 27 2026" << utcMs(2026, 8, 27, 0, 0, 0);
    QTest::newRow("%c is the C locale's whole date and time")
        << "%c" << "Thu Aug 27 10:15:01 2026" << utcMs(2026, 8, 27, 10, 15, 1);
    QTest::newRow("space-padded day")
        << "%b %e %Y %H:%M:%S" << "Aug  5 2026 10:15:01" << utcMs(2026, 8, 5, 10, 15, 1);
    QTest::newRow("space-padded hour")
        << "%Y-%m-%d %k:%M" << "2026-08-27  9:15" << utcMs(2026, 8, 27, 9, 15, 0);
    QTest::newRow("12-hour clock, PM")
        << "%Y-%m-%d %I:%M:%S %p" << "2026-08-27 01:15:01 PM" << utcMs(2026, 8, 27, 13, 15, 1);
    QTest::newRow("12-hour clock, midnight")
        << "%Y-%m-%d %I:%M:%S %p" << "2026-08-27 12:15:01 AM" << utcMs(2026, 8, 27, 0, 15, 1);
    QTest::newRow("%r is the 12-hour composite")
        << "%Y-%m-%d %r" << "2026-08-27 01:15:01 PM" << utcMs(2026, 8, 27, 13, 15, 1);
    QTest::newRow("%Q keeps the milliseconds and drops the microseconds")
        << "%Y-%m-%d %H:%M:%S.%Q" << "2026-08-27 10:15:01.123.456"
        << utcMs(2026, 8, 27, 10, 15, 1, 123);
    QTest::newRow("a day of the year is matched and dropped")
        << "%Y-%m-%d %H:%M:%S %j" << "2026-08-27 10:15:01 239"
        << utcMs(2026, 8, 27, 10, 15, 1);
    QTest::newRow("a week number is matched and dropped")
        << "%Y-%m-%d %H:%M:%S W%W" << "2026-08-27 10:15:01 W34"
        << utcMs(2026, 8, 27, 10, 15, 1);
    QTest::newRow("a zone abbreviation is matched and dropped")
        << "%Y-%m-%d %H:%M:%S %Z" << "2026-08-27 10:15:01 UTC"
        << utcMs(2026, 8, 27, 10, 15, 1);
    // rsyslog's RSYSLOG_FileFormat, the default /var/log/syslog shape on current
    // Debian and Ubuntu: six fractional digits run straight on from the seconds.
    QTest::newRow("rsyslog RFC3339")
        << "%Y-%m-%dT%H:%M:%S.%Q%z" << "2026-08-24T22:04:19.341116+03:00"
        << utcMs(2026, 8, 24, 19, 4, 19, 341);
    QTest::newRow("%Q with no remainder at all")
        << "%H:%M:%S.%Q" << "10:15:01.123" << utcMs(1970, 1, 1, 10, 15, 1, 123);
    QTest::newRow("lower-case meridiem")
        << "%Y-%m-%d %I:%M:%S %P" << "2026-08-27 01:15:01 pm" << utcMs(2026, 8, 27, 13, 15, 1);
    QTest::newRow("space-padded 12-hour clock")
        << "%Y-%m-%d %l:%M:%S %p" << "2026-08-27  1:15:01 PM" << utcMs(2026, 8, 27, 13, 15, 1);
    QTest::newRow("a weekday number is matched and dropped")
        << "%Y-%m-%d %H:%M:%S %u" << "2026-08-27 10:15:01 4" << utcMs(2026, 8, 27, 10, 15, 1);
    QTest::newRow("a tab separator")
        << "%Y-%m-%d%t%H:%M:%S" << "2026-08-27\t10:15:01" << utcMs(2026, 8, 27, 10, 15, 1);
}

void TestTimestampParser::theRegexAndTheParserAgree()
{
    QFETCH(QString, inner);
    QFETCH(QString, sample);
    QFETCH(qint64, expected);

    QString captured;
    const qint64 ms = roundTrip(inner, sample, QTimeZone::utc(), &captured);
    QCOMPARE(captured, sample);
    QCOMPARE(ms, expected);
}

// The /var/log/messages shape: a month, a day and a time, and no year at all. The
// year is inferred from the clock, and a record that would land in the future is
// read as last year's — which is what makes a log spanning a New Year read right.
void TestTimestampParser::aYearlessTimestampTakesTheYearThatPutsItInThePast()
{
    // Thirty days ago, rendered the way syslog renders it. In January that is last
    // year, which is exactly the branch worth exercising.
    const QDateTime target = QDateTime::currentDateTimeUtc().addDays(-30);
    const QString rendered = syslogStamp(target);

    const qint64 ms = roundTrip(QStringLiteral("%b %e %H:%M:%S"), rendered, QTimeZone::utc());
    QVERIFY(ms != Record::kNoTimestamp);

    const QDateTime got = QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::utc());
    QCOMPARE(got.date(), target.date());
    QCOMPARE(got.time().hour(), target.time().hour());
    QCOMPARE(got.time().minute(), target.time().minute());
    QCOMPARE(got.time().second(), target.time().second());
}

// A log written on a machine whose clock runs a little ahead of this one must not
// be thrown back a year: the rollback needs a day of grace to be usable at all.
void TestTimestampParser::aYearlessTimestampJustAheadOfTheClockStaysInThisYear()
{
    const QDateTime target = QDateTime::currentDateTimeUtc().addSecs(2 * 60 * 60);
    const QString rendered = syslogStamp(target);

    const qint64 ms = roundTrip(QStringLiteral("%b %e %H:%M:%S"), rendered, QTimeZone::utc());
    QVERIFY(ms != Record::kNoTimestamp);
    QCOMPARE(QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::utc()).date(), target.date());
}

// %s is seconds since the epoch: an instant, not a wall clock, so the source zone
// has nothing to apply to it.
void TestTimestampParser::epochSecondsNameAnInstantThatNoZoneMoves()
{
    const qint64 expected = utcMs(2026, 8, 27, 10, 15, 1);
    const QString sample = QString::number(expected / 1000);

    QCOMPARE(roundTrip(QStringLiteral("%s"), sample, QTimeZone::utc()), expected);
    QCOMPARE(roundTrip(QStringLiteral("%s"), sample,
                       QTimeZone(-5 * 3600)), expected);
}

void TestTimestampParser::aUtcOffsetInTheTextOverridesTheSourceZone()
{
    // 10:15:01 two hours east of UTC is 08:15:01 UTC, whatever the source zone says.
    const qint64 expected = utcMs(2026, 8, 27, 8, 15, 1);
    QCOMPARE(roundTrip(QStringLiteral("%Y-%m-%dT%H:%M:%S%z"),
                       QStringLiteral("2026-08-27T10:15:01+0200"), QTimeZone::utc()),
             expected);
    // The colon spelling, and Z for UTC.
    QCOMPARE(roundTrip(QStringLiteral("%Y-%m-%dT%H:%M:%S%z"),
                       QStringLiteral("2026-08-27T10:15:01+02:00"), QTimeZone::utc()),
             expected);
    QCOMPARE(roundTrip(QStringLiteral("%Y-%m-%dT%H:%M:%S%z"),
                       QStringLiteral("2026-08-27T08:15:01Z"),
                       QTimeZone(-5 * 3600)),
             expected);
}

// The other side of the grace: a stamp well ahead of this machine's clock cannot
// be this year's, so the year that puts it in the past is the one taken. That is
// what makes a log written before New Year read correctly when it is opened after
// one — the year is inferred from the clock, so the December records of a file
// read in January are the only ones the clock can be wrong about.
//
// The relation, not a year: whatever today is, the text carries a month, a day
// and a time and no year, and the instant that comes back has exactly those and
// sits behind the clock. A future date landing in the next calendar year (the
// last days of December) satisfies both halves through the same arithmetic.
void TestTimestampParser::aYearlessTimestampWellAheadOfTheClockIsReadAsLastYears()
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QDateTime future = now.addDays(30);
    const QString rendered = syslogStamp(future);

    const qint64 ms = roundTrip(QStringLiteral("%b %e %H:%M:%S"), rendered, QTimeZone::utc());
    QVERIFY(ms != Record::kNoTimestamp);
    const QDateTime got = QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::utc());

    // Everything the text says is read as it says it.
    QCOMPARE(got.date().month(), future.date().month());
    QCOMPARE(got.date().day(), future.date().day());
    QCOMPARE(got.time().hour(), future.time().hour());
    QCOMPARE(got.time().minute(), future.time().minute());
    // And the one thing it does not say is chosen so the record is in the past.
    QVERIFY(got < now.addDays(1));
    QCOMPARE(got.date().year(), future.date().year() - 1);
}

// strftime space-pads %e, %k and %l, and that pad is not always beside a literal
// space the format has already absorbed: put a space-padded field straight after
// any other separator and the pad arrives at the number reader itself.
void TestTimestampParser::aSpacePaddedFieldIsReadWhereNoLiteralSpacePrecedesIt()
{
    // A single-digit hour after the 'T' of an ISO date: nothing before it can eat
    // the pad.
    QCOMPARE(roundTrip(QStringLiteral("%Y-%m-%dT%k:%M"), QStringLiteral("2026-08-27T 9:15"),
                       QTimeZone::utc()),
             utcMs(2026, 8, 27, 9, 15, 0));
    // And a bracketed day, where the pad sits between the bracket and the digit.
    QCOMPARE(roundTrip(QStringLiteral("[%e %b %Y %H:%M:%S]"),
                       QStringLiteral("[ 5 Aug 2026 10:15:01]"), QTimeZone::utc()),
             utcMs(2026, 8, 5, 10, 15, 1));
}

// strftime renders %b in the process's locale and several locales abbreviate a
// month with a full stop after it ("sep."). The generated regex allows that stop
// and the parser consumes it, and the two have to agree: a regex that matches the
// text while the parser refuses it is a blank Time column on every record of a log
// that renders perfectly well. The relation is that the stop changes nothing —
// the same month, the same instant, as the spelling without it.
void TestTimestampParser::aMonthAbbreviationCarryingItsLocalesFullStopIsStillThatMonth()
{
    const QString inner = QStringLiteral("%b %e %Y %H:%M:%S");
    const qint64 plain = roundTrip(inner, QStringLiteral("Aug  5 2026 10:15:01"), QTimeZone::utc());
    const qint64 dotted = roundTrip(inner, QStringLiteral("Aug.  5 2026 10:15:01"), QTimeZone::utc());
    QCOMPARE(plain, utcMs(2026, 8, 5, 10, 15, 1));
    QCOMPARE(dotted, plain);
}

// A format that produced no tokens at all — an empty %d{}, which translates to no
// regex and no tokens — falls back to QDateTime::fromString on the Qt spelling.
// It is the slow path, and it is kept precisely so that correctness never depends
// on the fast one: it must apply the source zone exactly as the token walk does,
// and refuse text that does not fit exactly as the token walk does.
void TestTimestampParser::aFormatWithNoTokensOfItsOwnFallsBackToQtsParser()
{
    DateFormat df;
    df.qtFormat = QStringLiteral("yyyy-MM-dd HH:mm:ss");
    df.isValid = true;
    QVERIFY(df.tokens.isEmpty());

    const TimestampParser utc(df, QTimeZone::utc());
    QVERIFY(utc.isValid());
    QCOMPARE(utc.parse(QStringLiteral("2026-08-27 10:15:01")), utcMs(2026, 8, 27, 10, 15, 1));

    // The zone is applied to the wall clock it read, which is the one conversion
    // invariant #10 allows on the way in — so the same text two hours east of UTC
    // is two hours earlier as an instant.
    const TimestampParser east(df, QTimeZone(2 * 3600));
    QCOMPARE(east.parse(QStringLiteral("2026-08-27 10:15:01")), utcMs(2026, 8, 27, 8, 15, 1));

    QCOMPARE(utc.parse(QStringLiteral("not a date at all")), Record::kNoTimestamp);
    QCOMPARE(utc.parse(QString()), Record::kNoTimestamp);

    // A format with neither tokens nor a Qt spelling never became a parser at all,
    // and a parser that is not one answers about no record rather than about the
    // epoch.
    const TimestampParser none{DateFormat{}, QTimeZone::utc()};
    QVERIFY(!none.isValid());
    QCOMPARE(none.parse(QStringLiteral("2026-08-27 10:15:01")), Record::kNoTimestamp);
}

// Every token reads a field, and a field that is not where the format says it is
// has to end the parse. The alternative is worse than a blank Time column: a
// half-read stamp is a plausible-looking instant that nothing on screen marks as
// invented, and it would sort, filter and bound the same as a real one. These go
// straight to parse() because most of them are text the generated regex would
// never hand over — which is the point, parse() being a public function whose
// answer for text that does not fit is stated in its own header.
void TestTimestampParser::aFieldThatIsNotThereStopsTheParseRatherThanGuessing_data()
{
    QTest::addColumn<QString>("inner");
    QTest::addColumn<QString>("text");

    QTest::newRow("letters where a number belongs")
        << "%Y-%m-%d" << "abcd-08-27";
    QTest::newRow("the number runs out")
        << "%Y-%m-%d %H:%M:%S" << "2026-08-27 10:15:";
    QTest::newRow("a meridiem of one letter")
        << "%I:%M %p" << "01:15 P";
    QTest::newRow("a meridiem that is neither")
        << "%I:%M %p" << "01:15 XM";
    QTest::newRow("no weekday name where one belongs")
        << "%a %Y" << "1 2026";
    QTest::newRow("no month name where one belongs")
        << "%b %d %Y" << "8 27 2026";
    QTest::newRow("a month name that is no month")
        << "%b %d %Y" << "Zug 27 2026";
    QTest::newRow("a separator that is not a space")
        << "%Y %m" << "2026-08";
    // The space in a format absorbs a RUN of spaces, which is what makes "Aug  5"
    // read — but a run of none is still not a space, and text with the separator
    // missing altogether must not be read as though it were there.
    QTest::newRow("no separator at all")
        << "%Y %m" << "202608";
    QTest::newRow("a separator that is not the one named")
        << "%Y-%m" << "2026/08";
    QTest::newRow("the text stops before the zone")
        << "%H:%M%z" << "10:15";
    QTest::newRow("a zone with no sign")
        << "%H:%M%z" << "10:15X0230";
    QTest::newRow("an hour no clock has")
        << "%H:%M:%S" << "99:15:01";
    QTest::newRow("a minute no clock has")
        << "%H:%M:%S" << "10:75:01";
}

void TestTimestampParser::aFieldThatIsNotThereStopsTheParseRatherThanGuessing()
{
    QFETCH(QString, inner);
    QFETCH(QString, text);

    const LogFormat fmt = compileDate(inner);
    QVERIFY(fmt.impliedDateFormat.isValid);
    const TimestampParser parser(fmt.impliedDateFormat, QTimeZone::utc());
    QVERIFY(parser.isValid());
    QCOMPARE(parser.parse(text), Record::kNoTimestamp);
}

void TestTimestampParser::textThatDoesNotFitTheFormatHasNoTimestamp()
{
    const LogFormat fmt = compileDate(QStringLiteral("%b %e %H:%M:%S"));
    const TimestampParser parser(fmt.impliedDateFormat, QTimeZone::utc());
    QVERIFY(parser.isValid());
    QCOMPARE(parser.parse(QStringLiteral("Zzz 27 10:15:01")), Record::kNoTimestamp);
    QCOMPARE(parser.parse(QStringLiteral("Aug 27 10:15")), Record::kNoTimestamp);
    QCOMPARE(parser.parse(QString()), Record::kNoTimestamp);
}

QTEST_MAIN(TestTimestampParser)
#include "tst_timestampparser.moc"
