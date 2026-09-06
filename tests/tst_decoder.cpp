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

using namespace loftail;

// Decoder coverage: the §6.1 line-terminator trap (invariant #8). The same text
// in UTF-8, UTF-16LE, and UTF-16BE must yield identical lines, and a naive '\n'
// byte search must never be what finds a boundary.
class TestDecoder : public QObject
{
    Q_OBJECT

private:
    // Encode "AB\nC" style content in a given encoding, no BOM.
    static QByteArray utf16(const QString &s, bool bigEndian)
    {
        QByteArray out;
        for (QChar ch : s) {
            const ushort u = ch.unicode();
            if (bigEndian) {
                out.append(char(u >> 8));
                out.append(char(u & 0xFF));
            } else {
                out.append(char(u & 0xFF));
                out.append(char(u >> 8));
            }
        }
        return out;
    }

private slots:
    void detectsBom_data();
    void detectsBom();

    void detectsUtf16NoBom();
    void aBomIsSkippedUnderAForcedEncodingAsWell_data();
    void aBomIsSkippedUnderAForcedEncodingAsWell();

    void aSampleCutInsideACharacterIsStillUtf8_data();
    void aSampleCutInsideACharacterIsStillUtf8();
    void anInvalidByteInTheMiddleOfTheSampleStillAnswersSystem();
    void aTrimmedSampleStillCountsEveryNulForTheParityTest();

    void splitsLinesUtf8();
    void splitsLinesUtf16LE();
    void splitsLinesUtf16BE();

    void stripsCrlf();

    void sameLinesAcrossEncodings();

    void textIsWrittenBackInTheEncodingItWasReadIn_data();
    void textIsWrittenBackInTheEncodingItWasReadIn();
    void encodingAddsNoByteOrderMarkOfItsOwn();
};

void TestDecoder::detectsBom_data()
{
    QTest::addColumn<QByteArray>("bytes");
    QTest::addColumn<int>("resolved"); // Encoding as int
    QTest::addColumn<int>("bomLength");

    QTest::newRow("utf8 bom")
        << QByteArray("\xEF\xBB\xBFhello", 8) << int(Encoding::Utf8) << 3;
    QTest::newRow("utf16le bom")
        << QByteArray("\xFF\xFEh\x00", 4) << int(Encoding::Utf16LE) << 2;
    QTest::newRow("utf16be bom")
        << QByteArray("\xFE\xFF\x00h", 4) << int(Encoding::Utf16BE) << 2;
    QTest::newRow("plain ascii -> utf8")
        << QByteArray("hello world") << int(Encoding::Utf8) << 0;
}

void TestDecoder::detectsBom()
{
    QFETCH(QByteArray, bytes);
    QFETCH(int, resolved);
    QFETCH(int, bomLength);

    const Decoder d = Decoder::detect(bytes, Encoding::Auto);
    QCOMPARE(int(d.resolvedEncoding()), resolved);
    QCOMPARE(int(d.bomLength()), bomLength);
}

void TestDecoder::detectsUtf16NoBom()
{
    // ASCII-heavy UTF-16LE with no BOM: NULs at odd byte positions.
    const QByteArray le = utf16(QStringLiteral("2026-07-21 INFO hello\nnext line here\n"), false);
    const Decoder dle = Decoder::detect(le, Encoding::Auto);
    QCOMPARE(dle.resolvedEncoding(), Encoding::Utf16LE);

    const QByteArray be = utf16(QStringLiteral("2026-07-21 INFO hello\nnext line here\n"), true);
    const Decoder dbe = Decoder::detect(be, Encoding::Auto);
    QCOMPARE(dbe.resolvedEncoding(), Encoding::Utf16BE);
}

