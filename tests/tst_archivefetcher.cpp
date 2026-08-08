#include <QtTest>

#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>

#include "ArchiveFetcher.h"
#include "ArchiveFixtures.h"
#include "ArchiveLocation.h"
#include "FakeFetcher.h"
#include "SourceFetcher.h"
#include "SourceSpool.h"

using namespace loftail;
using namespace loftail::fixtures;

// M12 — the fetcher that expands one archive member into a spool (ARCHITECTURE.md §6.4).
//
// Two things here are load-bearing beyond "the bytes come out right". The PRIME: start()
// must return with enough expanded that Document::prepare's 64 KB format sample sees
// real bytes, or every archive would autodetect as empty. And the PUBLISH ORDERING:
// State::Complete is published after the final committedSize, so a reader that observes
// Complete is guaranteed to observe the final size — get it backwards and the live
// controller stops one chunk short, silently losing the last records.
//
// Gated on LOFTAIL_HAVE_ARCHIVE. Unlike M11's transport, this needs no network and no
// credentials, so it runs in CI for real (PLAN.md M12).
class TestArchiveFetcher : public QObject
{
    Q_OBJECT

private slots:
    void expandsAGzipStreamByteForByte();
    void expandsAnXzStream();
    void extractsTheNamedMemberAndOnlyThat();
    void startPrimesEnoughForAFormatSample();
    void completeIsPublishedAfterTheFinalSize();
    void committedSizeNeverRunsAheadOfTheSpoolFile();
    void aCorruptArchiveFailsAndKeepsWhatItExpanded();
    void stopReturnsPromptlyMidExpansion();
    void anUnanswerableSpaceQuestionDoesNotRefuseTheOpen();
    void aStoppedExpansionKeepsWhatItWrote();

private:
    QString path(const QString &name) const { return m_dir.path() + u'/' + name; }
    QString spoolDir();
    static ArchiveLocation at(const QString &container, const QString &member = QString());
    static bool runToEnd(SourceFetcher &fetcher, int timeoutMs = 30000);
    bool waitForPrime(SourceFetcher &fetcher, int timeoutMs = 30000);
    static bool waitForError(SourceFetcher &fetcher, int timeoutMs = 30000);
    static QByteArray spoolContents(const SourceFetcher &fetcher);
    static QByteArray fileBytes(const QString &path);

    QTemporaryDir m_dir;
    int           m_spools = 0;
    // The first committedSize waitForPrime() saw; see the prime-atomicity rule.
    qint64        m_firstCommitted = 0;
};

QString TestArchiveFetcher::spoolDir()
{
    const QString dir = m_dir.path() + QStringLiteral("/spool-%1").arg(++m_spools);
    QDir().mkpath(dir);
    return dir;
}

ArchiveLocation TestArchiveFetcher::at(const QString &container, const QString &member)
{
    ArchiveLocation loc;
    loc.container = container;
    loc.member = member;
    return loc;
}

bool TestArchiveFetcher::runToEnd(SourceFetcher &fetcher, int timeoutMs)
{
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < timeoutMs) {
        const FetchStatus status = fetcher.status();
        if (status.state == FetchStatus::State::Complete)
            return true;
        if (status.state == FetchStatus::State::Error)
            return false;
        QThread::msleep(5);
    }
    return false;
}

// Wait for the prime to land: the first committed bytes, or a terminal state.
//
// Needed since M17, where start() spawns the worker and returns rather than expanding
// anything itself — so "the bytes are there when start() returns" stopped being true.
// What replaced it is that they arrive PROMPTLY and ALL AT ONCE (SourceFetcher.h), which
// is what the callers of this then assert.
bool TestArchiveFetcher::waitForPrime(SourceFetcher &fetcher, int timeoutMs)
{
    m_firstCommitted = 0;
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < timeoutMs) {
        const FetchStatus status = fetcher.status();
        if (status.committedSize > 0) {
            // The first size anyone could have seen — which is the figure the
            // prime-atomicity rule is about, and the only one a later poll cannot
            // recover once the worker has carried on past it.
            m_firstCommitted = status.committedSize;
            return true;
        }
        if (status.state == FetchStatus::State::Error
            || status.state == FetchStatus::State::Complete
            || status.state == FetchStatus::State::Disconnected)
            return status.committedSize > 0;
        QThread::msleep(5);
    }
    return false;
}

bool TestArchiveFetcher::waitForError(SourceFetcher &fetcher, int timeoutMs)
{
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < timeoutMs) {
        if (fetcher.status().state == FetchStatus::State::Error)
            return true;
        QThread::msleep(5);
    }
    return false;
}

QByteArray TestArchiveFetcher::fileBytes(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QByteArray();
    return file.readAll();
}

