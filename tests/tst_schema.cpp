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


// THE GOLDEN CORPUS, AND WHY IT IS BYTES RATHER THAN CODE.
//
// Every other version test in this tree writes `kSchemaVersion` — the CONSTANT — into the
// file it then reads back. That is a round trip, and a round trip is exactly what cannot
// fail when a format breaks: bump the constant and those tests start writing the new
// version, reading the new version, and passing while saying nothing whatever about the
// old one. tst_session is the single exception in the tree, and it is the exception
// because SessionStore is the one store that has actually bumped: it writes a literal 1,
// a literal 2 and a literal 3, and asserts what they migrate to.
//
// So the fixtures here are LITERAL TEXT, committed as the bytes a shipped build wrote,
// and nothing in them is derived from a constant this build could move. Reading one and
// getting the right struct back is the whole assertion. When a version is bumped, the
// corpus grows a file — it is never edited — and the reading test for the old one stays
// exactly as it is.
//
// WHAT TO DO AT A BUMP (ARCHITECTURE.md §8.4):
//   1. add the new version's literal beside the old one, leaving the old one alone;
//   2. extend that store's migrate step so vN -> vN+1 is one step in the chain;
//   3. leave every existing case in this file untouched. A case that has to be edited to
//      keep passing is the bug this file exists to report.

#include "HostBookmarkStore.h"
#include "LogFileSettings.h"
#include "LogFileStore.h"
#include "LogProfile.h"
#include "LogSettingsStore.h"
#include "SchemaVersion.h"
#include "SessionStore.h"

#if defined(LOFTAIL_HAVE_PRESETS)
#include "PresetStore.h"
#endif

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

using namespace loftail;

namespace {

// Written as the store writes it, not as the struct is spelled: a fixture that is
// assembled from the same helpers the reader uses would follow a rename and prove nothing.
bool put(const QString &path, const char *bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    return f.write(QByteArray(bytes)) == qsizetype(qstrlen(bytes));
}

QJsonObject readObject(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

// ---------------------------------------------------------------------------------
// logsettings.json — the two inherited levels (M20).
// ---------------------------------------------------------------------------------
constexpr auto kLogSettingsV1 = R"({
    "defaults": {
        "configPath": "../conf/log4cplus.properties",
        "encoding": 2,
        "pattern": "%d{%Y-%m-%d %H:%M:%S} [%t] %-5p %c - %m%n",
        "restartScript": "systemctl restart audit",
        "runStartCase": true,
        "runStartPattern": "=== START ===",
        "runStartRegex": true,
        "sourceZone": "offset:7200",
        "timeDisplay": "sincePrevious",
        "wrapMode": 2
    },
    "patterns": [
        {
            "caseSensitive": true,
            "fullPath": true,
            "id": "p1",
            "kind": "regex",
            "match": ".*\\.audit\\.log$",
            "profile": {
                "configPath": "../conf/log4cplus.properties",
                "encoding": 2,
                "pattern": "%d %p %m%n",
                "restartScript": "systemctl restart audit",
                "runStartCase": true,
                "runStartPattern": "=== START ===",
                "runStartRegex": true,
                "sourceZone": "offset:7200",
                "timeDisplay": "sincePrevious",
                "wrapMode": 2
            }
        }
    ],
    "schemaVersion": 1
}
)";

// ---------------------------------------------------------------------------------
// The per-log pool (M21): an index and one numbered file per record.
// ---------------------------------------------------------------------------------
constexpr auto kPoolMapV1 = R"({
    "entries": [
        {
            "address": "@ADDRESS@",
            "slot": 0,
            "used": 1
        }
    ],
    "schemaVersion": 1,
    "tick": 1
}
)";

constexpr auto kPoolRecordV1 = R"({
    "address": "@ADDRESS@",
    "highlighters": [
    ],
    "profile": {
        "configPath": "../conf/log4cplus.properties",
        "encoding": 2,
        "pattern": "%m%n",
        "restartScript": "systemctl restart audit",
        "runStartCase": true,
        "runStartPattern": "=== START ===",
        "runStartRegex": true,
        "sourceZone": "offset:7200",
        "timeDisplay": "sincePrevious",
        "wrapMode": 2
    },
    "run": {
        "all": false,
        "startOffset": 4096,
        "startTimestamp": 1700000000000
    },
    "schemaVersion": 1
}
)";

