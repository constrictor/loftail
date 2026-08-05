#include <QtTest>

#include <QByteArray>
#include <QFile>
#include <QTemporaryDir>

#include "ArchiveLocation.h"
#include "Document.h"
#include "Filter.h"
#include "LiveController.h"
#include "LogModel.h"
#include "ManualFormatProvider.h"
#include "Priority.h"
#include "RecordIndex.h"

using namespace loftail;

// M13 — a log that is not there (SPEC.md §3, ARCHITECTURE.md §6.5).
//
// Drives LiveController against a real temp file the test creates and deletes,
// calling checkNow() SYNCHRONOUSLY so each transition is deterministic — the same
// harness shape tst_tail uses, for the same reason (the watcher and the poll timer
// never fire because the test never spins an event loop).
//
// The four things it pins:
//   (a) opening a path that does not exist SUCCEEDS, into the waiting state, and the
//       log's format is settled from the bytes that eventually arrive rather than
//       guessed from the empty open;
//   (b) deleting an open log clears it to waiting and it comes back when the log does,
//       with the per-file state (filters, highlighters, run pattern) intact;
//   (c) a rotation inside the grace period NEVER enters waiting — the regression guard
//       for the hysteresis, and the reason the grace period exists at all;
//   (d) an address that is broken rather than absent still fails outright, with no
//       waiting state to hide it.
//
// Ungated and network-free: waiting is a contract of the live seam, exactly as
// isComplete() is (tst_complete), so it must hold in every build configuration.
//
// Linux only, like tst_tail: the mmap + unlink identity behaviour this leans on is
// POSIX, and Windows must be exercised separately.
class TestWaiting : public QObject
{
    Q_OBJECT

private:
    static constexpr auto kPattern = "%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n";

    static QByteArray rec(int sec, const char *prio, const char *logger, const char *msg)
    {
        QByteArray out = "2026-08-05 00:00:";
        out += QByteArray::number(sec).rightJustified(2, '0');
        out += ",000 [main] ";
        out += prio;
        out += "  ";
        out += logger;
        out += " - ";
        out += msg;
        out += '\n';
        return out;
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

    // What MainWindow does on resumeRequested, reduced to the one line core needs: the
    // owner supplies the provider because the pattern lives there, never in a Document
    // (invariant #3). Connecting this is what makes a waiting document resumable at
    // all — a document whose owner does not is documented to wait forever.
    static void wireResume(LiveController &live, Document &doc, LogModel &model,
                           const QString &pattern, int *resumes = nullptr)
    {
        QObject::connect(&live, &LiveController::resumeRequested, &live,
                         [&doc, &model, pattern, resumes] {
                             ManualFormatProvider provider(pattern);
                             model.beginFilterReset();
                             const bool ok = doc.resume(provider);
                             if (ok) {
                                 doc.resolveHighlighters();
                                 if (doc.filters().anyActive() || doc.viewRestricted())
                                     doc.applyFilters();
                             }
                             model.endFilterReset();
                             if (ok && resumes)
                                 ++*resumes;
                         });
    }

private slots:
    void absentPathOpensWaitingAndSettlesFormatOnArrival();
    void deletingAnOpenLogClearsToWaitingAndComesBack();
    void perFileStateSurvivesTheWait();
    void rotationInsideTheGracePeriodNeverWaits();
    void theGracePeriodIsElapsedTimeNotACheckCount();
    void aBrokenAddressStillFailsOutright();
};

void TestWaiting::absentPathOpensWaitingAndSettlesFormatOnArrival()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("notyet.log"));

    Document doc;
    ManualFormatProvider provider(QString::fromLatin1(kPattern));
    // The open SUCCEEDS. This is the whole change: a path that is not there is not a
    // failure any more, it is a log that has not arrived.
    QVERIFY(doc.prepare(path, provider, Encoding::Utf8, QTimeZone::utc()));
    QVERIFY(doc.isWaiting());
    QVERIFY(!doc.source()); // a LOCAL wait holds nothing at all
    QVERIFY(doc.lastError().isEmpty());
    QCOMPARE(doc.path(), path);
    QCOMPARE(doc.index().records.size(), 0);
    // Nothing has been sampled, so nothing about the format is known yet — which is
    // exactly what resume() has to put right when the bytes turn up.
    QVERIFY(!doc.formatSettled());
    QVERIFY(!doc.waitReason().isEmpty());

    LogModel model(&doc);
    LiveController live(&doc, &model);
    live.setVanishGrace(0);
    int resumes = 0;
    wireResume(live, doc, model, QString::fromLatin1(kPattern), &resumes);
    live.start();

    // Still nothing: a tick over an absent path changes exactly nothing.
    live.checkNow();
    QVERIFY(doc.isWaiting());
    QCOMPARE(resumes, 0);

    QVERIFY(writeWhole(path, rec(0, "INFO ", "net.io", "hello")
                                 + rec(1, "ERROR", "db.pool", "boom")));
    live.checkNow();

    QCOMPARE(resumes, 1);
    QVERIFY(!doc.isWaiting());
    QVERIFY(doc.waitReason().isEmpty());
    QCOMPARE(model.rowCount(), 2);

    // The format was settled from the bytes that ARRIVED, not from the empty open:
    // the records are parsed, with their fields and priorities picked up.
    QVERIFY(doc.formatSettled());
    QVERIFY(!doc.format().fields.isEmpty());
    QCOMPARE(doc.index().records.at(1).priorityEnum(), Priority::Error);
    QCOMPARE(doc.index().loggers.name(doc.index().records.at(0).loggerId),
             QStringLiteral("net.io"));

    // And it is an ordinary live log from here: appends land as they always do.
    QFile f(path);
    QVERIFY(f.open(QIODevice::Append));
    f.write(rec(2, "WARN ", "net.io", "slow"));
    f.close();
    live.checkNow();
    QCOMPARE(model.rowCount(), 3);
}