// A forced encoding skips detection, but a file that carries the byte-order mark
// of that very encoding still has one, and the content starts after it (SPEC.md
// §4: "UTF-8, BOM tolerated and skipped"). The mark must MATCH what was forced —
// the two bytes of a UTF-16BE mark are ordinary content to a reader that was told
// the file is little-endian — and the system codepage has no mark at all, so a
// file forced to it keeps every byte it has.
void TestDecoder::aBomIsSkippedUnderAForcedEncodingAsWell_data()
{
    QTest::addColumn<QByteArray>("bytes");
    QTest::addColumn<int>("forced");
    QTest::addColumn<int>("bomLength");

    const QByteArray u8Bom("\xEF\xBB\xBF", 3);
    const QByteArray leBom("\xFF\xFE", 2);
    const QByteArray beBom("\xFE\xFF", 2);

    QTest::newRow("utf8, its own mark")     << u8Bom + "hello" << int(Encoding::Utf8)    << 3;
    QTest::newRow("utf8, no mark")          << QByteArray("hello") << int(Encoding::Utf8) << 0;
    QTest::newRow("utf16le, its own mark")  << leBom + QByteArray("h\x00", 2) << int(Encoding::Utf16LE) << 2;
    QTest::newRow("utf16le, no mark")       << QByteArray("h\x00", 2) << int(Encoding::Utf16LE) << 0;
    QTest::newRow("utf16be, its own mark")  << beBom + QByteArray("\x00h", 2) << int(Encoding::Utf16BE) << 2;
    QTest::newRow("utf16be, no mark")       << QByteArray("\x00h", 2) << int(Encoding::Utf16BE) << 0;
    // The other byte order's mark is content, not a mark.
    QTest::newRow("utf16le over a be mark") << beBom + QByteArray("\x00h", 2) << int(Encoding::Utf16LE) << 0;
    QTest::newRow("utf16be over a le mark") << leBom + QByteArray("h\x00", 2) << int(Encoding::Utf16BE) << 0;
    // The system codepage has no mark of its own, so nothing is skipped even where
    // the file opens with bytes that would be one under UTF-8.
    QTest::newRow("system over a utf8 mark") << u8Bom + "hello" << int(Encoding::System) << 0;
    QTest::newRow("system, plain")           << QByteArray("hello") << int(Encoding::System) << 0;
}

void TestDecoder::aBomIsSkippedUnderAForcedEncodingAsWell()
{
    QFETCH(QByteArray, bytes);
    QFETCH(int, forced);
    QFETCH(int, bomLength);

    const Decoder d = Decoder::detect(bytes, Encoding(forced));
    // Forced means forced: detection never gets a say, whatever the bytes look like.
    QCOMPARE(int(d.resolvedEncoding()), forced);
    QCOMPARE(int(d.requestedEncoding()), forced);
    QCOMPARE(d.bomLength(), qint64(bomLength));
    // The mark is measured in bytes and the content that follows it is code-unit
    // aligned, which is the relation the indexer reads both of these for.
    QCOMPARE(d.bomLength() % d.unitSize(), qint64(0));
}

// bugs.md 36. The detection sample is the first ~64 KB of the file, cut at an
// arbitrary BYTE boundary — so a multi-byte character straddling that cut used
// to make the Stateless validation report an error and answer System, which on
// Windows is the ANSI codepage and renders the whole log as mojibake. Every
// sequence length has a cut point of its own, so all three are driven here; the
// text before the cut is ordinary UTF-8 log text and must decide the answer.
void TestDecoder::aSampleCutInsideACharacterIsStillUtf8_data()
{
    QTest::addColumn<QByteArray>("character"); // the one straddling the cut
    QTest::addColumn<int>("kept");             // bytes of it left in the sample

    QTest::newRow("2-byte, 1 kept")  << QByteArray("\xD0\x96", 2)             << 1; // Ж
    QTest::newRow("3-byte, 1 kept")  << QByteArray("\xE4\xB8\xAD", 3)         << 1; // 中
    QTest::newRow("3-byte, 2 kept")  << QByteArray("\xE4\xB8\xAD", 3)         << 2;
    QTest::newRow("4-byte, 1 kept")  << QByteArray("\xF0\x9F\x98\x80", 4)     << 1; // 😀
    QTest::newRow("4-byte, 2 kept")  << QByteArray("\xF0\x9F\x98\x80", 4)     << 2;
    QTest::newRow("4-byte, 3 kept")  << QByteArray("\xF0\x9F\x98\x80", 4)     << 3;
}