// The one thing in the corpus that cannot be a literal, because the KEY a record is filed
// under is absolute and a drive letter is prepended on Windows (CLAUDE.md's
// QDir::rootPath() rule). The shape either side of it is still verbatim.
QString poolAddress()
{
    return QDir::rootPath() + QStringLiteral("var/log/app.log");
}

QByteArray withAddress(const char *fixture)
{
    return QByteArray(fixture).replace("@ADDRESS@", poolAddress().toUtf8());
}

bool putPool(const QString &dir)
{
    QDir().mkpath(dir);
    QFile map(QDir(dir).filePath(QStringLiteral("map")));
    QFile rec(QDir(dir).filePath(QStringLiteral("0")));
    if (!map.open(QIODevice::WriteOnly) || !rec.open(QIODevice::WriteOnly))
        return false;
    return map.write(withAddress(kPoolMapV1)) > 0 && rec.write(withAddress(kPoolRecordV1)) > 0;
}

// ---------------------------------------------------------------------------------
// hosts.json (M11/M14).
// ---------------------------------------------------------------------------------
constexpr auto kHostsV1 = R"({
    "hosts": [
        {
            "auth": "key",
            "compress": true,
            "host": "web1.example.com",
            "keyFile": "/home/me/.ssh/id_prod",
            "label": "Prod",
            "paths": [
                "/var/log/app.log",
                "/var/log/sys.log"
            ],
            "pollMs": 2000,
            "port": 2222,
            "savePassword": false,
            "tailStartBytes": 65536,
            "user": "deploy"
        }
    ],
    "schemaVersion": 1
}
)";

#if defined(LOFTAIL_HAVE_PRESETS)
constexpr auto kFilterPresetsV1 = R"({
    "kind": "filters",
    "presets": {
        "Errors only": {
            "minPriority": 3
        }
    },
    "schemaVersion": 1
}
)";

constexpr auto kExportedPresetV1 = R"({
    "content": {
        "minPriority": 3
    },
    "kind": "filters",
    "name": "Errors only",
    "schemaVersion": 1
}
)";
#endif

// A file stamped one past whatever this build calls current. Built from the constant ON
// PURPOSE — unlike every fixture above — because "later than this build" is the one claim
// that has to track the constant rather than stand still.
QByteArray fromTheFuture(int current)
{
    QJsonObject root;
    root.insert(QStringLiteral("schemaVersion"), current + 1);
    root.insert(QStringLiteral("somethingThisBuildHasNeverHeardOf"), true);
    return QJsonDocument(root).toJson();
}

} // namespace

class TstSchema : public QObject
{
    Q_OBJECT

private slots:
    void theV1LogSettingsFileStillReadsAsItsAuthorMeantIt();
    void theV1PoolStillReadsAsItsAuthorMeantIt();
    void theV1HostsFileStillReadsAsItsAuthorMeantIt();
#if defined(LOFTAIL_HAVE_PRESETS)
    void theV1PresetFilesStillReadAsTheirAuthorMeantThem();
#endif

    void aFileFromTheFutureIsNeitherReadNorWrittenOver();
    void aRecordFromTheFutureKeepsItsSlotAndIsNotOverwritten();
    void aSessionFromTheFutureIsNotReplacedByThisBuildsTabs();
    void anUnstampedFileIsNotReadAsIfItWereCurrent();
    void aFileBehindThisBuildIsCopiedAsideBeforeItIsMigrated();
    void everyStoreAgreesOnWhatAStampMeans();
};

// ===================================================================================
// The corpus, read.
// ===================================================================================

