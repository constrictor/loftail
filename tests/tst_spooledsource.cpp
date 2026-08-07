#include <QtTest>

#include <QByteArray>

#include "FakeFetcher.h"
#include "LogSource.h"
#include "SourceSpool.h"
#include "SpooledLogSource.h"

using namespace loftail;

// M11 — the spooled source (ARCHITECTURE.md §6.3). A remote file is read through a
// local spool an independent thread is filling, so the contract under test is what
// the GUI thread is allowed to see while that is happening:
//   (a) never a byte the fetcher has not COMMITTED — this is what stands in for a
//       lock between the two threads, so it has to hold exactly;
//   (b) a rotation arrives as a new generation, never as a rewrite of the file the
//       index may be mmapping, and is reported through wasReplaced()/wasTruncated();
//   (c) isRandomAccess() is true, because the thing actually being read is local.
// No network and no libssh2: the transport is a fake the test drives by hand.
class TestSpooledSource : public QObject
{
    Q_OBJECT

private:
    static constexpr auto kUrl = "ssh://deploy@web1/var/log/app.log";

    // Open a spooled source over a fake whose initial content is `initial`.
    static std::unique_ptr<LogSource> openSource(QString *error = nullptr)
    {
        return openLogSource(QString::fromLatin1(kUrl), OpenPolicy::Interactive, error);
    }

private slots:
    void readsInitialContentThroughTheSpool();
    void sizeClampsToCommittedNotToTheSpoolFile();
    void bytesNeverExposesWithheldTail();
    void appendBecomesVisibleOnRefresh();
    void rotationBumpsGenerationAndIsReported();
    void rotationSwapsTheSpoolFileNotItsContents();
    void truncationLooksLikeARotation();
    void spoolIsSharedBetweenTwoSources();
    void reusePolicyRefusesToConnect();
    void reusePolicyJoinsALiveSpool();
    void openFailureReportsTheTransportError();
    void unconfiguredRemoteReportsNotBuiltIn();
};

void TestSpooledSource::readsInitialContentThroughTheSpool()
{
    FakeRemoteFarm farm;
    farm.at(QString::fromLatin1(kUrl))->setInitialContent(QByteArrayLiteral("hello remote"));

    auto src = openSource();
    QVERIFY(src);
    QCOMPARE(src->size(), 12);
    QCOMPARE(src->bytes(0, 12).toByteArray(), QByteArrayLiteral("hello remote"));
    // The spool is an ordinary local file, so the paint path keeps random access —
    // §6.2 predicted false here, and the spool is why it is true (see §6.3).
    QVERIFY(src->isRandomAccess());
    QVERIFY(!src->wasTruncated());
    QVERIFY(!src->wasReplaced());
}

void TestSpooledSource::sizeClampsToCommittedNotToTheSpoolFile()
{
    FakeRemoteFarm farm;
    auto remote = farm.at(QString::fromLatin1(kUrl));
    remote->setInitialContent(QByteArrayLiteral("0123456789"));

    auto src = openSource();
    QVERIFY(src);
    QCOMPARE(src->size(), 10);

    // Bytes on disk but not published: the fetcher's write has landed, its
    // committedSize store has not. This is the torn-chunk window, and the ONLY thing
    // protecting a reader from it is the clamp.
    remote->appendWithheld(QByteArrayLiteral("ABCDEFGHIJ"));
    QCOMPARE(src->refreshSize(), 10);
    QCOMPARE(src->size(), 10);

    remote->publish();
    QCOMPARE(src->refreshSize(), 20);
    QCOMPARE(src->bytes(0, 20).toByteArray(), QByteArrayLiteral("0123456789ABCDEFGHIJ"));
}

void TestSpooledSource::bytesNeverExposesWithheldTail()
{
    FakeRemoteFarm farm;
    auto remote = farm.at(QString::fromLatin1(kUrl));
    remote->setInitialContent(QByteArrayLiteral("0123456789"));

    auto src = openSource();
    QVERIFY(src);
    remote->appendWithheld(QByteArrayLiteral("XXXXX"));
    QCOMPARE(src->refreshSize(), 10);

    // A read that would run into the withheld region is clamped, not served.
    QCOMPARE(src->bytes(5, 100).toByteArray(), QByteArrayLiteral("56789"));
    // A read starting inside it returns nothing at all.
    QVERIFY(src->bytes(10, 5).isEmpty());
    QVERIFY(src->bytes(12, 1).isEmpty());
    // Out-of-range arguments behave as they do for a local source.
    QVERIFY(src->bytes(-1, 5).isEmpty());
    QVERIFY(src->bytes(0, 0).isEmpty());
}

