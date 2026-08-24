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
//   (e) live append that STARTS a new run freezes a PINNED run and lists the new one
//       (the "stay on current run" decision) without auto-jumping — while the pane's
//       default, following the last run, retargets onto it;
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
    void liveNewRunIsFollowedWhenLastRunIsSelected();
    void followingSurvivesAPatternWithNoMatchesYet();
    void liveEarlierRunUnaffectedByAppend();
    void rescanReDetectsAndSelectsNewest();

    // The RunSeconds display mode's baseline lookup (SPEC.md §4). It runs on the
    // paint path over the same run partition, so it is tested here rather than in
    // the rendering tests, which cover the formatting.
    void runBaseTimestampBinarySearch();
    void runBaseTimestampFollowsAppend();

    // What the Runs pane puts under a run's name (SPEC.md §3a): the span of instants
    // it covers and how many FATAL/ERROR/WARN records are in it. Memoised on the same
    // terms as the baselines above, so the live cases are the ones that matter: a memo
    // that froze would stop counting a tailed run, and one that refolded would double.
    void runStatsSpanAndCounts();
    void runStatsFollowALiveAppend();
    void aNewRunsRecordsStopBeingCountedInTheRunBeforeIt();
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
    // PINNED to run 1, which is where the "stay on current run" claim lives now: the
    // pane's default is to FOLLOW the last run, and that one moves (the test below).
    // Picking the run that happens to be last is deliberately not the same gesture.
    doc.selectRun(1);
    QVERIFY(!doc.followingLastRun());
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
    QVERIFY(!doc.retargetLastRun());           // ...and nothing retargets a pinned run

    // Switching to the new run shows the appended records.
    doc.selectRun(2);
    doc.applyFilters();
    QCOMPARE(doc.filtered().recordCount(), 3);
    QCOMPARE(doc.filtered().sourceRow(0), 4); // the new banner
}

void TestRunSelect::liveNewRunIsFollowedWhenLastRunIsSelected()
{
    // The other half of the decision above, and the pane's DEFAULT (SPEC.md §3a):
    // "Last run" is not an ordinal but a standing instruction, so a restart moves the
    // view onto the run that just started rather than leaving it on the finished one.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("livelastrun.log"));

    QByteArray whole;
    int sec = 0;
    whole += banner(sec++, 0);
    whole += rec(sec++, "t1", "INFO ", "svc", "a0");
    QVERIFY(writeWhole(path, whole));

    Document doc;
    QVERIFY(openDoc(doc, path));
    doc.setRunStart(QString::fromLatin1(kMarker), false, Qt::CaseInsensitive);
    QVERIFY(doc.followingLastRun()); // what setRunStart leaves behind, unasked
    QCOMPARE(doc.selectedRun(), 0);
    doc.applyFilters();

    LogModel model(&doc);
    LiveController live(&doc, &model);
    live.start();
    QCOMPARE(doc.filtered().recordCount(), 2);

    // Growth INSIDE the run is not a retarget: the selection is already the last run,
    // so nothing moves and the caller is told so — this is the answer on every tick
    // but a restart, and the one that keeps the re-apply out of the tail path.
    QVERIFY(append(path, rec(sec++, "t1", "INFO ", "svc", "a1")));
    live.checkNow();
    QCOMPARE(doc.filtered().recordCount(), 3);
    QVERIFY(!doc.retargetLastRun());

    // The app restarts. The append itself still freezes at the boundary — the ingest
    // is mid-flight and must not mix two runs' records — and the retarget afterwards
    // is a whole re-apply, which is why it is the caller who does it.
    QByteArray chunk;
    chunk += banner(sec++, 1);
    chunk += rec(sec++, "t1", "INFO ", "svc", "b0");
    QVERIFY(append(path, chunk));
    live.checkNow();
    QCOMPARE(doc.runs().size(), 2);
    QCOMPARE(doc.filtered().recordCount(), 3); // frozen, as for a pinned run

    QVERIFY(doc.retargetLastRun());
    QCOMPARE(doc.selectedRun(), 1);
    QVERIFY(doc.followingLastRun()); // still following, ready for the next restart
    doc.applyFilters();
    QCOMPARE(doc.filtered().recordCount(), 2);
    QCOMPARE(doc.filtered().sourceRow(0), 3); // the new banner
    QVERIFY(!doc.retargetLastRun());          // idempotent once it has landed
}