void TstSchema::theV1LogSettingsFileStillReadsAsItsAuthorMeantIt()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    LogSettingsStore store(dir.path());
    QVERIFY(put(store.filePath(), kLogSettingsV1));

    const LogSettingsTree tree = store.load();
    QVERIFY(!store.readOnly());

    // ONE ASSERTION PER KEY THE FIXTURE CARRIES, and a value chosen so that losing the
    // key is visible: every field is set away from its default, because a fixture that
    // says `false` where the struct also defaults to `false` cannot tell a key that was
    // read from one that was dropped. Named one by one rather than compared against a
    // struct this build assembles, which would pass just as well if BOTH sides had lost
    // the field.
    const LogProfile d = tree.defaults();
    QCOMPARE(d.format.pattern, QStringLiteral("%d{%Y-%m-%d %H:%M:%S} [%t] %-5p %c - %m%n"));
    QCOMPARE(d.format.encoding, Encoding::Utf16LE);
    QCOMPARE(d.format.sourceZone.kind, ZoneChoice::Kind::FixedOffset);
    QCOMPARE(d.format.sourceZone.offsetSeconds, 7200);
    QCOMPARE(d.format.timeDisplay, TimeDisplay::SincePrevious);
    QCOMPARE(d.format.runStartPattern, QStringLiteral("=== START ==="));
    QVERIFY(d.format.runStartIsRegex);
    QVERIFY(d.format.runStartCaseSensitive);
    QCOMPARE(d.wrapMode, WrapMode::AlwaysOn);
    QCOMPARE(d.configPath, QStringLiteral("../conf/log4cplus.properties"));
    QCOMPARE(d.restartScript, QStringLiteral("systemctl restart audit"));

    QCOMPARE(tree.patterns().size(), 1);
    const LogPatternNode &n = tree.patterns().at(0);
    QCOMPARE(n.id, QStringLiteral("p1"));
    QCOMPARE(n.match, QStringLiteral(".*\\.audit\\.log$"));
    QCOMPARE(n.kind, LogPatternNode::Kind::Regex);
    QVERIFY(n.caseSensitive);
    QVERIFY(n.matchFullPath);
    QCOMPARE(n.profile.format.pattern, QStringLiteral("%d %p %m%n"));
    QCOMPARE(n.profile.configPath, QStringLiteral("../conf/log4cplus.properties"));
    QCOMPARE(n.profile.restartScript, QStringLiteral("systemctl restart audit"));
}

void TstSchema::theV1PoolStillReadsAsItsAuthorMeantIt()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(putPool(QDir(dir.path()).filePath(QStringLiteral("fileSettings"))));

    LogFileStore store(dir.path());
    store.load();
    QVERIFY(!store.readOnly());

    const LogFileSettings s = store.read(poolAddress());
    QCOMPARE(s.address, poolAddress());
    QVERIFY(s.profile.has_value());
    QCOMPARE(s.profile->format.pattern, QStringLiteral("%m%n"));
    QCOMPARE(s.profile->format.encoding, Encoding::Utf16LE);
    QCOMPARE(s.profile->format.sourceZone.kind, ZoneChoice::Kind::FixedOffset);
    QCOMPARE(s.profile->format.sourceZone.offsetSeconds, 7200);
    QCOMPARE(s.profile->format.timeDisplay, TimeDisplay::SincePrevious);
    QCOMPARE(s.profile->format.runStartPattern, QStringLiteral("=== START ==="));
    QVERIFY(s.profile->format.runStartIsRegex);
    QVERIFY(s.profile->format.runStartCaseSensitive);
    QCOMPARE(s.profile->wrapMode, WrapMode::AlwaysOn);
    QCOMPARE(s.profile->configPath, QStringLiteral("../conf/log4cplus.properties"));
    // The field a misread turns into somebody else's shell command. Named here as well as
    // in the pattern level above, because the two are read by different functions.
    QCOMPARE(s.profile->restartScript, QStringLiteral("systemctl restart audit"));

    // PRESENCE, NOT EMPTINESS — the stored empty rule list is the user having deleted
    // every rule, and a read that loses the distinction re-seeds the three level colours
    // on every launch. It is asserted here as well as in tst_logfilestore because THIS is
    // the copy that is reading bytes rather than its own output.
    QVERIFY(s.highlighters.has_value());
    QVERIFY(s.highlighters->isEmpty());

    QVERIFY(!s.run.all);
    QCOMPARE(s.run.startOffset, 4096);
    QCOMPARE(s.run.startTimestamp, 1700000000000LL);
}

