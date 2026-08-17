#include <QtTest>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "BufferedLogSource.h"
#include "LogSource.h"

using namespace loftail;

// Rotation-by-replace: the file now AT THE PATH is not the file this source opened
// (invariant #5, §6). `logrotate`'s default and every Windows roller do it — rename the
// log, create a new one under the old name — and an open source sees nothing at all,
// because it follows the file it opened rather than the name. Only re-resolving the
// name catches it, which is pathIdentity().
//
// THIS FILE IS THE WINDOWS HALF. On POSIX the answer is a stat and tst_tail has driven
// it since M6 through the whole LiveController loop; on Windows it needs a handle and
// was simply stubbed to 0 — "unknown" — for eight milestones, so wasReplaced() was
// always false there and a rotated log kept the old file's records until the size
// happened to shrink or the first kilobyte happened to differ. These cases are
// deliberately at the SOURCE level rather than the controller's, because that is the
// layer where the platforms differ and the layer the stub was in; they are ungated and
// say the same thing on both.
class TestPathIdentity : public QObject
{
    Q_OBJECT

private slots:
    void init();

    void aFileHasAnIdentityAndNothingHasNone();
    void twoFilesDifferAndOneFileIsStableAcrossAppends();
    void aRewrittenPathIsANewIdentity();
    void aSourceNoticesTheFileUnderItsPathBeingReplaced();
    void anAppendIsNotAReplacement();
    void aTruncationInPlaceIsNotAReplacementEither();
    void aRenameWithNothingPutBackIsNotAReplacement();

private:
    QTemporaryDir m_dir;
    QString       m_path;
    QString       m_rolled;

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

void TestPathIdentity::init()
{
    QVERIFY(m_dir.isValid());
    m_path = m_dir.filePath(QStringLiteral("app.log"));
    m_rolled = m_dir.filePath(QStringLiteral("app.log.1"));
    QFile::remove(m_path);
    QFile::remove(m_rolled);
    QVERIFY(writeWhole(m_path, QByteArrayLiteral("first\n")));
}

void TestPathIdentity::aFileHasAnIdentityAndNothingHasNone()
{
    QVERIFY(pathIdentity(m_path) != 0);
    // 0 is the sentinel for "unknown", and a path with nothing at it is the ordinary
    // way to get one. Every caller reads it as "not replaced".
    QCOMPARE(pathIdentity(m_dir.filePath(QStringLiteral("never-written.log"))), quint64(0));
    QCOMPARE(pathIdentity(QString()), quint64(0));
}

void TestPathIdentity::twoFilesDifferAndOneFileIsStableAcrossAppends()
{
    const QString other = m_dir.filePath(QStringLiteral("other.log"));
    QVERIFY(writeWhole(other, QByteArrayLiteral("elsewhere\n")));
    QVERIFY(pathIdentity(m_path) != pathIdentity(other));

    // Stability across writes is the whole basis of polling it: an identity that moved
    // when the file grew would rescan the log on every tick.
    const quint64 before = pathIdentity(m_path);
    QVERIFY(appendTo(m_path, QByteArrayLiteral("second\n")));
    QCOMPARE(pathIdentity(m_path), before);
}

void TestPathIdentity::aRewrittenPathIsANewIdentity()
{
    const quint64 before = pathIdentity(m_path);
    QVERIFY(QDir().rename(m_path, m_rolled));
    QVERIFY(writeWhole(m_path, QByteArrayLiteral("brand new\n")));
    const quint64 after = pathIdentity(m_path);
    QVERIFY(after != 0);
    QVERIFY(after != before);
}

void TestPathIdentity::aSourceNoticesTheFileUnderItsPathBeingReplaced()
{
    auto src = BufferedLogSource::open(m_path);
    QVERIFY(src);
    QVERIFY(!src->wasReplaced());

    // The roll: the log this source holds is renamed away and a new one takes its name.
    QVERIFY(QDir().rename(m_path, m_rolled));
    QVERIFY(writeWhole(m_path, QByteArrayLiteral("brand new\n")));

    QVERIFY(src->wasReplaced());
    // And not because the file went missing — it is still there, it is just a different
    // one. The two answers are distinct questions with distinct responses: rescan the
    // new file, versus wait for one to turn up (§6.5).
    QVERIFY(!src->originVanished());
}

void TestPathIdentity::anAppendIsNotAReplacement()
{
    auto src = BufferedLogSource::open(m_path);
    QVERIFY(src);
    QVERIFY(appendTo(m_path, QByteArrayLiteral("second\n")));
    QCOMPARE(src->refreshSize(), qint64(13));
    QVERIFY(!src->wasReplaced());
    QVERIFY(!src->wasTruncated());
}

void TestPathIdentity::aTruncationInPlaceIsNotAReplacementEither()
{
    auto src = BufferedLogSource::open(m_path);
    QVERIFY(src);

    // copytruncate: same file, emptied. wasReplaced() must stay false — the caller
    // rescans off wasTruncated() instead, and a "replaced" here would re-resolve a path
    // that never moved.
    QVERIFY(writeWhole(m_path, QByteArray()));
    src->refreshSize();
    QVERIFY(src->wasTruncated());
    QVERIFY(!src->wasReplaced());
}

void TestPathIdentity::aRenameWithNothingPutBackIsNotAReplacement()
{
    auto src = BufferedLogSource::open(m_path);
    QVERIFY(src);
    QVERIFY(QDir().rename(m_path, m_rolled));

    // This is the gap between a roll's rename and its recreate, and it is the reason 0
    // means "unknown" rather than "different": read as a replacement, every rotation
    // would rescan against a file that is not there yet.
    QCOMPARE(pathIdentity(m_path), quint64(0));
    QVERIFY(!src->wasReplaced());
    QVERIFY(src->originVanished()); // waiting, which the next tick resolves either way
}

QTEST_MAIN(TestPathIdentity)
#include "tst_pathidentity.moc"
