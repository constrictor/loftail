#include <QtTest>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "BufferedLogSource.h"
#include "SharedReadFile.h"

using namespace loftail;

// Reading a file its writer is still rolling (invariant #5: observing a log must not
// disturb the process producing it).
//
// THE REASON THIS FILE EXISTS is the second half of that sentence, and it is a Windows
// claim that nothing had ever asked on Windows. QFile's open there passes
// FILE_SHARE_READ | FILE_SHARE_WRITE and not FILE_SHARE_DELETE, so a log held open by a
// tab could not be rolled or deleted by the process writing it — a sharing violation on
// exactly the operation a logging framework performs unattended at midnight. Appends
// were unaffected, so every read-path test on the platform passed throughout.
//
// tst_tail and tst_waiting, which is where a claim like this would otherwise live, are
// both POSIX-only, and their CMake entry says the Windows sharing semantics "must be
// exercised separately (not done yet)". This is that, and it is deliberately as close to
// the platform as a test can be: the assertions are that the ordinary things a writer
// does to a log — append, rename, delete — still succeed while loftail holds it. On
// POSIX they are free and pass trivially; the point is that they now also run in the one
// configuration where they were not free.
class TestSharedReadFile : public QObject
{
    Q_OBJECT

private slots:
    void init();

    void readsAtAnOffsetAndClampsAtTheEnd();
    void seesTheFileGrowBehindIt();
    void theWriterCanStillAppend();
    void theWriterCanStillRenameTheFile();       // the rotation case
    void theWriterCanStillDeleteTheFile();       // the case Windows CI caught
    void aRenamedFileGoesOnReadingThroughTheHandle();
    void openingWhatIsNotThereFails();
    void aBufferedSourceHoldsTheFileTheSameWay();

private:
    QTemporaryDir m_dir;
    QString       m_path;

    static bool writeWhole(const QString &path, const QByteArray &data)
    {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return false;
        return f.write(data) == data.size();
    }

    static bool appendTo(const QString &path, const QByteArray &data)
    {
        QFile f(path);
        if (!f.open(QIODevice::Append))
            return false;
        return f.write(data) == data.size();
    }
};

void TestSharedReadFile::init()
{
    QVERIFY(m_dir.isValid());
    m_path = m_dir.filePath(QStringLiteral("app.log"));
    QFile::remove(m_path);
    // The rolled name too: several cases below rename onto it, and a rename whose target
    // already exists fails — which would make these cases order-dependent.
    QFile::remove(m_dir.filePath(QStringLiteral("app.log.1")));
    QVERIFY(writeWhole(m_path, QByteArrayLiteral("0123456789")));
}

void TestSharedReadFile::readsAtAnOffsetAndClampsAtTheEnd()
{
    SharedReadFile f;
    QVERIFY(f.open(m_path));
    QVERIFY(f.isOpen());
    QCOMPARE(f.size(), 10);
    QCOMPARE(f.read(0, 4), QByteArrayLiteral("0123"));
    QCOMPARE(f.read(6, 4), QByteArrayLiteral("6789"));
    // Past the end is short, not an error: a caller asking for more than is there is
    // the ordinary case on a file being written.
    QCOMPARE(f.read(8, 100), QByteArrayLiteral("89"));
    QCOMPARE(f.read(10, 4), QByteArray());
    QCOMPARE(f.read(-1, 4), QByteArray());
    QCOMPARE(f.read(0, 0), QByteArray());
}

void TestSharedReadFile::seesTheFileGrowBehindIt()
{
    SharedReadFile f;
    QVERIFY(f.open(m_path));
    QCOMPARE(f.size(), 10);

    QVERIFY(appendTo(m_path, QByteArrayLiteral("abcde")));

    // size() is asked of the OS every time rather than cached at open, which is the
    // whole basis of the watch tick.
    QCOMPARE(f.size(), 15);
    QCOMPARE(f.read(10, 5), QByteArrayLiteral("abcde"));
}

void TestSharedReadFile::theWriterCanStillAppend()
{
    SharedReadFile f;
    QVERIFY(f.open(m_path));
    QVERIFY(appendTo(m_path, QByteArrayLiteral("more")));
    QCOMPARE(f.size(), 14);
}

void TestSharedReadFile::theWriterCanStillRenameTheFile()
{
    SharedReadFile f;
    QVERIFY(f.open(m_path));

    // QDir::rename rather than QFile::rename: the latter falls back to copy-then-remove
    // when the OS refuses, which would report success for a rename that did not happen
    // and hide the very failure this asserts against.
    const QString rolled = m_dir.filePath(QStringLiteral("app.log.1"));
    QVERIFY(QDir().rename(m_path, rolled));
    QVERIFY(!QFile::exists(m_path));

    // And the path is free again, which is what lets the writer start a new log there.
    QVERIFY(writeWhole(m_path, QByteArrayLiteral("fresh")));
}

void TestSharedReadFile::theWriterCanStillDeleteTheFile()
{
    SharedReadFile f;
    QVERIFY(f.open(m_path));
    QVERIFY(QFile::remove(m_path));
}

void TestSharedReadFile::aRenamedFileGoesOnReadingThroughTheHandle()
{
    SharedReadFile f;
    QVERIFY(f.open(m_path));
    QVERIFY(QDir().rename(m_path, m_dir.filePath(QStringLiteral("app.log.1"))));

    // The handle follows the bytes, not the name — so a rotation noticed one tick late
    // still serves the records already on screen rather than blanking them. What
    // re-resolves the path is wasReplaced(), one level up.
    QCOMPARE(f.size(), 10);
    QCOMPARE(f.read(0, 4), QByteArrayLiteral("0123"));
}

void TestSharedReadFile::openingWhatIsNotThereFails()
{
    SharedReadFile f;
    QVERIFY(!f.open(m_dir.filePath(QStringLiteral("never-written.log"))));
    QVERIFY(!f.isOpen());
    QCOMPARE(f.size(), 0);
    QCOMPARE(f.read(0, 4), QByteArray());
}

void TestSharedReadFile::aBufferedSourceHoldsTheFileTheSameWay()
{
    // The seam is only worth anything if the source actually goes through it: this is
    // the level the failing Windows test was at, minus the window.
    auto src = BufferedLogSource::open(m_path);
    QVERIFY(src);
    QCOMPARE(src->size(), 10);
    QCOMPARE(src->bytes(0, 4).toByteArray(), QByteArrayLiteral("0123"));

    QVERIFY(appendTo(m_path, QByteArrayLiteral("abcde")));
    QCOMPARE(src->refreshSize(), 15);
    QVERIFY(!src->wasTruncated());

    // Rolled and deleted under the source, both while it holds the file.
    QVERIFY(QDir().rename(m_path, m_dir.filePath(QStringLiteral("app.log.1"))));
    QVERIFY(QFile::remove(m_dir.filePath(QStringLiteral("app.log.1"))));

    // What the source then REPORTS about a deletion — originVanished(), the M13 waiting
    // state — is asserted by tst_waiting on POSIX and by tst_reload through a real
    // window on both. It is left alone here on purpose: a Windows delete against an open
    // handle leaves the name in a "delete pending" limbo whose visibility to
    // QFileInfo::exists() is not something this project can settle from a Linux box.
}

QTEST_MAIN(TestSharedReadFile)
#include "tst_sharedreadfile.moc"
