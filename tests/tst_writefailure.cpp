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

// What the three write paths do when the write does NOT work, and what a crash in the
// middle of one leaves behind. Nothing tested any of it: AtomicJson.cpp was 60% covered
// and ConfigFileIO.cpp 49%, and the uncovered remainder in both was the failure handling
// — the half nobody exercises by using the application correctly.
//
// NO SEAM WAS ADDED TO src/ FOR THIS. Every failure below is provoked from outside, with
// the filesystem the code is already talking to: a parent that is a file so the directory
// cannot be created, a target that is a directory so the rename cannot land, and
// RLIMIT_FSIZE so that a write of a known size fails while a smaller one succeeds. A
// test-only hook would have been a branch in the shipped code that nothing but a test
// ever takes, and — worse for this particular subject — it would prove the code reports
// the failure the hook invented rather than one the operating system actually returns.
//
// POSIX-only, and beside tst_tail in tests/CMakeLists.txt for the same kind of reason:
// setrlimit, SIGXFSZ and a mode of 000 are what the provocation is made of. The rules
// themselves are platform-neutral and the Windows leg simply does not see them, which is
// the standing arrangement for this tree (CLAUDE.md, "Windows CI is the only
// cross-platform check").

#include "AtomicJson.h"
#include "ConfigFileIO.h"
#include "LogFileSettings.h"
#include "LogFileStore.h"
#include "LogSettings.h"
#include "RemoteLocation.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QTemporaryDir>
#include <QtTest>

#include <csignal>
#include <sys/resource.h>
#include <unistd.h>

using namespace loftail;

namespace {

// Absolute on BOTH platforms and therefore unchanged by logSettingsKey() — the rule
// tst_logfilestore's own abs() records.
QString abs(const QString &tail)
{
    return QDir::rootPath() + tail;
}

LogProfile profileWith(const QString &pattern)
{
    LogProfile p = LogProfile::builtIn();
    p.format.pattern = pattern;
    return p;
}

// A write that FAILS PART WAY, provoked with the one facility that can do it to an
// ordinary file in an ordinary directory without root: a soft RLIMIT_FSIZE.
//
// Two things about it are load-bearing. SIGXFSZ is ignored for the length of the limit,
// or the kernel kills the test process instead of letting write() return EFBIG — which
// is exactly the difference between exercising the error path and not running at all.
// And the limit is by SIZE, which is what lets one of two writes fail while the other
// succeeds: a per-log record carrying a long restart script is far bigger than the map
// that indexes it, so a limit between the two picks out which of the pair is refused
// without knowing anything about the order they are attempted in.
class FileSizeLimit
{
public:
    explicit FileSizeLimit(rlim_t bytes)
    {
        m_handler = std::signal(SIGXFSZ, SIG_IGN);
        if (getrlimit(RLIMIT_FSIZE, &m_previous) != 0)
            return;
        rlimit narrowed = m_previous;
        narrowed.rlim_cur = bytes;
        m_ok = setrlimit(RLIMIT_FSIZE, &narrowed) == 0;
    }
    ~FileSizeLimit()
    {
        if (m_ok)
            setrlimit(RLIMIT_FSIZE, &m_previous);
        std::signal(SIGXFSZ, m_handler);
    }
    FileSizeLimit(const FileSizeLimit &) = delete;
    FileSizeLimit &operator=(const FileSizeLimit &) = delete;

