#include <QtTest>

#include "Decoder.h"
#include "FormatPreview.h"
#include "LogFormat.h"
#include "PatternCompiler.h"

using namespace loftail;

// M3 — the pure preview / field-breakdown builder (SPEC.md §4). It splits sample
// bytes into records with the indexer's record-start rule (invariant #2) through
// the Decoder (invariant #8), with no LogSource, file, or QApplication.
class TestFormatPreview : public QObject
{
    Q_OBJECT

private:
    static constexpr auto kPattern = "%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n";

    static LogFormat fmt()
    {
        auto r = PatternCompiler::compile(QString::fromUtf8(kPattern));
        Q_ASSERT(r);
        return r.value();
    }

    static Decoder dec(const QByteArray &sample)
    {
        return Decoder::detect(sample, Encoding::Utf8);
    }

private slots:
    void headersAndFieldBreakdown();
    void multiLineFoldsIntoMessage();
    void badPatternAllUnparsed();
    void leadingUnparsedThenMatch();
    void respectsMaxRecords();
};

void TestFormatPreview::headersAndFieldBreakdown()
{
    const QByteArray log =
        "2026-07-21 14:32:05,123 [main] INFO  net.socket - Connection opened\n"
        "2026-07-21 14:32:06,000 [worker] WARN  db.pool - Pool exhausted\n";

    const PreviewResult pv = FormatPreview::build(fmt(), log, dec(log));

    QCOMPARE(pv.headers, (QStringList{QStringLiteral("Time"), QStringLiteral("Thread"),
                                      QStringLiteral("Priority"), QStringLiteral("Subsystem"),
                                      QStringLiteral("Message")}));
    QCOMPARE(pv.totalCount, 2);
    QCOMPARE(pv.matchedCount, 2);

    const PreviewRow &r0 = pv.rows.at(0);
    QVERIFY(r0.matched);
    QCOMPARE(r0.fields.at(0), QStringLiteral("2026-07-21 14:32:05,123"));
    QCOMPARE(r0.fields.at(1), QStringLiteral("main"));
    QCOMPARE(r0.fields.at(2), QStringLiteral("INFO"));
    QCOMPARE(r0.fields.at(3), QStringLiteral("net.socket"));
    QCOMPARE(r0.fields.at(4), QStringLiteral("Connection opened"));
}

void TestFormatPreview::multiLineFoldsIntoMessage()
{
    const QByteArray log =
        "2026-07-21 14:32:05,123 [main] ERROR app.core - Exception:\n"
        "    at foo()\n"
        "    at bar()\n";

    const PreviewResult pv = FormatPreview::build(fmt(), log, dec(log));
    QCOMPARE(pv.totalCount, 1);   // continuations are not separate records (#2)
    QCOMPARE(pv.matchedCount, 1);
    const QString msg = pv.rows.at(0).fields.at(4);
    QVERIFY(msg.startsWith(QStringLiteral("Exception:")));
    QVERIFY(msg.contains(QStringLiteral("at foo()")));
    QVERIFY(msg.contains(QStringLiteral("at bar()")));
}

void TestFormatPreview::badPatternAllUnparsed()
{
    // An empty/uncompiled format previews every line as plain text (SPEC.md §4).
    const QByteArray log =
        "not a log line at all\n"
        "another mystery line\n";

    const PreviewResult pv = FormatPreview::build(LogFormat(), log, dec(log));
    QCOMPARE(pv.matchedCount, 0);
    QCOMPARE(pv.totalCount, 2);
    QVERIFY(!pv.rows.at(0).matched);
    QCOMPARE(pv.rows.at(0).rawFirstLine, QStringLiteral("not a log line at all"));
}

void TestFormatPreview::leadingUnparsedThenMatch()
{
    const QByteArray log =
        "### banner, not a record ###\n"
        "2026-07-21 14:32:05,123 [main] INFO  app - real record\n";

    const PreviewResult pv = FormatPreview::build(fmt(), log, dec(log));
    QCOMPARE(pv.totalCount, 2);
    QCOMPARE(pv.matchedCount, 1);
    QVERIFY(!pv.rows.at(0).matched);
    QVERIFY(pv.rows.at(1).matched);
    QCOMPARE(pv.rows.at(1).fields.at(3), QStringLiteral("app"));
}

void TestFormatPreview::respectsMaxRecords()
{
    QByteArray log;
    for (int i = 0; i < 50; ++i)
        log += "2026-07-21 14:32:05,123 [main] INFO  app - line\n";

    const PreviewResult pv = FormatPreview::build(fmt(), log, dec(log), /*maxRecords=*/10);
    QCOMPARE(pv.totalCount, 10);
    QCOMPARE(pv.matchedCount, 10);
}

QTEST_APPLESS_MAIN(TestFormatPreview)
#include "tst_formatpreview.moc"
