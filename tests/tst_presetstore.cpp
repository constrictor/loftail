#include <QtTest>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "AtomicJson.h"
#include "PresetStore.h"

using namespace loftail;

// M5 — preset store (SPEC.md §9, ARCHITECTURE.md §8/§8.1). Named filter and
// highlighter presets as schema-versioned JSON, written atomically, plus
// export/import to an arbitrary file. Core-only, no QApplication.
class TestPresetStore : public QObject
{
    Q_OBJECT

private:
    static QJsonObject sampleContent();

private slots:
    void atomicWriteRoundTrip();
    void atomicWriteLeavesOldFileOnBadPath();
    void saveApplyRenameDelete();
    void filtersAndHighlightersAreIndependent();
    void collectionHasSchemaVersion();
    void exportImportRoundTrip();
    void exportImportIsThemePortable();
    void importRejectsWrongSchema();
};

QJsonObject TestPresetStore::sampleContent()
{
    // A highlighter-preset content blob: rules with palette INDICES, never RGB (§8).
    QJsonObject rule;
    rule.insert(QStringLiteral("enabled"), true);
    rule.insert(QStringLiteral("matchPriority"), true);
    rule.insert(QStringLiteral("minPriority"), QStringLiteral("ERROR"));
    rule.insert(QStringLiteral("background"), 0);
    rule.insert(QStringLiteral("foreground"), -1);
    QJsonArray rules;
    rules.append(rule);
    QJsonObject content;
    content.insert(QStringLiteral("rules"), rules);
    return content;
}

void TestPresetStore::atomicWriteRoundTrip()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("sub/dir/data.json")); // nested
    QJsonObject o;
    o.insert(QStringLiteral("k"), 42);

    QVERIFY(AtomicJson::write(path, QJsonDocument(o))); // creates parent dirs
    QVERIFY(QFile::exists(path));

    bool ok = false;
    const QJsonDocument back = AtomicJson::read(path, &ok);
    QVERIFY(ok);
    QCOMPARE(back.object().value(QStringLiteral("k")).toInt(), 42);
}

void TestPresetStore::atomicWriteLeavesOldFileOnBadPath()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("good.json"));
    QJsonObject first;
    first.insert(QStringLiteral("v"), 1);
    QVERIFY(AtomicJson::write(path, QJsonDocument(first)));

    // Writing to an impossible location fails WITHOUT leaving a partial file and
    // without disturbing the good file that already exists (temp-file+rename, §8.1).
    const QString bad = QStringLiteral("/proc/nonexistent-dir/\0/x.json");
    QVERIFY(!AtomicJson::write(bad, QJsonDocument(first)));

    bool ok = false;
    const QJsonDocument back = AtomicJson::read(path, &ok);
    QVERIFY(ok);
    QCOMPARE(back.object().value(QStringLiteral("v")).toInt(), 1); // intact
}

void TestPresetStore::saveApplyRenameDelete()
{
    QTemporaryDir dir;
    PresetStore store(dir.path());
    const auto K = PresetStore::Kind::Highlighters;

    QVERIFY(store.names(K).isEmpty());
    QVERIFY(store.save(K, QStringLiteral("Errors"), sampleContent()));
    QCOMPARE(store.names(K), QStringList{QStringLiteral("Errors")});

    // Apply == read the content back verbatim.
    const QJsonObject got = store.preset(K, QStringLiteral("Errors"));
    QCOMPARE(got, sampleContent());

    QVERIFY(store.rename(K, QStringLiteral("Errors"), QStringLiteral("Severe")));
    QCOMPARE(store.names(K), QStringList{QStringLiteral("Severe")});
    QCOMPARE(store.preset(K, QStringLiteral("Severe")), sampleContent());

    QVERIFY(store.remove(K, QStringLiteral("Severe")));
    QVERIFY(store.names(K).isEmpty());
}