    bool ok() const { return m_ok; }

private:
    rlimit m_previous{};
    bool m_ok = false;
    void (*m_handler)(int) = SIG_DFL;
};

QJsonDocument someJson(int filler = 0)
{
    QJsonObject o;
    o.insert(QStringLiteral("hello"), QStringLiteral("world"));
    if (filler > 0)
        o.insert(QStringLiteral("filler"), QString(filler, QLatin1Char('x')));
    return QJsonDocument(o);
}

QByteArray contentsOf(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

bool writeFileWith(const QString &path, const QByteArray &bytes)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    return f.write(bytes) == bytes.size();
}

// One flat directory of files, by name — the whole of what a LogFileStore's pool is on
// disk, and therefore the whole of what a crash can leave in a given state.
using DirImage = QMap<QString, QByteArray>;

DirImage imageOf(const QString &dir)
{
    DirImage out;
    const QStringList names = QDir(dir).entryList(QDir::Files | QDir::NoDotAndDotDot);
    for (const QString &n : names)
        out.insert(n, contentsOf(QDir(dir).filePath(n)));
    return out;
}

void restoreImage(const QString &dir, const DirImage &image)
{
    QDir d(dir);
    const QStringList names = d.entryList(QDir::Files | QDir::NoDotAndDotDot);
    for (const QString &n : names)
        QFile::remove(d.filePath(n));
    for (auto it = image.cbegin(); it != image.cend(); ++it)
        writeFileWith(d.filePath(it.key()), it.value());
}

// The record for `address`, as a store reading this directory would serve it.
LogFileSettings served(const QString &configDir, const QString &address)
{
    LogFileStore store(configDir);
    store.load();
    return store.read(address);
}

} // namespace

class TstWriteFailure : public QObject
{
    Q_OBJECT

private slots:
    // --- AtomicJson: what a write that does not work reports and leaves behind --------
    void aWriteBelowAPathThatIsAFileFailsAndNamesTheDirectory();
    void aWriteOntoAPathThatIsADirectoryFailsAndDestroysNothing();
    void aWriteThatCannotBeFinishedIsReportedAndKeepsThePreviousContents();
    void aPrivateWriteLeavesTheSecretReadableByNobodyElse();

    // --- ConfigFileIO: somebody else's file ------------------------------------------
    void aConfigInADirectoryThatIsNotThereIsRefusedByNameAndNothingIsCreated();
    void aConfigWriteThatCannotBeFinishedIsReportedAndKeepsThePreviousContents();
    void aConfigThatIsThereAndShutIsNotDescribedAsOneThatIsNotThere();
    void aDirectoryWhereAConfigWasExpectedIsRefusedRatherThanRead();
    void aConfigTooLargeToEditIsRefusedRatherThanLoaded();

    // --- LogFileStore: which of the two writes goes first, and what a crash leaves ----
    void aSaveWhoseSlotFileCannotBeWrittenNeverPutsTheAddressInTheMap();
    void aSaveWhoseMapCannotBeWrittenLeavesTheRecordAsAnUnreferencedFile();
    void everyInterruptionOfAWriteRecoversWithoutServingOneLogAnothersRecord_data();
    void everyInterruptionOfAWriteRecoversWithoutServingOneLogAnothersRecord();
    void aLegacyAdoptionWithNoRoomToSpareLeavesTheOldRecordWhereItIs();
};

// ---------------------------------------------------------------------------
// AtomicJson
// ---------------------------------------------------------------------------

void TstWriteFailure::aWriteBelowAPathThatIsAFileFailsAndNamesTheDirectory()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString blocker = QDir(tmp.path()).filePath(QStringLiteral("occupied"));
    QVERIFY(writeFileWith(blocker, QByteArrayLiteral("not a directory\n")));

    // AtomicJson mkpath()s its OWN tree — the deliberate opposite of ConfigFileIO's rule
    // below — so the only way it cannot have its directory is that something else is
    // already standing there.
    QString error;
    QVERIFY(!AtomicJson::write(QDir(blocker).filePath(QStringLiteral("deeper/x.json")),
                               someJson(), &error));
    // The sentence has to NAME the directory or there is nothing to act on.
    QVERIFY2(error.contains(QStringLiteral("occupied")), qPrintable(error));
    QCOMPARE(contentsOf(blocker), QByteArrayLiteral("not a directory\n"));
}

