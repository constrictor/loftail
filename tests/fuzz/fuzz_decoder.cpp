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

// Decoder over arbitrary bytes: the detection sniff, the line walk and the
// decode, plus the encode that reverses it.
//
// This is invariant #8's layer — "the easiest invariant to violate by accident" —
// and it is the one that has already been violated for real: bugs.md 36, where a
// 64 KB sample cut inside a multi-byte character reached a stateless converter as
// an error and flipped a whole UTF-8 file to the system codepage. So the first
// byte of the input picks the requested encoding and the sample is deliberately
// CUT at an arbitrary point of the remaining bytes, which is that bug's shape:
// what detect() is handed is a prefix of a file, and a prefix is where a sequence
// runs off the end.
//
// The line walk is the other half. lineEnd() is asked to advance from every
// boundary it returns until the buffer is exhausted, and the property asserted is
// the one an indexer's single forward pass depends on absolutely: every step goes
// FORWARD. A step that returns its own argument is not a wrong answer, it is a
// scan that never finishes — and the indexer runs it on a worker thread over a
// file the user is watching, so it presents as a hang with no output.

#include "FuzzSupport.h"

#include "Decoder.h"
#include "Encoding.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

using namespace loftail;

namespace {

Encoding encodingFor(std::uint8_t selector)
{
    switch (selector % 4) {
    case 0:
        return Encoding::Auto;
    case 1:
        return Encoding::Utf8;
    case 2:
        return Encoding::Utf16LE;
    default:
        return Encoding::Utf16BE;
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size)
{
    if (size > kMaxFuzzInput || size < 2)
        return 0;

    const Encoding requested = encodingFor(data[0]);
    const std::uint8_t cutSelector = data[1];
    const QByteArray body(reinterpret_cast<const char *>(data + 2), qsizetype(size - 2));

    // The detection sample is a PREFIX of the content, of a length the fuzzer
    // chooses — bugs.md 36's cut. detect() must survive being handed a sequence
    // that stops halfway, and must then decode the whole content anyway.
    const qsizetype cut = body.isEmpty() ? 0 : qsizetype(cutSelector) % body.size();
    const Decoder dec = Decoder::detect(QByteArrayView(body).first(cut), requested);

    FUZZ_CHECK(dec.unitSize() == 1 || dec.unitSize() == 2, "a code unit is one byte or two");
    FUZZ_CHECK(dec.bomLength() >= 0, "a BOM length is not negative");
    FUZZ_CHECK(dec.bomLength() <= body.size() + 4, "a BOM is not longer than the file");
    if (requested != Encoding::Auto)
        FUZZ_CHECK(dec.requestedEncoding() == requested, "a forced encoding is honoured");

    const QByteArrayView all(body);
    qsizetype pos = qMin<qsizetype>(dec.bomLength(), all.size());
    int lines = 0;
    while (pos < all.size()) {
        bool hadNewline = false;
        const qsizetype end = dec.lineEnd(all, pos, &hadNewline);

        FUZZ_CHECK(end > pos, "the line walk always moves forward");
        FUZZ_CHECK(end <= all.size(), "a line ends inside the buffer");
        if (!hadNewline)
            FUZZ_CHECK(end == all.size(), "a line with no terminator runs to the end");

        // The content of the line is everything but its terminator, which is what
        // the indexer hands to decodeLine.
        const qsizetype contentEnd = hadNewline ? qMax<qsizetype>(pos, end - dec.unitSize()) : end;
        const QString text = dec.decodeLine(all.sliced(pos, contentEnd - pos));

        // The round trip is the config editor's rule: one object owns both
        // directions so an encoder and a decoder cannot disagree about what a file
        // is (Decoder::encode). Asserted one way only — re-decoding what we
        // encoded must give the same text back — because arbitrary bytes need not
        // be the encoding of anything; and only where the resolved encoding is
        // UTF-8, because the System codepage is lossy BY DEFINITION (a character
        // it cannot spell is a substitution, not a bug) and Qt's UTF-16 decoder
        // replaces an unpaired surrogate rather than keeping it.
        //
        // And not for text whose FIRST character is U+FEFF: decode() drops one
        // there and encode() writes one back, so a line that begins with a
        // zero-width no-break space comes back one character shorter. That is a
        // real asymmetry, found by this target in its first minute and REPORTED
        // rather than fixed (it is Qt's UTF-8 decoder treating a leading BOM as a
        // BOM wherever the range starts); corpus/decoder/utf8_leading_bom_in_line
        // is the minimised input, kept so the day it is fixed the case is here.
        if (dec.resolvedEncoding() == Encoding::Utf8
            && !(!text.isEmpty() && text.at(0) == QChar(0xFEFF))) {
            const QByteArray reEncoded = dec.encode(text);
            FUZZ_CHECK(dec.decode(reEncoded) == text, "decode(encode(text)) is text in UTF-8");
        }

        pos = end;
        if (++lines > 100000)
            break;
    }

    (void)dec.decode(all);
    return 0;
}
