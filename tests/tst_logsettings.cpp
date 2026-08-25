#include <QtTest>

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>

#include "LogSettings.h"
#include "LogSettingsStore.h"
#include "RemoteLocation.h"

using namespace loftail;

// M20 — the settings TREE (SPEC.md §4, ARCHITECTURE.md §8): defaults, an ordered list
// of file patterns, and per-file nodes, with the deepest match winning WHOLE. Core
// only; no QApplication, no widgets.
class TestLogSettings : public QObject
{
    Q_OBJECT

private slots:
    void theRootAnswersForAFileNoPatternMatches();
    void theFirstMatchingPatternWins();
    void aWildcardMatchesNameAndExtensionOnly();
    void aFullPathPatternSeesTheWholeAddress();
    void aWildcardStarCrossesPathSeparators();
    void caseSensitivityIsPerPattern();
    void anInvalidRegexNeverMatches();
    void aRemoteAddressMatchesOnItsFileName();
    void anArchiveMemberMatchesOnTheMemberName();
    void deletingAPatternReHomesItsFiles();
    void theTreeRoundTripsThroughJson();
    void aLegacyFileLevelIsDrainedOnceAndThenNoLongerWritten();
    void anEmptyPatternIsStillAnAnswer();
    void legacyStoresMigrateOnceAndAreRemoved();
    void aNewerSchemaLoadsEmptyAndRefusesToSave();
    void aProfileDiffersWhenAnyOneFieldOfItDoes();
    void aRestartScriptRoundTripsThroughJsonIncludingItsNewlines();
    void aProfileStoredBeforeRestartScriptsExistedReadsAsNotConfigured();
};