void TstWriteFailure::aWriteOntoAPathThatIsADirectoryFailsAndDestroysNothing()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString target = QDir(tmp.path()).filePath(QStringLiteral("map"));
    QVERIFY(QDir(tmp.path()).mkdir(QStringLiteral("map")));
    QVERIFY(writeFileWith(QDir(target).filePath(QStringLiteral("inside")),
                          QByteArrayLiteral("kept")));

    QString error;
    QVERIFY(!AtomicJson::write(target, someJson(), &error));
    QVERIFY(!error.isEmpty());
    // QSaveFile discards its temporary on a failure, so nothing of the caller's is gone
    // and — the part that matters for the pool — no half-written file is left under the
    // name a reader would take for a record.
    QVERIFY(QFileInfo(target).isDir());
    QCOMPARE(contentsOf(QDir(target).filePath(QStringLiteral("inside"))),
             QByteArrayLiteral("kept"));
    QCOMPARE(QDir(tmp.path()).entryList(QDir::Files | QDir::NoDotAndDotDot).size(), 0);
}

void TstWriteFailure::aWriteThatCannotBeFinishedIsReportedAndKeepsThePreviousContents()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString target = QDir(tmp.path()).filePath(QStringLiteral("settings.json"));
    QVERIFY(AtomicJson::write(target, someJson()));
    const QByteArray before = contentsOf(target);
    QVERIFY(!before.isEmpty());

    {
        FileSizeLimit limit(4096);
        if (!limit.ok())
            QSKIP("RLIMIT_FSIZE could not be narrowed on this machine");
        QString error;
        // Whether the refusal surfaces from write() or from the flush inside commit() is
        // the platform's business; that it is REPORTED and that the previous contents are
        // still there is this function's.
        QVERIFY(!AtomicJson::write(target, someJson(64 * 1024), &error));
        QVERIFY(!error.isEmpty());
    }

    QCOMPARE(contentsOf(target), before);
    // And no temporary is left in the directory beside it, which for a pool directory
    // would be a file a rebuild has to be strict enough to ignore.
    QCOMPARE(QDir(tmp.path()).entryList(QDir::Files | QDir::NoDotAndDotDot),
             QStringList{QStringLiteral("settings.json")});
}

void TstWriteFailure::aPrivateWriteLeavesTheSecretReadableByNobodyElse()
{
    if (geteuid() == 0)
        QSKIP("running as root: file modes say nothing about what anyone else can read");
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString target = QDir(tmp.path()).filePath(QStringLiteral("hosts.json"));

    // This is the path a remembered SSH password takes when there is no keychain, so the
    // mode is the whole of the promise the dialog makes when it names the file.
    // The claim is about who OTHER than the owner can read it, so that is what is
    // asserted — never an equality against a QFile::Permissions literal, which on Unix
    // carries the Owner and User spellings of the same three bits and would be an
    // assertion about Qt's enum rather than about the file.
    const QFile::Permissions others = QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup
        | QFile::ReadOther | QFile::WriteOther | QFile::ExeOther;

    QVERIFY(AtomicJson::writePrivate(target, someJson()));
    QVERIFY(QFile::permissions(target).testFlag(QFile::ReadOwner));
    QCOMPARE(QFile::permissions(target) & others, QFile::Permissions());

    // Over an EXISTING file too — the rename makes a new inode every time, so a mode set
    // once is not a mode that stays.
    QVERIFY(AtomicJson::writePrivate(target, someJson(16)));
    QVERIFY(QFile::permissions(target).testFlag(QFile::ReadOwner));
    QCOMPARE(QFile::permissions(target) & others, QFile::Permissions());

    // And a write that did not go through never claims to have restricted anything: the
    // mode is set on the file the rename PUT there, so there is nothing to set when the
    // rename did not happen, and answering true would leave the caller believing a
    // secret is on disk and unreadable when neither half is true.
    {
        FileSizeLimit limit(4096);
        if (!limit.ok())
            QSKIP("RLIMIT_FSIZE could not be narrowed on this machine");
        QString error;
        QVERIFY(!AtomicJson::writePrivate(target, someJson(64 * 1024), &error));
        QVERIFY(!error.isEmpty());
    }
    QCOMPARE(QFile::permissions(target) & others, QFile::Permissions());
}

