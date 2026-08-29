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

#include "Highlight.h"
#include "LogFileSettings.h"
#include "LogFileStore.h"
#include "LogSettings.h"
#include "MatchCriteria.h"
#include "RemoteLocation.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

using namespace loftail;

namespace {

// Absolute on BOTH platforms, and therefore unchanged by logSettingsKey(). A path
// written "/var/log/app.log" is absolute on POSIX and is NOT on Windows, where it is
// resolved against the current drive — so a literal spelled the POSIX way comes back out
// of the key wearing a drive letter and every comparison against it misses (CLAUDE.md;
// tst_archivelocation's abs() helper is the older precedent for the same rule).
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

// A filter state that genuinely narrows: one axis switched on and restricting.
QJsonObject narrowingFilters()
{
    MatchCriteria c;
    c.text.enabled = true;
    c.text.matcher.set(QStringLiteral("boom"), false, Qt::CaseInsensitive);
    return c.toJson();
}

// What an untouched Filters pane over an INDEXED log serializes to: the value axes are
// switched off but list every name the scan has discovered, because every discovered
// subsystem starts ticked (SPEC.md §6). This is the shape that makes a value comparison
// against a default-constructed MatchCriteria the wrong test.
QJsonObject pristineFiltersOverAnIndexedLog()
{
    MatchCriteria c;
    c.loggerNames = QStringList{QStringLiteral("net.http"), QStringLiteral("db")};
    c.threadNames = QStringList{QStringLiteral("main")};
    // Switched off, covering everything — which is what setCriteria/criteria round-trip
    // to for a pane nobody has touched.
    c.loggerEnabled = false;
    c.threadEnabled = false;
    c.loggerCoversAll = true;
    c.threadCoversAll = true;
    return c.toJson();
}

} // namespace

class TstLogFileStore : public QObject
{
    Q_OBJECT

private slots:
    // --- The record ------------------------------------------------------------------
    void aRecordRoundTripsWhole();
    void anInheritedProfileIsAMarkAndNotAnAbsence();
    void everySectionThatFallsIntoLineIsDropped();
    void aStoredEmptyRuleListIsAnAnswerAndSurvives();
    void aPristinePaneOverAnIndexedLogSaysNothing();
    void aTimeBoundBehindASwitchedOffAxisSaysNothing();

    // --- The pool --------------------------------------------------------------------
    void aRecordSayingNothingIsNeverWritten();
    void bringingARecordBackIntoLineFreesItsSlot();
    void slotsAreHandedOutLowestFirstAndReused();
    void theLeastRecentlyOpenedRecordIsEvicted();
    void anOpenLogIsNeverEvicted();
    void withEveryLogOpenTheWriteIsRefusedRatherThanStealingASlot();

    // --- Robustness ------------------------------------------------------------------
    void aSlotHoldingAnotherLogsRecordIsNotServed();
    void aMapEntryWhoseSlotIsMissingIsDropped();
    void aLostMapIsRebuiltFromTheRecords();
    void aNewerSchemaLoadsEmptyAndRefusesToSave();
    void equivalentSpellingsOfOneAddressAreOneRecord();
    void aRecordWrittenUnderTheOldCanonicalKeyIsCopiedNotMoved();

    // --- The sweep -------------------------------------------------------------------
    void aPatternTaughtWhatItsLogsSaidLeavesThemNothingToSay();
    void theSweepSparesARecordThatStillDiffers();
};

// ---------------------------------------------------------------------------
// The record
// ---------------------------------------------------------------------------