void TestRunSelect::followingSurvivesAPatternWithNoMatchesYet()
{
    // "Show nothing if there is nothing" is not what a run-less file does: with no run
    // detected there is no bound, and a file with no runs has always been the whole
    // file. Following it is then inert until the first marker turns up — at which
    // point it becomes a genuine restriction, with no gesture from the user.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("nomarkers.log"));

    int sec = 0;
    QVERIFY(writeWhole(path, rec(sec++, "t1", "INFO ", "svc", "a0")
                                 + rec(sec++, "t1", "INFO ", "svc", "a1")));
    Document doc;
    QVERIFY(openDoc(doc, path));
    doc.setRunStart(QString::fromLatin1(kMarker), false, Qt::CaseInsensitive);
    QVERIFY(doc.runs().isEmpty());
    QVERIFY(doc.followingLastRun());
    QCOMPARE(doc.selectedRun(), -1);
    QVERIFY(!doc.viewRestricted()); // the whole file
    doc.applyFilters();

    LogModel model(&doc);
    LiveController live(&doc, &model);
    live.start();
    QCOMPARE(doc.filtered().recordCount(), 2);

    QVERIFY(append(path, banner(sec++, 0) + rec(sec++, "t1", "INFO ", "svc", "b0")));
    live.checkNow();
    QCOMPARE(doc.runs().size(), 2); // the preamble the marker created, and the run

    QVERIFY(doc.retargetLastRun());
    QCOMPARE(doc.selectedRun(), 1);
    QVERIFY(doc.viewRestricted());
    doc.applyFilters();
    QCOMPARE(doc.filtered().recordCount(), 2);
    QCOMPARE(doc.filtered().sourceRow(0), 2); // the banner
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

void TestRunSelect::runBaseTimestampBinarySearch()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("basesearch.log"));

    // Two pre-marker records, then three marked runs of two records each.
    QByteArray whole;
    int sec = 0;
    whole += rec(sec++, "t0", "INFO ", "boot", "pre one");
    whole += rec(sec++, "t0", "INFO ", "boot", "pre two");
    for (int r = 0; r < 3; ++r) {
        whole += banner(sec++, r);
        whole += rec(sec++, "t1", "INFO ", "svc", "m");
    }
    QVERIFY(writeWhole(path, whole)); // 8 records

    Document doc;
    QVERIFY(openDoc(doc, path));
    QCOMPARE(doc.index().records.size(), 8);

    // With no run-start pattern the whole file is ONE run, so every row bases on the
    // file's first record — this is what keeps the display mode always usable.
    QVERIFY(doc.runs().isEmpty());
    const qint64 first = doc.index().records.at(0).timestamp;
    for (int row = 0; row < 8; ++row)
        QCOMPARE(doc.runBaseTimestamp(row), first);

    doc.setRunStart(QString::fromLatin1(kMarker), false, Qt::CaseInsensitive);
    QCOMPARE(doc.runs().size(), 4); // preamble + 3 marked runs

    // Each row resolves to its OWN run's first record. Walked backwards as well as
    // forwards so the one-entry hint is exercised in both directions, not just on
    // the contiguous forward sweep a repaint produces.
    const int startOf[8] = { 0, 0, 2, 2, 4, 4, 6, 6 };
    for (int row = 0; row < 8; ++row) {
        QCOMPARE(doc.runBaseTimestamp(row),
                 doc.index().records.at(startOf[row]).timestamp);
    }
    for (int row = 7; row >= 0; --row) {
        QCOMPARE(doc.runBaseTimestamp(row),
                 doc.index().records.at(startOf[row]).timestamp);
    }

    // Out of range in either direction yields no baseline rather than reading past
    // the index — cellText guards on it, but the lookup must be safe on its own.
    QCOMPARE(doc.runBaseTimestamp(-1), qint64(Record::kNoTimestamp));
    QCOMPARE(doc.runBaseTimestamp(8), qint64(Record::kNoTimestamp));
}