namespace {

LogPatternNode wildcard(const QString &match, const QString &pattern)
{
    LogPatternNode n;
    n.kind = LogPatternNode::Kind::Wildcard;
    n.match = match;
    n.profile.format.pattern = pattern;
    return n;
}

// A tree rooted in a distinguishable default rather than the built-in, so a test that
// falls through to the root cannot pass by accident.
LogSettingsTree treeWithRoot(const QString &rootPattern)
{
    LogSettingsTree t;
    LogProfile p;
    p.format.pattern = rootPattern;
    t.setDefaults(p);
    return t;
}

// The file on disk, for the assertions that are about what was WRITTEN rather than about
// what load() makes of it.
QByteArray readAll(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

} // namespace

void TestLogSettings::theRootAnswersForAFileNoPatternMatches()
{
    LogSettingsTree t = treeWithRoot(QStringLiteral("ROOT"));
    t.addPattern(wildcard(QStringLiteral("*.audit"), QStringLiteral("AUDIT")));

    QCOMPARE(t.inherited(QStringLiteral("/var/log/app.log")).format.pattern,
             QStringLiteral("ROOT"));
    // -1 is what "no pattern matched" is asked with, and the whole of what a log with no
    // pattern over it means: the defaults are the level it takes, not a level it can be
    // promoted to.
    QCOMPARE(t.matchingPattern(QStringLiteral("/var/log/app.log")), -1);
}

void TestLogSettings::theFirstMatchingPatternWins()
{
    LogSettingsTree t = treeWithRoot(QStringLiteral("ROOT"));
    t.addPattern(wildcard(QStringLiteral("app.*"), QStringLiteral("FIRST")));
    t.addPattern(wildcard(QStringLiteral("*.log"), QStringLiteral("SECOND")));

    QCOMPARE(t.inherited(QStringLiteral("/var/log/app.log")).format.pattern,
             QStringLiteral("FIRST"));

    // Order is the only tie-break there is, so reordering changes the answer.
    t.movePattern(0, 1);
    QCOMPARE(t.inherited(QStringLiteral("/var/log/app.log")).format.pattern,
             QStringLiteral("SECOND"));
}

void TestLogSettings::aWildcardMatchesNameAndExtensionOnly()
{
    LogSettingsTree t = treeWithRoot(QStringLiteral("ROOT"));
    t.addPattern(wildcard(QStringLiteral("*.log"), QStringLiteral("BY-NAME")));

    QCOMPARE(t.inherited(QStringLiteral("/var/log/app.log")).format.pattern,
             QStringLiteral("BY-NAME"));
    QCOMPARE(t.inherited(QStringLiteral("/somewhere/else/other.log")).format.pattern,
             QStringLiteral("BY-NAME"));

    // The directory is not part of what is matched by default, so a pattern naming one
    // matches nothing at all.
    t.patternAt(0).match = QStringLiteral("/var/log/*");
    QCOMPARE(t.inherited(QStringLiteral("/var/log/app.log")).format.pattern,
             QStringLiteral("ROOT"));
}

void TestLogSettings::aFullPathPatternSeesTheWholeAddress()
{
    LogSettingsTree t = treeWithRoot(QStringLiteral("ROOT"));
    LogPatternNode n = wildcard(QStringLiteral("/var/log/*"), QStringLiteral("BY-PATH"));
    n.matchFullPath = true;
    t.addPattern(n);

    QCOMPARE(t.inherited(QStringLiteral("/var/log/app.log")).format.pattern,
             QStringLiteral("BY-PATH"));
    QCOMPARE(t.inherited(QStringLiteral("/opt/app.log")).format.pattern,
             QStringLiteral("ROOT"));
}

void TestLogSettings::aWildcardStarCrossesPathSeparators()
{
    // Deliberately NOT QRegularExpression::wildcardToRegularExpression()'s behaviour,
    // whose `*` stops at a separator and whose opt-out is Qt 6.6 — above the floor.
    LogSettingsTree t = treeWithRoot(QStringLiteral("ROOT"));
    LogPatternNode n = wildcard(QStringLiteral("*log*app.log"), QStringLiteral("CROSSED"));
    n.matchFullPath = true;
    t.addPattern(n);

    QCOMPARE(t.inherited(QStringLiteral("/var/log/nested/app.log")).format.pattern,
             QStringLiteral("CROSSED"));
}

void TestLogSettings::caseSensitivityIsPerPattern()
{
    LogSettingsTree t = treeWithRoot(QStringLiteral("ROOT"));
    t.addPattern(wildcard(QStringLiteral("APP.log"), QStringLiteral("MATCHED")));

    // Insensitive by default.
    QCOMPARE(t.inherited(QStringLiteral("/var/log/app.log")).format.pattern,
             QStringLiteral("MATCHED"));

    t.patternAt(0).caseSensitive = true;
    QCOMPARE(t.inherited(QStringLiteral("/var/log/app.log")).format.pattern,
             QStringLiteral("ROOT"));
    QCOMPARE(t.inherited(QStringLiteral("/var/log/APP.log")).format.pattern,
             QStringLiteral("MATCHED"));
}

void TestLogSettings::anInvalidRegexNeverMatches()
{
    LogSettingsTree t = treeWithRoot(QStringLiteral("ROOT"));
    LogPatternNode n = wildcard(QStringLiteral("app("), QStringLiteral("BROKEN"));
    n.kind = LogPatternNode::Kind::Regex;
    t.addPattern(n);

    // A half-typed pattern in the dialog must not start claiming files.
    QCOMPARE(t.inherited(QStringLiteral("/var/log/app.log")).format.pattern,
             QStringLiteral("ROOT"));

    // And a valid one matches UNANCHORED — it is a search, not a whole-name test.
    t.patternAt(0).match = QStringLiteral("app-\\d+");
    QCOMPARE(t.inherited(QStringLiteral("/var/log/app-42.log")).format.pattern,
             QStringLiteral("BROKEN"));
}

void TestLogSettings::aRemoteAddressMatchesOnItsFileName()
{
    LogSettingsTree t = treeWithRoot(QStringLiteral("ROOT"));
    t.addPattern(wildcard(QStringLiteral("*.log"), QStringLiteral("BY-NAME")));

    const QString remote = QStringLiteral("ssh://deploy@web1/var/log/app.log");
    QCOMPARE(logMatchTarget(remote, false), QStringLiteral("app.log"));
    QCOMPARE(t.inherited(remote).format.pattern, QStringLiteral("BY-NAME"));

    // The full-path form keeps the whole normal-form address, port and all.
    QVERIFY(logMatchTarget(remote, true).startsWith(QLatin1String("ssh://deploy@web1:22/")));
}

void TestLogSettings::anArchiveMemberMatchesOnTheMemberName()
{
    LogSettingsTree t = treeWithRoot(QStringLiteral("ROOT"));
    t.addPattern(wildcard(QStringLiteral("*.log"), QStringLiteral("BY-NAME")));

    // The container is transport, exactly as SSH is; the log is called what the writer
    // called it.
    QCOMPARE(logMatchTarget(QStringLiteral("/logs/bundle.tar.gz/var/log/app.log"), false),
             QStringLiteral("app.log"));
    QCOMPARE(t.inherited(QStringLiteral("/logs/bundle.tar.gz/var/log/app.log"))
                 .format.pattern,
             QStringLiteral("BY-NAME"));

    // A bare compressed stream is the log with its suffix taken off.
    QCOMPARE(logMatchTarget(QStringLiteral("/logs/app.log.gz"), false),
             QStringLiteral("app.log"));
    QCOMPARE(t.inherited(QStringLiteral("/logs/app.log.gz")).format.pattern,
             QStringLiteral("BY-NAME"));
}





void TestLogSettings::deletingAPatternReHomesItsFiles()
{
    LogSettingsTree t = treeWithRoot(QStringLiteral("ROOT"));
    t.addPattern(wildcard(QStringLiteral("app.*"), QStringLiteral("FIRST")));
    t.addPattern(wildcard(QStringLiteral("*.log"), QStringLiteral("SECOND")));

    QCOMPARE(t.matchingPattern(QStringLiteral("/var/log/app.log")), 0);
    QCOMPARE(t.inherited(QStringLiteral("/var/log/app.log")).format.pattern,
             QStringLiteral("FIRST"));

    // Nothing stores a parent link, so a log simply re-homes under whatever matches next
    // — no dangling reference to clean up. What that costs is the pool sweep
    // (LogFileStore::pruneAgainst): a record that agreed with the pattern just deleted
    // now agrees with a different one, and nothing writes it.
    t.removePattern(0);
    QCOMPARE(t.matchingPattern(QStringLiteral("/var/log/app.log")), 0); // now the *.log one
    QCOMPARE(t.patternAt(0).match, QStringLiteral("*.log"));
    QCOMPARE(t.inherited(QStringLiteral("/var/log/app.log")).format.pattern,
             QStringLiteral("SECOND"));
}

void TestLogSettings::theTreeRoundTripsThroughJson()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    LogSettingsTree t = treeWithRoot(QStringLiteral("ROOT"));