void TstLogFileStore::aRecordRoundTripsWhole()
{
    LogFileSettings s;
    s.address = abs(QStringLiteral("var/log/app.log"));
    s.profile = profileWith(QStringLiteral("%d %p %m%n"));
    s.profile->format.encoding = Encoding::Utf16LE;
    s.profile->format.timeDisplay = TimeDisplay::Utc;
    s.profile->format.runStartPattern = QStringLiteral("STARTING");
    s.profile->format.runStartIsRegex = true;
    s.profile->format.runStartCaseSensitive = true;
    s.profile->wrapMode = WrapMode::AlwaysOn;
    s.profile->configPath = QStringLiteral("../conf/log4cplus.properties");
    s.filters = narrowingFilters();
    s.highlighters = HighlighterSet::defaults().toJson();
    s.run.all = false;
    s.run.startOffset = 4096;
    s.run.startTimestamp = 1700000000000LL;

    const LogFileSettings back = LogFileSettings::fromJson(s.toJson());
    QCOMPARE(back, s);
    QVERIFY(back.profile.has_value());
    QCOMPARE(back.profile->format.encoding, Encoding::Utf16LE);
    QCOMPARE(back.profile->wrapMode, WrapMode::AlwaysOn);
    QCOMPARE(back.profile->configPath, QStringLiteral("../conf/log4cplus.properties"));
    QCOMPARE(back.run.startOffset, 4096);
    QCOMPARE(back.run.startTimestamp, 1700000000000LL);
}

void TstLogFileStore::anInheritedProfileIsAMarkAndNotAnAbsence()
{
    LogFileSettings s;
    s.address = abs(QStringLiteral("var/log/app.log"));
    s.filters = narrowingFilters(); // it is kept for its filters
    QVERIFY(!s.profile.has_value());

    const QJsonObject o = s.toJson();
    // A STRING where an object would be. A record that states filters and nothing else
    // still says something deliberate about its format rather than reading as a file
    // that was half written.
    QVERIFY(o.contains(QStringLiteral("profile")));
    QCOMPARE(o.value(QStringLiteral("profile")).toString(), QStringLiteral("inherited"));

    QVERIFY(!LogFileSettings::fromJson(o).profile.has_value());
}

void TstLogFileStore::everySectionThatFallsIntoLineIsDropped()
{
    const LogProfile inherited = profileWith(QStringLiteral("%m%n"));

    LogFileSettings s;
    s.address = abs(QStringLiteral("var/log/app.log"));
    s.profile = inherited;                        // exactly what it would inherit
    s.filters = pristineFiltersOverAnIndexedLog(); // narrows nothing
    s.highlighters = HighlighterSet::defaults().toJson(); // exactly the seed
    s.run = RunSelection();                       // follow the last run

    s.reduce(inherited);
    QVERIFY(!s.profile.has_value());
    QVERIFY(s.filters.isEmpty());
    QVERIFY(!s.highlighters.has_value());
    QVERIFY(!s.saysSomething());
}

void TstLogFileStore::aStoredEmptyRuleListIsAnAnswerAndSurvives()
{
    // PRESENCE, NOT EMPTINESS. An empty list is the user having deleted every rule and
    // must stay deleted; absent is "nobody has ever said anything", which the caller
    // seeds with the three level colours. Reading the emptiness re-seeds them on every
    // launch — the core half of tst_sessiongui::aDeletedDefaultRuleStaysDeletedAcross-
    // ARelaunch.
    LogFileSettings s;
    s.address = abs(QStringLiteral("var/log/app.log"));
    s.highlighters = QJsonArray();

    s.reduce(LogProfile::builtIn());
    QVERIFY(s.highlighters.has_value());
    QVERIFY(s.highlighters->isEmpty());
    QVERIFY(s.saysSomething());

    const LogFileSettings back = LogFileSettings::fromJson(s.toJson());
    QVERIFY(back.highlighters.has_value());
    QVERIFY(back.highlighters->isEmpty());

    // And the other side of the same coin: a record that never spoke about rules must
    // come back silent, not empty, or the caller cannot tell the two apart.
    LogFileSettings quiet;
    quiet.address = s.address;
    QVERIFY(!LogFileSettings::fromJson(quiet.toJson()).highlighters.has_value());
}

