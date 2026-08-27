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

#include <QJsonArray>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>

#include "Highlight.h"
#include "MatchCriteria.h"
#include "SessionStore.h"

using namespace loftail;

// Session persistence (SPEC.md §10, ARCHITECTURE.md §12). Schema v3 has two arrays:
// `documents` (one per open file) and `views` (one per view, in tab order, pointing
// back at its file), because one file may be open in several views. Core-only (a
// QSettings ini in a temp dir).
class TestSession : public QObject
{
    Q_OBJECT

private slots:
    void documentsArrayRoundTrip();
    void emptyOnFirstLaunch();
    void unknownSchemaYieldsEmpty();
    void arrayShrinksWithoutStaleTail();
    void perFileScopingSurvivesMultipleDocuments();
    void runSelectionRoundTrip();
    void runSelectionAbsentInOldSession();
    void severalViewsOnOneDocument();
    void viewsShrinkWithoutStaleTail();
    void v1SessionMigrates();
    void v2SessionMigratesWithoutItsWindowState();

    // M19 — highlight actions ride inside the same opaque `highlighters` blob, so the
    // session schema did not have to move for them.
    void highlightActionsRoundTripWithoutASchemaBump();
    void aVersion3SessionMigratesAndKeepsItsPaneLayout();
    void anEditorPageRoundTripsAndOnlyAChosenSyntaxIsStored();
};

namespace {

// One `documents` entry in the shape a build BEFORE M21 wrote: the path plus the five
// per-log keys. save() cannot produce this any more — that is the change — so the
// migration's read side has to be arranged by hand.
void writeLegacyDocument(const QString &ini, const QString &path,
                         const QJsonObject &filters = QJsonObject(),
                         const QJsonObject &highlighters = QJsonObject(),
                         bool runAll = false, qint64 runOffset = -1, qint64 runTs = 0)
{
    QSettings s(ini, QSettings::IniFormat);
    s.beginGroup(QStringLiteral("session"));
    s.setValue(QStringLiteral("schemaVersion"), SessionStore::kSchemaVersion);
    s.setValue(QStringLiteral("activeView"), 0);
    s.beginWriteArray(QStringLiteral("documents"), 1);
    s.setArrayIndex(0);
    s.setValue(QStringLiteral("path"), path);
    s.setValue(QStringLiteral("runAll"), runAll);
    s.setValue(QStringLiteral("selectedRunOffset"), runOffset);
    s.setValue(QStringLiteral("selectedRunTs"), runTs);
    s.setValue(QStringLiteral("filters"),
               QString::fromUtf8(QJsonDocument(filters).toJson(QJsonDocument::Compact)));
    s.setValue(QStringLiteral("highlighters"),
               QString::fromUtf8(QJsonDocument(highlighters).toJson(QJsonDocument::Compact)));
    s.endArray();
    s.beginWriteArray(QStringLiteral("views"), 1);
    s.setArrayIndex(0);
    s.setValue(QStringLiteral("document"), 0);
    s.endArray();
    s.endGroup();
    s.sync();
}

} // namespace

void TestSession::documentsArrayRoundTrip()
{
    QTemporaryDir dir;
    const QString ini = dir.filePath(QStringLiteral("s.ini"));

    Session in;
    in.geometry = QByteArrayLiteral("GEOM");
    in.windowState = QByteArrayLiteral("STATE");
    in.activeView = 0;

    SessionDocument d;
    d.path = QStringLiteral("/logs/app.log");
    QJsonObject filters;
    filters.insert(QStringLiteral("priorityEnabled"), true);
    d.filters = filters;
    QJsonObject hl;
    QJsonArray rules;
    QJsonObject rule;
    rule.insert(QStringLiteral("background"), 3);
    rules.append(rule);
    hl.insert(QStringLiteral("rules"), rules);
    d.highlighters = hl;
    in.documents = {d};

    SessionView v;
    v.documentIndex = 0;
    v.columnState = QByteArrayLiteral("COLS");
    v.wrapMode = 2;
    in.views = {v};

    {
        QSettings s(ini, QSettings::IniFormat);
        SessionStore::save(s, in);
    }

    QSettings s(ini, QSettings::IniFormat);
    const Session out = SessionStore::load(s);

    QCOMPARE(out.geometry, in.geometry);
    QCOMPARE(out.windowState, in.windowState);
    QCOMPARE(out.activeView, 0);
    QCOMPARE(out.documents.size(), 1);
    const SessionDocument &od = out.documents.first();
    QCOMPARE(od.path, d.path);

    // The view's own state: which file it shows, its columns and its wrap mode (§5).
    QCOMPARE(out.views.size(), 1);
    const SessionView &ov = out.views.first();
    QCOMPARE(ov.documentIndex, 0);
    QCOMPARE(ov.columnState, QByteArrayLiteral("COLS"));
    QCOMPARE(ov.wrapMode, 2);
    QCOMPARE(out.documentFor(ov)->path, d.path);
}