QByteArray TestArchiveFetcher::spoolContents(const SourceFetcher &fetcher)
{
    QFile spool(fetcher.spoolPath(fetcher.status().generation));
    if (!spool.open(QIODevice::ReadOnly))
        return QByteArray();
    return spool.readAll();
}

void TestArchiveFetcher::expandsAGzipStreamByteForByte()
{
    const QByteArray body = logBody(4000); // comfortably past the prime and several chunks
    const QString gz = path(QStringLiteral("app.log.gz"));
    QVERIFY(writeGzip(gz, body));

    QString error;
    auto fetcher = makeArchiveFetcher(at(gz), &error);
    QVERIFY2(fetcher, qPrintable(error));
    QVERIFY2(fetcher->start(spoolDir(), &error), qPrintable(error));
    QVERIFY(runToEnd(*fetcher));

    // The acceptance property: what comes out is what went in. Anything less and the
    // record offsets the indexer produced would not describe the file the user reads.
    QCOMPARE(spoolContents(*fetcher), body);
    QCOMPARE(fetcher->status().committedSize, qint64(body.size()));
}

void TestArchiveFetcher::expandsAnXzStream()
{
    const QByteArray body = logBody(2000);
    const QString xz = path(QStringLiteral("app.log.xz"));
    QVERIFY(writeXz(xz, body));

    QString error;
    auto fetcher = makeArchiveFetcher(at(xz), &error);
    QVERIFY2(fetcher, qPrintable(error));
    QVERIFY2(fetcher->start(spoolDir(), &error), qPrintable(error));
    QVERIFY(runToEnd(*fetcher));
    QCOMPARE(spoolContents(*fetcher), body);
}

void TestArchiveFetcher::extractsTheNamedMemberAndOnlyThat()
{
    const QByteArray wanted = logBody(1500);
    const QByteArray other = QByteArrayLiteral("this must not appear in the spool\n");
    const QString tgz = path(QStringLiteral("logs.tar.gz"));
    QVERIFY(writeTarGz(tgz, {{QStringLiteral("db.log"), other},
                             {QStringLiteral("app.log"), wanted},
                             {QStringLiteral("web.log"), other}}));

    QString error;
    auto fetcher = makeArchiveFetcher(at(tgz, QStringLiteral("app.log")), &error);
    QVERIFY2(fetcher, qPrintable(error));
    QVERIFY2(fetcher->start(spoolDir(), &error), qPrintable(error));
    QVERIFY(runToEnd(*fetcher));

    const QByteArray spooled = spoolContents(*fetcher);
    QCOMPARE(spooled, wanted);
    QVERIFY(!spooled.contains(other)); // neither the member before nor the one after
}

void TestArchiveFetcher::startPrimesEnoughForAFormatSample()
{
    const QByteArray body = logBody(20000); // far more than start() should expand
    const QString gz = path(QStringLiteral("big.log.gz"));
    QVERIFY(writeGzip(gz, body));

    QString error;
    auto fetcher = makeArchiveFetcher(at(gz), &error);
    QVERIFY2(fetcher, qPrintable(error));
    QVERIFY2(fetcher->start(spoolDir(), &error), qPrintable(error));

    // The document settles its format and its encoding from a 64 KB sample taken at the
    // FIRST committed bytes. Since M17 those arrive on the worker rather than inside
    // start(), so what has to hold is not that they are there immediately but that when
    // they appear there are enough of them: the prime is published all at once, never
    // one short read at a time (SourceFetcher.h).
    QVERIFY(waitForPrime(*fetcher));
    QVERIFY2(m_firstCommitted >= 64 * 1024,
             qPrintable(QStringLiteral("the first published size was only %1 bytes")
                            .arg(m_firstCommitted)));

    // NOT asserted any more: that the prime stopped short of the whole member. That was
    // a statement about start() returning early, and start() no longer expands anything
    // itself — the worker carries straight on from the prime into the rest, so by the
    // time any observer looks, a member this size may legitimately be finished. What
    // survives, and is what the document actually depends on, is the line above: the
    // FIRST size anyone can see already covers a full format sample.

    QVERIFY(runToEnd(*fetcher));
    QCOMPARE(spoolContents(*fetcher), body);
}

void TestArchiveFetcher::completeIsPublishedAfterTheFinalSize()
{
    const QByteArray body = logBody(8000);
    const QString gz = path(QStringLiteral("ordering.log.gz"));
    QVERIFY(writeGzip(gz, body));

    QString error;
    auto fetcher = makeArchiveFetcher(at(gz), &error);
    QVERIFY2(fetcher, qPrintable(error));
    QVERIFY2(fetcher->start(spoolDir(), &error), qPrintable(error));

    // Poll the way LiveController does, and assert the invariant on EVERY sample that
    // sees Complete: the size is already final. This is the one true race in M12 —
    // publish Complete first and the watch stops a chunk short, losing records with no
    // error anywhere.
    QElapsedTimer clock;
    clock.start();
    bool sawComplete = false;
    while (clock.elapsed() < 30000 && !sawComplete) {
        const FetchStatus status = fetcher->status();
        if (status.state == FetchStatus::State::Complete) {
            sawComplete = true;
            QCOMPARE(status.committedSize, qint64(body.size()));
            QCOMPARE(status.totalSize, status.committedSize);
        }
        QThread::msleep(1);
    }
    QVERIFY(sawComplete);
    QCOMPARE(spoolContents(*fetcher), body);
}

