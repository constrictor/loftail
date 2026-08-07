#include <QtTest>

#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <chrono>
#include <thread>

#include "ArchiveFixtures.h"
#include "ArchiveReader.h"
#include "FakeFetcher.h"

using namespace loftail;
using namespace loftail::fixtures;

// M12 — enumerating what is inside an archive, which is what the member picker shows
// (SPEC.md §3). Gated on LOFTAIL_HAVE_ARCHIVE: without libarchive there is nothing to
// enumerate, and the path layer that still works without it is covered, ungated, by
// tst_archivelocation. No network, no credentials, no committed binary fixtures —
// every archive here is built at runtime by libarchive's write side.
class TestArchiveMembers : public QObject
{
    Q_OBJECT

private slots:
    void listsAZipInArchiveOrderWithSizes();
    void listsACompressedTar();
    void skipsDirectoriesAndEmptyEntries();
    void aBareCompressedStreamHasOneSyntheticMember();
    void reportsAContainerThatIsNotAnArchive();
    void listingARemoteContainerWaitsForAllOfIt();
    void listingCanBeGivenUpOn();

private:
    QString path(const QString &name) const { return m_dir.path() + u'/' + name; }
    QTemporaryDir m_dir;
};

void TestArchiveMembers::listsAZipInArchiveOrderWithSizes()
{
    const QString zip = path(QStringLiteral("bundle.zip"));
    QVERIFY(writeZip(zip, {{QStringLiteral("app.log"), QByteArrayLiteral("one\ntwo\n")},
                           {QStringLiteral("app.log.1"), QByteArrayLiteral("older\n")},
                           {QStringLiteral("var/log/nested.log"), QByteArrayLiteral("deep\n")}}));

    QString error;
    const QVector<ArchiveEntry> members = listArchiveMembers(zip, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(members.size(), 3);

    // Archive order, not sorted: the writer's order is information the reader keeps.
    QCOMPARE(members.at(0).path, QStringLiteral("app.log"));
    QCOMPARE(members.at(1).path, QStringLiteral("app.log.1"));
    QCOMPARE(members.at(2).path, QStringLiteral("var/log/nested.log"));
    QCOMPARE(members.at(0).size, qint64(8));
    QCOMPARE(members.at(1).size, qint64(6));
}

void TestArchiveMembers::listsACompressedTar()
{
    const QString tgz = path(QStringLiteral("logs.tar.gz"));
    QVERIFY(writeTarGz(tgz, {{QStringLiteral("./app.log"), QByteArrayLiteral("hello\n")},
                             {QStringLiteral("./db.log"), QByteArrayLiteral("world\n")}}));

    QString error;
    const QVector<ArchiveEntry> members = listArchiveMembers(tgz, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(members.size(), 2);
    // A tar records "./app.log"; one spelling, so the member picked from this list is
    // the member found on reopen.
    QCOMPARE(members.at(0).path, QStringLiteral("app.log"));
    QCOMPARE(members.at(1).path, QStringLiteral("db.log"));
}

void TestArchiveMembers::skipsDirectoriesAndEmptyEntries()
{
    const QString tar = path(QStringLiteral("mixed.tar"));
    QVERIFY(writeTar(tar, {{QStringLiteral("app.log"), QByteArrayLiteral("content\n")},
                           {QStringLiteral("placeholder.log"), QByteArray()}}));

    QString error;
    const QVector<ArchiveEntry> members = listArchiveMembers(tar, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    // The empty entry is not a log to open; offering it would be offering nothing.
    QCOMPARE(members.size(), 1);
    QCOMPARE(members.at(0).path, QStringLiteral("app.log"));
}

void TestArchiveMembers::aBareCompressedStreamHasOneSyntheticMember()
{
    const QString gz = path(QStringLiteral("app.log.gz"));
    QVERIFY(writeGzip(gz, QByteArrayLiteral("only\n")));

    QString error;
    const QVector<ArchiveEntry> members = listArchiveMembers(gz, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    // Answered from the NAME, without decompressing a byte — a .gz holds one member by
    // construction, so the caller needs no special case and the user is never asked.
    QCOMPARE(members.size(), 1);
    QCOMPARE(members.at(0).path, QStringLiteral("app.log"));
    QCOMPARE(members.at(0).size, qint64(-1)); // not recorded, and cannot be without reading
}

void TestArchiveMembers::reportsAContainerThatIsNotAnArchive()
{
    const QString fake = path(QStringLiteral("bundle.zip"));
    QFile f(fake);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("this is just text, not a zip at all");
    f.close();

    QString error;
    const QVector<ArchiveEntry> members = listArchiveMembers(fake, &error);
    QVERIFY(members.isEmpty());
    QVERIFY2(!error.isEmpty(), "a container that cannot be read must say so");
}

void TestArchiveMembers::listingARemoteContainerWaitsForAllOfIt()
{
    // A BUG THAT PREDATES M17 and that M17 would have turned from partial into empty.
    //
    // This code passed no AwaitInput, on the stated assumption that "the container is
    // fully available by the time it is listed". Nothing ever made that true: a remote
    // container arrives progressively, so listing one enumerated whatever the transport
    // had committed at that instant — the 128 KB prime — and called that the archive. A
    // support bundle would quietly show its first few members and hide the rest.
    //
    // The fake hands over its bytes in pieces so that a listing which does not wait sees
    // an incomplete container, which is exactly what the old code did.
    QVector<fixtures::Member> members;
    for (int i = 0; i < 40; ++i) {
        members.append({QStringLiteral("var/log/app-%1.log").arg(i, 2, 10, QChar(u'0')),
                        logBody(200)});
    }
    const QString tgz = path(QStringLiteral("bundle.tar.gz"));
    QVERIFY(writeTarGz(tgz, members));

    QFile packed(tgz);
    QVERIFY(packed.open(QIODevice::ReadOnly));
    const QByteArray compressed = packed.readAll();
    packed.close();
    QVERIFY(compressed.size() > 4096); // several chunks' worth, or this proves nothing

    const QString url = QStringLiteral("ssh://deploy@web1/var/log/bundle.tar.gz");
    FakeRemoteFarm farm;
    auto remote = farm.at(url);
    // Only the head to begin with; the rest arrives while the listing is under way. The
    // total is what a stat would have reported before a byte was fetched, which is how
    // the listing can tell "still coming" from "that is all there is".
    remote->setInitialContent(compressed.left(2048));
    remote->setTotalSize(compressed.size());

    // Feed the remainder from another thread, as a transport would.
    //
    // std::thread rather than QThread, which matters here for one reason: this thread's
    // stack sits inside the main thread's live test frame, and QThread::wait() joins
    // through Qt's own condition variable — invisible to ThreadSanitizer in an
    // uninstrumented libQt6Core, so the join is not seen and every later reuse of that
    // stack region reads as a race. std::thread::join() goes through pthread_join, which
    // TSan intercepts. See ARCHITECTURE.md §13.
    std::thread feeder([&remote, &compressed]() {
        // Not until the source has actually been opened. start() writes the initial
        // content and resets the fake's byte count, so anything appended before it is
        // thrown away — and the appends would land first, because opening the container
        // is what the main thread is about to do. Bounded, so that a fetcher which never
        // starts ends the feeding rather than hanging the join: the listing assertions
        // below are what then report it.
        for (int i = 0; i < 10000 && remote->startCount() == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        for (int at = 2048; at < compressed.size(); at += 2048) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            remote->append(compressed.mid(at, 2048));
        }
    });

    QString error;
    const QVector<ArchiveEntry> listed = listArchiveMembers(url, &error);
    // Before any QVERIFY: those return on failure, and the thread reads `compressed`.
    feeder.join();

    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(listed.size(), 40); // every member, not the handful in the first chunk
    QCOMPARE(listed.first().path, QStringLiteral("var/log/app-00.log"));
    QCOMPARE(listed.last().path, QStringLiteral("var/log/app-39.log"));
}

void TestArchiveMembers::listingCanBeGivenUpOn()
{
    // The picker's Cancel button, from below. A container that never finishes arriving
    // would otherwise make the listing wait for ever, which since M17 is a wait on a
    // worker thread that the window is polling — so it has to be interruptible or Cancel
    // is a button that does nothing.
    const QString tgz = path(QStringLiteral("stalled.tar.gz"));
    QVERIFY(writeTarGz(tgz, {{QStringLiteral("a.log"), logBody(200)},
                             {QStringLiteral("b.log"), logBody(200)}}));

    QFile packed(tgz);
    QVERIFY(packed.open(QIODevice::ReadOnly));
    const QByteArray compressed = packed.readAll();
    packed.close();

    const QString url = QStringLiteral("ssh://deploy@web2/var/log/stalled.tar.gz");
    FakeRemoteFarm farm;
    // A head and then nothing, for ever: the host is still connected and simply has not
    // sent the rest.
    farm.at(url)->setInitialContent(compressed.left(512));

    // The predicate times itself rather than being flipped by a QTimer: this test runs
    // no event loop, so a timer would never fire and the listing would wait for ever —
    // which is exactly the hang being guarded against, and a poor way to demonstrate it.
    QElapsedTimer clock;
    clock.start();
    QString error;
    const QVector<ArchiveEntry> listed =
        listArchiveMembers(url, &error, [&clock]() { return clock.elapsed() > 200; });
    Q_UNUSED(listed);

    QVERIFY2(clock.elapsed() < 5000,
             qPrintable(QStringLiteral("the listing ignored the cancel for %1 ms")
                            .arg(clock.elapsed())));
}

QTEST_GUILESS_MAIN(TestArchiveMembers)
#include "tst_archivemembers.moc"