    LogPatternNode n = wildcard(QStringLiteral("*.audit"), QStringLiteral("AUDIT"));
    n.kind = LogPatternNode::Kind::Regex;
    n.match = QStringLiteral("^audit-");
    n.caseSensitive = true;
    n.matchFullPath = true;
    n.profile.format.encoding = Encoding::Utf16LE;
    n.profile.format.sourceZone = ZoneChoice{ZoneChoice::Kind::FixedOffset, -5 * 3600};
    n.profile.format.timeDisplay = TimeDisplay::RunSeconds;
    n.profile.format.runStartPattern = QStringLiteral("=== BOOT ===");
    n.profile.format.runStartIsRegex = true;
    n.profile.format.runStartCaseSensitive = true;
    n.profile.wrapMode = WrapMode::AlwaysOn;
    n.profile.configPath = QStringLiteral("../conf/log4cplus.properties");
    t.addPattern(n);

    LogSettingsStore store(dir.path());
    QVERIFY(store.save(t));

    LogSettingsStore reader(dir.path());
    const LogSettingsTree back = reader.load();
    QVERIFY(!reader.readOnly());
    QCOMPARE(back.defaults().format.pattern, QStringLiteral("ROOT"));
    QCOMPARE(back.patterns().size(), 1);

    const LogPatternNode &p = back.patterns().at(0);
    QCOMPARE(p.id, t.patterns().at(0).id);
    QVERIFY(p.kind == LogPatternNode::Kind::Regex);
    QCOMPARE(p.match, QStringLiteral("^audit-"));
    QVERIFY(p.caseSensitive);
    QVERIFY(p.matchFullPath);
    QVERIFY(p.profile.format.encoding == Encoding::Utf16LE);
    QVERIFY(p.profile.format.sourceZone == n.profile.format.sourceZone);
    QVERIFY(p.profile.format.timeDisplay == TimeDisplay::RunSeconds);
    QCOMPARE(p.profile.format.runStartPattern, QStringLiteral("=== BOOT ==="));
    QVERIFY(p.profile.format.runStartIsRegex);
    QVERIFY(p.profile.format.runStartCaseSensitive);
    QVERIFY(p.profile.wrapMode == WrapMode::AlwaysOn);
    QCOMPARE(p.profile.configPath, QStringLiteral("../conf/log4cplus.properties"));

