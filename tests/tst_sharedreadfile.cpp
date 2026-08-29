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
#include <QFile>
#include <QTemporaryDir>

#include <atomic>
#include <thread>

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
    void twoThreadsReadingOneHandleEachGetTheirOwnOffset();
    void twoThreadsReadingOneSourceEachGetTheirOwnBytes();

private:
    QTemporaryDir m_dir;
    QString       m_path;

    // A file whose every kBlock-byte block spells its own block number, so a read that
    // came back with somebody else's bytes says WHOSE in the failure message rather than
    // merely differing.
    static constexpr qint64 kBlock  = 64;
    static constexpr qint64 kBlocks = 256;
    static constexpr int    kRounds = 20000;

    static QByteArray blockAt(qint64 index)
    {
        return QByteArray::number(index).rightJustified(static_cast<qsizetype>(kBlock), '.');
    }

    static QByteArray blockedFile()
    {
        QByteArray out;
        out.reserve(static_cast<qsizetype>(kBlock * kBlocks));
        for (qint64 i = 0; i < kBlocks; ++i)
            out += blockAt(i);
        return out;
    }

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
    QCOMPARE(src->bytesCopy(0, 4), QByteArrayLiteral("0123"));

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

// --- bugs.md 25: one source, two threads ------------------------------------
//
// A log that is scanning is read by TWO threads at once and always has been: the index
// worker walks chunks out of the source while the GUI thread paints cells out of the
// same source, each visible cell reaching LogModel::data(). Nothing in the suite had
// ever driven a LogSource that way, which is why this went nine milestones.
//
// Two things were wrong, and each of these cases pins one of them, because either alone
// leaves the other's failure looking like the one that was fixed.
//
// std::thread and not QThread, per ARCHITECTURE.md §13.1: QThread::wait() joins through
// a QWaitCondition inside an unannotated libQt6Core, which breaks TSan's happens-before
// chain over the very code a run like this is worth pointing TSan at. Neither case needs
// a sanitizer to fail, though — both fail on CONTENT, which is the point: the bug is
// observable as one thread being handed the other's bytes, not merely as a report.

void TestSharedReadFile::twoThreadsReadingOneHandleEachGetTheirOwnOffset()
{
    // The lower half. SharedReadFile::read() was seek-then-read on one shared handle, so
    // two readers could each be served the other's offset with no allocation freed
    // anywhere. It is pread(2) / ReadFile-with-OVERLAPPED now, which takes the position
    // as an argument and leaves nothing on the handle to race over.
    QVERIFY(writeWhole(m_path, blockedFile()));

    SharedReadFile f;
    QVERIFY(f.open(m_path));

    std::atomic<int> wrong{0};
    QByteArray firstWrongGot;
    QByteArray firstWrongWant;
    std::atomic_flag reported = ATOMIC_FLAG_INIT;

    const auto reader = [&](qint64 firstBlock) {
        QByteArray into;
        for (int round = 0; round < kRounds; ++round) {
            const qint64 block = firstBlock + 2 * (round % (kBlocks / 2));
            f.read(block * kBlock, kBlock, into);
            const QByteArray want = blockAt(block);
            if (into != want) {
                ++wrong;
                if (!reported.test_and_set()) {
                    firstWrongGot = into;
                    firstWrongWant = want;
                }
            }
        }
    };

    std::thread even(reader, 0);
    std::thread odd(reader, 1);
    even.join();
    odd.join();

    if (wrong.load() != 0)
        qWarning("first mismatch: got %s, wanted %s",
                 firstWrongGot.constData(), firstWrongWant.constData());
    QCOMPARE(wrong.load(), 0);
}

void TestSharedReadFile::twoThreadsReadingOneSourceEachGetTheirOwnBytes()
{
    // The upper half, and the one the entry is named for. bytes() used to store the data
    // in a MEMBER buffer and return a view over it, so the second thread's call freed the
    // allocation the first was still reading — the indexer parsing freed or foreign bytes
    // into Record offsets, and a painted cell decoding whatever the indexer's chunk left
    // behind. The storage is the caller's now (LogSource::bytes), so there is no shared
    // buffer left to clobber.
    //
    // The ranges are DISJOINT and equal in length on purpose. Equal lengths make the
    // allocator hand the clobbering call the block it has just freed, so the unfixed code
    // fails by returning the wrong CONTENT rather than by crashing — which is what makes
    // this an assertion about behaviour rather than a sanitizer run.
    QVERIFY(writeWhole(m_path, blockedFile()));

    auto src = BufferedLogSource::open(m_path);
    QVERIFY(src);
    QCOMPARE(src->size(), kBlock * kBlocks);

    std::atomic<int> wrong{0};
    QByteArray firstWrongGot;
    QByteArray firstWrongWant;
    std::atomic_flag reported = ATOMIC_FLAG_INIT;

    const auto reader = [&](qint64 firstBlock) {
        // Each thread's own storage — that IS the fix, expressed from the caller's side.
        QByteArray into;
        for (int round = 0; round < kRounds; ++round) {
            const qint64 block = firstBlock + 2 * (round % (kBlocks / 2));
            const QByteArrayView view = src->bytes(block * kBlock, kBlock, into);
            const QByteArray want = blockAt(block);
            if (view != QByteArrayView(want)) {
                ++wrong;
                if (!reported.test_and_set()) {
                    firstWrongGot = view.toByteArray();
                    firstWrongWant = want;
                }
            }
        }
    };

    std::thread even(reader, 0);
    std::thread odd(reader, 1);
    even.join();
    odd.join();

    if (wrong.load() != 0)
        qWarning("first mismatch: got %s, wanted %s",
                 firstWrongGot.constData(), firstWrongWant.constData());
    QCOMPARE(wrong.load(), 0);
}

QTEST_MAIN(TestSharedReadFile)
#include "tst_sharedreadfile.moc"