// ---------------------------------------------------------------------------
// ConfigFileIO — the one thing loftail writes that it did not create
// ---------------------------------------------------------------------------

void TstWriteFailure::aConfigInADirectoryThatIsNotThereIsRefusedByNameAndNothingIsCreated()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString missing = QDir(tmp.path()).filePath(QStringLiteral("no-such-dir"));
    const QString target = QDir(missing).filePath(QStringLiteral("log4cplus.properties"));

    const ConfigWriteResult out = writeConfigFile(target, QByteArrayLiteral("a=1\n"));
    QVERIFY(!out.ok);
    // BOTH halves of the rule. The refusal names the directory, because a mistyped path
    // is what a missing directory almost always means...
    QVERIFY2(out.error.contains(QStringLiteral("no-such-dir")), qPrintable(out.error));
    // ...and it is a refusal to CREATE, which is the deliberate inverse of AtomicJson's
    // mkpath above: loftail sprouting a config tree somewhere nobody asked for is worse
    // than a save that does not go through.
    QVERIFY(!QDir(missing).exists());
    QVERIFY(!QFileInfo::exists(target));
}

void TstWriteFailure::aConfigWriteThatCannotBeFinishedIsReportedAndKeepsThePreviousContents()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString target = QDir(tmp.path()).filePath(QStringLiteral("log4cplus.properties"));
    QVERIFY(writeFileWith(target, QByteArrayLiteral("log4cplus.rootLogger=INFO\n")));

    {
        FileSizeLimit limit(4096);
        if (!limit.ok())
            QSKIP("RLIMIT_FSIZE could not be narrowed on this machine");
        const ConfigWriteResult out =
            writeConfigFile(target, QByteArray(64 * 1024, 'x'));
        // Reported, not swallowed: this is somebody else's file and the editor's tab
        // must not clear its modified mark over a save that did not happen.
        QVERIFY(!out.ok);
        QVERIFY(!out.error.isEmpty());
    }

    QCOMPARE(contentsOf(target), QByteArrayLiteral("log4cplus.rootLogger=INFO\n"));
}

void TstWriteFailure::aConfigThatIsThereAndShutIsNotDescribedAsOneThatIsNotThere()
{
    if (geteuid() == 0)
        QSKIP("running as root: a mode of 000 refuses nobody");
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString target = QDir(tmp.path()).filePath(QStringLiteral("shut.properties"));
    QVERIFY(writeFileWith(target, QByteArrayLiteral("a=1\n")));
    QVERIFY(QFile::setPermissions(target, QFile::Permissions()));

    const ConfigReadResult out = readConfigFile(target);
    // "Not there" and "there and shut" are different sentences. A file the user can see
    // in their file manager must not be reported as one that has not appeared yet — and
    // an unreadable file is emphatically not the supported empty-editor case, which is
    // what `ok` with `existed` false means.
    QVERIFY(!out.ok);
    QVERIFY(!out.existed);
    QVERIFY2(out.error.contains(QStringLiteral("shut.properties")), qPrintable(out.error));

    QFile::setPermissions(target, QFile::ReadOwner | QFile::WriteOwner);
}

void TstWriteFailure::aDirectoryWhereAConfigWasExpectedIsRefusedRatherThanRead()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    QVERIFY(QDir(tmp.path()).mkdir(QStringLiteral("conf")));
    const QString target = QDir(tmp.path()).filePath(QStringLiteral("conf"));

    const ConfigReadResult out = readConfigFile(target);
    QVERIFY(!out.ok);
    QVERIFY2(out.error.contains(QStringLiteral("conf")), qPrintable(out.error));
    QVERIFY(out.bytes.isEmpty());
}

void TstWriteFailure::aConfigTooLargeToEditIsRefusedRatherThanLoaded()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString target = QDir(tmp.path()).filePath(QStringLiteral("huge.properties"));
    {
        QFile f(target);
        QVERIFY(f.open(QIODevice::WriteOnly));
        // Sparse: the cap is about what would be pulled into a QPlainTextEdit, so the
        // bytes need to be claimed rather than written.
        QVERIFY(f.resize(17LL * 1024 * 1024));
    }

    const ConfigReadResult out = readConfigFile(target);
    QVERIFY(!out.ok);
    QVERIFY(out.bytes.isEmpty());
    QVERIFY2(out.error.contains(QStringLiteral("huge.properties")), qPrintable(out.error));
}