void TestSession::emptyOnFirstLaunch()
{
    QTemporaryDir dir;
    QSettings s(dir.filePath(QStringLiteral("s.ini")), QSettings::IniFormat);
    const Session out = SessionStore::load(s);
    QVERIFY(!out.hasDocuments());
    QVERIFY(out.views.isEmpty());
}

void TestSession::unknownSchemaYieldsEmpty()
{
    QTemporaryDir dir;
    const QString ini = dir.filePath(QStringLiteral("s.ini"));
    {
        QSettings s(ini, QSettings::IniFormat);
        s.setValue(QStringLiteral("session/schemaVersion"), SessionStore::kSchemaVersion + 5);
        s.setValue(QStringLiteral("session/geometry"), QByteArrayLiteral("X"));
        s.sync();
    }
    QSettings s(ini, QSettings::IniFormat);
    const Session out = SessionStore::load(s);
    QVERIFY(!out.hasDocuments());
    QVERIFY(out.geometry.isEmpty()); // not read under an unknown schema
}

void TestSession::arrayShrinksWithoutStaleTail()
{
    QTemporaryDir dir;
    const QString ini = dir.filePath(QStringLiteral("s.ini"));

    Session two;
    SessionDocument a;
    a.path = QStringLiteral("/a.log");
    SessionDocument b;
    b.path = QStringLiteral("/b.log");
    two.documents = {a, b};
    {
        QSettings s(ini, QSettings::IniFormat);
        SessionStore::save(s, two);
    }

    Session one;
    SessionDocument c;
    c.path = QStringLiteral("/c.log");
    one.documents = {c};
    {
        QSettings s(ini, QSettings::IniFormat);
        SessionStore::save(s, one);
    }

    QSettings s(ini, QSettings::IniFormat);
    const Session out = SessionStore::load(s);
    QCOMPARE(out.documents.size(), 1); // the second /b.log tail must be gone
    QCOMPARE(out.documents.first().path, QStringLiteral("/c.log"));
}

void TestSession::perFileScopingSurvivesMultipleDocuments()
{
    // The array is what makes multi-file additive: two documents keep independent
    // per-file state (§12.4). Exercise a two-element array explicitly.
    QTemporaryDir dir;
    const QString ini = dir.filePath(QStringLiteral("s.ini"));

    Session in;
    in.activeView = 1;
    SessionDocument a;
    a.path = QStringLiteral("/a.log");
    a.filters.insert(QStringLiteral("priorityEnabled"), true);
    SessionDocument b;
    b.path = QStringLiteral("/b.log");
    b.filters.insert(QStringLiteral("priorityEnabled"), false);
    in.documents = {a, b};
    SessionView va;
    va.documentIndex = 0;
    SessionView vb;
    vb.documentIndex = 1;
    in.views = {va, vb};
    {
        QSettings s(ini, QSettings::IniFormat);
        SessionStore::save(s, in);
    }

    QSettings s(ini, QSettings::IniFormat);
    const Session out = SessionStore::load(s);
    QCOMPARE(out.documents.size(), 2);
    QCOMPARE(out.documents.at(0).path, QStringLiteral("/a.log"));
    QCOMPARE(out.documents.at(1).path, QStringLiteral("/b.log"));
    // activeView indexes the views array, and that view names its own file.
    QCOMPARE(out.documentFor(out.views.at(out.activeView))->path, QStringLiteral("/b.log"));
}

