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