    // NOTHING ABOUT ONE CONCRETE LOG is written here any more (M21): what survives is the
    // pair of INHERITED levels, and a log's own settings are one record per log in the
    // pool. The round trip of a per-log record, including that an `ssh://` address and
    // its `:22` spelling are one of them, is tst_logfilestore's.
    QVERIFY(!QJsonDocument::fromJson(readAll(store.filePath()))
                 .object()
                 .contains(QStringLiteral("files")));
}

// The other half of that removal. A file written by an older build still carries
// `files[]`, and those entries are somebody's whole per-log format configuration — so
// they are read one last time, handed over, and then gone from this file for good. Being
// gone is what makes the migration once-only, with no flag anywhere to remember it by.
void TestLogSettings::aLegacyFileLevelIsDrainedOnceAndThenNoLongerWritten()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // Hand-written in the shape a pre-M21 build wrote, since nothing can produce it now.
    QJsonObject profile;
    profile.insert(QStringLiteral("pattern"), QStringLiteral("MINE"));
    QJsonObject node;
    node.insert(QStringLiteral("path"), QStringLiteral("/var/log/app.log"));
    node.insert(QStringLiteral("profile"), profile);
    QJsonObject root;
    root.insert(QStringLiteral("schemaVersion"), LogSettingsStore::kSchemaVersion);
    root.insert(QStringLiteral("files"), QJsonArray{node});

    LogSettingsStore store(dir.path());
    QFile out(store.filePath());
    QVERIFY(out.open(QIODevice::WriteOnly));
    out.write(QJsonDocument(root).toJson());
    out.close();

    const LogSettingsTree tree = store.load();
    const auto legacy = store.takeLegacyFiles();
    QCOMPARE(legacy.size(), 1);
    QCOMPARE(legacy.at(0).path, QStringLiteral("/var/log/app.log"));
    QCOMPARE(legacy.at(0).profile.format.pattern, QStringLiteral("MINE"));
    // A TAKE, because it is a drain: the caller has adopted them, so asking again yields
    // nothing and a second adoption cannot duplicate what the first stored.
    QVERIFY(store.takeLegacyFiles().isEmpty());

    // Rewriting the file is what closes the drain — nothing else records that it ran.
    QVERIFY(store.save(tree));
    LogSettingsStore reader(dir.path());
    reader.load();
    QVERIFY(reader.takeLegacyFiles().isEmpty());
}


void TestLogSettings::anEmptyPatternIsStillAnAnswer()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // "Ask me about every log" — an empty pattern parses nothing, so every file it
    // applies to reaches the format dialog. Reading empty as "nothing saved" would
    // reinstate the built-in and make that setting unreachable.
    LogSettingsTree t;
    LogProfile blank;
    blank.format.pattern.clear();
    t.setDefaults(blank);

    LogSettingsStore store(dir.path());
    QVERIFY(store.save(t));

    LogSettingsStore reader(dir.path());
    QVERIFY(reader.load().defaults().format.pattern.isEmpty());
}

