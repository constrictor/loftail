#include <QtTest>

#include <QTemporaryFile>

#include "Document.h"
#include "Record.h"

using namespace loftail;

// M3 — the source-zone change-cost (§5.1, invariant #10): changing the SOURCE zone
// re-derives Record::timestamp over the existing index WITHOUT a rescan — record
// count, offsets, and lengths are untouched. Changing the DISPLAY zone touches no
// timestamp at all. GUILESS: Document maps a real temp file, no QApplication.
class TestDocumentZone : public QObject
{
    Q_OBJECT

private:
    static constexpr auto kPattern = "%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n";

    static bool writeLog(QTemporaryFile &file, const QByteArray &bytes)
    {
        if (!file.open())
            return false;
        file.write(bytes);
        file.flush();
        return true;
    }

private slots:
    void reparseShiftsTimestampsOnly();
    void reparseLeavesStructureUnchanged();
    void displayZoneDoesNotTouchTimestamps();
    void reparseIsNoOpWithoutDateField();
    void badPatternOpensAsPlainText();
    void inferredSourceZoneFollowsTheDateSpecifier();
};

// With no explicit source zone, Document infers one from the date specifier. This
// is the layer where getting %d/%D backwards is user-visible: the indexed instant
// moves by the local UTC offset while every displayed field looks plausible. Per
// log4cplus's layout.h, %d writes UTC and %D writes local time.
//
// Note this test degenerates where the machine's zone IS UTC (common on CI), since
// both branches then expect the same instant. tst_patterncompiler's impliedZone
// rows compare Qt::UTC against Qt::LocalTime directly and hold the line there.
void TestDocumentZone::inferredSourceZoneFollowsTheDateSpecifier()
{
    const QByteArray line = "2026-07-21 12:00:00,000 [main] INFO  app - a\n";
    const QDateTime wall(QDate(2026, 7, 21), QTime(12, 0, 0));

    QTemporaryFile utcFile;
    QVERIFY(writeLog(utcFile, line));
    Document utcDoc;
    QVERIFY2(utcDoc.open(utcFile.fileName(),
                         QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"),
                         Encoding::Utf8), // no source zone => inferred
             qPrintable(utcDoc.lastError()));
    QCOMPARE(utcDoc.sourceZone(), QTimeZone::utc());
    QCOMPARE(utcDoc.index().records.at(0).timestamp,
             QDateTime(wall.date(), wall.time(), QTimeZone::utc()).toMSecsSinceEpoch());

    QTemporaryFile localFile;
    QVERIFY(writeLog(localFile, line));
    Document localDoc;
    QVERIFY2(localDoc.open(localFile.fileName(),
                           QStringLiteral("%D{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"),
                           Encoding::Utf8),
             qPrintable(localDoc.lastError()));
    QCOMPARE(localDoc.sourceZone(), QTimeZone::systemTimeZone());
    QCOMPARE(localDoc.index().records.at(0).timestamp,
             QDateTime(wall.date(), wall.time(), QTimeZone::systemTimeZone()).toMSecsSinceEpoch());
}

void TestDocumentZone::reparseShiftsTimestampsOnly()
{
    QTemporaryFile file;
    QVERIFY(writeLog(file,
        "2026-07-21 12:00:00,000 [main] INFO  app - a\n"
        "2026-07-21 12:00:00,250 [main] WARN  app - b\n"));

    Document doc;
    QVERIFY2(doc.open(file.fileName(), QString::fromLatin1(kPattern),
                      Encoding::Utf8, QTimeZone::utc()),
             qPrintable(doc.lastError()));

    const qint64 utc0 = doc.index().records.at(0).timestamp;
    const qint64 utc1 = doc.index().records.at(1).timestamp;

    // Reinterpret the same wall-clock text as +02:00: each instant is 2h earlier
    // in UTC, and the 250 ms gap between the two records is preserved.
    doc.reparseTimestamps(QTimeZone(2 * 3600));
    const qint64 p0 = doc.index().records.at(0).timestamp;
    const qint64 p1 = doc.index().records.at(1).timestamp;

    QCOMPARE(utc0 - p0, qint64(2 * 3600 * 1000));
    QCOMPARE(utc1 - p1, qint64(2 * 3600 * 1000));
    QCOMPARE(p1 - p0, qint64(250));

    // The Document now reports the new source zone.
    QCOMPARE(doc.sourceZone(), QTimeZone(2 * 3600));
}

