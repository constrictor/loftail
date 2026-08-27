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

#pragma once

#include "LogFormat.h"
#include "Record.h"

#include <QHash>
#include <QString>
#include <QStringView>
#include <QTimeZone>
#include <QVector>

namespace loftail {

// Parses a %d timestamp field into UTC epoch milliseconds (invariant #10, §5.1).
// Zone conversion happens EXACTLY ONCE on the way in, here: the source zone is
// applied to the parsed wall-clock text. Nothing downstream is zone-aware.
//
// Driven by the DateToken list PatternCompiler emitted beside the regex, NOT by
// DateFormat::qtFormat, which is a display string and cannot spell a space-padded
// day, epoch seconds, a UTC offset or %Q's fractional milliseconds (see
// LogFormat.h). Indexing calls parse() once per record on a hot path, so it reads
// integers straight out of the text rather than going through
// QDateTime::fromString — which is kept only as the fallback for a format that
// produced no tokens at all, so correctness never depends on the fast path.
class TimestampParser
{
public:
    TimestampParser() = default;

    // `format` is LogFormat::impliedDateFormat; `sourceZone` is the zone the log
    // was written in (inferred from the pattern or set by the user). A %z in the
    // format overrides the zone per record; a %s ignores it entirely.
    TimestampParser(const DateFormat &format, const QTimeZone &sourceZone);

    bool isValid() const { return m_valid; }

    // Parse one timestamp field. Returns UTC epoch ms, or Record::kNoTimestamp
    // when the text does not match the format.
    qint64 parse(QStringView text) const;

private:
    // A month name to a 1-based month number, lowercased. English AND the system
    // locale, because strftime renders %b in the process's locale while the C
    // locale — every syslog line on the machine — is English.
    QHash<QString, int> m_months;

    QVector<DateToken> m_tokens;
    QTimeZone m_zone;
    QString   m_qtFormat;      // the fromString fallback, for a token-less format
    bool      m_valid = false;

    // A format carrying a month and a day but NO year is the syslog shape
    // ("Aug 27 10:15:01"). The year is inferred from the clock once, here, rather
    // than per record — a whole index pass must agree with itself — and a record
    // that lands in the future is read as last year's, which is how a log written
    // across a New Year reads correctly for all but its own first day.
    bool   m_inferYear = false;
    int    m_assumedYear = 1970;
    qint64 m_futureCutoffMs = 0;
};

} // namespace loftail