void TestLogSettings::legacyStoresMigrateOnceAndAreRemoved()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString ini = QDir(dir.path()).filePath(QStringLiteral("legacy.ini"));

    {
        QSettings s(ini, QSettings::IniFormat);
        s.beginGroup(QStringLiteral("defaultFormat"));
        s.setValue(QStringLiteral("pattern"), QStringLiteral("OLD-DEFAULT"));
        s.setValue(QStringLiteral("encoding"), uint(Encoding::Utf8));
        s.endGroup();

        s.beginWriteArray(QStringLiteral("formatCache"), 1);
        s.setArrayIndex(0);
        s.setValue(QStringLiteral("path"), QStringLiteral("/var/log/app.log"));
        s.setValue(QStringLiteral("pattern"), QStringLiteral("OLD-FILE"));
        // The pre-M-whatever key for the display axis; it must still read.
        s.setValue(QStringLiteral("displayZone"), QStringLiteral("utc"));
        s.endArray();
        s.sync();
    }

    LogSettingsStore store(dir.path());
    QSettings s(ini, QSettings::IniFormat);
    QVERIFY(store.migrateLegacy(s));

    const LogSettingsTree t = store.load();
    QCOMPARE(t.defaults().format.pattern, QStringLiteral("OLD-DEFAULT"));
    QVERIFY(t.defaults().format.encoding == Encoding::Utf8);

    // The PER-LOG half goes to the drain rather than into the tree (M21) — through the
    // same takeLegacyFiles() that carries M20's `files[]`, so two upgrade paths reach the
    // pool by one route. The old display-zone key must still read on the way.
    const auto legacy = store.takeLegacyFiles();
    QCOMPARE(legacy.size(), 1);
    QCOMPARE(legacy.at(0).path, QStringLiteral("/var/log/app.log"));
    QCOMPARE(legacy.at(0).profile.format.pattern, QStringLiteral("OLD-FILE"));
    QVERIFY(legacy.at(0).profile.format.timeDisplay == TimeDisplay::Utc);

    // One home for a setting, not two that can disagree.
    QSettings check(ini, QSettings::IniFormat);
    QVERIFY(!check.childGroups().contains(QStringLiteral("defaultFormat")));
    check.beginGroup(QStringLiteral("defaultFormat"));
    QVERIFY(!check.contains(QStringLiteral("pattern")));
    check.endGroup();
    QCOMPARE(check.beginReadArray(QStringLiteral("formatCache")), 0);
    check.endArray();

    // And it does not run a second time over a file that is already there.
    QVERIFY(!store.migrateLegacy(check));
}

void TestLogSettings::aNewerSchemaLoadsEmptyAndRefusesToSave()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    QJsonObject root;
    root.insert(QStringLiteral("schemaVersion"), LogSettingsStore::kSchemaVersion + 1);
    QFile f(QDir(dir.path()).filePath(QStringLiteral("logsettings.json")));
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(QJsonDocument(root).toJson());
    f.close();

    LogSettingsStore store(dir.path());
    const LogSettingsTree t = store.load();
    QVERIFY(store.readOnly());
    // Nothing was read, and — the point — nothing may be written back over it: running
    // an older build for one session must not discard a newer one's configuration.
    QCOMPARE(t.patterns().size(), 0);
    QVERIFY(!store.save(t));

    QVERIFY(f.open(QIODevice::ReadOnly));
    const QJsonObject still = QJsonDocument::fromJson(f.readAll()).object();
    QCOMPARE(still.value(QStringLiteral("schemaVersion")).toInt(),
             LogSettingsStore::kSchemaVersion + 1);
}