void TstLogFileStore::aPristinePaneOverAnIndexedLogSaysNothing()
{
    // The trap this exists for: an untouched pane lists every subsystem the scan has
    // discovered, so `MatchCriteria::fromJson(state) == MatchCriteria{}` is false for
    // every log that has finished indexing — which would give every log a record.
    const QJsonObject pristine = pristineFiltersOverAnIndexedLog();
    QVERIFY(pristine != MatchCriteria().toJson());
    QVERIFY(filterStateSaysNothing(pristine));

    QVERIFY(filterStateSaysNothing(QJsonObject()));
    QVERIFY(!filterStateSaysNothing(narrowingFilters()));

    // A value axis switched ON but covering everything narrows nothing either.
    MatchCriteria wide;
    wide.loggerEnabled = true;
    wide.loggerNames = QStringList{QStringLiteral("a"), QStringLiteral("b")};
    wide.loggerCoversAll = true;
    QVERIFY(filterStateSaysNothing(wide.toJson()));

    MatchCriteria narrow = wide;
    narrow.loggerCoversAll = false;
    QVERIFY(!filterStateSaysNothing(narrow.toJson()));

    // Context is a deliberate edit even though it is inert without a text axis.
    QJsonObject withContext = pristine;
    withContext.insert(QStringLiteral("contextBefore"), 2);
    QVERIFY(!filterStateSaysNothing(withContext));
}

void TstLogFileStore::aTimeBoundBehindASwitchedOffAxisSaysNothing()
{
    // AxisEditor::criteria() is not the inverse of setCriteria(): a QDateTimeEdit always
    // holds a datetime, so an untouched time axis reads back as a VALID bound. Reading it
    // while the axis is off is what used to rewrite this log's seeded highlight rules on
    // a bare run click.
    MatchCriteria c;
    c.timeEnabled = false;
    c.start = QDateTime(QDate(2000, 1, 1), QTime(0, 0));
    c.end = QDateTime(QDate(2000, 1, 1), QTime(0, 0));
    QVERIFY(filterStateSaysNothing(c.toJson()));

    c.timeEnabled = true;
    QVERIFY(!filterStateSaysNothing(c.toJson()));
}

// ---------------------------------------------------------------------------
// The pool
// ---------------------------------------------------------------------------

void TstLogFileStore::aRecordSayingNothingIsNeverWritten()
{
    QTemporaryDir dir;
    LogFileStore store(dir.path());
    store.load();

    const QString path = abs(QStringLiteral("var/log/app.log"));
    const LogProfile inherited = profileWith(QStringLiteral("%m%n"));

    LogFileSettings s;
    s.address = path;
    s.profile = inherited; // says exactly what it would inherit

    QVERIFY(!store.save(s, inherited));
    QVERIFY(store.addresses().isEmpty());
    // No slot file, no map: opening a log and changing nothing leaves NOTHING behind.
    QVERIFY(!QFile::exists(store.slotPath(0)));
    QVERIFY(!QFile::exists(store.mapPath()));
}

void TstLogFileStore::bringingARecordBackIntoLineFreesItsSlot()
{
    QTemporaryDir dir;
    LogFileStore store(dir.path());
    store.load();

    const QString path = abs(QStringLiteral("var/log/app.log"));
    const LogProfile inherited = profileWith(QStringLiteral("%m%n"));

    LogFileSettings s;
    s.address = path;
    s.profile = profileWith(QStringLiteral("%d %m%n"));
    QVERIFY(store.save(s, inherited));
    QCOMPARE(store.addresses(), QStringList{path});
    QVERIFY(QFile::exists(store.slotPath(0)));

    // Brought back into line: the record has nothing left to say, so it goes.
    s.profile = inherited;
    QVERIFY(store.save(s, inherited));
    QVERIFY(store.addresses().isEmpty());
    QVERIFY(!QFile::exists(store.slotPath(0)));

    // And the freed slot is handed to the next log rather than skipped.
    LogFileSettings other;
    other.address = abs(QStringLiteral("var/log/other.log"));
    other.profile = profileWith(QStringLiteral("%p %m%n"));
    QVERIFY(store.save(other, inherited));
    QVERIFY(QFile::exists(store.slotPath(0)));
}