// ---------------------------------------------------------------------------
// LogFileStore — the write order, and every state a crash can leave
// ---------------------------------------------------------------------------

void TstWriteFailure::aSaveWhoseSlotFileCannotBeWrittenNeverPutsTheAddressInTheMap()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    LogFileStore store(tmp.path());
    store.load();
    const LogProfile inherited = LogProfile::builtIn();

    const QString a = abs(QStringLiteral("var/log/a.log"));
    const QString b = abs(QStringLiteral("var/log/b.log"));

    LogFileSettings sa;
    sa.address = a;
    sa.profile = profileWith(QStringLiteral("aye"));
    QVERIFY(store.save(sa, inherited));

    LogFileSettings sb;
    sb.address = b;
    // Big enough that its slot file is refused under the limit below while the map that
    // would index it — a few hundred bytes for two entries — is not. THE SIZE IS WHAT
    // SELECTS WHICH OF THE TWO WRITES FAILS, and it is what makes this an assertion
    // about the ORDER rather than about failure reporting: with the map written first,
    // the map write goes through and the record it names never arrives.
    LogProfile pb = profileWith(QStringLiteral("bee"));
    pb.restartScript = QString(64 * 1024, QLatin1Char('#'));
    sb.profile = pb;

    {
        FileSizeLimit limit(4096);
        if (!limit.ok())
            QSKIP("RLIMIT_FSIZE could not be narrowed on this machine");
        QString error;
        QVERIFY(!store.save(sb, inherited, &error));
        QVERIFY(!error.isEmpty());
    }

    // SLOT FILE FIRST, MAP SECOND. The write that failed was the first one, so the map
    // on disk cannot have heard of B at all — the forbidden state is a live map entry
    // naming a slot that holds something else or nothing.
    bool ok = false;
    const QJsonObject root = AtomicJson::read(store.mapPath(), &ok).object();
    QVERIFY(ok);
    const QJsonArray entries = root.value(QStringLiteral("entries")).toArray();
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.at(0).toObject().value(QStringLiteral("address")).toString(),
             logSettingsKey(a));
    QVERIFY(!QFileInfo::exists(store.slotPath(1)));

    // And the store that comes back is the one that was there before the attempt.
    QCOMPARE(served(tmp.path(), a).profile->format.pattern, QStringLiteral("aye"));
    QVERIFY(!served(tmp.path(), b).saysSomething());
}

void TstWriteFailure::aSaveWhoseMapCannotBeWrittenLeavesTheRecordAsAnUnreferencedFile()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString pool = QDir(tmp.path()).filePath(QStringLiteral("fileSettings"));
    QVERIFY(QDir().mkpath(pool));
    // A directory standing where the map goes: the slot files can still be written and
    // the map cannot, which is the other half of the pair the case above provokes.
    QVERIFY(QDir(pool).mkdir(QStringLiteral("map")));

    LogFileStore store(tmp.path());
    store.load();
    const QString b = abs(QStringLiteral("var/log/b.log"));
    LogFileSettings sb;
    sb.address = b;
    sb.profile = profileWith(QStringLiteral("bee"));
    QVERIFY(!store.save(sb, LogProfile::builtIn()));

    // THE RECORD IS THERE AND NOTHING POINTS AT IT — an orphan, which is what every
    // crash on this path is meant to leave. With the map written first, the save would
    // have given up before writing anything and this file would not exist.
    QVERIFY(QFileInfo::exists(store.slotPath(0)));
    bool ok = false;
    const QJsonObject rec = AtomicJson::read(store.slotPath(0), &ok).object();
    QVERIFY(ok);
    QCOMPARE(rec.value(QStringLiteral("address")).toString(), logSettingsKey(b));

    // An orphan costs nothing and is not even lost: every record names its own address,
    // so the rebuild finds it.
    QCOMPARE(served(tmp.path(), b).profile->format.pattern, QStringLiteral("bee"));
}