void TestDocumentZone::reparseLeavesStructureUnchanged()
{
    QTemporaryFile file;
    QVERIFY(writeLog(file,
        "2026-07-21 12:00:00,000 [main] ERROR app - boom:\n"
        "    at foo()\n"
        "2026-07-21 12:00:01,000 [main] INFO  app - ok\n"));

    Document doc;
    QVERIFY2(doc.open(file.fileName(), QString::fromLatin1(kPattern),
                      Encoding::Utf8, QTimeZone::utc()),
             qPrintable(doc.lastError()));

    const int count = doc.index().records.size();
    const qint64 off0 = doc.index().records.at(0).offset;
    const quint32 len0 = doc.index().records.at(0).length;
    const quint16 lines0 = doc.index().records.at(0).lineCount;

    doc.reparseTimestamps(QTimeZone(-5 * 3600));

    QCOMPARE(doc.index().records.size(), count); // no rescan: same records
    QCOMPARE(doc.index().records.at(0).offset, off0);
    QCOMPARE(doc.index().records.at(0).length, len0);
    QCOMPARE(doc.index().records.at(0).lineCount, lines0); // multi-line span intact
}

void TestDocumentZone::displayZoneDoesNotTouchTimestamps()
{
    QTemporaryFile file;
    QVERIFY(writeLog(file,
        "2026-07-21 12:00:00,000 [main] INFO  app - a\n"));

    Document doc;
    QVERIFY2(doc.open(file.fileName(), QString::fromLatin1(kPattern),
                      Encoding::Utf8, QTimeZone::utc(), QTimeZone::utc()),
             qPrintable(doc.lastError()));

    const qint64 before = doc.index().records.at(0).timestamp;
    doc.setDisplayZone(QTimeZone(9 * 3600)); // Tokyo display; storage stays UTC ms
    QCOMPARE(doc.index().records.at(0).timestamp, before);
    QCOMPARE(doc.displayZone(), QTimeZone(9 * 3600));
}

void TestDocumentZone::reparseIsNoOpWithoutDateField()
{
    QTemporaryFile file;
    QVERIFY(writeLog(file, "INFO  app - no timestamp here\n"));

    Document doc;
    // A pattern with no %d field: records carry kNoTimestamp.
    QVERIFY2(doc.open(file.fileName(), QStringLiteral("%p %c - %m%n"),
                      Encoding::Utf8, QTimeZone::utc()),
             qPrintable(doc.lastError()));
    QCOMPARE(doc.index().records.at(0).timestamp, Record::kNoTimestamp);

    doc.reparseTimestamps(QTimeZone(3 * 3600)); // must not invent a timestamp
    QCOMPARE(doc.index().records.at(0).timestamp, Record::kNoTimestamp);
}

void TestDocumentZone::badPatternOpensAsPlainText()
{
    // A bad/uncompilable pattern must NOT block opening: the file opens with every
    // line as an unparsed plain-text record (SPEC.md §4), and the compile error is
    // surfaced for the dialog to point at.
    QTemporaryFile file;
    QVERIFY(writeLog(file,
        "totally unstructured line one\n"
        "and line two\n"));

    Document doc;
    QVERIFY2(doc.open(file.fileName(), QStringLiteral("%p %z %m")), // %z is unknown
             "a bad pattern must still open the file");
    QVERIFY(doc.formatError().isError());
    QCOMPARE(int(doc.formatError().code), int(CompileError::Code::UnknownSpecifier));

    // No format compiled -> the indexer keeps every line as an Unparsed record.
    QCOMPARE(doc.index().records.size(), 2);
    QCOMPARE(doc.index().records.at(0).priorityEnum(), Priority::Unknown);
    QCOMPARE(doc.format().fields.size(), 0); // empty format == plain text
}

QTEST_GUILESS_MAIN(TestDocumentZone)
#include "tst_documentzone.moc"