void TestSession::severalViewsOnOneDocument()
{
    // Two views onto ONE file: the document appears once, the views twice, each with
    // its own column layout and wrap mode (ARCHITECTURE.md §12).
    QTemporaryDir dir;
    const QString ini = dir.filePath(QStringLiteral("s.ini"));

    Session in;
    SessionDocument d;
    d.path = QStringLiteral("/logs/one.log");
    in.documents = {d};
    SessionView first;
    first.documentIndex = 0;
    first.columnState = QByteArrayLiteral("WIDE");
    first.wrapMode = 0;
    SessionView second;
    second.documentIndex = 0;
    second.columnState = QByteArrayLiteral("NARROW");
    second.wrapMode = 2;
    in.views = {first, second};
    in.activeView = 1;
    {
        QSettings s(ini, QSettings::IniFormat);
        SessionStore::save(s, in);
    }

    QSettings s(ini, QSettings::IniFormat);
    const Session out = SessionStore::load(s);
    QCOMPARE(out.documents.size(), 1);
    QCOMPARE(out.views.size(), 2);
    QCOMPARE(out.views.at(0).documentIndex, out.views.at(1).documentIndex);
    QCOMPARE(out.views.at(0).columnState, QByteArrayLiteral("WIDE"));
    QCOMPARE(out.views.at(1).columnState, QByteArrayLiteral("NARROW"));
    QCOMPARE(out.views.at(1).wrapMode, 2);
    QCOMPARE(out.activeView, 1);
}

void TestSession::viewsShrinkWithoutStaleTail()
{
    // Same shrink hazard as the documents array: closing a tab must not leave a
    // stale view entry behind, or restore would resurrect it.
    QTemporaryDir dir;
    const QString ini = dir.filePath(QStringLiteral("s.ini"));

    Session two;
    SessionDocument d;
    d.path = QStringLiteral("/a.log");
    two.documents = {d};
    SessionView v1;
    v1.columnState = QByteArrayLiteral("FIRST");
    SessionView v2;
    v2.columnState = QByteArrayLiteral("SECOND");
    two.views = {v1, v2};
    {
        QSettings s(ini, QSettings::IniFormat);
        SessionStore::save(s, two);
    }

    Session one = two;
    one.views = {v1};
    {
        QSettings s(ini, QSettings::IniFormat);
        SessionStore::save(s, one);
    }

    QSettings s(ini, QSettings::IniFormat);
    const Session out = SessionStore::load(s);
    QCOMPARE(out.views.size(), 1);
    QCOMPARE(out.views.first().columnState, QByteArrayLiteral("FIRST"));
}

void TestSession::v1SessionMigrates()
{
    // A session written by the single-file release must still open the user's file
    // rather than being silently discarded. Its windowState is deliberately dropped:
    // it describes a window laid out nothing like this one.
    QTemporaryDir dir;
    const QString ini = dir.filePath(QStringLiteral("s.ini"));
    {
        QSettings s(ini, QSettings::IniFormat);
        s.beginGroup(QStringLiteral("session"));
        s.setValue(QStringLiteral("schemaVersion"), 1);
        s.setValue(QStringLiteral("geometry"), QByteArrayLiteral("GEOM"));
        s.setValue(QStringLiteral("windowState"), QByteArrayLiteral("OLDSTATE"));
        s.setValue(QStringLiteral("activeDocument"), 0);
        s.beginWriteArray(QStringLiteral("documents"), 1);
        s.setArrayIndex(0);
        s.setValue(QStringLiteral("path"), QStringLiteral("/logs/old.log"));
        s.setValue(QStringLiteral("pattern"), QStringLiteral("%m%n"));
        s.setValue(QStringLiteral("columnState"), QByteArrayLiteral("OLDCOLS"));
        s.endArray();
        s.endGroup();
        s.sync();
    }

    QSettings s(ini, QSettings::IniFormat);
    const Session out = SessionStore::load(s);
    QCOMPARE(out.schemaVersion, SessionStore::kSchemaVersion);
    QCOMPARE(out.geometry, QByteArrayLiteral("GEOM"));
    QVERIFY(out.windowState.isEmpty()); // the v1 dock layout is NOT carried over
    QCOMPARE(out.documents.size(), 1);
    QCOMPARE(out.documents.first().path, QStringLiteral("/logs/old.log"));
    // One synthesized view, carrying the column state that used to live on the file.
    QCOMPARE(out.views.size(), 1);
    QCOMPARE(out.views.first().documentIndex, 0);
    QCOMPARE(out.views.first().columnState, QByteArrayLiteral("OLDCOLS"));
}