void TestPresetStore::filtersAndHighlightersAreIndependent()
{
    QTemporaryDir dir;
    PresetStore store(dir.path());
    QJsonObject f;
    f.insert(QStringLiteral("kind"), QStringLiteral("filter-state"));
    QVERIFY(store.save(PresetStore::Kind::Filters, QStringLiteral("Quiet"), f));
    QVERIFY(store.save(PresetStore::Kind::Highlighters, QStringLiteral("Loud"), sampleContent()));

    // The two axes are separate, separately-recallable collections (§9).
    QCOMPARE(store.names(PresetStore::Kind::Filters), QStringList{QStringLiteral("Quiet")});
    QCOMPARE(store.names(PresetStore::Kind::Highlighters), QStringList{QStringLiteral("Loud")});
    QVERIFY(store.preset(PresetStore::Kind::Filters, QStringLiteral("Loud")).isEmpty());
}

void TestPresetStore::collectionHasSchemaVersion()
{
    QTemporaryDir dir;
    PresetStore store(dir.path());
    QVERIFY(store.save(PresetStore::Kind::Filters, QStringLiteral("X"), QJsonObject()));

    bool ok = false;
    const QJsonDocument doc =
        AtomicJson::read(dir.filePath(QStringLiteral("filter-presets.json")), &ok);
    QVERIFY(ok);
    QCOMPARE(doc.object().value(QStringLiteral("schemaVersion")).toInt(),
             PresetStore::kSchemaVersion);
    QCOMPARE(doc.object().value(QStringLiteral("kind")).toString(), QStringLiteral("filters"));
}

void TestPresetStore::exportImportRoundTrip()
{
    QTemporaryDir dir;
    PresetStore source(dir.path());
    QVERIFY(source.save(PresetStore::Kind::Highlighters, QStringLiteral("Errors"), sampleContent()));

    const QString file = dir.filePath(QStringLiteral("shared.json"));
    QVERIFY(source.exportPreset(PresetStore::Kind::Highlighters, QStringLiteral("Errors"), file));

    // A different user's store imports the file and recovers name, kind, content.
    QTemporaryDir otherDir;
    PresetStore dest(otherDir.path());
    PresetStore::Kind kind{};
    QString name;
    QVERIFY(dest.importPreset(file, &kind, &name));
    QCOMPARE(int(kind), int(PresetStore::Kind::Highlighters));
    QCOMPARE(name, QStringLiteral("Errors"));
    QCOMPARE(dest.preset(PresetStore::Kind::Highlighters, QStringLiteral("Errors")), sampleContent());
}

void TestPresetStore::exportImportIsThemePortable()
{
    // Portability across themes is structural: the exported content carries palette
    // INDICES, not resolved RGB, so the importing user's palette supplies colors.
    QTemporaryDir dir;
    PresetStore store(dir.path());
    QVERIFY(store.save(PresetStore::Kind::Highlighters, QStringLiteral("E"), sampleContent()));
    const QString file = dir.filePath(QStringLiteral("e.json"));
    QVERIFY(store.exportPreset(PresetStore::Kind::Highlighters, QStringLiteral("E"), file));

    bool ok = false;
    const QJsonDocument doc = AtomicJson::read(file, &ok);
    QVERIFY(ok);
    const QJsonArray rules =
        doc.object().value(QStringLiteral("content")).toObject()
            .value(QStringLiteral("rules")).toArray();
    QCOMPARE(rules.size(), 1);
    const QJsonObject r = rules.first().toObject();
    // Roles are integers (indices), and there is no "color"/"rgb" key anywhere.
    QVERIFY(r.value(QStringLiteral("background")).isDouble());
    QVERIFY(!r.contains(QStringLiteral("rgb")));
    QVERIFY(!r.contains(QStringLiteral("color")));
}

void TestPresetStore::importRejectsWrongSchema()
{
    QTemporaryDir dir;
    const QString file = dir.filePath(QStringLiteral("future.json"));
    QJsonObject root;
    root.insert(QStringLiteral("schemaVersion"), PresetStore::kSchemaVersion + 99);
    root.insert(QStringLiteral("kind"), QStringLiteral("highlighters"));
    root.insert(QStringLiteral("name"), QStringLiteral("N"));
    root.insert(QStringLiteral("content"), QJsonObject());
    QVERIFY(AtomicJson::write(file, QJsonDocument(root)));

    PresetStore store(dir.path());
    QVERIFY(!store.importPreset(file)); // an unknown schema is refused, not mangled
}

QTEST_APPLESS_MAIN(TestPresetStore)
#include "tst_presetstore.moc"