void TestSpooledSource::appendBecomesVisibleOnRefresh()
{
    FakeRemoteFarm farm;
    auto remote = farm.at(QString::fromLatin1(kUrl));
    remote->setInitialContent(QByteArrayLiteral("first\n"));

    auto src = openSource();
    QVERIFY(src);
    remote->append(QByteArrayLiteral("second\n"));

    // size() is the size as of the last refreshSize(), for a remote source exactly as
    // for a local one — growth is not observed until it is asked for.
    QCOMPARE(src->size(), 6);
    QCOMPARE(src->refreshSize(), 13);
    QCOMPARE(src->bytes(0, 13).toByteArray(), QByteArrayLiteral("first\nsecond\n"));
    QVERIFY(!src->wasTruncated());
    QVERIFY(!src->wasReplaced());
}

void TestSpooledSource::rotationBumpsGenerationAndIsReported()
{
    FakeRemoteFarm farm;
    auto remote = farm.at(QString::fromLatin1(kUrl));
    remote->setInitialContent(QByteArrayLiteral("old content"));

    auto src = openSource();
    QVERIFY(src);
    const quint64 firstGeneration = src->identity();
    QVERIFY(firstGeneration != 0);

    remote->replaceWith(QByteArrayLiteral("brand new"));

    // wasReplaced() is answered from the published generation, WITHOUT a refresh:
    // LiveController reads it before refreshSize(), so it has to be true already.
    QVERIFY(src->wasReplaced());

    QCOMPARE(src->refreshSize(), 9);
    QCOMPARE(src->bytes(0, 9).toByteArray(), QByteArrayLiteral("brand new"));
    QVERIFY(src->identity() != firstGeneration);
    // Truncation is latched as well, so a caller that never asks wasReplaced() still
    // learns the byte stream is discontinuous rather than reading stale offsets.
    QVERIFY(src->wasTruncated());
    // Having adopted the new generation, it is no longer "replaced".
    QVERIFY(!src->wasReplaced());
}

void TestSpooledSource::rotationSwapsTheSpoolFileNotItsContents()
{
    FakeRemoteFarm farm;
    auto remote = farm.at(QString::fromLatin1(kUrl));
    remote->setInitialContent(QByteArrayLiteral("generation one"));

    auto src = openSource();
    QVERIFY(src);
    auto *spooled = dynamic_cast<SpooledLogSource *>(src.get());
    QVERIFY(spooled);
    const QString firstPath = spooled->spool()->spoolPath(1);
    QVERIFY(QFile::exists(firstPath));

    remote->replaceWith(QByteArrayLiteral("generation two"));
    src->refreshSize();

    // The old generation's file is UNTOUCHED. That is the invariant that makes
    // rotation safe while the index worker may be mmapping it: a new file, never a
    // rewrite of the live one.
    QFile old(firstPath);
    QVERIFY(old.open(QIODevice::ReadOnly));
    QCOMPARE(old.readAll(), QByteArrayLiteral("generation one"));
    QVERIFY(spooled->spool()->spoolPath(2) != firstPath);
}

void TestSpooledSource::truncationLooksLikeARotation()
{
    FakeRemoteFarm farm;
    auto remote = farm.at(QString::fromLatin1(kUrl));
    remote->setInitialContent(QByteArrayLiteral("aaaaaaaaaaaaaaaaaaaa")); // 20 bytes

    auto src = openSource();
    QVERIFY(src);
    QCOMPARE(src->size(), 20);

    // A remote copytruncate: the file shrank. The fetcher's response is the same as
    // for a rotation — a new generation — so the source reports it the same way and
    // the controller takes the same rescan path.
    remote->replaceWith(QByteArrayLiteral("bb"));
    QVERIFY(src->wasReplaced());
    QCOMPARE(src->refreshSize(), 2);
    QVERIFY(src->wasTruncated());
    QCOMPARE(src->bytes(0, 2).toByteArray(), QByteArrayLiteral("bb"));
}