void TestSession::v2SessionMigratesWithoutItsWindowState()
{
    // v2 was the all-docks shell, where open files were dock widgets and the central
    // widget was collapsed to nothing. Its files and per-view state still restore;
    // its windowState must NOT, or the document well would come back zero-sized —
    // a silent failure that looks like the tabs having vanished.
    QTemporaryDir dir;
    const QString ini = dir.filePath(QStringLiteral("s.ini"));
    {
        QSettings s(ini, QSettings::IniFormat);
        s.beginGroup(QStringLiteral("session"));
        s.setValue(QStringLiteral("schemaVersion"), 2);
        s.setValue(QStringLiteral("geometry"), QByteArrayLiteral("GEOM"));
        s.setValue(QStringLiteral("windowState"), QByteArrayLiteral("DOCKSTATE"));
        s.setValue(QStringLiteral("activeView"), 1);
        s.beginWriteArray(QStringLiteral("documents"), 1);
        s.setArrayIndex(0);
        s.setValue(QStringLiteral("path"), QStringLiteral("/logs/two.log"));
        s.setValue(QStringLiteral("pattern"), QStringLiteral("%m%n"));
        s.endArray();
        s.beginWriteArray(QStringLiteral("views"), 2);
        s.setArrayIndex(0);
        s.setValue(QStringLiteral("document"), 0);
        s.setValue(QStringLiteral("dockName"), QStringLiteral("docView-old-1"));
        s.setValue(QStringLiteral("columnState"), QByteArrayLiteral("WIDE"));
        s.setArrayIndex(1);
        s.setValue(QStringLiteral("document"), 0);
        s.setValue(QStringLiteral("dockName"), QStringLiteral("docView-old-2"));
        s.setValue(QStringLiteral("wrapMode"), 2);
        s.endArray();
        s.endGroup();
        s.sync();
    }

    QSettings s(ini, QSettings::IniFormat);
    const Session out = SessionStore::load(s);
    QCOMPARE(out.schemaVersion, SessionStore::kSchemaVersion);
    QCOMPARE(out.geometry, QByteArrayLiteral("GEOM"));
    QVERIFY(out.windowState.isEmpty()); // the v2 dock layout is NOT carried over
    QCOMPARE(out.documents.size(), 1);
    // Both views come back, in their saved order — which is now the tab order; the
    // dockName each carried is simply read past.
    QCOMPARE(out.views.size(), 2);
    QCOMPARE(out.views.at(0).columnState, QByteArrayLiteral("WIDE"));
    QCOMPARE(out.views.at(1).wrapMode, 2);
    QCOMPARE(out.activeView, 1);
}

// THE FIVE PER-LOG KEYS ARE READ AND NO LONGER WRITTEN (M21). A log's filters, its
// highlight rules and which run it was on are per-FILE state and live one record per log
// (LogFileStore.h) — which is what makes them survive closing the tab, where the session
// only ever remembered them while the log was open in one.
//
// load() still reads all five, so the first launch after the upgrade can hand them over;
// save() writes none of them, and because both arrays are removed before being rewritten,
// the first quit takes them off the disk for good. That is what makes the migration
// once-only with no flag anywhere saying it ran. No schema bump came with it: a removed
// key is exactly what a backward read handles.
void TestSession::runSelectionRoundTrip()
{
    QTemporaryDir dir;
    const QString ini = dir.filePath(QStringLiteral("s.ini"));

    QJsonObject filters;
    filters.insert(QStringLiteral("priorityEnabled"), true);
    QJsonArray rules;
    QJsonObject rule;
    rule.insert(QStringLiteral("background"), 3);
    rules.append(rule);
    QJsonObject hl;
    hl.insert(QStringLiteral("rules"), rules);

    writeLegacyDocument(ini, QStringLiteral("/tmp/a.log"), filters, hl,
                        /*runAll=*/false, /*runOffset=*/4096, /*runTs=*/1700000000000LL);

    {
        QSettings s(ini, QSettings::IniFormat);
        const Session out = SessionStore::load(s);
        QCOMPARE(out.documents.size(), 1);
        const SessionDocument &od = out.documents.first();
        // All five arrive, which is the whole of what the migration needs.
        QCOMPARE(od.filters.value(QStringLiteral("priorityEnabled")).toBool(), true);
        QCOMPARE(od.highlighters.value(QStringLiteral("rules")).toArray()
                     .first().toObject().value(QStringLiteral("background")).toInt(),
                 3);
        QCOMPARE(od.runAll, false);
        QCOMPARE(od.selectedRunStartOffset, qint64(4096));
        QCOMPARE(od.selectedRunStartTimestamp, qint64(1700000000000LL));

        // And writing the same session back takes every one of them off the disk.
        SessionStore::save(s, out);
    }

    QSettings check(ini, QSettings::IniFormat);
    check.beginGroup(QStringLiteral("session"));
    QCOMPARE(check.beginReadArray(QStringLiteral("documents")), 1);
    check.setArrayIndex(0);
    QCOMPARE(check.value(QStringLiteral("path")).toString(), QStringLiteral("/tmp/a.log"));
    for (const char *key : {"filters", "highlighters", "runAll", "selectedRunOffset",
                            "selectedRunTs"}) {
        QVERIFY2(!check.contains(QLatin1String(key)),
                 QByteArray("the session still writes ") + key);
    }
    check.endArray();
    check.endGroup();
}