void TstLogFileStore::slotsAreHandedOutLowestFirstAndReused()
{
    QTemporaryDir dir;
    LogFileStore store(dir.path());
    store.load();
    const LogProfile inherited = LogProfile::builtIn();

    QStringList paths;
    for (int i = 0; i < 3; ++i) {
        LogFileSettings s;
        s.address = abs(QStringLiteral("var/log/a%1.log").arg(i));
        s.profile = profileWith(QStringLiteral("p%1").arg(i));
        paths.append(logSettingsKey(s.address));
        QVERIFY(store.save(s, inherited));
    }
    for (int i = 0; i < 3; ++i)
        QVERIFY(QFile::exists(store.slotPath(i)));

    QVERIFY(store.remove(paths.at(1)));
    QVERIFY(!QFile::exists(store.slotPath(1)));

    LogFileSettings s;
    s.address = abs(QStringLiteral("var/log/new.log"));
    s.profile = profileWith(QStringLiteral("new"));
    QVERIFY(store.save(s, inherited));
    QVERIFY(QFile::exists(store.slotPath(1)));
    QCOMPARE(store.read(s.address).profile->format.pattern, QStringLiteral("new"));
}

void TstLogFileStore::theLeastRecentlyOpenedRecordIsEvicted()
{
    QTemporaryDir dir;
    LogFileStore store(dir.path());
    store.load();
    const LogProfile inherited = LogProfile::builtIn();

    QStringList paths;
    for (int i = 0; i < LogFileStore::kSlots; ++i) {
        LogFileSettings s;
        s.address = abs(QStringLiteral("var/log/a%1.log").arg(i));
        s.profile = profileWith(QStringLiteral("p%1").arg(i));
        paths.append(logSettingsKey(s.address));
        QVERIFY(store.save(s, inherited));
    }
    QCOMPARE(store.addresses().size(), LogFileStore::kSlots);

    // Open one of the oldest again, so it is no longer the least recently opened.
    store.touch(paths.at(0));

    LogFileSettings extra;
    extra.address = abs(QStringLiteral("var/log/extra.log"));
    extra.profile = profileWith(QStringLiteral("extra"));
    QVERIFY(store.save(extra, inherited));

    // The pool never grows past its bound.
    QCOMPARE(store.addresses().size(), LogFileStore::kSlots);
    QVERIFY(store.addresses().contains(logSettingsKey(extra.address)));
    // a0 was re-opened, so a1 — the next oldest — is what went.
    QVERIFY(store.addresses().contains(paths.at(0)));
    QVERIFY(!store.addresses().contains(paths.at(1)));
    QVERIFY(!store.read(paths.at(1)).saysSomething());
}

void TstLogFileStore::anOpenLogIsNeverEvicted()
{
    QTemporaryDir dir;
    LogFileStore store(dir.path());
    store.load();
    const LogProfile inherited = LogProfile::builtIn();

    QStringList paths;
    for (int i = 0; i < LogFileStore::kSlots; ++i) {
        LogFileSettings s;
        s.address = abs(QStringLiteral("var/log/a%1.log").arg(i));
        s.profile = profileWith(QStringLiteral("p%1").arg(i));
        paths.append(logSettingsKey(s.address));
        QVERIFY(store.save(s, inherited));
    }

    // a0 is the least recently opened AND is open in a tab. Evicting it would take
    // settings out from under a tab that is still using them.
    store.setPinned({paths.at(0)});

    LogFileSettings extra;
    extra.address = abs(QStringLiteral("var/log/extra.log"));
    extra.profile = profileWith(QStringLiteral("extra"));
    QVERIFY(store.save(extra, inherited));

    QVERIFY(store.addresses().contains(paths.at(0)));
    QCOMPARE(store.read(paths.at(0)).profile->format.pattern, QStringLiteral("p0"));
    QVERIFY(!store.addresses().contains(paths.at(1)));
}