void TestDecoder::aSampleCutInsideACharacterIsStillUtf8()
{
    QFETCH(QByteArray, character);
    QFETCH(int, kept);

    // A realistic line: ASCII timestamp and level in front of non-ASCII text,
    // repeated past the ~64 KB Document::openAndSettleFormat() hands over, and
    // then the one character the slice's end lands inside, cut. Whole lines
    // first so the ONLY incomplete sequence is the trailing one under test.
    QByteArray sample;
    while (sample.size() < 64 * 1024)
        sample += QByteArray("2026-08-29 10:11:12 INFO  app - ") + character + character + "\n";
    sample += character.left(kept);
    QVERIFY(sample.size() > 64 * 1024);

    const Decoder d = Decoder::detect(sample, Encoding::Auto);
    QCOMPARE(d.resolvedEncoding(), Encoding::Utf8);
    QCOMPARE(d.bomLength(), qint64(0));
    QCOMPARE(d.unitSize(), 1);
}

// The other half of the same rule: the trim walks back at most three bytes from
// the END, so text that is genuinely not UTF-8 is still reported as such. Left
// out, the fix would be a licence to decode invalid bytes as UTF-8.
void TestDecoder::anInvalidByteInTheMiddleOfTheSampleStillAnswersSystem()
{
    QByteArray sample("2026-08-29 10:11:12 INFO  app - caf\xE9 latin-1 text\n");
    sample += QByteArray("2026-08-29 10:11:13 INFO  app - plain ascii tail\n");
    QCOMPARE(int(Decoder::detect(sample, Encoding::Auto).resolvedEncoding()), int(Encoding::System));

    // And a trailing byte that is not the head of any sequence is not a cut
    // character either: 0x80 alone is a stranded continuation byte.
    QByteArray stray("plain ascii and then\x80", 21);
    QCOMPARE(int(Decoder::detect(stray, Encoding::Auto).resolvedEncoding()), int(Encoding::System));

    // Nor is a byte that could never lead any sequence at all — 0xFE and 0xFF are
    // not UTF-8 in any position. The walk-back has to say so rather than treat the
    // tail as an incomplete character and trim it away, which would hand back a
    // prefix that validates and call a file UTF-8 on the strength of the bytes it
    // dropped.
    for (const char bad : {char(0xF8), char(0xFE), char(0xFF)}) {
        QByteArray tail("plain ascii and then");
        tail.append(bad);
        QCOMPARE(int(Decoder::detect(tail, Encoding::Auto).resolvedEncoding()),
                 int(Encoding::System));
    }
}

// The trim is step 3's alone: step 2's NUL-parity count runs before it and is a
// frequency over the bytes that are actually there, so hoisting the trim above
// that count would change the denominator. It can only move it ONE way — the
// bytes a trim eats are a lead byte and its continuations, never a NUL — so a
// hoist raises the fraction and calls UTF-16 on a sample the rule says is not.
//
// A plain UTF-16 sample cannot show that and the obvious case is a trap: a
// UTF-16LE sample chopped by a byte ends on the low byte of its last code unit,
// which for ASCII text is an ordinary character, so completeUtf8Prefix() trims
// NOTHING and the case passes whichever side of step 2 the trim sits on. The
// sample therefore has to be built ON the 0.20 bar. 4 NULs in 21 bytes is
// 0.190 and falls through to step 3, which trims and answers UTF-8; the same 4
// NULs in the 18 bytes a hoisted trim would leave is 0.222 and answers
// UTF-16LE. The tail is three bytes of a four-byte character, which is the
// deepest cut there is and so the largest denominator change available.
void TestDecoder::aTrimmedSampleStillCountsEveryNulForTheParityTest()
{
    QByteArray sample("a\0b\0c\0d\0", 8);      // 4 NULs, all at odd positions
    sample += QByteArray("INFO  app ", 10);    // ASCII, no NULs
    sample += QByteArray("\xF0\x9F\x98", 3);   // 😀, cut three bytes in

    // The bar is what the case turns on, so state it rather than leaving it in
    // the byte counts above, where an edit to the text would silently step off
    // it and leave the case guarding nothing.
    qsizetype nuls = 0;
    for (char c : sample) {
        if (c == '\0')
            ++nuls;
    }
    QCOMPARE(sample.size(), qsizetype(21));
    QCOMPARE(nuls, qsizetype(4));
    QVERIFY(double(nuls) / double(sample.size()) <= 0.20);     // as it stands
    QVERIFY(double(nuls) / double(sample.size() - 3) > 0.20);  // after a hoist

    QCOMPARE(Decoder::detect(sample, Encoding::Auto).resolvedEncoding(), Encoding::Utf8);
}