void TestArchiveFetcher::committedSizeNeverRunsAheadOfTheSpoolFile()
{
    const QByteArray body = logBody(12000);
    const QString gz = path(QStringLiteral("torn.log.gz"));
    QVERIFY(writeGzip(gz, body));

    QString error;
    auto fetcher = makeArchiveFetcher(at(gz), &error);
    QVERIFY2(fetcher, qPrintable(error));
    QVERIFY2(fetcher->start(spoolDir(), &error), qPrintable(error));

    // The clamp a reader relies on: committedSize is published only after the write
    // lands, so a reader clamping to it can never see a half-written chunk.
    QElapsedTimer clock;
    clock.start();
    int samples = 0;
    while (clock.elapsed() < 30000) {
        const FetchStatus status = fetcher->status();
        const QFileInfo onDisk(fetcher->spoolPath(status.generation));
        QVERIFY2(status.committedSize <= onDisk.size(),
                 qPrintable(QStringLiteral("committed %1 > on disk %2")
                                .arg(status.committedSize).arg(onDisk.size())));
        ++samples;
        if (status.state == FetchStatus::State::Complete)
            break;
        QThread::msleep(1);
    }
    QVERIFY(samples > 1);
}

void TestArchiveFetcher::aCorruptArchiveFailsAndKeepsWhatItExpanded()
{
    // A gzip stream cut in half: the header and some data are readable, the rest is not.
    const QByteArray body = logBody(6000);
    const QString gz = path(QStringLiteral("truncated.log.gz"));
    QVERIFY(writeGzip(gz, body));

    QFile whole(gz);
    QVERIFY(whole.open(QIODevice::ReadOnly));
    const QByteArray compressed = whole.readAll();
    whole.close();
    QVERIFY(compressed.size() > 200);

    const QString cut = path(QStringLiteral("cut.log.gz"));
    QFile shortened(cut);
    QVERIFY(shortened.open(QIODevice::WriteOnly));
    shortened.write(compressed.left(compressed.size() / 2));
    shortened.close();

    QString error;
    auto fetcher = makeArchiveFetcher(at(cut), &error);
    QVERIFY2(fetcher, qPrintable(error));
    // start() may succeed (the head decompresses) or fail (the cut lands in the header);
    // either way the outcome must be an explained failure and never a silent success.
    fetcher->start(spoolDir(), &error);
    runToEnd(*fetcher);

    const FetchStatus status = fetcher->status();
    QCOMPARE(status.state, FetchStatus::State::Error);
    QVERIFY2(!status.error.isEmpty(), "a corrupt archive must explain itself");
    QVERIFY(status.state != FetchStatus::State::Complete);

    // Whatever did expand stays readable rather than being thrown away: a partly
    // recoverable log is worth more than an empty window.
    QVERIFY(spoolContents(*fetcher).size() == status.committedSize);
}

void TestArchiveFetcher::stopReturnsPromptlyMidExpansion()
{
    const QByteArray body = logBody(200000); // big enough that it cannot finish at once
    const QString gz = path(QStringLiteral("huge.log.gz"));
    QVERIFY(writeGzip(gz, body));

    QString error;
    auto fetcher = makeArchiveFetcher(at(gz), &error);
    QVERIFY2(fetcher, qPrintable(error));
    QVERIFY2(fetcher->start(spoolDir(), &error), qPrintable(error));
    QVERIFY(fetcher->status().state != FetchStatus::State::Complete);

    // Closing a tab drops the last handle, whose destructor retires the fetcher and lets
    // the registry delete the spool directory once the worker has gone. Two separate
    // promises, and both matter: asking must not block the asker at all, and the worker
    // must then wind up quickly — a loop that only checked its flag between members
    // would go on expanding for the length of the whole file.
    QElapsedTimer clock;
    clock.start();
    fetcher->requestStop();
    QVERIFY2(clock.elapsed() < 100,
             qPrintable(QStringLiteral("requestStop() took %1 ms").arg(clock.elapsed())));

    while (!fetcher->isStopped() && clock.elapsed() < 2000)
        QThread::msleep(5);
    QVERIFY2(fetcher->isStopped(),
             qPrintable(QStringLiteral("the worker was still expanding after %1 ms")
                            .arg(clock.elapsed())));
}

