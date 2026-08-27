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

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "AtomicJson.h"
#include "Highlight.h"
#include "MatchCriteria.h"
#include "PresetStore.h"
#include "Priority.h"

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

    // M19 — actions ride inside the same content blob, at the same schema version.
    void aPresetWithNoActionsRestoresColouringRules();
    void aPresetWithActionsRoundTripsAtTheSameSchema();
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
    // The bad path nests under `good.json`, an existing REGULAR file: creating that
    // parent "directory" is impossible on every platform. (Avoid an embedded-NUL or
    // /proc path here — the NUL crashes QFileInfo on Windows and /proc is Linux-only.)
    const QString bad = path + QStringLiteral("/x.json");
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

void TestPresetStore::aPresetWithNoActionsRestoresColouringRules()
{
    QTemporaryDir dir;
    PresetStore store(dir.path());

    // sampleContent() is deliberately the ORIGINAL two-axis, no-actions rule shape —
    // a preset a user saved before either the axis set or the action set existed. It
    // must come back colouring, at the same schema version, with no migration.
    QVERIFY(store.save(PresetStore::Kind::Highlighters, QStringLiteral("old"),
                       sampleContent()));

    const QJsonObject back =
        store.preset(PresetStore::Kind::Highlighters, QStringLiteral("old"));
    const HighlighterSet set =
        HighlighterSet::fromJson(back.value(QStringLiteral("rules")).toArray());

    QCOMPARE(set.rules.size(), 1);
    QCOMPARE(set.rules.first().actions, HighlightActions(HighlightAction::Color));
    QCOMPARE(set.rules.first().match.minPriority, Priority::Error);
    QCOMPARE(PresetStore::kSchemaVersion, 1); // and the version did not move
}

void TestPresetStore::aPresetWithActionsRoundTripsAtTheSameSchema()
{
    QTemporaryDir dir;
    PresetStore store(dir.path());

    HighlightRule digestOnly;
    digestOnly.actions = HighlightAction::Digest | HighlightAction::Notify;
    digestOnly.match.priorityEnabled = true;
    digestOnly.match.minPriority = Priority::Error;
    HighlightRule parked;
    parked.actions = HighlightActions(); // matches and does nothing
    parked.match.priorityEnabled = true;

    HighlighterSet set;
    set.rules = {digestOnly, parked};

    QJsonObject content;
    content.insert(QStringLiteral("rules"), set.toJson());
    QVERIFY(store.save(PresetStore::Kind::Highlighters, QStringLiteral("new"), content));

    const HighlighterSet back = HighlighterSet::fromJson(
        store.preset(PresetStore::Kind::Highlighters, QStringLiteral("new"))
            .value(QStringLiteral("rules"))
            .toArray());

    QCOMPARE(back.rules.size(), 2);
    QCOMPARE(back.rules.at(0).actions, digestOnly.actions);
    // The one an isEmpty()-based read would silently turn back into a colouring rule.
    QCOMPARE(back.rules.at(1).actions, HighlightActions());
    QCOMPARE(PresetStore::kSchemaVersion, 1);
}

QTEST_APPLESS_MAIN(TestPresetStore)
#include "tst_presetstore.moc"
