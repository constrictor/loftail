// loftail — a desktop viewer for log4cplus logs.
// Copyright (C) 2026 Valentyn Pavliuchenko
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QtTest>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

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
    void aConnectingArchiveSaysWhatItIsOpeningRatherThanConnecting();
};

void TestSpooledSource::readsInitialContentThroughTheSpool()
{
    FakeRemoteFarm farm;
    farm.at(QString::fromLatin1(kUrl))->setInitialContent(QByteArrayLiteral("hello remote"));

    auto src = openSource();
    QVERIFY(src);
    QCOMPARE(src->size(), 12);
    QCOMPARE(src->bytesCopy(0, 12), QByteArrayLiteral("hello remote"));
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
    QCOMPARE(src->bytesCopy(0, 20), QByteArrayLiteral("0123456789ABCDEFGHIJ"));
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
    QCOMPARE(src->bytesCopy(5, 100), QByteArrayLiteral("56789"));
    // A read starting inside it returns nothing at all.
    QVERIFY(src->bytesCopy(10, 5).isEmpty());
    QVERIFY(src->bytesCopy(12, 1).isEmpty());
    // Out-of-range arguments behave as they do for a local source.
    QVERIFY(src->bytesCopy(-1, 5).isEmpty());
    QVERIFY(src->bytesCopy(0, 0).isEmpty());
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
    QCOMPARE(src->bytesCopy(0, 13), QByteArrayLiteral("first\nsecond\n"));
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
    QCOMPARE(src->bytesCopy(0, 9), QByteArrayLiteral("brand new"));
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
    QCOMPARE(src->bytesCopy(0, 2), QByteArrayLiteral("bb"));
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
    QCOMPARE(reused->bytesCopy(0, 4), QByteArrayLiteral("live"));
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

void TestSpooledSource::aConnectingArchiveSaysWhatItIsOpeningRatherThanConnecting()
{
    // WHAT WENT WRONG. State::Connecting is shared by both fetchers — it means
    // "establishing the session" to SshFetcher and "opening the container and seeking to
    // the member" to ArchiveFetcher (§6.4) — and sourceStatusText() rendered it as the
    // one word "connecting…" for both. So a log opened out of a zip on the user's own
    // disk announced a connection, with no network anywhere in the picture, and for a
    // remote container it said so for the whole of the download rather than for the
    // handshake. The state below it, Priming, had made this exact split since M12
    // ("expanding" against "fetching"); this one had not.
    //
    // The decision is PURE STRING WORK on the address — a spool does not know who fills
    // it — so it is asked here directly, over one fetcher held in Connecting, rather
    // than raced for behind a real libarchive expansion. Ungated for the same reason
    // tst_archivelocation is: the path layer must answer alike in both configurations.
    FakeRemoteFarm farm;
    auto remote = farm.at(QString::fromLatin1(kUrl));
    remote->setInitialContent(QByteArrayLiteral("hello remote"));
    remote->setConnectDelayed();

    auto src = openSource();
    QVERIFY(src);
    QCOMPARE(src->size(), 0);

    // A plain remote log: unchanged, and it must stay unchanged — this is the state's
    // original meaning and every M17 case reads that word.
    QCOMPARE(sourceStatusText(*src, QString::fromLatin1(kUrl)),
             QStringLiteral("connecting…"));

    // A member inside a local container names the CONTAINER, and names it as spelled:
    // logSourceDisplayName() strips a single-stream suffix, so it would report a wait on
    // `app.log` while the file being opened is `app.log.gz`.
    const QString local = QDir::rootPath() + QStringLiteral("srv/logs/bundle.tar.gz");
    QCOMPARE(sourceStatusText(*src, local + QStringLiteral("/var/log/app.log")),
             QStringLiteral("opening bundle.tar.gz…"));
    QCOMPARE(sourceStatusText(*src, QDir::rootPath() + QStringLiteral("srv/app.log.gz")),
             QStringLiteral("opening app.log.gz…"));

    // A member inside a REMOTE container takes the archive wording too, because the
    // archive fetcher stays in this state for the whole of the container's download —
    // the connect is one step inside the opening rather than the thing being reported.
    QCOMPARE(sourceStatusText(*src, QStringLiteral(
                                        "ssh://web1/srv/bundle.tar.gz/var/log/app.log")),
             QStringLiteral("opening bundle.tar.gz…"));

    // AND NEVER A PASSWORD. The container half of an archive address is kept verbatim
    // when it cannot be normalized, and the last component of a pathless remote address
    // is the whole userinfo — which is exactly how one gets on screen (RemoteLocation.h).
    const QString withPassword =
        sourceStatusText(*src, QStringLiteral("ssh://deploy:hunter2@web1/b.tar.gz/a.log"));
    QVERIFY2(!withPassword.contains(QStringLiteral("hunter2")), qPrintable(withPassword));

    src.reset(); // drop the spool (and its still-connecting fetcher) before the next case

    // AND A PLAIN LOCAL FILE SAYS NOTHING, whatever it is called. It is not a spool, so
    // it never reaches the switch above at all — which is the reason the wording only
    // ever had to be right for the two spooled transports, and the reason a local log
    // named `app.log.gz.txt` cannot be talked into announcing an expansion. The healthy
    // states are silent for the same reason: only trouble gets a line.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString plain = dir.path() + QStringLiteral("/app.log");
    QFile f(plain);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(QByteArrayLiteral("2026-08-05 hello\n"));
    f.close();

    auto localSrc = openLogSource(plain, OpenPolicy::Interactive);
    QVERIFY(localSrc);
    QVERIFY(sourceStatusText(*localSrc, plain).isEmpty());
    QVERIFY(sourceStatusText(*localSrc, local + QStringLiteral("/var/log/app.log")).isEmpty());
}

QTEST_GUILESS_MAIN(TestSpooledSource)
#include "tst_spooledsource.moc"