void TestArchiveFetcher::anUnanswerableSpaceQuestionDoesNotRefuseTheOpen()
{
    // The free-space check has two halves and this covers the second one: when
    // QStorageInfo cannot say — an unreadable or absent spool directory — the open must
    // PROCEED, because refusing over a question that cannot be answered would break
    // opens on filesystems the check does not understand.
    //
    // NOT COVERED HERE: the refusal itself. A genuinely full filesystem cannot be
    // conjured portably without root, and adding an injection seam for a courtesy check
    // would cost more than it is worth. The refusal message is exercised by hand.
    const QByteArray body = logBody(500);
    const QString gz = path(QStringLiteral("space.log.gz"));
    QVERIFY(writeGzip(gz, body));

    QString error;
    auto fetcher = makeArchiveFetcher(at(gz), &error);
    QVERIFY2(fetcher, qPrintable(error));
    const QString missing = m_dir.path() + QStringLiteral("/no-such-dir");
    // Unanswerable, so it proceeds — and then fails on the write, not on the guess.
    // Reported through the published status rather than out of start(), which since M17
    // returns before anything has been attempted.
    QVERIFY(fetcher->start(missing, &error));
    QVERIFY(waitForError(*fetcher));
    const QString reported = fetcher->status().error;
    QVERIFY(!reported.isEmpty());
    QVERIFY2(!reported.contains(QStringLiteral("Not enough space")), qPrintable(reported));
}

void TestArchiveFetcher::aStoppedExpansionKeepsWhatItWrote()
{
    // Cancelling the initial scan cancels the expansion (MainWindow::onIndexFinished),
    // and what was expanded stays readable: a cancelled scan leaves whatever it scanned
    // usable, and the same promise has to hold for the bytes underneath it.
    //
    // The stop has to land while the expansion is genuinely unfinished, and that is
    // arranged rather than raced for. A LOCAL container cannot do it: expanding one is
    // pure CPU and page cache, so even a deliberately huge member is over in a few
    // milliseconds -- measured at ~6 ms for logBody(200000), which is less than one
    // Windows scheduler tick. A test that primes, sleeps and then stops is not testing
    // a stop at all; it is betting that the poll after the sleep lands inside those few
    // milliseconds, and CI eventually collects on that bet.
    //
    // So the container arrives from a fake remote with its tail still outstanding
    // (setTotalSize past what has been delivered). The expansion then parks in
    // ArchiveFetcher::awaitInput() waiting for container bytes that never come, and
    // "still expanding when the stop arrives" is true by construction, on every machine
    // and at any speed. Only the transport is fake; the archive fetcher is the real one.
    const QByteArray body = logBody(20000);
    const QString staging = path(QStringLiteral("staging.log.gz"));
    QVERIFY(writeGzip(staging, body));
    const QByteArray compressed = fileBytes(staging);
    QVERIFY(compressed.size() > 2);

    const QString url = QStringLiteral("ssh://deploy@web1/var/log/stopped.log.gz");
    FakeRemoteFarm farm;
    const auto remote = farm.at(url);
    // Half the container, and a size that says the other half is still coming.
    remote->setInitialContent(compressed.left(compressed.size() / 2));
    remote->setTotalSize(compressed.size());

    QString error;
    auto fetcher = makeArchiveFetcher(at(url), &error);
    QVERIFY2(fetcher, qPrintable(error));
    QVERIFY2(fetcher->start(spoolDir(), &error), qPrintable(error));

    QVERIFY(waitForPrime(*fetcher));
    const qint64 committedBefore = fetcher->status().committedSize;
    QVERIFY(committedBefore > 0);
    // Nothing can have finished it: the rest of the container has not been delivered.
    QVERIFY(fetcher->status().state != FetchStatus::State::Complete);
    fetcher->requestStop();
    while (!fetcher->isStopped())
        QThread::msleep(5);

    const FetchStatus after = fetcher->status();
    // Disconnected, which is what requestStop() publishes -- not Complete (a cut-short
    // expansion is not a finished one) and not Error. The Error is the one worth spelling
    // out: the stop ends the input under libarchive, so the cancelled read comes back as
    // "truncated gzip input", and reporting that would tell the user their archive is
    // corrupt when what actually happened is that they cancelled the scan.
    QCOMPARE(after.state, FetchStatus::State::Disconnected);
    QVERIFY2(after.error.isEmpty(), qPrintable(after.error));
    // Never fewer bytes than were published before the stop, and the spool file still
    // holds at least what was committed: nothing is retracted or truncated.
    QVERIFY(after.committedSize >= committedBefore);
    QCOMPARE(spoolContents(*fetcher).size(), after.committedSize);
    QVERIFY(spoolContents(*fetcher).startsWith(body.left(1024)));
}

QTEST_GUILESS_MAIN(TestArchiveFetcher)
#include "tst_archivefetcher.moc"