void TestSpooledSource::spoolIsSharedBetweenTwoSources()
{
    FakeRemoteFarm farm;
    auto remote = farm.at(QString::fromLatin1(kUrl));
    remote->setInitialContent(QByteArrayLiteral("shared"));

    auto first = openSource();
    QVERIFY(first);
    auto second = openSource();
    QVERIFY(second);

    // Two Documents on one remote file must mean one connection and one spool — this
    // is what makes a second tab, and a rescan, free.
    QCOMPARE(remote->startCount(), 1);
    auto *a = dynamic_cast<SpooledLogSource *>(first.get());
    auto *b = dynamic_cast<SpooledLogSource *>(second.get());
    QVERIFY(a && b);
    QCOMPARE(a->spool().get(), b->spool().get());

    remote->append(QByteArrayLiteral("!"));
    QCOMPARE(first->refreshSize(), 7);
    QCOMPARE(second->refreshSize(), 7);

    // The fetcher lives exactly as long as the last handle.
    first.reset();
    QCOMPARE(remote->stopCount(), 0);
    second.reset();
    QCOMPARE(remote->stopCount(), 1);
}

void TestSpooledSource::reusePolicyRefusesToConnect()
{
    FakeRemoteFarm farm;
    farm.at(QString::fromLatin1(kUrl))->setInitialContent(QByteArrayLiteral("x"));

    // Reuse is what Document::rescan() passes. With nothing connected it must fail
    // fast rather than start a connection — that call happens on the GUI thread from
    // the live watch tick, where connecting would be a hang.
    QString error;
    auto src = openLogSource(QString::fromLatin1(kUrl), OpenPolicy::Reuse, &error);
    QVERIFY(!src);
    QVERIFY(!error.isEmpty());
    QCOMPARE(farm.at(QString::fromLatin1(kUrl))->startCount(), 0);
}

void TestSpooledSource::reusePolicyJoinsALiveSpool()
{
    FakeRemoteFarm farm;
    auto remote = farm.at(QString::fromLatin1(kUrl));
    remote->setInitialContent(QByteArrayLiteral("live"));

    auto held = openSource();
    QVERIFY(held);

    // With a live spool the same call is a pointer swap: no second connection.
    QString error;
    auto reused = openLogSource(QString::fromLatin1(kUrl), OpenPolicy::Reuse, &error);
    QVERIFY(reused);
    QVERIFY(error.isEmpty());
    QCOMPARE(remote->startCount(), 1);
    QCOMPARE(reused->bytes(0, 4).toByteArray(), QByteArrayLiteral("live"));
}

void TestSpooledSource::openFailureReportsTheTransportError()
{
    FakeRemoteFarm farm;
    farm.at(QString::fromLatin1(kUrl))
        ->setStartFailure(QStringLiteral("Connection refused by web1"));

    QString error;
    auto src = openSource(&error);
    QVERIFY(!src);
    // The transport's own wording reaches the caller: "Cannot open ssh://..." alone
    // would tell the user nothing about what went wrong.
    QCOMPARE(error, QStringLiteral("Connection refused by web1"));
}

void TestSpooledSource::unconfiguredRemoteReportsNotBuiltIn()
{
    // No farm installed, so the registry falls back to its default factory. The two
    // builds answer differently, and both answers are correct.
    SourceSpoolRegistry::instance().clear();
    SourceSpoolRegistry::instance().setFetcherFactory(nullptr);

    QString error;
    auto src = openLogSource(QStringLiteral("ssh://nonexistent.invalid/var/log/a.log"),
                             OpenPolicy::Interactive, &error);
#if defined(LOFTAIL_HAVE_SSH)
    // M13: a host that cannot be reached is not a failure to open any more — it is a
    // log that is not there YET. M17 makes that true one step earlier: opening does not
    // even wait to find out, so what comes back here is an empty source whose fetcher is
    // still connecting. Either way the document upstream shows itself as waiting; which
    // of the two predicates says so depends only on how far the worker has got, so this
    // asks the question the document asks, not one of its halves.
    QVERIFY(src);
    QCOMPARE(src->size(), 0);
    QVERIFY(src->originVanished() || src->notReadyYet());
    src.reset(); // drop the spool (and its retrying fetcher) before the next case
#else
    // Without libssh2 there is nothing to wait FOR: no fetcher can ever be built for
    // this address, so it stays a plain refusal that says why.
    QVERIFY(!src);
    QVERIFY(!error.isEmpty());
#endif

    // A malformed remote address is rejected before any of that, in both builds: there
    // is no log here to wait for, only a string that names none.
    QString badError;
    QVERIFY(!openLogSource(QStringLiteral("ssh://"), OpenPolicy::Interactive, &badError));
    QVERIFY(!badError.isEmpty());
}

QTEST_GUILESS_MAIN(TestSpooledSource)
#include "tst_spooledsource.moc"
