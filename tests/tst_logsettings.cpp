#include <QtTest>

#include <QDir>
#include <QFile>
#include <QJsonDocument>
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
    void aFileNodeOutranksItsPattern();
    void aProfileEqualToWhatItInheritsStoresNoFileNode();
    void bringingAFileBackIntoLineRemovesItsNode();
    void deletingAPatternReHomesItsFiles();
    void theTreeRoundTripsThroughJson();
    void aLoadedFileNodeIsKeptEvenWhenItMatchesItsParent();
    void anEmptyPatternIsStillAnAnswer();
    void legacyStoresMigrateOnceAndAreRemoved();
    void aNewerSchemaLoadsEmptyAndRefusesToSave();
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

} // namespace

void TestLogSettings::theRootAnswersForAFileNoPatternMatches()
{
    LogSettingsTree t = treeWithRoot(QStringLiteral("ROOT"));
    t.addPattern(wildcard(QStringLiteral("*.audit"), QStringLiteral("AUDIT")));

    const auto r = t.resolve(QStringLiteral("/var/log/app.log"));
    QCOMPARE(r.profile.format.pattern, QStringLiteral("ROOT"));
    QCOMPARE(r.patternIndex, -1);
    QCOMPARE(r.fileIndex, -1);
    QVERIFY(!r.fromNode()); // this is what "no pattern matched" is asked with
}

void TestLogSettings::theFirstMatchingPatternWins()
{
    LogSettingsTree t = treeWithRoot(QStringLiteral("ROOT"));
    t.addPattern(wildcard(QStringLiteral("app.*"), QStringLiteral("FIRST")));
    t.addPattern(wildcard(QStringLiteral("*.log"), QStringLiteral("SECOND")));

    QCOMPARE(t.resolve(QStringLiteral("/var/log/app.log")).profile.format.pattern,
             QStringLiteral("FIRST"));

    // Order is the only tie-break there is, so reordering changes the answer.
    t.movePattern(0, 1);
    QCOMPARE(t.resolve(QStringLiteral("/var/log/app.log")).profile.format.pattern,
             QStringLiteral("SECOND"));
}

void TestLogSettings::aWildcardMatchesNameAndExtensionOnly()
{
    LogSettingsTree t = treeWithRoot(QStringLiteral("ROOT"));
    t.addPattern(wildcard(QStringLiteral("*.log"), QStringLiteral("BY-NAME")));

    QCOMPARE(t.resolve(QStringLiteral("/var/log/app.log")).profile.format.pattern,
             QStringLiteral("BY-NAME"));
    QCOMPARE(t.resolve(QStringLiteral("/somewhere/else/other.log")).profile.format.pattern,
             QStringLiteral("BY-NAME"));

    // The directory is not part of what is matched by default, so a pattern naming one
    // matches nothing at all.
    t.patternAt(0).match = QStringLiteral("/var/log/*");
    QCOMPARE(t.resolve(QStringLiteral("/var/log/app.log")).profile.format.pattern,
             QStringLiteral("ROOT"));
}