void TstLogFileStore::withEveryLogOpenTheWriteIsRefusedRatherThanStealingASlot()
{
    QTemporaryDir dir;
    LogFileStore store(dir.path());
    store.load();
    const LogProfile inherited = LogProfile::builtIn();

    QSet<QString> everyone;
    for (int i = 0; i < LogFileStore::kSlots; ++i) {
        LogFileSettings s;
        s.address = abs(QStringLiteral("var/log/a%1.log").arg(i));
        s.profile = profileWith(QStringLiteral("p%1").arg(i));
        everyone.insert(logSettingsKey(s.address));
        QVERIFY(store.save(s, inherited));
    }
    store.setPinned(everyone);

    LogFileSettings extra;
    extra.address = abs(QStringLiteral("var/log/extra.log"));
    extra.profile = profileWith(QStringLiteral("extra"));

    QString error;
    QVERIFY(!store.save(extra, inherited, &error));
    QVERIFY(!error.isEmpty());
    // Nothing was taken from anybody.
    QCOMPARE(store.addresses().size(), LogFileStore::kSlots);
    for (int i = 0; i < LogFileStore::kSlots; ++i) {
        QCOMPARE(store.read(abs(QStringLiteral("var/log/a%1.log").arg(i)))
                     .profile->format.pattern,
                 QStringLiteral("p%1").arg(i));
    }
}

// ---------------------------------------------------------------------------
// Robustness
// ---------------------------------------------------------------------------

void TstLogFileStore::aSlotHoldingAnotherLogsRecordIsNotServed()
{
    QTemporaryDir dir;
    LogFileStore store(dir.path());
    store.load();
    const LogProfile inherited = LogProfile::builtIn();

    const QString a = abs(QStringLiteral("var/log/a.log"));
    const QString b = abs(QStringLiteral("var/log/b.log"));

    LogFileSettings sb;
    sb.address = b;
    sb.profile = profileWith(QStringLiteral("bee"));
    QVERIFY(store.save(sb, inherited));

    // Hand-write a map claiming slot 0 — which holds B's record — belongs to A as well.
    // This is what two instances racing on an allocation leaves behind.
    QJsonObject entryA;
    entryA.insert(QStringLiteral("address"), logSettingsKey(a));
    entryA.insert(QStringLiteral("slot"), 0);
    entryA.insert(QStringLiteral("used"), 99);
    QJsonObject entryB;
    entryB.insert(QStringLiteral("address"), logSettingsKey(b));
    entryB.insert(QStringLiteral("slot"), 0);
    entryB.insert(QStringLiteral("used"), 1);
    QJsonObject root;
    root.insert(QStringLiteral("schemaVersion"), LogFileStore::kSchemaVersion);
    root.insert(QStringLiteral("tick"), 99);
    root.insert(QStringLiteral("entries"), QJsonArray{entryA, entryB});
    QFile map(store.mapPath());
    QVERIFY(map.open(QIODevice::WriteOnly));
    map.write(QJsonDocument(root).toJson());
    map.close();

    LogFileStore reopened(dir.path());
    reopened.load();
    // THE FILE WINS. A's claim is refused because slot 0 does not name A...
    QVERIFY(!reopened.read(a).saysSomething());
    // ...and the load's own duplicate-slot rule kept the more recent claim, so B's entry
    // was the one dropped from the map. Either way, neither log is served the other's
    // record — which is the only outcome that matters.
    QVERIFY(!reopened.addresses().contains(logSettingsKey(a)));
}

