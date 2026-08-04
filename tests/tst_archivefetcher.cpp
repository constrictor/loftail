#include <QtTest>

#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>

#include "ArchiveFetcher.h"
#include "ArchiveFixtures.h"
#include "ArchiveLocation.h"
#include "SourceFetcher.h"

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
    static QByteArray spoolContents(const SourceFetcher &fetcher);

    QTemporaryDir m_dir;
    int           m_spools = 0;
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

    // Document::prepare() takes a 64 KB sample the instant start() returns. Without a
    // synchronous prime it would sample an empty file and autodetect nothing.
    const FetchStatus primed = fetcher->status();
    QVERIFY2(primed.committedSize >= 64 * 1024,
             qPrintable(QStringLiteral("only %1 bytes were primed").arg(primed.committedSize)));
    QVERIFY(primed.committedSize < body.size()); // and it did NOT expand the whole thing

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

    // Closing a tab drops the last handle, whose destructor stops the fetcher and then
    // deletes the spool directory. A loop that only checked its flag between members
    // would hold that up for the length of the whole expansion.
    QElapsedTimer clock;
    clock.start();
    fetcher->stop();
    QVERIFY2(clock.elapsed() < 2000,
             qPrintable(QStringLiteral("stop() took %1 ms").arg(clock.elapsed())));
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
    fetcher->start(missing, &error);
    QVERIFY(!error.isEmpty());
    QVERIFY2(!error.contains(QStringLiteral("Not enough space")), qPrintable(error));
}

void TestArchiveFetcher::aStoppedExpansionKeepsWhatItWrote()
{
    // Cancelling the initial scan cancels the expansion (MainWindow::onIndexFinished),
    // and what was expanded stays readable: a cancelled scan leaves whatever it scanned
    // usable, and the same promise has to hold for the bytes underneath it.
    const QByteArray body = logBody(200000);
    const QString gz = path(QStringLiteral("stopped.log.gz"));
    QVERIFY(writeGzip(gz, body));

    QString error;
    auto fetcher = makeArchiveFetcher(at(gz), &error);
    QVERIFY2(fetcher, qPrintable(error));
    QVERIFY2(fetcher->start(spoolDir(), &error), qPrintable(error));

    const qint64 committedBefore = fetcher->status().committedSize;
    QVERIFY(committedBefore > 0);
    fetcher->stop();

    const FetchStatus after = fetcher->status();
    QVERIFY(after.state != FetchStatus::State::Complete);
    // Never fewer bytes than were published before the stop, and the spool file still
    // holds at least what was committed: nothing is retracted or truncated.
    QVERIFY(after.committedSize >= committedBefore);
    QCOMPARE(spoolContents(*fetcher).size(), after.committedSize);
    QVERIFY(spoolContents(*fetcher).startsWith(body.left(1024)));
}

QTEST_GUILESS_MAIN(TestArchiveFetcher)
#include "tst_archivefetcher.moc"