void TestWaiting::deletingAnOpenLogClearsToWaitingAndComesBack()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("gone.log"));
    QVERIFY(writeWhole(path, rec(0, "INFO ", "app", "one") + rec(1, "INFO ", "app", "two")));

    Document doc;
    QVERIFY(doc.open(path, QString::fromLatin1(kPattern), Encoding::Utf8, QTimeZone::utc()));
    QCOMPARE(doc.index().records.size(), 2);
    QVERIFY(doc.formatSettled());

    LogModel model(&doc);
    LiveController live(&doc, &model);
    live.setVanishGrace(0);
    int resumes = 0;
    wireResume(live, doc, model, QString::fromLatin1(kPattern), &resumes);
    live.start();

    QVector<QPair<bool, QString>> waitEvents;
    connect(&live, &LiveController::waitingChanged, &live,
            [&](bool waiting, const QString &reason) { waitEvents.append({waiting, reason}); });

    QVERIFY(QFile::remove(path));
    live.checkNow();

    // Cleared, not frozen on stale content. The mmap still holds the unlinked inode
    // and could go on serving those bytes, which is exactly why originVanished() is
    // asked instead of the size: nothing else notices a deletion.
    QVERIFY(doc.isWaiting());
    QCOMPARE(model.rowCount(), 0);
    QCOMPARE(doc.index().records.size(), 0);
    QCOMPARE(waitEvents.size(), 1);
    QVERIFY(waitEvents.first().first);
    QVERIFY(waitEvents.first().second.contains(QStringLiteral("gone.log")));
    // The source was released: nothing is held open for a file that is not there.
    QVERIFY(!doc.source());

    // Waiting does not stop the watch — that is the difference between waiting and
    // completing, and getting it wrong is how the log would never come back.
    live.checkNow();
    QVERIFY(doc.isWaiting());
    QCOMPARE(resumes, 0);

    QVERIFY(writeWhole(path, rec(5, "WARN ", "app", "back again")));
    live.checkNow();

    QCOMPARE(resumes, 1);
    QVERIFY(!doc.isWaiting());
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(waitEvents.size(), 2);
    QVERIFY(!waitEvents.last().first);
    // The format was ALREADY settled, so it is not recompiled — the same rule a
    // rotation follows (invariant #3), and the reason the new content parses at once.
    QCOMPARE(doc.index().records.at(0).priorityEnum(), Priority::Warn);
}

void TestWaiting::perFileStateSurvivesTheWait()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("filtered.log"));
    QVERIFY(writeWhole(path, rec(0, "INFO ", "app", "quiet") + rec(1, "ERROR", "app", "loud")));

    Document doc;
    QVERIFY(doc.open(path, QString::fromLatin1(kPattern), Encoding::Utf8, QTimeZone::utc()));
    doc.filters().priorityEnabled = true;
    doc.filters().minPriority = Priority::Error;
    doc.applyFilters();
    QCOMPARE(doc.filtered().recordCount(), 1);

    LogModel model(&doc);
    LiveController live(&doc, &model);
    live.setVanishGrace(0);
    wireResume(live, doc, model, QString::fromLatin1(kPattern));
    live.start();

    QVERIFY(QFile::remove(path));
    live.checkNow();
    QVERIFY(doc.isWaiting());
    // The FILTER is per-file state and the file is coming back, so it survives the
    // wait; only the index, which described bytes that are gone, does not.
    QVERIFY(doc.filters().anyActive());
    QCOMPARE(doc.filters().minPriority, Priority::Error);

    QVERIFY(writeWhole(path, rec(0, "INFO ", "app", "quiet again")
                                 + rec(1, "ERROR", "app", "loud again")
                                 + rec(2, "FATAL", "app", "louder")));
    live.checkNow();

    QVERIFY(!doc.isWaiting());
    QCOMPARE(doc.index().records.size(), 3);
    // Re-applied on the way out, so the view is narrowed exactly as it was before.
    QCOMPARE(doc.filtered().recordCount(), 2);
    QCOMPARE(model.rowCount(), 2);
}