void TestSession::runSelectionAbsentInOldSession()
{
    // A session written before this feature has none of the run keys. Reading it back
    // must yield tolerant defaults (no pattern, no restriction) — the fields were
    // added additively within a schema version, NOT by bumping the schema.
    QTemporaryDir dir;
    const QString ini = dir.filePath(QStringLiteral("s.ini"));

    Session in;
    SessionDocument d;
    d.path = QStringLiteral("/logs/old.log");
    in.documents = {d};
    {
        QSettings s(ini, QSettings::IniFormat);
        SessionStore::save(s, in);
    }

    // Simulate an OLD ini by removing the run keys the writer added.
    {
        QSettings s(ini, QSettings::IniFormat);
        s.beginGroup(QStringLiteral("session"));
        s.beginReadArray(QStringLiteral("documents"));
        s.setArrayIndex(0);
        for (const char *k : {"runAll", "selectedRunOffset", "selectedRunTs"})
            s.remove(QLatin1String(k));
        s.endArray();
        s.endGroup();
    }

    QSettings s(ini, QSettings::IniFormat);
    const Session out = SessionStore::load(s);
    QCOMPARE(out.documents.size(), 1); // still loads (schema unchanged)
    const SessionDocument &od = out.documents.first();
    QCOMPARE(od.runAll, false);
    QCOMPARE(od.selectedRunStartOffset, qint64(-1));
}

void TestSession::highlightActionsRoundTripWithoutASchemaBump()
{
    QTemporaryDir dir;
    const QString ini = dir.filePath(QStringLiteral("s.ini"));

    HighlightRule colouring; // the shape everything written before M19 has
    colouring.match.priorityEnabled = true;
    HighlightRule digestOnly;
    digestOnly.actions = HighlightAction::Digest | HighlightAction::Tab;
    digestOnly.match.priorityEnabled = true;
    HighlightRule parked;
    parked.actions = HighlightActions();
    parked.match.priorityEnabled = true;

    HighlighterSet set;
    set.rules = {colouring, digestOnly, parked};

    QJsonObject hl;
    hl.insert(QStringLiteral("rules"), set.toJson());
    // Written in the pre-M21 shape, because that is the only thing this case can still be
    // about: the actions ride inside an opaque `highlighters` blob, and what has to keep
    // working is READING one — which is both the migration and, one store over, exactly
    // what LogFileStore does with the same array.
    writeLegacyDocument(ini, QStringLiteral("/tmp/a.log"), QJsonObject(), hl);

    QSettings s(ini, QSettings::IniFormat);
    const Session out = SessionStore::load(s);

    QCOMPARE(out.documents.size(), 1);
    const HighlighterSet back = HighlighterSet::fromJson(
        out.documents.first().highlighters.value(QStringLiteral("rules")).toArray());

    QCOMPARE(back.rules.size(), 3);
    QCOMPARE(back.rules.at(0).actions, HighlightActions(HighlightAction::Color));
    QCOMPARE(back.rules.at(1).actions, digestOnly.actions);
    QCOMPARE(back.rules.at(2).actions, HighlightActions());

    QCOMPARE(out.schemaVersion, SessionStore::kSchemaVersion);

    // THE CLAIM IS THAT ACTIONS WERE ADDITIVE, and it used to be spelled
    // `QCOMPARE(kSchemaVersion, 3)`. That spelling pinned a NUMBER as a proxy for it,
    // which any later and unrelated bump moves — as the editors array (v4) did. The
    // claim itself is the line below: a colour-only rule serializes byte-for-byte as it
    // did before actions existed, so nothing about M19 could have forced a bump. The
    // reason bumps are expensive still stands and is stated in SessionStore.h: a bumped
    // file is unreadable by any already-shipped binary, and the preset store — the same
    // rule blob — gates on exact equality with no migration at all.
    QVERIFY(!colouring.toJson().contains(QStringLiteral("actions")));
}

