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

    void splitsLinesUtf8();
    void splitsLinesUtf16LE();
    void splitsLinesUtf16BE();

    void stripsCrlf();

    void sameLinesAcrossEncodings();
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

QTEST_APPLESS_MAIN(TestDecoder)
#include "tst_decoder.moc"
