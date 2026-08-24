#include "TimestampParser.h"

#include <utility>

namespace loftail {

namespace {

// Consume a run of the same letter starting at i; return the run length.
int runLength(const QString &s, int i)
{
    const QChar c = s.at(i);
    int j = i;
    while (j < s.size() && s.at(j) == c)
        ++j;
    return j - i;
}

} // namespace

TimestampParser::TimestampParser(const QString &qtFormat, QTimeZone sourceZone)
    : m_zone(std::move(sourceZone)), m_qtFormat(qtFormat)
{
    if (qtFormat.isEmpty())
        return;

    m_valid = true;
    m_fastPath = true;

    int i = 0;
    while (i < qtFormat.size()) {
        const QChar c = qtFormat.at(i);
        if (c == QLatin1Char('\'')) {
            // Quoted literal: copy verbatim until the closing quote.
            ++i;
            while (i < qtFormat.size() && qtFormat.at(i) != QLatin1Char('\'')) {
                m_tokens.append({Tok::Literal, 1, qtFormat.at(i)});
                ++i;
            }
            if (i < qtFormat.size())
                ++i; // skip closing quote
            continue;
        }

        auto pushNumeric = [&](Tok kind, int width) {
            m_tokens.append({kind, width, QChar()});
        };

        if (c == QLatin1Char('y')) {
            const int n = runLength(qtFormat, i);
            pushNumeric(n >= 4 ? Tok::Year4 : Tok::Year2, n >= 4 ? 4 : 2);
            i += n;
        } else if (c == QLatin1Char('M')) {
            const int n = runLength(qtFormat, i);
            pushNumeric(Tok::Month, 2);
            i += n;
            if (n > 2) m_fastPath = false; // MMM/MMMM (names) — fall back
        } else if (c == QLatin1Char('d')) {
            const int n = runLength(qtFormat, i);
            pushNumeric(Tok::Day, 2);
            i += n;
            if (n > 2) m_fastPath = false;
        } else if (c == QLatin1Char('H')) {
            const int n = runLength(qtFormat, i);
            pushNumeric(Tok::Hour24, 2);
            i += n;
        } else if (c == QLatin1Char('h')) {
            const int n = runLength(qtFormat, i);
            pushNumeric(Tok::Hour12, 2);
            i += n;
            m_fastPath = false; // 12-hour needs an AM/PM token we do not fast-parse
        } else if (c == QLatin1Char('m')) {
            const int n = runLength(qtFormat, i);
            pushNumeric(Tok::Minute, 2);
            i += n;
        } else if (c == QLatin1Char('s')) {
            const int n = runLength(qtFormat, i);
            pushNumeric(Tok::Second, 2);
            i += n;
        } else if (c == QLatin1Char('z')) {
            const int n = runLength(qtFormat, i);
            pushNumeric(Tok::Millis, 3);
            i += n;
        } else if (c == QLatin1Char('A') || c == QLatin1Char('a')) {
            m_fastPath = false; // AM/PM marker
            ++i;
        } else {
            m_tokens.append({Tok::Literal, 1, c});
            ++i;
        }
    }
}

qint64 TimestampParser::parse(QStringView text) const
{
    if (!m_valid)
        return Record::kNoTimestamp;

    if (!m_fastPath) {
        QDateTime dt = QDateTime::fromString(text.toString(), m_qtFormat);
        if (!dt.isValid())
            return Record::kNoTimestamp;
        dt.setTimeZone(m_zone);
        return dt.toMSecsSinceEpoch();
    }

    int year = 1970;
    int month = 1;
    int day = 1;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int millis = 0;
    int pos = 0;
    const int n = int(text.size());

    auto readInt = [&](int maxWidth, int &out) -> bool {
        int value = 0;
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

    for (const Token &tok : m_tokens) {
        switch (tok.kind) {
        case Tok::Year4: if (!readInt(4, year)) return Record::kNoTimestamp; break;
        case Tok::Year2:
            if (!readInt(2, year)) return Record::kNoTimestamp;
            year += (year < 70) ? 2000 : 1900;
            break;
        case Tok::Month:  if (!readInt(2, month))  return Record::kNoTimestamp; break;
        case Tok::Day:    if (!readInt(2, day))    return Record::kNoTimestamp; break;
        // Hour12 reads exactly as Hour24 does because it never gets here: a 'h' run
        // clears m_fastPath (12-hour needs an AM/PM token this parser does not carry),
        // so such a format goes to QDateTime instead. The label stays for exhaustiveness.
        case Tok::Hour24:
        case Tok::Hour12: if (!readInt(2, hour))   return Record::kNoTimestamp; break;
        case Tok::Minute: if (!readInt(2, minute)) return Record::kNoTimestamp; break;
        case Tok::Second: if (!readInt(2, second)) return Record::kNoTimestamp; break;
        case Tok::Millis: if (!readInt(3, millis)) return Record::kNoTimestamp; break;
        case Tok::Literal:
            if (pos >= n || text[pos] != tok.literal)
                return Record::kNoTimestamp;
            ++pos;
            break;
        }
    }

    const QDate date(year, month, day);
    const QTime time(hour, minute, second, millis);
    if (!date.isValid() || !time.isValid())
        return Record::kNoTimestamp;

    // Construct directly in the source zone (fast relative to string parsing) and
    // read back UTC epoch ms — the single "in" conversion of §5.1.
    const QDateTime dt(date, time, m_zone);
    if (!dt.isValid())
        return Record::kNoTimestamp;
    return dt.toMSecsSinceEpoch();
}

} // namespace loftail