void TstSchema::theV1HostsFileStillReadsAsItsAuthorMeantIt()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    HostBookmarkStore store(dir.path());
    QVERIFY(put(store.filePath(), kHostsV1));

    const QVector<HostBookmark> all = store.all();
    QCOMPARE(all.size(), 1);
    QCOMPARE(all.at(0).label, QStringLiteral("Prod"));
    QCOMPARE(all.at(0).host, QStringLiteral("web1.example.com"));
    QCOMPARE(all.at(0).user, QStringLiteral("deploy"));
    QCOMPARE(all.at(0).port, 2222);
    QCOMPARE(all.at(0).pollMs, 2000);
    QCOMPARE(all.at(0).auth, HostBookmark::Auth::KeyFile);
    QCOMPARE(all.at(0).keyFile, QStringLiteral("/home/me/.ssh/id_prod"));
    QCOMPARE(all.at(0).tailStartBytes, 65536);
    QVERIFY(all.at(0).compress);
    QVERIFY(!all.at(0).savePassword);
    QCOMPARE(all.at(0).paths,
             (QStringList{QStringLiteral("/var/log/app.log"),
                          QStringLiteral("/var/log/sys.log")}));
}

#if defined(LOFTAIL_HAVE_PRESETS)
void TstSchema::theV1PresetFilesStillReadAsTheirAuthorMeantThem()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    PresetStore store(dir.path());
    QVERIFY(put(QDir(dir.path()).filePath(QStringLiteral("filter-presets.json")),
                kFilterPresetsV1));

    QCOMPARE(store.names(PresetStore::Kind::Filters),
             QStringList{QStringLiteral("Errors only")});
    QCOMPARE(store.preset(PresetStore::Kind::Filters, QStringLiteral("Errors only"))
                 .value(QStringLiteral("minPriority"))
                 .toInt(),
             3);

    // An EXPORTED preset is the one file here that travels between installations, so it
    // is the one most likely to arrive stamped below the build reading it.
    const QString exported = QDir(dir.path()).filePath(QStringLiteral("shared.json"));
    QVERIFY(put(exported, kExportedPresetV1));
    PresetStore::Kind kind{};
    QString name;
    QVERIFY(store.importPreset(exported, &kind, &name));
    QCOMPARE(kind, PresetStore::Kind::Filters);
    QCOMPARE(name, QStringLiteral("Errors only"));
}
#endif

// ===================================================================================
// A file this build must not touch.
// ===================================================================================

void TstSchema::aFileFromTheFutureIsNeitherReadNorWrittenOver()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // logsettings.json already had this and keeps it; the two below did NOT, and each
    // would have discarded the file and then written the discard back over it.
    {
        LogSettingsStore store(dir.path());
        const QByteArray before = fromTheFuture(LogSettingsStore::kSchemaVersion);
        QVERIFY(put(store.filePath(), before.constData()));
        const LogSettingsTree tree = store.load();
        QVERIFY(store.readOnly());
        QCOMPARE(tree.patterns().size(), 0);
        QVERIFY(!store.save(tree));
        QCOMPARE(readObject(store.filePath()).value(QStringLiteral("schemaVersion")).toInt(),
                 LogSettingsStore::kSchemaVersion + 1);
    }

    {
        HostBookmarkStore store(dir.path());
        const QByteArray before = fromTheFuture(HostBookmarkStore::kSchemaVersion);
        QVERIFY(put(store.filePath(), before.constData()));
        QVERIFY(store.all().isEmpty());

        // THE HALF THAT WAS MISSING. Reading nothing is harmless on its own; it is the
        // next save that does the damage, because this store is stateless and every
        // mutation is read-fold-write.
        HostBookmark b;
        b.host = QStringLiteral("new.example.com");
        QVERIFY(!store.save(b));
        QVERIFY(!store.replaceAll({}));
        // remove() answers TRUE here and writes nothing: it reads the list, finds no such
        // name in it and returns "already gone" without reaching the write funnel at all.
        // Asserted as the file being untouched rather than as the return value, which is
        // about the name and not about the version.
        QVERIFY(store.remove(QStringLiteral("new.example.com")));
        const QJsonObject after = readObject(store.filePath());
        QCOMPARE(after.value(QStringLiteral("schemaVersion")).toInt(),
                 HostBookmarkStore::kSchemaVersion + 1);
        QVERIFY(after.contains(QStringLiteral("somethingThisBuildHasNeverHeardOf")));
    }

