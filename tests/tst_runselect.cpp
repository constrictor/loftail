#include <QtTest>

#include <QByteArray>
#include <QFile>
#include <QTemporaryDir>

#include "Document.h"
#include "FilteredIndex.h"
#include "LiveController.h"
#include "LogModel.h"
#include "Priority.h"
#include "RecordIndex.h"

using namespace loftail;

// Run selection (SPEC.md §3a): a user-supplied run-start regexp splits a log file
// into "runs" (concatenated app sessions) and the user views/tails one at a time.
// A run is a contiguous record range restricted through the SAME FilteredIndex view
// as filtering. This test drives Document (+ LiveController for the live cases)
// against a real temp file. Core-only, GUILESS — checkNow() ingests synchronously,
// no event loop. Asserts:
//   (a) detection (run count + boundaries, and a leading "preamble" run);
//   (b) selecting a run restricts the visible subset with a correct sourceRow map;
//   (c) run ∩ user filter composes in one pass;
//   (d) live append inside the last run grows the view;
//   (e) live append that STARTS a new run freezes the watched run and lists the new
//       one (the "stay on current run" decision) without auto-jumping;
//   (f) an earlier selected run is untouched by appends;
//   (g) rescan re-detects runs and defaults to the newest.
class TestRunSelect : public QObject
{
    Q_OBJECT

private:
    static constexpr auto kPattern = "%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n";
    static constexpr auto kMarker = "RUN START"; // whole-line match target

    static QByteArray rec(int sec, const char *thread, const char *prio, const char *logger,
                          const QByteArray &msg)
    {
        QByteArray out = "2026-07-21 00:00:";
        out += QByteArray::number(sec).rightJustified(2, '0');
        out += ",000 [";
        out += thread;
        out += "] ";
        out += prio;
        out += "  ";
        out += logger;
        out += " - ";
        out += msg;
        out += '\n';
        return out;
    }

    // A run-start record: a normal record whose message carries the marker, so it
    // matches the whole-line run-start regexp. Priority INFO (never a filter target).
    static QByteArray banner(int sec, int runNo)
    {
        return rec(sec, "main", "INFO ", "app",
                   QByteArray(kMarker) + " #" + QByteArray::number(runNo));
    }

    static bool writeWhole(const QString &path, const QByteArray &bytes)
    {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return false;
        f.write(bytes);
        f.flush();
        f.close();
        return true;
    }

    static bool append(const QString &path, const QByteArray &bytes)
    {
        QFile f(path);
        if (!f.open(QIODevice::Append))
            return false;
        f.write(bytes);
        f.flush();
        f.close();
        return true;
    }

    static bool openDoc(Document &doc, const QString &path)
    {
        return doc.open(path, QString::fromLatin1(kPattern), Encoding::Utf8, QTimeZone::utc());
    }

private slots:
    void detectsRunsAndSelectsNewest();
    void leadingPreambleRun();
    void selectRunRestrictsAndMapsSourceRows();
    void composesWithUserFilter();
    void selectAllRunsShowsWholeFile();
    void liveAppendGrowsLastRun();
    void liveNewRunStaysOnCurrent();
    void liveEarlierRunUnaffectedByAppend();
    void rescanReDetectsAndSelectsNewest();
};

void TestRunSelect::detectsRunsAndSelectsNewest()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("runs.log"));

    // Three runs of 3 records each (banner + WARN + INFO); file starts with a banner.
    QByteArray whole;
    int sec = 0;
    for (int r = 0; r < 3; ++r) {
        whole += banner(sec++, r);
        whole += rec(sec++, "t1", "WARN ", "svc", "warn");
        whole += rec(sec++, "t1", "INFO ", "svc", "info");
    }
    QVERIFY(writeWhole(path, whole));

    Document doc;
    QVERIFY(openDoc(doc, path));
    QCOMPARE(doc.index().records.size(), 9);

    doc.setRunStart(QString::fromLatin1(kMarker), /*regex=*/false, Qt::CaseInsensitive);

    QCOMPARE(doc.runs().size(), 3);
    QVERIFY(!doc.runs().at(0).isPreamble); // file opens with a banner, no preamble
    QCOMPARE(doc.runs().at(0).startRecord, 0);
    QCOMPARE(doc.runs().at(1).startRecord, 3);
    QCOMPARE(doc.runs().at(2).startRecord, 6);
    for (int i = 0; i < 3; ++i)
        QCOMPARE(doc.runRecordCount(i), 3);

    // setRunStart defaults to the newest run.
    QCOMPARE(doc.selectedRun(), 2);
    QVERIFY(doc.viewRestricted());

    doc.applyFilters();
    QVERIFY(doc.filtered().active());
    QCOMPARE(doc.filtered().recordCount(), 3);
    QCOMPARE(doc.filtered().sourceRow(0), 6); // newest run's first record
}

