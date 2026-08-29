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

#include "Decoder.h"

#include <QStringEncoder>
#include <QStringDecoder>

namespace loftail {

namespace {

// A concrete (non-Auto) encoding, its BOM length within `sample`, and its unit
// size. Pulled out so detect() reads as a decision list.
struct Resolved
{
    Encoding encoding;
    qint64   bomLength;
    int      unitSize;
};

Resolved resolveForced(Encoding e, QByteArrayView sample)
{
    // A forced encoding skips detection but still tolerates and skips a BOM that
    // matches it (SPEC.md §4: "UTF-8, BOM tolerated and skipped").
    const auto *p = reinterpret_cast<const unsigned char *>(sample.data());
    const qsizetype n = sample.size();
    switch (e) {
    case Encoding::Utf8:
        if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF)
            return {Encoding::Utf8, 3, 1};
        return {Encoding::Utf8, 0, 1};
    case Encoding::Utf16LE:
        if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE)
            return {Encoding::Utf16LE, 2, 2};
        return {Encoding::Utf16LE, 0, 2};
    case Encoding::Utf16BE:
        if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF)
            return {Encoding::Utf16BE, 2, 2};
        return {Encoding::Utf16BE, 0, 2};
    case Encoding::System:
        return {Encoding::System, 0, 1};
    case Encoding::Auto:
        break; // handled by the caller
    }
    return {Encoding::Utf8, 0, 1};
}

// How much of `sample` ends on a complete UTF-8 sequence.
//
// The sample is the first ~64 KB of a file — an arbitrary BYTE boundary, not a
// character or line one — so a multi-byte character straddling the cut arrives
// as a lead byte with its continuation bytes left behind in the file. Step 3
// below validates with a Stateless converter, for which such a tail is an
// *error* rather than carried-over state, so an unaligned cut used to flip the
// whole file to the system codepage: mojibake on Windows, where System is the
// ANSI codepage, on precisely the logs that carry non-ASCII text. Trimming the
// dangling head of the cut character is the whole of the fix.
//
// Two things about it are easy to undo. The trim applies to the VALIDATION
// ONLY — step 2's NUL-parity count must keep seeing the whole sample, since it
// is a frequency over the bytes that are actually there. And only a sequence
// that runs off the END is dropped: an invalid byte anywhere else is left where
// it is, so genuinely non-UTF-8 text still answers System, which is why this
// walks back at most three bytes rather than switching the converter away from
// Stateless (which would accept invalid UTF-8 outright).
qsizetype completeUtf8Prefix(const unsigned char *p, qsizetype n)
{
    // A UTF-8 sequence is at most four bytes, so a cut can strand at most three
    // continuation bytes; anything further back is a complete character or an
    // error that is not this function's business.
    qsizetype i = n - 1;
    while (i >= 0 && (n - i) <= 3 && (p[i] & 0xC0) == 0x80)
        --i;
    if (i < 0)
        return n;

    qsizetype need = 0;
    if (p[i] < 0x80)
        need = 1;
    else if ((p[i] & 0xE0) == 0xC0)
        need = 2;
    else if ((p[i] & 0xF0) == 0xE0)
        need = 3;
    else if ((p[i] & 0xF8) == 0xF0)
        need = 4;
    else
        return n; // not a lead byte at all — a real error, and step 3's to report

    return (i + need > n) ? i : n;
}

// The §6.1 heuristic for a file with no BOM.
Resolved sniff(QByteArrayView sample)
{
    const auto *p = reinterpret_cast<const unsigned char *>(sample.data());
    const qsizetype n = sample.size();

    // 1. BOM is decisive.
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF)
        return {Encoding::Utf8, 3, 1};
    if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE)
        return {Encoding::Utf16LE, 2, 2};
    if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF)
        return {Encoding::Utf16BE, 2, 2};

    // 2. No BOM: count NUL bytes at even vs odd positions. UTF-16 ASCII-heavy
    //    text has a NUL in every other byte; which parity carries them gives the
    //    byte order (LE => NULs at odd positions, BE => at even positions).
    qint64 nulEven = 0;
    qint64 nulOdd = 0;
    qint64 total = 0;
    for (qsizetype i = 0; i < n; ++i) {
        if (p[i] == 0x00) {
            if ((i & 1) == 0)
                ++nulEven;
            else
                ++nulOdd;
        }
        ++total;
    }
    if (total > 0) {
        const double nulFraction = double(nulEven + nulOdd) / double(total);
        if (nulFraction > 0.20) {
            if (nulOdd >= nulEven)
                return {Encoding::Utf16LE, 0, 2};
            return {Encoding::Utf16BE, 0, 2};
        }
    }

    // 3. Validate as UTF-8; on failure fall back to the system codepage. The
    //    sample is cut at a byte boundary, so the last character may be
    //    incomplete through no fault of the file — see completeUtf8Prefix().
    QStringDecoder dec(QStringConverter::Utf8,
                       QStringConverter::Flag::Stateless | QStringConverter::Flag::ConvertInvalidToNull);
    QString decoded = dec.decode(sample.sliced(0, completeUtf8Prefix(p, n)));
    if (dec.hasError())
        return {Encoding::System, 0, 1};
    return {Encoding::Utf8, 0, 1};
}