#if defined(LOFTAIL_HAVE_PRESETS)
    {
        PresetStore store(dir.path());
        const QString path = QDir(dir.path()).filePath(QStringLiteral("filter-presets.json"));
        const QByteArray before = fromTheFuture(PresetStore::kSchemaVersion);
        QVERIFY(put(path, before.constData()));
        QVERIFY(store.names(PresetStore::Kind::Filters).isEmpty());
        QVERIFY(!store.save(PresetStore::Kind::Filters, QStringLiteral("x"), QJsonObject()));
        QCOMPARE(readObject(path).value(QStringLiteral("schemaVersion")).toInt(),
                 PresetStore::kSchemaVersion + 1);
    }
#endif
}

void TstSchema::aRecordFromTheFutureKeepsItsSlotAndIsNotOverwritten()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pool = QDir(dir.path()).filePath(QStringLiteral("fileSettings"));
    QVERIFY(putPool(pool));

    // The MAP stays at this build's version — the point is that the two stamps are
    // independent, so a record a newer build rewrote in place is not announced by the
    // index at all.
    QJsonObject record = readObject(QDir(pool).filePath(QStringLiteral("0")));
    record.insert(QStringLiteral("schemaVersion"), LogFileSettings::kSchemaVersion + 1);
    QVERIFY(put(QDir(pool).filePath(QStringLiteral("0")),
                QJsonDocument(record).toJson().constData()));

    LogFileStore store(dir.path());
    store.load();

    // Read as nothing: there is no reading this build can defend.
    const LogFileSettings s = store.read(poolAddress());
    QVERIFY(!s.profile.has_value());
    QVERIFY(!s.highlighters.has_value());

    // AND THE MAP ENTRY SURVIVES, which is the half that distinguishes this from a stale
    // slot. Dropping it would unreference the file and free the slot for the next log to
    // overwrite — a newer build's configuration destroyed by an older one that could not
    // read it.
    QVERIFY(!readObject(QDir(pool).filePath(QStringLiteral("map")))
                 .value(QStringLiteral("entries"))
                 .toArray()
                 .isEmpty());

    LogFileSettings edit;
    edit.address = poolAddress();
    LogProfile p = LogProfile::builtIn();
    p.format.pattern = QStringLiteral("%p %m%n");
    edit.profile = p;
    QVERIFY(!store.save(edit, LogProfile::builtIn()));

    QCOMPARE(readObject(QDir(pool).filePath(QStringLiteral("0")))
                 .value(QStringLiteral("schemaVersion"))
                 .toInt(),
             LogFileSettings::kSchemaVersion + 1);
}

void TstSchema::aSessionFromTheFutureIsNotReplacedByThisBuildsTabs()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("session.ini"));

    {
        QSettings s(path, QSettings::IniFormat);
        s.setValue(QStringLiteral("session/schemaVersion"), SessionStore::kSchemaVersion + 1);
        s.setValue(QStringLiteral("session/geometry"), QByteArray("later"));
        s.sync();
    }

    QSettings s(path, QSettings::IniFormat);
    const Session loaded = SessionStore::load(s);
    QVERIFY(loaded.documents.isEmpty());

    Session mine;
    mine.geometry = QByteArray("mine");
    SessionStore::save(s, mine);
    s.sync();

    QSettings again(path, QSettings::IniFormat);
    QCOMPARE(again.value(QStringLiteral("session/schemaVersion")).toInt(),
             SessionStore::kSchemaVersion + 1);
    QCOMPARE(again.value(QStringLiteral("session/geometry")).toByteArray(),
             QByteArray("later"));
}