void TestRunSelect::runBaseTimestampFollowsAppend()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("baseappend.log"));

    QByteArray whole;
    int sec = 0;
    whole += banner(sec++, 0);
    whole += rec(sec++, "t1", "INFO ", "svc", "a0");
    whole += banner(sec++, 1);
    whole += rec(sec++, "t1", "INFO ", "svc", "b0");
    QVERIFY(writeWhole(path, whole));

    Document doc;
    QVERIFY(openDoc(doc, path));
    doc.setRunStart(QString::fromLatin1(kMarker), false, Qt::CaseInsensitive);
    QCOMPARE(doc.runs().size(), 2);
    doc.applyFilters();

    const qint64 run0 = doc.index().records.at(0).timestamp;
    const qint64 run1 = doc.index().records.at(2).timestamp;
    // Resolve BEFORE the append so the memos are populated: the point of this case
    // is that a later append neither invalidates them nor lets them go stale.
    QCOMPARE(doc.runBaseTimestamp(1), run0);
    QCOMPARE(doc.runBaseTimestamp(3), run1);

    LogModel model(&doc);
    LiveController live(&doc, &model);
    live.start();

    // The app restarts: a third run arrives by append.
    QByteArray chunk;
    chunk += banner(sec++, 2);
    chunk += rec(sec++, "t1", "INFO ", "svc", "c0");
    QVERIFY(append(path, chunk));
    live.checkNow();
    QCOMPARE(doc.runs().size(), 3);

    // The earlier runs' baselines are untouched...
    QCOMPARE(doc.runBaseTimestamp(0), run0);
    QCOMPARE(doc.runBaseTimestamp(1), run0);
    QCOMPARE(doc.runBaseTimestamp(2), run1);
    QCOMPARE(doc.runBaseTimestamp(3), run1);
    // ...and the appended rows base on the NEW run's marker, not on run 1's.
    const qint64 run2 = doc.index().records.at(4).timestamp;
    QVERIFY(run2 != run1);
    QCOMPARE(doc.runBaseTimestamp(4), run2);
    QCOMPARE(doc.runBaseTimestamp(5), run2);
}

void TestRunSelect::runStatsSpanAndCounts()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("stats.log"));

    // Run 0: banner + WARN + ERROR. Run 1: banner + FATAL + INFO + DEBUG.
    QByteArray whole;
    int sec = 0;
    whole += banner(sec++, 0);
    whole += rec(sec++, "t1", "WARN ", "svc", "w");
    whole += rec(sec++, "t1", "ERROR", "svc", "e");
    whole += banner(sec++, 1);
    whole += rec(sec++, "t1", "FATAL", "svc", "f");
    whole += rec(sec++, "t1", "INFO ", "svc", "i");
    whole += rec(sec++, "t1", "DEBUG", "svc", "d");
    QVERIFY(writeWhole(path, whole));

    Document doc;
    QVERIFY(openDoc(doc, path));
    doc.setRunStart(QString::fromLatin1(kMarker), false, Qt::CaseInsensitive);
    QCOMPARE(doc.runs().size(), 2);

    const Document::RunStats a = doc.runStats(0);
    QCOMPARE(a.fatal, 0);
    QCOMPARE(a.error, 1);
    QCOMPARE(a.warn, 1);
    // The span is the run's OWN records and stops at the boundary: the first and third
    // record, never the file's last one. Both come off Record::timestamp, which is UTC
    // epoch ms (invariant #10), so the comparison is against the same.
    QCOMPARE(a.firstTimestamp, doc.index().records.at(0).timestamp);
    QCOMPARE(a.lastTimestamp, doc.index().records.at(2).timestamp);

    const Document::RunStats b = doc.runStats(1);
    QCOMPARE(b.fatal, 1);
    QCOMPARE(b.error, 0);
    QCOMPARE(b.warn, 0);
    QCOMPARE(b.firstTimestamp, doc.index().records.at(3).timestamp);
    QCOMPARE(b.lastTimestamp, doc.index().records.at(6).timestamp);

    // Asking twice answers the same — the memo is a resume cursor, not an accumulator
    // that folds every call in again.
    QCOMPARE(doc.runStats(1).fatal, 1);
    QCOMPARE(doc.runStats(1).warn, 0);

    // Out of range is not a crash and not a count.
    QCOMPARE(doc.runStats(-1).error, 0);
    QCOMPARE(doc.runStats(7).error, 0);
}