void TstWriteFailure::everyInterruptionOfAWriteRecoversWithoutServingOneLogAnothersRecord_data()
{
    QTest::addColumn<int>("operation");
    QTest::addColumn<int>("landed");

    // THE INTERRUPTION POINTS ARE ENUMERATED, not sampled, and the enumeration is over
    // the POWER SET of the durable changes rather than over the prefixes of one order.
    // Two things make that the right shape. A durable change here is a QSaveFile rename
    // or an unlink, each of which the filesystem performs atomically, so the states a
    // crash can leave are exactly the subsets of those changes and there is nothing
    // finer to interrupt. And enumerating every subset means the claim below holds
    // whichever order the two writes are attempted in — the order itself is a separate
    // rule, proved by the two cases above, and its job is to keep the pool from leaking
    // a slot rather than to keep a reader safe. What keeps a reader safe is that the
    // file wins, and this is where that is asked at every point it could be needed.
    static const char *const names[] = {
        "a save reusing the slot an interrupted remove orphaned",
        "a remove",
        "a save of a log the pool has not seen",
    };
    for (int op = 0; op < 3; ++op) {
        for (int landed = 0; landed < 4; ++landed) {
            QTest::newRow(qPrintable(QStringLiteral("%1, interruption point %2 of 4")
                                         .arg(QLatin1String(names[op]))
                                         .arg(landed)))
                << op << landed;
        }
    }
}

void TstWriteFailure::everyInterruptionOfAWriteRecoversWithoutServingOneLogAnothersRecord()
{
    QFETCH(int, operation);
    QFETCH(int, landed);

    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const LogProfile inherited = LogProfile::builtIn();
    const QString a = abs(QStringLiteral("var/log/a.log"));
    const QString b = abs(QStringLiteral("var/log/b.log"));
    const QString c = abs(QStringLiteral("var/log/c.log"));
    const QString d = abs(QStringLiteral("var/log/d.log"));

    // Whatever each log may legitimately be serving in ANY of these states. A record
    // that is gone is fine and a record that is stale is fine; a record belonging to a
    // different log is not, and that is the whole assertion.
    QMap<QString, QSet<QString>> allowed;
    allowed[logSettingsKey(a)] = {QStringLiteral("aye")};
    allowed[logSettingsKey(b)] = {QStringLiteral("bee")};
    allowed[logSettingsKey(c)] = {QStringLiteral("see")};
    allowed[logSettingsKey(d)] = {QStringLiteral("dee")};

    const auto saveOne = [&](LogFileStore &store, const QString &address,
                             const QString &pattern) {
        LogFileSettings s;
        s.address = address;
        s.profile = profileWith(pattern);
        return store.save(s, inherited);
    };

    QString pool;
    {
        LogFileStore store(tmp.path());
        store.load();
        QVERIFY(saveOne(store, a, QStringLiteral("aye")));
        QVERIFY(saveOne(store, b, QStringLiteral("bee")));
        pool = store.directory();
    }

    if (operation == 0) {
        // The pre-state is itself a crash aftermath: a remove that wrote the map and did
        // not get as far as deleting the file, leaving slot 0 holding A's record with
        // nothing naming it. That is what makes the next allocation a REUSE, which is
        // the only shape in which one log can be served another's record at all.
        bool ok = false;
        QJsonObject root = AtomicJson::read(QDir(pool).filePath(QStringLiteral("map")), &ok)
                               .object();
        QVERIFY(ok);
        QJsonArray kept;
        for (const auto &v : root.value(QStringLiteral("entries")).toArray()) {
            if (v.toObject().value(QStringLiteral("address")).toString() != logSettingsKey(a))
                kept.append(v);
        }
        root.insert(QStringLiteral("entries"), kept);
        QVERIFY(AtomicJson::write(QDir(pool).filePath(QStringLiteral("map")),
                                  QJsonDocument(root)));
    }

    const DirImage before = imageOf(pool);

    {
        LogFileStore store(tmp.path());
        store.load();
        switch (operation) {
        case 0:
            QVERIFY(saveOne(store, c, QStringLiteral("see")));
            break;
        case 1:
            QVERIFY(store.remove(a));
            break;
        default:
            // The ordinary path: a free slot rather than a reused one, which changes the
            // same two files and is the case every first save of a log takes.
            QVERIFY(saveOne(store, d, QStringLiteral("dee")));
            break;
        }
    }

    const DirImage after = imageOf(pool);

    // The durable changes, derived from the two real images rather than written down:
    // every name whose contents moved, appeared or went.
    QStringList changed;
    for (auto it = after.cbegin(); it != after.cend(); ++it) {
        if (!before.contains(it.key()) || before.value(it.key()) != it.value())
            changed.append(it.key());
    }
    for (auto it = before.cbegin(); it != before.cend(); ++it) {
        if (!after.contains(it.key()))
            changed.append(it.key());
    }
    changed.sort();
    QCOMPARE(changed.size(), 2); // the slot file and the map, in one order or the other

    // The state a crash would have left with this subset of them landed.
    DirImage state = before;
    for (int i = 0; i < changed.size(); ++i) {
        if (!(landed & (1 << i)))
            continue;
        const QString name = changed.at(i);
        if (after.contains(name))
            state.insert(name, after.value(name));
        else
            state.remove(name);
    }
    restoreImage(pool, state);

    for (const QString &address : {a, b, c, d}) {
        const LogFileSettings s = served(tmp.path(), address);
        if (!s.saysSomething())
            continue; // Losing a record to a crash is allowed; serving the wrong one is not.
        QCOMPARE(s.address, logSettingsKey(address));
        QVERIFY2(allowed.value(logSettingsKey(address))
                     .contains(s.profile->format.pattern),
                 qPrintable(QStringLiteral("%1 was served the pattern %2")
                                .arg(address, s.profile->format.pattern)));
    }
}

