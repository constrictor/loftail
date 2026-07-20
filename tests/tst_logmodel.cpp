#include <QtTest>

#include <QTemporaryFile>

#include "Document.h"
#include "LogFormat.h"
#include "LogModel.h"

using namespace loftail;

// LogModel coverage: lazy data() returns the correct field for each column,
// pulling eager fields off the Record and decoding the rest from the mapped bytes
// (invariant #1). Runs through Document::open against a real temp file, so the
// LogSource + Decoder + Indexer path is exercised end to end without a
// QApplication (GUILESS: a QCoreApplication only).
class TestLogModel : public QObject
{
    Q_OBJECT

private:
    // A fresh temp file per call, kept alive by the caller so Document can map it.
    static bool writeLog(QTemporaryFile &file, const QByteArray &bytes)
    {
        if (!file.open())
            return false;
        file.write(bytes);
        file.flush();
        return true;
    }

private slots:
    void columnsAndRows();
    void lazyFieldsAreCorrect();
    void multiLineMessageJoined();
};

void TestLogModel::columnsAndRows()
{
    QTemporaryFile file;
    QVERIFY(writeLog(file,
        "2026-07-21 14:32:05,123 [main] INFO  net.socket - Connection opened\n"
        "2026-07-21 14:32:06,000 [worker] WARN  db.pool - Pool exhausted\n"));

    Document doc;
    QVERIFY2(doc.open(file.fileName(), QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"),
                      Encoding::Utf8, QTimeZone::utc()),
             qPrintable(doc.lastError()));

    LogModel model(&doc);
    QCOMPARE(model.rowCount(), 2);
    // Fields: Time, Thread, Priority, Subsystem, Message.
    QCOMPARE(model.columnCount(), 5);
    QCOMPARE(model.headerData(0, Qt::Horizontal, Qt::DisplayRole).toString(), QStringLiteral("Time"));
    QCOMPARE(model.headerData(4, Qt::Horizontal, Qt::DisplayRole).toString(), QStringLiteral("Message"));
}

void TestLogModel::lazyFieldsAreCorrect()
{
    QTemporaryFile file;
    QVERIFY(writeLog(file,
        "2026-07-21 14:32:05,123 [main] INFO  net.socket - Connection opened\n"));

    Document doc;
    QVERIFY2(doc.open(file.fileName(), QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"),
                      Encoding::Utf8, QTimeZone::utc(), QTimeZone::utc()),
             qPrintable(doc.lastError()));

    LogModel model(&doc);
    auto cell = [&](int c) { return model.data(model.index(0, c)).toString(); };

    QCOMPARE(cell(0), QStringLiteral("2026-07-21 14:32:05,123")); // Time, display zone UTC
    QCOMPARE(cell(1), QStringLiteral("main"));                    // Thread
    QCOMPARE(cell(2), QStringLiteral("INFO"));                    // Priority
    QCOMPARE(cell(3), QStringLiteral("net.socket"));              // Subsystem
    QCOMPARE(cell(4), QStringLiteral("Connection opened"));       // Message (lazy decode)
}

void TestLogModel::multiLineMessageJoined()
{
    QTemporaryFile file;
    QVERIFY(writeLog(file,
        "2026-07-21 14:32:05,123 [main] ERROR app.core - Exception:\n"
        "    at foo()\n"
        "    at bar()\n"));

    Document doc;
    QVERIFY2(doc.open(file.fileName(), QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"),
                      Encoding::Utf8, QTimeZone::utc()),
             qPrintable(doc.lastError()));

    LogModel model(&doc);
    QCOMPARE(model.rowCount(), 1);
    const QString msg = model.data(model.index(0, 4)).toString();
    QVERIFY(msg.startsWith(QStringLiteral("Exception:")));
    QVERIFY(msg.contains(QStringLiteral("at foo()")));
    QVERIFY(msg.contains(QStringLiteral("at bar()")));
}

QTEST_GUILESS_MAIN(TestLogModel)
#include "tst_logmodel.moc"