void TestWaiting::rotationInsideTheGracePeriodNeverWaits()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("rotate.log"));
    QVERIFY(writeWhole(path, rec(0, "INFO ", "app", "orig")));

    Document doc;
    QVERIFY(doc.open(path, QString::fromLatin1(kPattern), Encoding::Utf8, QTimeZone::utc()));
    LogModel model(&doc);
    LiveController live(&doc, &model);
    // The DEFAULT grace period, deliberately — this case is about what it buys.
    wireResume(live, doc, model, QString::fromLatin1(kPattern));
    live.start();

    int waits = 0;
    int rescans = 0;
    connect(&live, &LiveController::waitingChanged, &live, [&](bool w, const QString &) {
        if (w)
            ++waits;
    });
    connect(&live, &LiveController::rescanned, &live, [&] { ++rescans; });

    // logrotate renames and only then creates. A check landing in that gap must NOT
    // blank the view: it sees a path with nothing at it, which is indistinguishable
    // from a deletion in that instant and is told apart only by waiting a moment.
    QVERIFY(QFile::rename(path, path + QStringLiteral(".1")));
    live.checkNow();
    QVERIFY(!doc.isWaiting());
    QCOMPARE(waits, 0);
    QCOMPARE(model.rowCount(), 1); // still showing the old content, not an empty grid

    QVERIFY(writeWhole(path, rec(0, "WARN ", "app", "rotated one")
                                 + rec(1, "WARN ", "app", "rotated two")));
    live.checkNow();

    // Resolved as the rotation it always was: a silent rescan, and the waiting state
    // never entered at all.
    QCOMPARE(waits, 0);
    QCOMPARE(rescans, 1);
    QCOMPARE(model.rowCount(), 2);
}

void TestWaiting::theGracePeriodIsElapsedTimeNotACheckCount()
{
    // The grace period has to be real time, because checks are not evenly spaced: the
    // filesystem watcher fires them too, and a rotation produces a burst of them at
    // exactly the moment the path is briefly empty. Counting checks would shorten the
    // grace period precisely when it is doing its job.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("grace.log"));
    QVERIFY(writeWhole(path, rec(0, "INFO ", "app", "one")));

    Document doc;
    QVERIFY(doc.open(path, QString::fromLatin1(kPattern), Encoding::Utf8, QTimeZone::utc()));
    LogModel model(&doc);
    LiveController live(&doc, &model);
    live.setVanishGrace(300);
    wireResume(live, doc, model, QString::fromLatin1(kPattern));
    live.start();

    QVERIFY(QFile::remove(path));
    // Twenty checks in far less than 300 ms. A count-based grace period would have
    // given up long before here.
    for (int i = 0; i < 20; ++i)
        live.checkNow();
    QVERIFY(!doc.isWaiting());
    QCOMPARE(model.rowCount(), 1);

    QTest::qWait(350);
    live.checkNow();
    QVERIFY(doc.isWaiting());
    QCOMPARE(model.rowCount(), 0);
}

void TestWaiting::aBrokenAddressStillFailsOutright()
{
    // Absent is not the same as malformed, and only one of them is worth waiting for.
    // An address that names no log will never name one however long loftail waits.
    Document bad;
    ManualFormatProvider provider(QString::fromLatin1(kPattern));
    QVERIFY(!bad.prepare(QStringLiteral("ssh://"), provider, Encoding::Utf8, QTimeZone::utc()));
    QVERIFY(!bad.isWaiting());
    QVERIFY(!bad.lastError().isEmpty());

    // An archive that names no member is the same kind of refusal: the member is
    // chosen once, at the interactive entry point, and an address without one cannot
    // be opened by waiting for it.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString container = dir.filePath(QStringLiteral("bundle.tar"));
    QVERIFY(writeWhole(container, QByteArray("not really a tar")));
    const auto split = ArchiveLocation::split(container);
    QVERIFY(split);
    QVERIFY(split->needsMember()); // a multi-member container spelled without one

    Document noMember;
    QVERIFY(!noMember.prepare(container, provider, Encoding::Utf8, QTimeZone::utc()));
    QVERIFY(!noMember.isWaiting());
    QVERIFY(!noMember.lastError().isEmpty());
}

QTEST_GUILESS_MAIN(TestWaiting)
#include "tst_waiting.moc"
