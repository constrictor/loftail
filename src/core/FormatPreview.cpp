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

#include "FormatPreview.h"

#include "Decoder.h"

#include <QRegularExpression>

namespace loftail {

namespace {
// Logical column of the Message field, or -1. Continuation lines append here.
int messageColumn(const LogFormat &format)
{
    for (int i = 0; i < format.fields.size(); ++i) {
        if (format.fields.at(i).role == FieldRole::Message)
            return i;
    }
    return -1;
}
} // namespace

PreviewResult FormatPreview::build(const LogFormat &format, QByteArrayView sample,
                                   const Decoder &decoder, int maxRecords)
{
    PreviewResult out;
    for (const Field &f : format.fields)
        out.headers << f.name;

    const bool haveFormat = !format.recordStartRe.pattern().isEmpty()
                            && format.recordStartRe.isValid();
    const int unit = decoder.unitSize();
    const int msgCol = messageColumn(format);
    const qsizetype size = sample.size();

    int  openRow = -1;      // row currently open for continuations, or -1
    bool openMatched = false;

    qsizetype pos = decoder.bomLength();
    while (pos < size && out.rows.size() < maxRecords) {
        const qsizetype start = pos;
        bool hadNl = false;
        const qsizetype end = decoder.lineEnd(sample, start, &hadNl);
        const qsizetype contentLen = (end - start) - (hadNl ? unit : 0);
        const QString line =
            decoder.decodeLine(sample.sliced(start, qMax<qsizetype>(0, contentLen)));
        pos = end;
        if (pos <= start)
            break; // no forward progress (defensive; empty lines still advance)

        bool isStart = false;
        if (haveFormat)
            isStart = format.recordStartRe.match(line).hasMatch();

        if (isStart) {
            PreviewRow row;
            row.matched = true;
            row.rawFirstLine = line;
            const QRegularExpressionMatch fm = format.recordRe.match(line);
            for (const Field &f : format.fields)
                row.fields << ((f.group > 0 && fm.hasMatch()) ? fm.captured(f.group) : QString());
            out.rows.push_back(row);
            out.matchedCount++;
            openRow = int(out.rows.size()) - 1;
            openMatched = true;
        } else if (openRow >= 0 && openMatched) {
            // Continuation of a matched record (invariant #2): fold it into the
            // message field so multi-line records preview at full text.
            PreviewRow &row = out.rows[openRow];
            if (msgCol >= 0 && msgCol < row.fields.size())
                row.fields[msgCol] += QLatin1Char('\n') + line;
        } else {
            // A leading non-matching line, or the whole pattern is wrong: show it
            // as one plain-text row (SPEC.md §4).
            PreviewRow row;
            row.matched = false;
            row.rawFirstLine = line;
            for (int k = 0; k < format.fields.size(); ++k)
                row.fields << QString();
            out.rows.push_back(row);
            openRow = int(out.rows.size()) - 1;
            openMatched = false;
        }
    }

    out.totalCount = int(out.rows.size());
    return out;
}

} // namespace loftail