QStringConverter::Encoding converterFor(Encoding e)
{
    switch (e) {
    case Encoding::Utf16LE: return QStringConverter::Utf16LE;
    case Encoding::Utf16BE: return QStringConverter::Utf16BE;
    case Encoding::System:  return QStringConverter::System;
    case Encoding::Utf8:
    case Encoding::Auto:
        break;
    }
    return QStringConverter::Utf8;
}

} // namespace

Decoder Decoder::detect(QByteArrayView sample, Encoding requested)
{
    Decoder d;
    d.m_requested = requested;
    const Resolved r = (requested == Encoding::Auto) ? sniff(sample) : resolveForced(requested, sample);
    d.m_resolved = r.encoding;
    d.m_bomLength = r.bomLength;
    d.m_unitSize = r.unitSize;
    return d;
}

qsizetype Decoder::lineEnd(QByteArrayView buf, qsizetype from, bool *hadNewline) const
{
    const auto *p = reinterpret_cast<const unsigned char *>(buf.data());
    const qsizetype n = buf.size();
    if (hadNewline)
        *hadNewline = false;

    if (m_unitSize == 1) {
        for (qsizetype i = from; i < n; ++i) {
            if (p[i] == 0x0A) {
                if (hadNewline)
                    *hadNewline = true;
                return i + 1;
            }
        }
        return n;
    }

    // UTF-16: the newline code unit is 0x000A. Step two bytes at a time so a
    // 0x0A that is merely the low/high byte of an unrelated code unit is ignored
    // (invariant #8). `from` is code-unit aligned.
    if (m_resolved == Encoding::Utf16LE) {
        for (qsizetype i = from; i + 1 < n; i += 2) {
            if (p[i] == 0x0A && p[i + 1] == 0x00) {
                if (hadNewline)
                    *hadNewline = true;
                return i + 2;
            }
        }
    } else { // Utf16BE
        for (qsizetype i = from; i + 1 < n; i += 2) {
            if (p[i] == 0x00 && p[i + 1] == 0x0A) {
                if (hadNewline)
                    *hadNewline = true;
                return i + 2;
            }
        }
    }
    return n;
}

QString Decoder::decode(QByteArrayView bytes) const
{
    switch (m_resolved) {
    case Encoding::Utf8:
        return QString::fromUtf8(bytes);
    case Encoding::System:
        return QString::fromLocal8Bit(bytes);
    case Encoding::Utf16LE:
    case Encoding::Utf16BE: {
        QStringDecoder dec(converterFor(m_resolved), QStringConverter::Flag::Stateless);
        return dec.decode(bytes);
    }
    case Encoding::Auto:
        break;
    }
    return QString::fromUtf8(bytes);
}

QByteArray Decoder::encode(const QString &text) const
{
    switch (m_resolved) {
    case Encoding::System:
        return text.toLocal8Bit();
    case Encoding::Utf16LE:
    case Encoding::Utf16BE: {
        // WriteBom is deliberately OFF: whether the file carries one is the caller's
        // fact about the file it read, not this decoder's about the encoding, and
        // adding one unconditionally would grow a BOM onto a file that never had one.
        QStringEncoder enc(converterFor(m_resolved), QStringConverter::Flag::Stateless);
        return enc.encode(text);
    }
    case Encoding::Utf8:
    case Encoding::Auto:
        break;
    }
    return text.toUtf8();
}

QString Decoder::decodeLine(QByteArrayView content) const
{
    QString text = decode(content);
    if (!text.isEmpty() && text.back() == QLatin1Char('\r'))
        text.chop(1);
    return text;
}

} // namespace loftail