void TstSchema::anUnstampedFileIsNotReadAsIfItWereCurrent()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // A file that parses as JSON and says nothing about its version is damaged or
    // foreign, never somebody's current configuration — so it is read as nothing, and,
    // unlike a file from the future, it is NOT protected from being written over. Standing
    // off it would leave the store unable to write again after one hand edit.
    LogSettingsStore store(dir.path());
    QVERIFY(put(store.filePath(), R"({"patterns": [{"match": "*.log", "id": "x"}]})"));
    const LogSettingsTree tree = store.load();
    QCOMPARE(tree.patterns().size(), 0);
    QVERIFY(!store.readOnly());
    QVERIFY(store.save(tree));
    QCOMPARE(readObject(store.filePath()).value(QStringLiteral("schemaVersion")).toInt(),
             LogSettingsStore::kSchemaVersion);
}

void TstSchema::aFileBehindThisBuildIsCopiedAsideBeforeItIsMigrated()
{
    // Nothing in the tree is behind yet — every store is at v1 — so the claim is made
    // against the helper itself, over a real file. It is here rather than in a unit test
    // of its own because what it protects is the corpus above: the day a fixture here is
    // joined by a v2, this is the case that says the v1 file was kept.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("logsettings.json"));
    QVERIFY(put(path, kLogSettingsV1));

    QVERIFY(Schema::backupOnce(path, 1));
    const QString backup = Schema::backupPathFor(path, 1);
    QVERIFY(QFile::exists(backup));
    QCOMPARE(readObject(backup).value(QStringLiteral("schemaVersion")).toInt(), 1);

    // ONCE, AND EXISTING WINS. A second launch finding the same old file must not copy
    // over a backup that has since been the only good copy.
    QVERIFY(put(path, R"({"schemaVersion": 1, "patterns": []})"));
    QVERIFY(Schema::backupOnce(path, 1));
    QVERIFY(!readObject(backup).value(QStringLiteral("patterns")).toArray().isEmpty());
}

void TstSchema::everyStoreAgreesOnWhatAStampMeans()
{
    // The vocabulary itself, stated once. Two stores used to answer `!=` here, which
    // folds FromFuture together with Unstamped — and those want opposite treatment.
    QCOMPARE(Schema::judge(1, 1), Schema::Verdict::Usable);
    QCOMPARE(Schema::judge(1, 4), Schema::Verdict::Usable);
    QCOMPARE(Schema::judge(4, 4), Schema::Verdict::Usable);
    QCOMPARE(Schema::judge(5, 4), Schema::Verdict::FromFuture);
    QCOMPARE(Schema::judge(0, 1), Schema::Verdict::Unstamped);
    QCOMPARE(Schema::judge(-1, 1), Schema::Verdict::Unstamped);

    QJsonObject stringly;
    stringly.insert(QStringLiteral("schemaVersion"), QStringLiteral("1"));
    QCOMPARE(Schema::judge(stringly, 1), Schema::Verdict::Unstamped);
    QCOMPARE(Schema::judge(QJsonObject(), 1), Schema::Verdict::Unstamped);

    // EVERY STORE'S CURRENT VERSION READS ITS OWN FIXTURE. Stated as a loop over the
    // constants rather than left implicit, because a bump with no fixture beside it is
    // the omission this whole file exists to catch, and the corpus above can only catch
    // it for a store somebody remembered to add a case for.
    QVERIFY(LogSettingsStore::kSchemaVersion >= 1);
    QVERIFY(LogFileStore::kSchemaVersion >= 1);
    QVERIFY(LogFileSettings::kSchemaVersion >= 1);
    QVERIFY(HostBookmarkStore::kSchemaVersion >= 1);
    QVERIFY(SessionStore::kSchemaVersion >= 1);
}

QTEST_MAIN(TstSchema)
#include "tst_schema.moc"