void TestRunSelect::leadingPreambleRun()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("preamble.log"));

    // Two records BEFORE the first marker, then two runs.
    QByteArray whole;
    int sec = 0;
    whole += rec(sec++, "t0", "INFO ", "boot", "pre one");
    whole += rec(sec++, "t0", "INFO ", "boot", "pre two");
    whole += banner(sec++, 0);
    whole += rec(sec++, "t1", "INFO ", "svc", "a");
    whole += banner(sec++, 1);
    whole += rec(sec++, "t1", "INFO ", "svc", "b");
    QVERIFY(writeWhole(path, whole));

    Document doc;
    QVERIFY(openDoc(doc, path));
    doc.setRunStart(QString::fromLatin1(kMarker), false, Qt::CaseInsensitive);

    QCOMPARE(doc.runs().size(), 3); // preamble + 2 real runs
    QVERIFY(doc.runs().at(0).isPreamble);
    QCOMPARE(doc.runs().at(0).startRecord, 0);
    QCOMPARE(doc.runRecordCount(0), 2); // the two pre-marker records
    QCOMPARE(doc.runs().at(1).startRecord, 2);
    QCOMPARE(doc.runs().at(2).startRecord, 4);
    QCOMPARE(doc.selectedRun(), 2); // newest
}

void TestRunSelect::selectRunRestrictsAndMapsSourceRows()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("select.log"));

    QByteArray whole;
    int sec = 0;
    for (int r = 0; r < 3; ++r) {
        whole += banner(sec++, r);
        whole += rec(sec++, "t1", "INFO ", "svc", QByteArray("m") + QByteArray::number(r));
    }
    QVERIFY(writeWhole(path, whole)); // records: 0..5, runs at 0,2,4

    Document doc;
    QVERIFY(openDoc(doc, path));
    doc.setRunStart(QString::fromLatin1(kMarker), false, Qt::CaseInsensitive);
    QCOMPARE(doc.runs().size(), 3);

    // Select the MIDDLE run (records 2,3).
    doc.selectRun(1);
    doc.applyFilters();
    QCOMPARE(doc.filtered().recordCount(), 2);
    QCOMPARE(doc.filtered().sourceRow(0), 2);
    QCOMPARE(doc.filtered().sourceRow(1), 3);
    QCOMPARE(doc.filtered().sourceRow(2), -1); // out of range

    // Select the first run (records 0,1).
    doc.selectRun(0);
    doc.applyFilters();
    QCOMPARE(doc.filtered().recordCount(), 2);
    QCOMPARE(doc.filtered().sourceRow(0), 0);
    QCOMPARE(doc.filtered().sourceRow(1), 1);
}

