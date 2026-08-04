#include <QtTest>

#include <QTemporaryDir>

#include "ArchiveFixtures.h"
#include "ArchiveReader.h"

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

QTEST_APPLESS_MAIN(TestArchiveMembers)
#include "tst_archivemembers.moc"