void TstLogFileStore::aMapEntryWhoseSlotIsMissingIsDropped()
{
    QTemporaryDir dir;
    LogFileStore store(dir.path());
    store.load();
    const LogProfile inherited = LogProfile::builtIn();

    LogFileSettings s;
    s.address = abs(QStringLiteral("var/log/a.log"));
    s.profile = profileWith(QStringLiteral("aye"));
    QVERIFY(store.save(s, inherited));

    // A crash between the map write and the slot write cannot happen in that order, but
    // a file lost to anything else must read as "no settings" rather than as a fatality.
    QVERIFY(QFile::remove(store.slotPath(0)));

    LogFileStore reopened(dir.path());
    reopened.load();
    QVERIFY(!reopened.read(s.address).saysSomething());
    QVERIFY(reopened.addresses().isEmpty());
}

void TstLogFileStore::aLostMapIsRebuiltFromTheRecords()
{
    QTemporaryDir dir;
    const LogProfile inherited = LogProfile::builtIn();
    QStringList paths;
    {
        LogFileStore store(dir.path());
        store.load();
        for (int i = 0; i < 3; ++i) {
            LogFileSettings s;
            s.address = abs(QStringLiteral("var/log/a%1.log").arg(i));
            s.profile = profileWith(QStringLiteral("p%1").arg(i));
            paths.append(logSettingsKey(s.address));
            QVERIFY(store.save(s, inherited));
        }
    }

    // The index is a convenience; every record names its own address, so losing it costs
    // the eviction ORDER and no record at all.
    QVERIFY(QFile::remove(QDir(dir.path()).filePath(QStringLiteral("fileSettings/map"))));

    LogFileStore reopened(dir.path());
    reopened.load();
    QCOMPARE(reopened.addresses().size(), 3);
    for (int i = 0; i < 3; ++i) {
        QCOMPARE(reopened.read(paths.at(i)).profile->format.pattern,
                 QStringLiteral("p%1").arg(i));
    }
}

void TstLogFileStore::aNewerSchemaLoadsEmptyAndRefusesToSave()
{
    QTemporaryDir dir;
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("fileSettings")));

    QJsonObject root;
    root.insert(QStringLiteral("schemaVersion"), LogFileStore::kSchemaVersion + 1);
    root.insert(QStringLiteral("entries"), QJsonArray{});
    const QString mapPath = QDir(dir.path()).filePath(QStringLiteral("fileSettings/map"));
    QFile map(mapPath);
    QVERIFY(map.open(QIODevice::WriteOnly));
    const QByteArray written = QJsonDocument(root).toJson();
    map.write(written);
    map.close();

    LogFileStore store(dir.path());
    store.load();
    QVERIFY(store.readOnly());
    QVERIFY(store.addresses().isEmpty());

    LogFileSettings s;
    s.address = abs(QStringLiteral("var/log/a.log"));
    s.profile = profileWith(QStringLiteral("aye"));
    QVERIFY(!store.save(s, LogProfile::builtIn()));

    // Running an older build for one session must not discard a newer one's data.
    QFile check(mapPath);
    QVERIFY(check.open(QIODevice::ReadOnly));
    QCOMPARE(check.readAll(), written);
}

void TstLogFileStore::equivalentSpellingsOfOneAddressAreOneRecord()
{
    QTemporaryDir dir;
    LogFileStore store(dir.path());
    store.load();
    const LogProfile inherited = LogProfile::builtIn();

    LogFileSettings s;
    s.address = QStringLiteral("ssh://deploy@web1/var/log/app.log");
    s.profile = profileWith(QStringLiteral("remote"));
    QVERIFY(store.save(s, inherited));

    // logSettingsKey() always spells the port, so these are one log and one record —
    // the same guarantee the settings tree gave, now given by the pool.
    const LogFileSettings back =
        store.read(QStringLiteral("ssh://deploy@web1:22/var/log/app.log"));
    QVERIFY(back.profile.has_value());
    QCOMPARE(back.profile->format.pattern, QStringLiteral("remote"));
    QCOMPARE(store.addresses().size(), 1);
}

