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

// The two halves of a %d{...} driven as ONE target: compile a date format, then
// read candidate text back with the parser that format produced.
//
// Driving TimestampParser alone would mean hand-building a DateFormat, which is
// exactly the thing CLAUDE.md says must never be re-derived at a distance —
// "TimestampParser reads DateFormat::tokens, never qtFormat". A pair keeps the
// compiler as the only author of a token list, so what is fuzzed is the seam the
// two meet at rather than an invented struct.
//
// Input layout: everything up to the first newline is the inner format (what goes
// inside the braces of %d{...}); the rest is the text to parse with it. A newline
// cannot appear in either half — %n is the compiler's one refusal inside a date
// format, and a record starts at a line (invariant #2) — so the split loses
// nothing the parser could ever see.

#include "FuzzSupport.h"

#include "LogFormat.h"
#include "PatternCompiler.h"
#include "Record.h"
#include "TimestampParser.h"

#include <QByteArray>
#include <QString>
#include <QTimeZone>

using namespace loftail;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size)
{
    if (size > kMaxFuzzInput)
        return 0;

    const QByteArray raw(reinterpret_cast<const char *>(data), qsizetype(size));
    const qsizetype cut = raw.indexOf('\n');
    const QByteArray formatBytes = cut < 0 ? raw : raw.left(cut);
    const QByteArray textBytes = cut < 0 ? QByteArray() : raw.mid(cut + 1);

    // The braces are supplied here rather than by the fuzzer, so that the input is
    // spent on the date vocabulary rather than on rediscovering that `%d{` needs a
    // `}` — which fuzz_patterncompiler already covers from the outside.
    const QString pattern =
        QStringLiteral("%d{") + QString::fromUtf8(formatBytes) + QStringLiteral("} %m%n");

    auto compiled = PatternCompiler::compile(pattern);
    if (!compiled)
        return 0;

    const LogFormat f = compiled.value();
    if (!f.impliedDateFormat.isValid)
        return 0;

    // Both zones are built ONCE. QTimeZone::systemTimeZone() re-reads the system
    // zone database, which is a file system walk per call — the indexer builds one
    // parser per document and would never notice, a fuzzer executing thousands of
    // inputs a second spends the whole run in it.
    static const QTimeZone utc = QTimeZone::utc();
    static const QTimeZone local = QTimeZone::systemTimeZone();
    const QTimeZone &zone = f.impliedZone == Qt::UTC ? utc : local;
    const TimestampParser parser(f.impliedDateFormat, zone);
    if (!parser.isValid())
        return 0;

    const QString text = QString::fromUtf8(textBytes);
    const qint64 ms = parser.parse(text);

    // A parse either refuses in the one documented way or answers an instant Qt
    // itself can hold. Anything else is a value that will be sorted, filtered and
    // rendered as a date somewhere downstream (invariant #10).
    if (ms != Record::kNoTimestamp) {
        FUZZ_CHECK(ms > Record::kNoTimestamp, "a parsed timestamp is not the refusal sentinel");
        const QDateTime dt = QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::utc());
        FUZZ_CHECK(dt.isValid(), "a parsed timestamp is a datetime Qt can hold");
    }

    // Parsing is a pure function of (format, text): the year inference is settled
    // once at construction precisely so that a whole index pass agrees with itself,
    // so the same parser must answer the same thing twice.
    FUZZ_CHECK(parser.parse(text) == ms, "parse() is deterministic for one parser");
    return 0;
}