void TestDecoder::splitsLinesUtf8()
{
    const QByteArray data("alpha\nbeta\ngamma", 16);
    const Decoder d = Decoder::detect(data, Encoding::Utf8);

    bool nl = false;
    qsizetype e0 = d.lineEnd(data, 0, &nl);
    QVERIFY(nl);
    QCOMPARE(e0, qsizetype(6)); // past the first '\n'
    QCOMPARE(d.decodeLine(data.sliced(0, e0 - 1)), QStringLiteral("alpha"));

    qsizetype e1 = d.lineEnd(data, e0, &nl);
    QVERIFY(nl);
    QCOMPARE(d.decodeLine(data.sliced(e0, e1 - e0 - 1)), QStringLiteral("beta"));

    qsizetype e2 = d.lineEnd(data, e1, &nl);
    QVERIFY(!nl); // last line has no terminator
    QCOMPARE(e2, qsizetype(data.size()));
    QCOMPARE(d.decodeLine(data.sliced(e1, e2 - e1)), QStringLiteral("gamma"));
}

void TestDecoder::splitsLinesUtf16LE()
{
    const QByteArray data = utf16(QStringLiteral("ab\ncd"), false);
    const Decoder d = Decoder::detect(data, Encoding::Utf16LE);
    QCOMPARE(d.unitSize(), 2);

    bool nl = false;
    // "ab\n" = 3 code units = 6 bytes; the '\n' code unit is bytes 4..5.
    qsizetype e0 = d.lineEnd(data, 0, &nl);
    QVERIFY(nl);
    QCOMPARE(e0, qsizetype(6));
    QCOMPARE(d.decodeLine(data.sliced(0, e0 - 2)), QStringLiteral("ab"));
}

void TestDecoder::splitsLinesUtf16BE()
{
    const QByteArray data = utf16(QStringLiteral("ab\ncd"), true);
    const Decoder d = Decoder::detect(data, Encoding::Utf16BE);
    QCOMPARE(d.unitSize(), 2);

    bool nl = false;
    qsizetype e0 = d.lineEnd(data, 0, &nl);
    QVERIFY(nl);
    QCOMPARE(e0, qsizetype(6));
    QCOMPARE(d.decodeLine(data.sliced(0, e0 - 2)), QStringLiteral("ab"));
}

void TestDecoder::stripsCrlf()
{
    const QByteArray data("line\r\nrest", 10);
    const Decoder d = Decoder::detect(data, Encoding::Utf8);
    bool nl = false;
    qsizetype e0 = d.lineEnd(data, 0, &nl);
    QVERIFY(nl);
    QCOMPARE(e0, qsizetype(6));
    // Content excludes the '\n'; decodeLine strips the trailing '\r'.
    QCOMPARE(d.decodeLine(data.sliced(0, e0 - 1)), QStringLiteral("line"));
}

void TestDecoder::sameLinesAcrossEncodings()
{
    const QString text = QStringLiteral("first line\nsecond line\nthird");

    auto linesOf = [](QByteArrayView buf, const Decoder &d) {
        QStringList out;
        qsizetype pos = d.bomLength();
        while (pos < buf.size()) {
            bool nl = false;
            qsizetype end = d.lineEnd(buf, pos, &nl);
            qsizetype contentLen = (end - pos) - (nl ? d.unitSize() : 0);
            out << d.decodeLine(buf.sliced(pos, qMax<qsizetype>(0, contentLen)));
            pos = end;
        }
        return out;
    };

    const QByteArray u8 = text.toUtf8();
    const QByteArray le = utf16(text, false);
    const QByteArray be = utf16(text, true);

    const QStringList expect{QStringLiteral("first line"), QStringLiteral("second line"),
                             QStringLiteral("third")};
    QCOMPARE(linesOf(u8, Decoder::detect(u8, Encoding::Utf8)), expect);
    QCOMPARE(linesOf(le, Decoder::detect(le, Encoding::Utf16LE)), expect);
    QCOMPARE(linesOf(be, Decoder::detect(be, Encoding::Utf16BE)), expect);
}

