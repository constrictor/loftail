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
};

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
    QCOMPARE(od.filters.value(QStringLiteral("priorityEnabled")).toBool(), true);

    // The view's own state: which file it shows, its columns and its wrap mode (§5).
    QCOMPARE(out.views.size(), 1);
    const SessionView &ov = out.views.first();
    QCOMPARE(ov.documentIndex, 0);
    QCOMPARE(ov.columnState, QByteArrayLiteral("COLS"));
    QCOMPARE(ov.wrapMode, 2);
    QCOMPARE(out.documentFor(ov)->path, d.path);
    QCOMPARE(od.highlighters.value(QStringLiteral("rules")).toArray()
                 .first().toObject().value(QStringLiteral("background")).toInt(),
             3);
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
    QCOMPARE(out.documents.at(0).filters.value(QStringLiteral("priorityEnabled")).toBool(),
             true);
    QCOMPARE(out.documents.at(1).filters.value(QStringLiteral("priorityEnabled")).toBool(),
             false);
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

void TestSession::runSelectionRoundTrip()
{
    QTemporaryDir dir;
    const QString ini = dir.filePath(QStringLiteral("s.ini"));

    Session in;
    SessionDocument d;
    d.path = QStringLiteral("/logs/multi.log");
    // The run-start PATTERN belongs to the settings tree (M20). What the session
    // records is WHICH run was being viewed (§3a).
    // A specific run selected, keyed by its stable start offset + timestamp.
    d.runAll = false;
    d.selectedRunStartOffset = 4096;
    d.selectedRunStartTimestamp = 1700000000000LL;
    in.documents = {d};

    {
        QSettings s(ini, QSettings::IniFormat);
        SessionStore::save(s, in);
    }
    QSettings s(ini, QSettings::IniFormat);
    // Hold the Session, not a reference into a temporary one: load() returns by value and
    // documents.first() hands back a reference INTO it, which lifetime extension does not
    // reach through — the Session would die at the semicolon and every QCOMPARE below
    // would read freed memory. Every other load() call site in this file binds by value.
    const Session          out = SessionStore::load(s);
    const SessionDocument &od = out.documents.first();

    QCOMPARE(od.runAll, false);
    QCOMPARE(od.selectedRunStartOffset, qint64(4096));
    QCOMPARE(od.selectedRunStartTimestamp, qint64(1700000000000LL));
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

    Session in;
    SessionDocument d;
    d.path = QStringLiteral("/tmp/a.log");
    QJsonObject hl;
    hl.insert(QStringLiteral("rules"), set.toJson());
    d.highlighters = hl;
    in.documents = {d};
    SessionView v;
    in.views = {v};

    {
        QSettings s(ini, QSettings::IniFormat);
        SessionStore::save(s, in);
    }
    QSettings s(ini, QSettings::IniFormat);
    const Session out = SessionStore::load(s);

    QCOMPARE(out.documents.size(), 1);
    const HighlighterSet back = HighlighterSet::fromJson(
        out.documents.first().highlighters.value(QStringLiteral("rules")).toArray());

    QCOMPARE(back.rules.size(), 3);
    QCOMPARE(back.rules.at(0).actions, HighlightActions(HighlightAction::Color));
    QCOMPARE(back.rules.at(1).actions, digestOnly.actions);
    QCOMPARE(back.rules.at(2).actions, HighlightActions());

    // The version did NOT move. A session bump migrates upward but is unreadable by any
    // already-shipped binary, and the preset file — the same rule blob — gates on exact
    // equality with no migration at all, so it would discard every preset a user has.
    QCOMPARE(out.schemaVersion, SessionStore::kSchemaVersion);
    QCOMPARE(SessionStore::kSchemaVersion, 3);

    // And a colour-only rule still writes exactly what it wrote before actions existed,
    // which is what makes that true rather than merely intended.
    QVERIFY(!colouring.toJson().contains(QStringLiteral("actions")));
}

QTEST_APPLESS_MAIN(TestSession)
#include "tst_session.moc"