void TestLogSettings::aFullPathPatternSeesTheWholeAddress()
{
    LogSettingsTree t = treeWithRoot(QStringLiteral("ROOT"));
    LogPatternNode n = wildcard(QStringLiteral("/var/log/*"), QStringLiteral("BY-PATH"));
    n.matchFullPath = true;
    t.addPattern(n);

    QCOMPARE(t.resolve(QStringLiteral("/var/log/app.log")).profile.format.pattern,
             QStringLiteral("BY-PATH"));
    QCOMPARE(t.resolve(QStringLiteral("/opt/app.log")).profile.format.pattern,
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

    QCOMPARE(t.resolve(QStringLiteral("/var/log/nested/app.log")).profile.format.pattern,
             QStringLiteral("CROSSED"));
}

void TestLogSettings::caseSensitivityIsPerPattern()
{
    LogSettingsTree t = treeWithRoot(QStringLiteral("ROOT"));
    t.addPattern(wildcard(QStringLiteral("APP.log"), QStringLiteral("MATCHED")));

    // Insensitive by default.
    QCOMPARE(t.resolve(QStringLiteral("/var/log/app.log")).profile.format.pattern,
             QStringLiteral("MATCHED"));

    t.patternAt(0).caseSensitive = true;
    QCOMPARE(t.resolve(QStringLiteral("/var/log/app.log")).profile.format.pattern,
             QStringLiteral("ROOT"));
    QCOMPARE(t.resolve(QStringLiteral("/var/log/APP.log")).profile.format.pattern,
             QStringLiteral("MATCHED"));
}

void TestLogSettings::anInvalidRegexNeverMatches()
{
    LogSettingsTree t = treeWithRoot(QStringLiteral("ROOT"));
    LogPatternNode n = wildcard(QStringLiteral("app("), QStringLiteral("BROKEN"));
    n.kind = LogPatternNode::Kind::Regex;
    t.addPattern(n);

    // A half-typed pattern in the dialog must not start claiming files.
    QCOMPARE(t.resolve(QStringLiteral("/var/log/app.log")).profile.format.pattern,
             QStringLiteral("ROOT"));

    // And a valid one matches UNANCHORED — it is a search, not a whole-name test.
    t.patternAt(0).match = QStringLiteral("app-\\d+");
    QCOMPARE(t.resolve(QStringLiteral("/var/log/app-42.log")).profile.format.pattern,
             QStringLiteral("BROKEN"));
}

void TestLogSettings::aRemoteAddressMatchesOnItsFileName()
{
    LogSettingsTree t = treeWithRoot(QStringLiteral("ROOT"));
    t.addPattern(wildcard(QStringLiteral("*.log"), QStringLiteral("BY-NAME")));

    const QString remote = QStringLiteral("ssh://deploy@web1/var/log/app.log");
    QCOMPARE(logMatchTarget(remote, false), QStringLiteral("app.log"));
    QCOMPARE(t.resolve(remote).profile.format.pattern, QStringLiteral("BY-NAME"));

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
    QCOMPARE(t.resolve(QStringLiteral("/logs/bundle.tar.gz/var/log/app.log"))
                 .profile.format.pattern,
             QStringLiteral("BY-NAME"));

    // A bare compressed stream is the log with its suffix taken off.
    QCOMPARE(logMatchTarget(QStringLiteral("/logs/app.log.gz"), false),
             QStringLiteral("app.log"));
    QCOMPARE(t.resolve(QStringLiteral("/logs/app.log.gz")).profile.format.pattern,
             QStringLiteral("BY-NAME"));
}

void TestLogSettings::aFileNodeOutranksItsPattern()
{
    LogSettingsTree t = treeWithRoot(QStringLiteral("ROOT"));
    t.addPattern(wildcard(QStringLiteral("*.log"), QStringLiteral("PATTERN")));

    LogProfile mine;
    mine.format.pattern = QStringLiteral("MINE");
    t.setFileProfile(QStringLiteral("/var/log/app.log"), mine);

    const auto r = t.resolve(QStringLiteral("/var/log/app.log"));
    QCOMPARE(r.profile.format.pattern, QStringLiteral("MINE"));
    QCOMPARE(r.patternIndex, 0); // still knows which pattern is its parent
    QVERIFY(r.fileIndex >= 0);

    // A sibling that the pattern also matches is unaffected.
    QCOMPARE(t.resolve(QStringLiteral("/var/log/other.log")).profile.format.pattern,
             QStringLiteral("PATTERN"));
}

void TestLogSettings::aProfileEqualToWhatItInheritsStoresNoFileNode()
{
    LogSettingsTree t = treeWithRoot(QStringLiteral("ROOT"));
    t.addPattern(wildcard(QStringLiteral("*.log"), QStringLiteral("PATTERN")));

    // Every open writes back what it used; storing that would leave a node behind for
    // every file the user ever looked at.
    t.setFileProfile(QStringLiteral("/var/log/app.log"), t.inherited(QStringLiteral("/var/log/app.log")));
    QCOMPARE(t.files().size(), 0);

    // And the same for a file with no pattern over it, against the root.
    t.setFileProfile(QStringLiteral("/var/log/app.other"), t.defaults());
    QCOMPARE(t.files().size(), 0);
}

void TestLogSettings::bringingAFileBackIntoLineRemovesItsNode()
{
    LogSettingsTree t = treeWithRoot(QStringLiteral("ROOT"));
    t.addPattern(wildcard(QStringLiteral("*.log"), QStringLiteral("PATTERN")));

    LogProfile mine;
    mine.format.pattern = QStringLiteral("MINE");
    t.setFileProfile(QStringLiteral("/var/log/app.log"), mine);
    QCOMPARE(t.files().size(), 1);

    // This is what "Copy to Parent Pattern" leaves behind: the pattern now says what
    // the file said, so the file node has nothing left to say.
    t.patternAt(0).profile = mine;
    t.setFileProfile(QStringLiteral("/var/log/app.log"), mine);
    QCOMPARE(t.files().size(), 0);
    QCOMPARE(t.resolve(QStringLiteral("/var/log/app.log")).profile.format.pattern,
             QStringLiteral("MINE"));
}

void TestLogSettings::deletingAPatternReHomesItsFiles()
{
    LogSettingsTree t = treeWithRoot(QStringLiteral("ROOT"));
    t.addPattern(wildcard(QStringLiteral("app.*"), QStringLiteral("FIRST")));
    t.addPattern(wildcard(QStringLiteral("*.log"), QStringLiteral("SECOND")));

    LogProfile mine;
    mine.format.pattern = QStringLiteral("MINE");
    t.setFileProfile(QStringLiteral("/var/log/app.log"), mine);

    QCOMPARE(t.resolve(QStringLiteral("/var/log/app.log")).patternIndex, 0);

    // Nothing stores a parent link, so the file simply re-homes under whatever matches
    // next — no dangling reference to clean up.
    t.removePattern(0);
    const auto r = t.resolve(QStringLiteral("/var/log/app.log"));
    QCOMPARE(r.patternIndex, 0); // now the *.log one
    QCOMPARE(t.patternAt(0).match, QStringLiteral("*.log"));
    QCOMPARE(r.profile.format.pattern, QStringLiteral("MINE")); // its own node still wins
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
    t.addPattern(n);

    LogProfile mine;
    mine.format.pattern = QStringLiteral("MINE");
    mine.wrapMode = WrapMode::SelectedRecordOnly;
    t.setFileProfile(QStringLiteral("ssh://deploy@web1/var/log/app.log"), mine);

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

    QCOMPARE(back.files().size(), 1);
    // Keyed by the NORMAL form, so the ":22" spelling and this one are one entry.
    QCOMPARE(back.resolve(QStringLiteral("ssh://deploy@web1:22/var/log/app.log"))
                 .profile.format.pattern,
             QStringLiteral("MINE"));
    QVERIFY(back.resolve(QStringLiteral("ssh://deploy@web1:22/var/log/app.log"))
                .profile.wrapMode == WrapMode::SelectedRecordOnly);
}

void TestLogSettings::aLoadedFileNodeIsKeptEvenWhenItMatchesItsParent()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    LogSettingsTree t = treeWithRoot(QStringLiteral("ROOT"));
    LogProfile mine;
    mine.format.pattern = QStringLiteral("MINE");
    t.setFileProfile(QStringLiteral("/var/log/app.log"), mine);

    // A pattern added later says exactly what the file node already said. On load the
    // node is now redundant — but dropping it would be a change the user never made.
    t.addPattern(wildcard(QStringLiteral("*.log"), QStringLiteral("MINE")));

    LogSettingsStore store(dir.path());
    QVERIFY(store.save(t));

    LogSettingsStore reader(dir.path());
    const LogSettingsTree back = reader.load();
    QCOMPARE(back.files().size(), 1);
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
    QCOMPARE(t.files().size(), 1);
    const auto r = t.resolve(QStringLiteral("/var/log/app.log"));
    QCOMPARE(r.profile.format.pattern, QStringLiteral("OLD-FILE"));
    QVERIFY(r.profile.format.timeDisplay == TimeDisplay::Utc);

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

QTEST_APPLESS_MAIN(TestLogSettings)
#include "tst_logsettings.moc"