void TestRunSelect::runStatsFollowALiveAppend()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("statslive.log"));

    QByteArray whole;
    int sec = 0;
    whole += banner(sec++, 0);
    whole += rec(sec++, "t1", "WARN ", "svc", "w");
    QVERIFY(writeWhole(path, whole));

    Document doc;
    QVERIFY(openDoc(doc, path));
    doc.setRunStart(QString::fromLatin1(kMarker), false, Qt::CaseInsensitive);
    doc.applyFilters();
    QCOMPARE(doc.runStats(0).warn, 1);   // memoised HERE, before the append

    LogModel model(&doc);
    LiveController live(&doc, &model);
    live.start();

    QByteArray chunk;
    chunk += rec(sec++, "t1", "ERROR", "svc", "e");
    chunk += rec(sec++, "t1", "WARN ", "svc", "w2");
    QVERIFY(append(path, chunk));
    live.checkNow();
    QCOMPARE(doc.index().records.size(), 4);

    const Document::RunStats s = doc.runStats(0);
    QCOMPARE(s.warn, 2);    // the memo resumed rather than froze...
    QCOMPARE(s.error, 1);
    QCOMPARE(s.lastTimestamp, doc.index().records.at(3).timestamp); // ...and the end moved
}

void TestRunSelect::aNewRunsRecordsStopBeingCountedInTheRunBeforeIt()
{
    // A restart mid-tail: the counts either side of the new boundary are each the
    // records of their OWN run, with a memo of run 0 standing from before the split.
    // (It holds because the split lands exactly where that memo stopped — the guard in
    // runStats() for an end that moves backwards is a consistency guard on the memo and
    // not a path any caller reaches; Document.cpp says so where the guard is.)
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("statssplit.log"));

    QByteArray whole;
    int sec = 0;
    whole += banner(sec++, 0);
    whole += rec(sec++, "t1", "WARN ", "svc", "w");
    QVERIFY(writeWhole(path, whole));

    Document doc;
    QVERIFY(openDoc(doc, path));
    doc.setRunStart(QString::fromLatin1(kMarker), false, Qt::CaseInsensitive);
    doc.applyFilters();

    LogModel model(&doc);
    LiveController live(&doc, &model);
    live.start();

    // Two ERROR records land, and the memo is asked about them...
    QVERIFY(append(path, rec(sec++, "t1", "ERROR", "svc", "e0")
                       + rec(sec++, "t1", "ERROR", "svc", "e1")));
    live.checkNow();
    QCOMPARE(doc.runStats(0).error, 2);

    // ...and only THEN does the application restart.
    QVERIFY(append(path, banner(sec++, 1) + rec(sec++, "t1", "FATAL", "svc", "f")));
    live.checkNow();
    QCOMPARE(doc.runs().size(), 2);

    QCOMPARE(doc.runStats(0).error, 2);  // run 0 keeps its own two...
    QCOMPARE(doc.runStats(0).fatal, 0);
    QCOMPARE(doc.runStats(1).fatal, 1);  // ...and the new run counts only what is its
    QCOMPARE(doc.runStats(1).error, 0);
}

QTEST_GUILESS_MAIN(TestRunSelect)
#include "tst_runselect.moc"