void TstWriteFailure::aLegacyAdoptionWithNoRoomToSpareLeavesTheOldRecordWhereItIs()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    QTemporaryDir logs;
    QVERIFY(logs.isValid());

    // A real file and a symbolic link to it: the link is the name as OPENED, and the
    // file's own name is the key an older build would have filed the record under.
    const QString real = QDir(logs.path()).filePath(QStringLiteral("real.log"));
    const QString link = QDir(logs.path()).filePath(QStringLiteral("latest.log"));
    QVERIFY(writeFileWith(real, QByteArrayLiteral("x\n")));
    QVERIFY(QFile::link(real, link));
    QVERIFY(logSettingsKey(link) != legacyLogSettingsKey(link));

    LogFileStore store(tmp.path());
    store.load();
    const LogProfile inherited = LogProfile::builtIn();

    LogFileSettings sr;
    sr.address = real;
    sr.profile = profileWith(QStringLiteral("under the old spelling"));
    QVERIFY(store.save(sr, inherited));

    QSet<QString> open{logSettingsKey(real)};
    for (int i = 0; i + 1 < LogFileStore::kSlots; ++i) {
        LogFileSettings s;
        s.address = abs(QStringLiteral("var/log/filler%1.log").arg(i));
        s.profile = profileWith(QStringLiteral("f%1").arg(i));
        QVERIFY(store.save(s, inherited));
        open.insert(logSettingsKey(s.address));
    }
    // Every slot belongs to a log that is open, so the adoption has no room that is not
    // another log's — and a COPY is the only migration this is allowed to be.
    store.setPinned(open);

    QVERIFY(!store.read(link).saysSomething());
    // The old record is untouched, which is what makes declining safe: the migration is
    // simply not spent, and the next launch with a slot free will make it.
    QCOMPARE(store.read(real).profile->format.pattern,
             QStringLiteral("under the old spelling"));
}

QTEST_APPLESS_MAIN(TstWriteFailure)
#include "tst_writefailure.moc"