// Decoder::encode() is the reverse direction on the same object, which is what
// stops the config-file editor writing a UTF-16 file back as UTF-8 (§6.8): one
// class owns both directions, so the two cannot disagree about what the file is.
// The claim here is a relation and not a byte table — whatever decode() reads,
// encode() writes back, and reading that again gives the text unchanged.
void TestDecoder::textIsWrittenBackInTheEncodingItWasReadIn_data()
{
    QTest::addColumn<QByteArray>("sample"); // what the file opened with
    QTest::addColumn<int>("resolved");

    QTest::newRow("utf8")    << QByteArray("2026-08-27 INFO app - hello")   << int(Encoding::Utf8);
    QTest::newRow("utf16le") << utf16(QStringLiteral("2026-08-27 INFO hello\nmore\n"), false)
                             << int(Encoding::Utf16LE);
    QTest::newRow("utf16be") << utf16(QStringLiteral("2026-08-27 INFO hello\nmore\n"), true)
                             << int(Encoding::Utf16BE);
    // Latin-1 in the middle of the sample is not UTF-8, so §6.1 step 3 answers the
    // system codepage — the one resolution whose bytes belong to the machine.
    QTest::newRow("system")  << QByteArray("2026-08-27 INFO app - caf\xE9 here")
                             << int(Encoding::System);
}

void TestDecoder::textIsWrittenBackInTheEncodingItWasReadIn()
{
    QFETCH(QByteArray, sample);
    QFETCH(int, resolved);

    const Decoder d = Decoder::detect(sample, Encoding::Auto);
    QCOMPARE(int(d.resolvedEncoding()), resolved);

    // ASCII only, so the round trip means the same thing under a system codepage
    // this machine may have set to anything at all.
    const QString text = QStringLiteral("2026-08-27 10:15:01 INFO  app - re-written");
    const QByteArray written = d.encode(text);
    QCOMPARE(d.decode(written), text);
    // And the encoding is the RESOLVED one, not UTF-8 with a different label: a
    // two-byte encoding spends two bytes on every one of those ASCII characters.
    QCOMPARE(written.size(), qsizetype(text.size()) * d.unitSize());
}

// The BOM is the CALLER's to replay, which is what bomLength() is read for: adding
// one here would grow a mark onto a file that never had one, and would put a
// second one on a file that did.
void TestDecoder::encodingAddsNoByteOrderMarkOfItsOwn()
{
    const QString text = QStringLiteral("hello");
    for (const Encoding e : {Encoding::Utf8, Encoding::Utf16LE, Encoding::Utf16BE,
                             Encoding::System}) {
        // A sample that DOES carry a mark, so a decoder that copied one would have
        // one to copy.
        QByteArray sample;
        if (e == Encoding::Utf8)
            sample = QByteArray("\xEF\xBB\xBF", 3);
        else if (e == Encoding::Utf16LE)
            sample = QByteArray("\xFF\xFE", 2);
        else if (e == Encoding::Utf16BE)
            sample = QByteArray("\xFE\xFF", 2);
        sample += (e == Encoding::Utf16LE || e == Encoding::Utf16BE) ? utf16(text, e == Encoding::Utf16BE)
                                                                     : text.toUtf8();

        const Decoder d = Decoder::detect(sample, e);
        const QByteArray written = d.encode(text);
        QCOMPARE(written.size(), qsizetype(text.size()) * d.unitSize());
        QCOMPARE(written, sample.sliced(d.bomLength()));
    }
}

QTEST_APPLESS_MAIN(TestDecoder)
#include "tst_decoder.moc"