void TestRunSelect::composesWithUserFilter()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("compose.log"));

    // Each run: banner(INFO) + WARN + INFO + ERROR. A WARN+ filter over the newest
    // run keeps only the WARN and the ERROR (banner + INFO excluded).
    QByteArray whole;
    int sec = 0;
    for (int r = 0; r < 2; ++r) {
        whole += banner(sec++, r);
        whole += rec(sec++, "t1", "WARN ", "svc", "warn");
        whole += rec(sec++, "t1", "INFO ", "svc", "info");
        whole += rec(sec++, "t1", "ERROR", "svc", "err");
    }
    QVERIFY(writeWhole(path, whole)); // 8 records; newest run = records 4,5,6,7

    Document doc;
    QVERIFY(openDoc(doc, path));
    doc.setRunStart(QString::fromLatin1(kMarker), false, Qt::CaseInsensitive);
    QCOMPARE(doc.selectedRun(), 1);

    doc.filters().priorityEnabled = true;
    doc.filters().minPriority = Priority::Warn;
    doc.applyFilters();

    // Newest run ∩ (>= WARN): the WARN (row 5) and the ERROR (row 7).
    QCOMPARE(doc.filtered().recordCount(), 2);
    QCOMPARE(doc.filtered().sourceRow(0), 5);
    QCOMPARE(doc.filtered().sourceRow(1), 7);
}

void TestRunSelect::selectAllRunsShowsWholeFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("all.log"));

    QByteArray whole;
    int sec = 0;
    for (int r = 0; r < 3; ++r) {
        whole += banner(sec++, r);
        whole += rec(sec++, "t1", "INFO ", "svc", "m");
    }
    QVERIFY(writeWhole(path, whole)); // 6 records

    Document doc;
    QVERIFY(openDoc(doc, path));
    doc.setRunStart(QString::fromLatin1(kMarker), false, Qt::CaseInsensitive);

    doc.selectRun(-1); // "all runs" — no restriction
    QVERIFY(!doc.viewRestricted());
    doc.applyFilters();
    QVERIFY(!doc.filtered().active()); // identity view
    QCOMPARE(doc.filtered().recordCount(), 6);
}

void TestRunSelect::liveAppendGrowsLastRun()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("livegrow.log"));

    QByteArray whole;
    int sec = 0;
    whole += banner(sec++, 0);
    whole += rec(sec++, "t1", "INFO ", "svc", "a0");
    whole += banner(sec++, 1);
    whole += rec(sec++, "t1", "INFO ", "svc", "b0");
    QVERIFY(writeWhole(path, whole)); // 4 records; newest run = records 2,3

    Document doc;
    QVERIFY(openDoc(doc, path));
    doc.setRunStart(QString::fromLatin1(kMarker), false, Qt::CaseInsensitive);
    QCOMPARE(doc.selectedRun(), 1);
    doc.applyFilters();

    LogModel model(&doc);
    LiveController live(&doc, &model);
    live.start();
    QCOMPARE(doc.filtered().recordCount(), 2);

    // Append two ordinary records (no marker) — they belong to the last run.
    QByteArray chunk;
    chunk += rec(sec++, "t1", "INFO ", "svc", "b1");
    chunk += rec(sec++, "t1", "WARN ", "svc", "b2");
    QVERIFY(append(path, chunk));
    live.checkNow();

    QCOMPARE(doc.index().records.size(), 6);
    QCOMPARE(doc.runs().size(), 2);           // no new run
    QCOMPARE(doc.filtered().recordCount(), 4); // grew by 2
    QCOMPARE(doc.filtered().sourceRow(2), 4);
    QCOMPARE(doc.filtered().sourceRow(3), 5);
}

void TestRunSelect::liveNewRunStaysOnCurrent()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("livenewrun.log"));

    QByteArray whole;
    int sec = 0;
    whole += banner(sec++, 0);
    whole += rec(sec++, "t1", "INFO ", "svc", "a0");
    whole += banner(sec++, 1);
    whole += rec(sec++, "t1", "INFO ", "svc", "b0");
    QVERIFY(writeWhole(path, whole)); // newest run = records 2,3

    Document doc;
    QVERIFY(openDoc(doc, path));
    doc.setRunStart(QString::fromLatin1(kMarker), false, Qt::CaseInsensitive);
    QCOMPARE(doc.selectedRun(), 1);
    doc.applyFilters();

    LogModel model(&doc);
    LiveController live(&doc, &model);
    live.start();
    QCOMPARE(doc.filtered().recordCount(), 2);

    // The app restarts and writes a NEW run into the same file.
    QByteArray chunk;
    chunk += banner(sec++, 2);
    chunk += rec(sec++, "t1", "INFO ", "svc", "c0");
    chunk += rec(sec++, "t1", "INFO ", "svc", "c1");
    QVERIFY(append(path, chunk));
    live.checkNow();

    QCOMPARE(doc.index().records.size(), 7);
    QCOMPARE(doc.runs().size(), 3);            // the new run is listed
    QCOMPARE(doc.selectedRun(), 1);            // selection unchanged — stay on current
    QCOMPARE(doc.filtered().recordCount(), 2); // view FROZEN at the boundary

    // Switching to the new run shows the appended records.
    doc.selectRun(2);
    doc.applyFilters();
    QCOMPARE(doc.filtered().recordCount(), 3);
    QCOMPARE(doc.filtered().sourceRow(0), 4); // the new banner
}

