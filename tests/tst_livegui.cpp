#include <QtTest>

#include <QByteArray>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>

#include "Document.h"
#include "LiveController.h"
#include "LogModel.h"
#include "Priority.h"
#include "RecordIndex.h"

using namespace loftail;

// M6 live smoke test. Unlike tst_tail (which drives checkNow() synchronously for
// determinism), this runs the REAL watch path end to end: LiveController::start()
// arms QFileSystemWatcher + the size-poll timer, a background QTimer appends records
// to the file "like another process", the event loop runs, and the model must grow
// on its own with NO manual poke — proving the watcher/poll actually fire and ingest
// (SPEC.md §3, "every file auto-updates as it grows with no user action"). GUILESS.
class TestLiveGui : public QObject
{
    Q_OBJECT

private:
    static constexpr auto kPattern = "%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n";

    static QByteArray rec(int i)
    {
        QByteArray out = "2026-07-21 00:00:";
        out += QByteArray::number(i % 60).rightJustified(2, '0');
        out += ",000 [t0] INFO  logger.live - line ";
        out += QByteArray::number(i);
        out += '\n';
        return out;
    }

private slots:
    void modelGrowsWithoutManualPoke();
};

void TestLiveGui::modelGrowsWithoutManualPoke()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("livegui.log"));

    QByteArray initial;
    for (int i = 0; i < 3; ++i)
        initial += rec(i);
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(initial);
        f.flush();
        f.close();
    }

    Document doc;
    QVERIFY2(doc.open(path, QString::fromLatin1(kPattern), Encoding::Utf8, QTimeZone::utc()),
             qPrintable(doc.lastError()));
    LogModel model(&doc);
    QCOMPARE(model.rowCount(), 3);

    LiveController live(&doc, &model);
    live.setPollInterval(40); // brisk poll so the fallback path resolves fast in-test
    live.start();

    // A "writer" appends one record at a time on its own timer while the event loop
    // runs — the app performs NO explicit checkNow(); growth must come from the watch.
    int written = 0;
    const int target = 8;
    QTimer writer;
    writer.setInterval(25);
    connect(&writer, &QTimer::timeout, &writer, [&] {
        if (written >= target) {
            writer.stop();
            return;
        }
        QFile f(path);
        if (f.open(QIODevice::Append)) {
            f.write(rec(100 + written));
            f.flush();
            f.close();
            ++written;
        }
    });
    writer.start();

    // The model must reach 3 + target on its own (watcher and/or poll), no manual poke.
    QTRY_COMPARE_WITH_TIMEOUT(model.rowCount(), 3 + target, 15000);
    QCOMPARE(doc.index().records.size(), 3 + target);
    QCOMPARE(doc.index().records.last().priorityEnum(), Priority::Info);
}

QTEST_GUILESS_MAIN(TestLiveGui)
#include "tst_livegui.moc"