// One case per LogProfile field, in the shape of
// tst_highlight::aRuleDiffersWhenAnyOneFieldOfItDoes and for the same reason.
//
// LogProfile::operator== is not a convenience: LogFileSettings::reduce() deletes a
// per-log entry whose profile equals what the log inherits, so a field the comparison
// does not look at is a field two profiles can never differ in — the entry is dropped on
// the next write and the setting vanishes with nothing on screen. A field added to the
// struct and not to the comparison is therefore SILENT DATA LOSS, and it is invisible to
// the round-trip case above, which only ever compares fields it was told about.
void TestLogSettings::aProfileDiffersWhenAnyOneFieldOfItDoes()
{
    const LogProfile base = LogProfile::builtIn();

    const auto differsFrom = [&base](auto &&mutate) {
        LogProfile other = base;
        mutate(other);
        return other != base && !(other == base);
    };

    QVERIFY(differsFrom([](LogProfile &p) { p.format.pattern = QStringLiteral("%m%n"); }));
    QVERIFY(differsFrom([](LogProfile &p) { p.format.encoding = Encoding::Utf16BE; }));
    QVERIFY(differsFrom([](LogProfile &p) {
        p.format.sourceZone = ZoneChoice{ZoneChoice::Kind::Utc, 0};
    }));
    QVERIFY(differsFrom([](LogProfile &p) { p.format.timeDisplay = TimeDisplay::Utc; }));
    QVERIFY(differsFrom([](LogProfile &p) { p.format.runStartPattern = QStringLiteral("BOOT"); }));
    QVERIFY(differsFrom([](LogProfile &p) { p.format.runStartIsRegex = true; }));
    QVERIFY(differsFrom([](LogProfile &p) { p.format.runStartCaseSensitive = true; }));
    QVERIFY(differsFrom([](LogProfile &p) { p.wrapMode = WrapMode::AlwaysOn; }));
    QVERIFY(differsFrom([](LogProfile &p) { p.configPath = QStringLiteral("x.properties"); }));
    QVERIFY(differsFrom(
        [](LogProfile &p) { p.restartScript = QStringLiteral("systemctl restart app"); }));

    // And the other direction, so the comparison cannot be satisfied by always
    // answering "different": two profiles built the same way are equal, which is what
    // reduce() relies on to delete an entry that says nothing of its own.
    QVERIFY(LogProfile::builtIn() == base);
}

void TestLogSettings::aRestartScriptRoundTripsThroughJsonIncludingItsNewlines()
{
    // A restart script is the first MULTI-LINE value either store holds. JSON carries a
    // newline as \n with no encoding of its own, but that is worth an assertion rather
    // than an assumption: a script that came back as one line would run its first command
    // and silently drop the rest.
    LogProfile p = LogProfile::builtIn();
    p.restartScript = QStringLiteral("#!/bin/sh\nset -e\n\nsystemctl restart app\n"
                                     "echo \"done $LOGFILE\"");

    const LogProfile back = logProfileFromJson(logProfileToJson(p));
    QCOMPARE(back.restartScript, p.restartScript);
    QCOMPARE(back, p);
}

void TestLogSettings::aProfileStoredBeforeRestartScriptsExistedReadsAsNotConfigured()
{
    // THE ASSERTION THE "NO SCHEMA BUMP" ARGUMENT RESTS ON. An added key is exactly what
    // a backward read handles: an older file simply has none, and the struct default —
    // empty, meaning "not configured" — is the correct answer for it. Bumping instead
    // would make an older binary refuse to write the file AT ALL, freezing every setting
    // for every log rather than losing one field.
    QJsonObject old = logProfileToJson(LogProfile::builtIn());
    old.remove(QStringLiteral("restartScript"));
    QVERIFY(!old.contains(QStringLiteral("restartScript")));

    const LogProfile back = logProfileFromJson(old);
    QVERIFY(back.restartScript.isEmpty());
    QCOMPARE(back, LogProfile::builtIn());
}

QTEST_APPLESS_MAIN(TestLogSettings)
#include "tst_logsettings.moc"