void TestRunSelect::liveEarlierRunUnaffectedByAppend()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("liveearlier.log"));

    QByteArray whole;
    int sec = 0;
    whole += banner(sec++, 0);
    whole += rec(sec++, "t1", "INFO ", "svc", "a0");
    whole += rec(sec++, "t1", "INFO ", "svc", "a1");
    whole += banner(sec++, 1);
    whole += rec(sec++, "t1", "INFO ", "svc", "b0");
    QVERIFY(writeWhole(path, whole)); // run0 = records 0,1,2 ; run1 = records 3,4

    Document doc;
    QVERIFY(openDoc(doc, path));
    doc.setRunStart(QString::fromLatin1(kMarker), false, Qt::CaseInsensitive);
    doc.selectRun(0); // watch the EARLIER run
    doc.applyFilters();

    LogModel model(&doc);
    LiveController live(&doc, &model);
    live.start();
    QCOMPARE(doc.filtered().recordCount(), 3);

    // Append more to the last run AND a new run — the earlier selection is untouched.
    QByteArray chunk;
    chunk += rec(sec++, "t1", "INFO ", "svc", "b1");
    chunk += banner(sec++, 2);
    chunk += rec(sec++, "t1", "INFO ", "svc", "c0");
    QVERIFY(append(path, chunk));
    live.checkNow();

    QCOMPARE(doc.selectedRun(), 0);
    QCOMPARE(doc.filtered().recordCount(), 3); // unchanged
    QCOMPARE(doc.filtered().sourceRow(0), 0);
    QCOMPARE(doc.filtered().sourceRow(2), 2);
}

void TestRunSelect::rescanReDetectsAndSelectsNewest()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("rescan.log"));

    // A large original: 2 runs of 5 records each (many bytes).
    QByteArray whole;
    int sec = 0;
    for (int r = 0; r < 2; ++r) {
        whole += banner(sec++, r);
        for (int k = 0; k < 4; ++k)
            whole += rec(sec++, "t1", "INFO ", "svc", "m");
    }
    QVERIFY(writeWhole(path, whole)); // 10 records, 2 runs
    Document doc;
    QVERIFY(openDoc(doc, path));
    doc.setRunStart(QString::fromLatin1(kMarker), false, Qt::CaseInsensitive);
    QCOMPARE(doc.runs().size(), 2);
    doc.applyFilters();

    LogModel model(&doc);
    LiveController live(&doc, &model);
    live.start();

    int rescans = 0;
    connect(&live, &LiveController::rescanned, &live, [&] { ++rescans; });

    // Copytruncate: the writer truncates the file and writes THREE fresh, SHORTER
    // runs (banner-only). Fewer bytes than the original ⇒ a shrink ⇒ a rescan.
    QByteArray fresh;
    sec = 0;
    for (int r = 0; r < 3; ++r)
        fresh += banner(sec++, r);
    QVERIFY(fresh.size() < whole.size());
    QVERIFY(writeWhole(path, fresh));
    live.checkNow();

    QCOMPARE(rescans, 1);
    QCOMPARE(doc.index().records.size(), 3);
    QCOMPARE(doc.runs().size(), 3);  // re-detected against the new content
    QCOMPARE(doc.selectedRun(), 2);  // newest
    QCOMPARE(doc.filtered().recordCount(), 1);
}

QTEST_GUILESS_MAIN(TestRunSelect)
#include "tst_runselect.moc"