// A v3 store — everything users have today — restores intact under v4.
//
// TWO THINGS, and the second is the one that would have gone wrong silently. The editors
// array is simply absent, which is what "every page was a log" means. And `windowState`
// must SURVIVE: load() took it only for the exact current version, because a v1 blob
// describes a different window and a v2 one describes the collapsed central widget of the
// all-docks shell. v3 describes THIS shell, so a `== kSchemaVersion` test there throws
// away every existing user's pane arrangement on the first launch after the upgrade, for
// no reason at all — and nothing on screen would connect the loss to the upgrade.
void TestSession::aVersion3SessionMigratesAndKeepsItsPaneLayout()
{
    QTemporaryDir dir;
    const QString ini = dir.filePath(QStringLiteral("v3.ini"));
    const QByteArray layout = QByteArrayLiteral("pane-layout-blob");
    {
        QSettings s(ini, QSettings::IniFormat);
        s.beginGroup(QStringLiteral("session"));
        s.setValue(QStringLiteral("schemaVersion"), 3);
        s.setValue(QStringLiteral("windowState"), layout);
        s.setValue(QStringLiteral("activeView"), 1);
        s.beginWriteArray(QStringLiteral("documents"), 1);
        s.setArrayIndex(0);
        s.setValue(QStringLiteral("path"), QStringLiteral("/tmp/a.log"));
        s.endArray();
        s.beginWriteArray(QStringLiteral("views"), 2);
        s.setArrayIndex(0);
        s.setValue(QStringLiteral("document"), 0);
        s.setArrayIndex(1);
        s.setValue(QStringLiteral("document"), 0);
        s.endArray();
        s.endGroup();
    }

    QSettings s(ini, QSettings::IniFormat);
    const Session out = SessionStore::load(s);
    QCOMPARE(out.schemaVersion, SessionStore::kSchemaVersion);
    QCOMPARE(out.documents.size(), 1);
    QCOMPARE(out.views.size(), 2);
    QVERIFY(out.editors.isEmpty());
    // With no editors the tab in front IS the active view, which is what makes the
    // migration a copy rather than a conversion.
    QCOMPARE(out.activeTab, 1);
    QCOMPARE(out.windowState, layout);
}

// An editor page round-trips, and a GUESSED syntax is not written.
void TestSession::anEditorPageRoundTripsAndOnlyAChosenSyntaxIsStored()
{
    QTemporaryDir dir;
    const QString ini = dir.filePath(QStringLiteral("v4.ini"));

    Session in;
    in.activeTab = 2;
    SessionEditor guessed;
    guessed.address = QStringLiteral("/etc/a.properties");
    guessed.tabIndex = 1;
    guessed.syntaxChosen = false;
    guessed.syntax = 1; // would be Ini, but nobody chose it
    SessionEditor chosen;
    chosen.address = QStringLiteral("/etc/b.conf");
    chosen.tabIndex = 2;
    chosen.syntaxChosen = true;
    chosen.syntax = 3;
    in.editors = {guessed, chosen};

    {
        QSettings s(ini, QSettings::IniFormat);
        SessionStore::save(s, in);
    }
    QSettings s(ini, QSettings::IniFormat);
    const Session out = SessionStore::load(s);

    QCOMPARE(out.editors.size(), 2);
    QCOMPARE(out.editors.at(0).address, guessed.address);
    QCOMPARE(out.editors.at(0).tabIndex, 1);
    QCOMPARE(out.activeTab, 2);

    // PRESENCE, NOT VALUE. A guess is re-made on restore from the file as it stands,
    // which is right because the file may have changed; only a decision is stored. Store
    // the guess too and a restored tab comes back frozen at whatever the file used to
    // look like — and a stored 0 (PlainText) cannot be told from "nothing was chosen"
    // at all, which would bring every restored tab back uncoloured.
    QVERIFY(!out.editors.at(0).syntaxChosen);
    QVERIFY(out.editors.at(1).syntaxChosen);
    QCOMPARE(out.editors.at(1).syntax, 3);
}

QTEST_APPLESS_MAIN(TestSession)
#include "tst_session.moc"
