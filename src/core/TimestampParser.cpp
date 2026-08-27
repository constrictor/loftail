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

#include "TimestampParser.h"

#include <QDateTime>
#include <QLocale>

namespace loftail {

namespace {

// One day's grace: a record stamped slightly ahead of this machine's clock is
// still this year's, not last year's.
constexpr qint64 kFutureGraceMs = 24 * 60 * 60 * 1000;

// The C locale's month names, which is what strftime writes for every log a
// daemon produces with no locale set — /var/log/messages included. Kept as plain
// literals rather than asked of QLocale::c(), whose month names are Qt's data and
// not necessarily the C library's.
constexpr const char *kEnglishShortMonths[12] = {
    "jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"
};
constexpr const char *kEnglishLongMonths[12] = {
    "january", "february", "march", "april", "may", "june",
    "july", "august", "september", "october", "november", "december"
};

} // namespace

// const reference, not by value + move — see Indexer's constructor for why QTimeZone in
// particular must not take modernize-pass-by-value's advice at the Qt 6.4 floor.
TimestampParser::TimestampParser(const DateFormat &format, const QTimeZone &sourceZone)
    : m_tokens(format.tokens), m_zone(sourceZone), m_qtFormat(format.qtFormat)
{
    if (m_tokens.isEmpty() && m_qtFormat.isEmpty())
        return;

    m_valid = true;

    bool hasYear = false;
    bool hasMonth = false;
    bool hasDay = false;
    bool hasNames = false;
    for (const DateToken &tok : m_tokens) {
        switch (tok.kind) {
        case DateTokenKind::Year4:
        case DateTokenKind::Year2:
        case DateTokenKind::EpochSeconds: hasYear = true; break;
        case DateTokenKind::MonthNumber:  hasMonth = true; break;
        case DateTokenKind::MonthName:    hasMonth = true; hasNames = true; break;
        case DateTokenKind::Day:          hasDay = true; break;
        case DateTokenKind::SkipWord:     hasNames = true; break;
        default: break;
        }
    }

    if (hasNames) {
        for (int i = 0; i < 12; ++i) {
            m_months.insert(QString::fromLatin1(kEnglishShortMonths[i]), i + 1);
            m_months.insert(QString::fromLatin1(kEnglishLongMonths[i]), i + 1);
        }
        // Whatever the machine is set to, in case the log was written under it.
        const QLocale sys = QLocale::system();
        for (int i = 1; i <= 12; ++i) {
            m_months.insert(sys.monthName(i, QLocale::ShortFormat).toLower(), i);
            m_months.insert(sys.monthName(i, QLocale::LongFormat).toLower(), i);
        }
    }

    if (!hasYear && hasMonth && hasDay) {
        m_inferYear = true;
        m_assumedYear = QDate::currentDate().year();
        m_futureCutoffMs = QDateTime::currentMSecsSinceEpoch() + kFutureGraceMs;
    }
}

qint64 TimestampParser::parse(QStringView text) const
{
    if (!m_valid)
        return Record::kNoTimestamp;

    if (m_tokens.isEmpty()) {
        QDateTime dt = QDateTime::fromString(text.toString(), m_qtFormat);
        if (!dt.isValid())
            return Record::kNoTimestamp;
        dt.setTimeZone(m_zone);
        return dt.toMSecsSinceEpoch();
    }

    int year = m_inferYear ? m_assumedYear : 1970;
    int month = 1;
    int day = 1;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int millis = 0;
    bool pm = false;
    bool hour12 = false;
    bool sawEpoch = false;
    qint64 epochSeconds = 0;
    bool sawOffset = false;
    int offsetSeconds = 0;

    int pos = 0;
    const int n = int(text.size());

    // strftime pads %e, %k and %l with a space and every other numeric with a
    // zero, so a leading space is skipped rather than required: one reader covers
    // both, and a log whose writer padded differently is read anyway.
    const auto readInt = [&](int maxWidth, qint64 &out) -> bool {
        while (pos < n && text[pos] == QLatin1Char(' '))
            ++pos;
        qint64 value = 0;
        int digits = 0;
        while (pos < n && digits < maxWidth && text[pos].isDigit()) {
            value = value * 10 + (text[pos].unicode() - u'0');
            ++pos;
            ++digits;
        }
        if (digits == 0)
            return false;
        out = value;
        return true;
    };
    const auto readIntTo = [&](int maxWidth, int &out) -> bool {
        qint64 v = 0;
        if (!readInt(maxWidth, v))
            return false;
        out = int(v);
        return true;
    };
    // A run of letters, plus the trailing '.' some locales' abbreviations carry.
    const auto readWord = [&]() {
        const int start = pos;
        while (pos < n && text[pos].isLetter())
            ++pos;
        const QStringView word = text.sliced(start, pos - start);
        if (pos < n && text[pos] == QLatin1Char('.'))
            ++pos;
        return word;
    };

    for (const DateToken &tok : m_tokens) {
        switch (tok.kind) {
        case DateTokenKind::Year4:
            if (!readIntTo(4, year)) return Record::kNoTimestamp;
            break;
        case DateTokenKind::Year2:
            if (!readIntTo(2, year)) return Record::kNoTimestamp;
            year += (year < 70) ? 2000 : 1900;
            break;
        case DateTokenKind::MonthNumber:
            if (!readIntTo(2, month)) return Record::kNoTimestamp;
            break;
        case DateTokenKind::MonthName: {
            const QStringView word = readWord();
            if (word.isEmpty())
                return Record::kNoTimestamp;
            const auto it = m_months.constFind(word.toString().toLower());
            if (it == m_months.constEnd())
                return Record::kNoTimestamp;
            month = it.value();
            break;
        }
        case DateTokenKind::Day:
            if (!readIntTo(2, day)) return Record::kNoTimestamp;
            break;
        case DateTokenKind::Hour24:
            if (!readIntTo(2, hour)) return Record::kNoTimestamp;
            break;
        case DateTokenKind::Hour12:
            if (!readIntTo(2, hour)) return Record::kNoTimestamp;
            hour12 = true;
            break;
        case DateTokenKind::Minute:
            if (!readIntTo(2, minute)) return Record::kNoTimestamp;
            break;
        case DateTokenKind::Second:
            if (!readIntTo(2, second)) return Record::kNoTimestamp;
            break;
        case DateTokenKind::Millis:
            if (!readIntTo(3, millis)) return Record::kNoTimestamp;
            break;
        case DateTokenKind::MillisFrac: {
            // The milliseconds, then a sub-millisecond remainder Record has no room
            // for (invariant #1) and which is read only to consume it. log4cplus
            // separates the two with a '.' ("123.456"); rsyslog runs them together
            // ("341116"). Both, and neither — the remainder is optional.
            if (!readIntTo(3, millis)) return Record::kNoTimestamp;
            if (pos < n && text[pos] == QLatin1Char('.'))
                ++pos;
            qint64 discarded = 0;
            if (pos < n && text[pos].isDigit() && !readInt(6, discarded))
                return Record::kNoTimestamp;
            break;
        }
        case DateTokenKind::AmPm: {
            const QStringView word = readWord();
            if (word.size() != 2)
                return Record::kNoTimestamp;
            const QChar first = word[0].toUpper();
            if (word[1].toUpper() != QLatin1Char('M')
                || (first != QLatin1Char('A') && first != QLatin1Char('P')))
                return Record::kNoTimestamp;
            pm = (first == QLatin1Char('P'));
            break;
        }
        case DateTokenKind::EpochSeconds: {
            qint64 v = 0;
            if (!readInt(tok.width, v)) return Record::kNoTimestamp;
            epochSeconds = v;
            sawEpoch = true;
            break;
        }
        case DateTokenKind::UtcOffset: {
            if (pos < n && (text[pos] == QLatin1Char('Z') || text[pos] == QLatin1Char('z'))) {
                ++pos;
                sawOffset = true;
                offsetSeconds = 0;
                break;
            }
            if (pos >= n)
                return Record::kNoTimestamp;
            const bool negative = (text[pos] == QLatin1Char('-'));
            if (!negative && text[pos] != QLatin1Char('+'))
                return Record::kNoTimestamp;
            ++pos;
            int oh = 0;
            int om = 0;
            if (!readIntTo(2, oh)) return Record::kNoTimestamp;
            if (pos < n && text[pos] == QLatin1Char(':'))
                ++pos;
            if (!readIntTo(2, om)) return Record::kNoTimestamp;
            offsetSeconds = (oh * 3600 + om * 60) * (negative ? -1 : 1);
            sawOffset = true;
            break;
        }
        case DateTokenKind::SkipDigits: {
            qint64 discarded = 0;
            if (!readInt(tok.width, discarded)) return Record::kNoTimestamp;
            break;
        }
        case DateTokenKind::SkipWord:
            if (readWord().isEmpty())
                return Record::kNoTimestamp;
            break;
        case DateTokenKind::Literal:
            // A space in the format absorbs a run of them: strftime's own space
            // padding lands right beside the separator that precedes it ("Aug  5").
            if (tok.literal == QLatin1Char(' ')) {
                if (pos >= n || !text[pos].isSpace())
                    return Record::kNoTimestamp;
                while (pos < n && text[pos].isSpace())
                    ++pos;
                break;
            }
            if (pos >= n || text[pos] != tok.literal)
                return Record::kNoTimestamp;
            ++pos;
            break;
        }
    }

    // %s names an instant outright: no calendar, and no zone to apply to it.
    if (sawEpoch)
        return epochSeconds * 1000 + millis;

    if (hour12) {
        if (hour == 12)
            hour = 0;
        if (pm)
            hour += 12;
    }

    const QTime time(hour, minute, second, millis);
    if (!time.isValid())
        return Record::kNoTimestamp;

    const QTimeZone zone = sawOffset ? QTimeZone(offsetSeconds) : m_zone;

    const auto instantAt = [&](int y) -> qint64 {
        const QDate date(y, month, day);
        if (!date.isValid())
            return Record::kNoTimestamp;
        const QDateTime dt(date, time, zone);
        if (!dt.isValid())
            return Record::kNoTimestamp;
        return dt.toMSecsSinceEpoch();
    };

    qint64 ms = instantAt(year);
    if (m_inferYear && (ms == Record::kNoTimestamp || ms > m_futureCutoffMs)) {
        // 29 February in a common year, or a record from before this New Year.
        const qint64 previous = instantAt(year - 1);
        if (previous != Record::kNoTimestamp)
            ms = previous;
    }
    return ms;
}

} // namespace loftail
