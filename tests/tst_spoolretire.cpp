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

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>

#include <memory>

#include "FakeFetcher.h"
#include "LogSource.h"
#include "SourceSpool.h"
#include "SpooledLogSource.h"

using namespace loftail;

// M17 — a dead spool is RETIRED, not joined (ARCHITECTURE.md §6.3.3).
//
// The thing being pinned is a promise about the caller, not about the fetcher: dropping
// the last handle to a spool must return at once even when the fetcher's thread has not
// noticed yet. On the GUI thread — which is where the last handle is dropped, every time
// the last tab on a log closes — joining a worker that is twenty seconds into a connect
// is the freeze this whole milestone exists to remove, and once that worker can be
// waiting on the GUI thread for a password it is a deadlock as well.
//
// So the fake models a fetcher that lingers: setStopsSlowly() makes isStopped() stay
// false until the test says otherwise, which is the one thing a fake that owns no thread
// cannot do on its own.
class TestSpoolRetire : public QObject
{
    Q_OBJECT

private:
    static constexpr auto kUrl = "ssh://deploy@web1/var/log/app.log";

    static std::unique_ptr<LogSource> openSource()
    {
        return openLogSource(QString::fromLatin1(kUrl), OpenPolicy::Interactive);
    }

    // The directory the spool behind `src` is writing into.
    static QString spoolDirOf(const LogSource &src)
    {
        const auto *spooled = dynamic_cast<const SpooledLogSource *>(&src);
        if (!spooled || !spooled->spool())
            return QString();
        return QFileInfo(spooled->spool()->spoolPath(spooled->fetchStatus().generation))
            .absolutePath();
    }

private slots:
    void droppingTheLastHandleDoesNotWaitForTheWorker();
    void theDirectoryGoesOnceTheWorkerExits();
    void reopeningWhileTheOldFetcherLingersGetsItsOwnDirectory();
    void cancelStopsFetchingAndLeavesTheBytesReadable();
    void shutdownGivesUpOnAWorkerThatWillNotStop();
};

void TestSpoolRetire::droppingTheLastHandleDoesNotWaitForTheWorker()
{
    FakeRemoteFarm farm;
    auto remote = farm.at(QString::fromLatin1(kUrl));
    remote->setInitialContent(QByteArrayLiteral("one line\n"));
    remote->setStopsSlowly(true); // the worker will not wind up while we watch

    auto src = openSource();
    QVERIFY(src);

    QElapsedTimer clock;
    clock.start();
    src.reset(); // the last handle: ~SpooledLogSource → ~SourceSpool → retire()
    const qint64 took = clock.elapsed();

    QVERIFY2(took < 100, qPrintable(QStringLiteral("dropping it took %1 ms").arg(took)));
    // Asked to stop all the same — retiring is not abandoning.
    QCOMPARE(remote->stopCount(), 1);
}

void TestSpoolRetire::theDirectoryGoesOnceTheWorkerExits()
{
    FakeRemoteFarm farm;
    auto remote = farm.at(QString::fromLatin1(kUrl));
    remote->setInitialContent(QByteArrayLiteral("one line\n"));
    remote->setStopsSlowly(true);

    auto src = openSource();
    QVERIFY(src);
    const QString dir = spoolDirOf(*src);
    QVERIFY(!dir.isEmpty());
    QVERIFY(QDir(dir).exists());

    src.reset();
    // Still there: the worker owns it until it stops, and nothing waited for that.
    QVERIFY(QDir(dir).exists());

    remote->finishStopping();
    // The reaper is a timer on this thread, so this needs a turn of the event loop —
    // which is exactly the point of it being a timer rather than a blocking join.
    QTRY_VERIFY_WITH_TIMEOUT(!QDir(dir).exists(), 5000);
}

void TestSpoolRetire::reopeningWhileTheOldFetcherLingersGetsItsOwnDirectory()
{
    // The collision the serial in spoolDirName() exists to prevent. Reopening a log
    // whose previous fetcher has not finished winding up must not hand the new spool
    // the old one's directory — the reaper would then delete a live spool's files.
    FakeRemoteFarm farm;
    auto remote = farm.at(QString::fromLatin1(kUrl));
    remote->setInitialContent(QByteArrayLiteral("first open\n"));
    remote->setStopsSlowly(true);

    auto first = openSource();
    QVERIFY(first);
    const QString firstDir = spoolDirOf(*first);
    first.reset(); // retired, still running

    SourceSpoolRegistry::instance().clear();
    auto second = openSource();
    QVERIFY(second);
    const QString secondDir = spoolDirOf(*second);

    QVERIFY(!secondDir.isEmpty());
    QVERIFY2(secondDir != firstDir, qPrintable(secondDir));

    // Now let the first one go, and the second must be untouched by its burial.
    remote->finishStopping();
    QTRY_VERIFY_WITH_TIMEOUT(!QDir(firstDir).exists(), 5000);
    QVERIFY(QDir(secondDir).exists());
    QCOMPARE(second->bytesCopy(0, second->size()),
             QByteArrayLiteral("first open\n"));
}

void TestSpoolRetire::cancelStopsFetchingAndLeavesTheBytesReadable()
{
    // Cancel is not teardown: the document stays on screen and what already arrived
    // stays readable (SourceSpool::cancel). What changes is that nothing more arrives.
    FakeRemoteFarm farm;
    auto remote = farm.at(QString::fromLatin1(kUrl));
    remote->setInitialContent(QByteArrayLiteral("kept\n"));
    remote->setStopsSlowly(true);

    auto src = openSource();
    QVERIFY(src);
    auto *spooled = dynamic_cast<SpooledLogSource *>(src.get());
    QVERIFY(spooled);

    QElapsedTimer clock;
    clock.start();
    spooled->spool()->cancel();
    QVERIFY2(clock.elapsed() < 100,
             qPrintable(QStringLiteral("cancel() took %1 ms").arg(clock.elapsed())));

    // Reported at once, not a poll later: the state a reader sees the moment it cancels
    // should be the one it asked for.
    QCOMPARE(spooled->fetchStatus().state, FetchStatus::State::Disconnected);
    QCOMPARE(src->bytesCopy(0, src->size()), QByteArrayLiteral("kept\n"));
    QVERIFY(QDir(spoolDirOf(*src)).exists()); // still ours; cancel is not a retirement
}

void TestSpoolRetire::shutdownGivesUpOnAWorkerThatWillNotStop()
{
    // Quitting must not hang on a connect to a host that is not answering. The fetcher
    // is abandoned and its directory left behind — the next launch's sweep removes it,
    // because an instance directory whose lock file is gone is granted to whoever asks.
    FakeRemoteFarm farm;
    auto remote = farm.at(QString::fromLatin1(kUrl));
    remote->setInitialContent(QByteArrayLiteral("stuck\n"));
    remote->setStopsSlowly(true);

    auto src = openSource();
    QVERIFY(src);
    src.reset();

    QElapsedTimer clock;
    clock.start();
    SourceSpoolRegistry::instance().shutdown();
    const qint64 took = clock.elapsed();

    // Bounded by the drain budget, and not by the worker: it never stopped at all.
    QVERIFY2(took < 5000, qPrintable(QStringLiteral("shutdown took %1 ms").arg(took)));
    QVERIFY(!remote->isStopped());
}

QTEST_MAIN(TestSpoolRetire)
#include "tst_spoolretire.moc"