// THE UPGRADE FALLBACK (bugs.md 27). logSettingsKey() answered canonicalFilePath() for a
// local log until the name as opened became authoritative, so every record a previous
// build wrote for a symlinked log is filed under a name nothing asks for any more.
// Losing all of them on the first launch after the upgrade is not an acceptable reading
// of the change, so the old spelling is looked up once, on a miss, and the record is
// COPIED under the name asked for — into a slot of its own, with the old record left
// exactly where it is.
//
// The copy is the half worth pinning, and it is not fastidiousness: the old spelling is a
// real file's real name, and logSettingsKey() answers that same string for that file
// today. So a record found here is equally one an old build mis-keyed and one it keyed
// correctly, and re-keying in place would hand the target file's configured settings to a
// symlink of it, permanently, for no gesture beyond opening the link once. The second half
// of the case reads the target's own name back afterwards, which is what fails against a
// migration that moves.
void TstLogFileStore::aRecordWrittenUnderTheOldCanonicalKeyIsCopiedNotMoved()
{
    QTemporaryDir logs;
    QVERIFY(logs.isValid());
    const QString target = logs.filePath(QStringLiteral("2026-08-29.log"));
    {
        QFile f(target);
        QVERIFY(f.open(QIODevice::WriteOnly));
    }
    const QString latest = logs.filePath(QStringLiteral("latest.log"));
    if (!QFile::link(target, latest) || !QFileInfo(latest).isSymLink())
        QSKIP("this filesystem will not make a symlink");

    const QString canonical = QFileInfo(latest).canonicalFilePath();
    QVERIFY(canonical != logSettingsKey(latest)); // or there is nothing to migrate
    QCOMPARE(legacyLogSettingsKey(latest), canonical);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        // What the old build left behind: the record filed under the canonical name.
        LogFileStore store(dir.path());
        store.load();
        LogFileSettings s;
        s.address = canonical;
        s.profile = profileWith(QStringLiteral("HOUSE"));
        QVERIFY(store.save(s, LogProfile::builtIn()));
    }

    LogFileStore store(dir.path());
    store.load();
    const LogFileSettings back = store.read(latest);
    QVERIFY2(back.profile.has_value(), "a record written under the old key was lost");
    QCOMPARE(back.profile->format.pattern, QStringLiteral("HOUSE"));
    // Answered under the name it was asked about, and filed under it — beside the old
    // record, which is still there under the name that is still a live key for the file.
    QCOMPARE(back.address, logSettingsKey(latest));
    QStringList after = store.addresses();
    after.sort();
    QStringList expected{canonical, logSettingsKey(latest)};
    expected.sort();
    QCOMPARE(after, expected);
    // The migration flushes, so the map on disk already names both.
    QVERIFY(store.flush());

    // Both stay found, which is the half only a fresh store can show: each slot file
    // carries its own name, so the file-wins check agrees with the map for both.
    LogFileStore reopened(dir.path());
    reopened.load();
    const LogFileSettings again = reopened.read(latest);
    QVERIFY2(again.profile.has_value(), "the copied record did not survive a reload");
    QCOMPARE(again.profile->format.pattern, QStringLiteral("HOUSE"));

    // THE FILE'S OWN NAME KEEPS ITS SETTINGS. This is what a migration that re-keyed in
    // place would lose — silently, permanently, and for nothing more than the symlink
    // having been opened once.
    const LogFileSettings targetsOwn = reopened.read(target);
    QVERIFY2(targetsOwn.profile.has_value(),
             "opening a symlink took the target file's record away");
    QCOMPARE(targetsOwn.profile->format.pattern, QStringLiteral("HOUSE"));

    // TWO SYMLINKS TO ONE FILE ARE TWO LOGS NOW, and that is the accepted cost of the
    // name being authoritative (logSettingsKey()). A second link is a second name, so it
    // gets a copy of its own rather than nothing — the legacy record is still there to be
    // found — and the three records are then independent.
    const QString other = logs.filePath(QStringLiteral("current.log"));
    if (QFile::link(target, other) && QFileInfo(other).isSymLink()) {
        const LogFileSettings second = reopened.read(other);
        QVERIFY(second.profile.has_value());
        QCOMPARE(second.address, logSettingsKey(other));
        QCOMPARE(reopened.addresses().size(), 3);
    }
}

// ---------------------------------------------------------------------------
// The sweep
// ---------------------------------------------------------------------------

void TstLogFileStore::aPatternTaughtWhatItsLogsSaidLeavesThemNothingToSay()
{
    QTemporaryDir dir;
    LogFileStore store(dir.path());
    store.load();

    LogSettingsTree tree;
    LogPatternNode pat;
    pat.match = QStringLiteral("*.log");
    tree.addPattern(pat);

    const LogProfile house = profileWith(QStringLiteral("HOUSE"));
    QStringList paths;
    for (int i = 0; i < 5; ++i) {
        LogFileSettings s;
        s.address = abs(QStringLiteral("var/log/a%1.log").arg(i));
        s.profile = house; // each log says by hand what the house layout will say
        paths.append(logSettingsKey(s.address));
        QVERIFY(store.save(s, tree.inherited(s.address)));
    }
    QCOMPARE(store.addresses().size(), 5);

    // The pattern above them is taught the same thing. Nothing writes those five records,
    // so without the sweep they go on shadowing the pattern for ever — editable, and no
    // longer followed by the logs that were meant to follow it.
    tree.patternAt(0).profile = house;
    QCOMPARE(store.pruneAgainst(tree), 5);
    QVERIFY(store.addresses().isEmpty());
    for (const QString &p : std::as_const(paths))
        QVERIFY(!store.read(p).saysSomething());
}

void TstLogFileStore::theSweepSparesARecordThatStillDiffers()
{
    QTemporaryDir dir;
    LogFileStore store(dir.path());
    store.load();

    LogSettingsTree tree;
    LogPatternNode pat;
    pat.match = QStringLiteral("*.log");
    tree.addPattern(pat);

    const LogProfile house = profileWith(QStringLiteral("HOUSE"));
    const QString same = abs(QStringLiteral("var/log/same.log"));
    const QString odd = abs(QStringLiteral("var/log/odd.log"));
    const QString filtered = abs(QStringLiteral("var/log/filtered.log"));

    LogFileSettings a;
    a.address = same;
    a.profile = house;
    QVERIFY(store.save(a, tree.inherited(same)));

    LogFileSettings b;
    b.address = odd;
    b.profile = profileWith(QStringLiteral("ODD"));
    QVERIFY(store.save(b, tree.inherited(odd)));

    // Kept for its FILTERS, with a profile that is about to become redundant: the sweep
    // must drop the profile section and keep the record, not the other way round.
    LogFileSettings c;
    c.address = filtered;
    c.profile = house;
    c.filters = narrowingFilters();
    QVERIFY(store.save(c, tree.inherited(filtered)));

    tree.patternAt(0).profile = house;
    QCOMPARE(store.pruneAgainst(tree), 2);

    QVERIFY(!store.read(same).saysSomething());
    QCOMPARE(store.read(odd).profile->format.pattern, QStringLiteral("ODD"));

    const LogFileSettings back = store.read(filtered);
    QVERIFY(back.saysSomething());
    QVERIFY(!back.profile.has_value());
    QCOMPARE(back.filters, narrowingFilters());
}

QTEST_APPLESS_MAIN(TstLogFileStore)
#include "tst_logfilestore.moc"
