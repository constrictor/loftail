#include <QtTest>

#include <QJsonArray>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>

#include "SessionStore.h"

using namespace loftail;

// Session persistence (SPEC.md §10, ARCHITECTURE.md §12). Schema v2 has two arrays:
// `documents` (one per open file) and `views` (one per view, pointing back at its
// file), because one file may be open in several views. Core-only (a QSettings ini in
// a temp dir).
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
    void legacyDisplayZoneKeyMigrates();
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
    d.format.pattern = QStringLiteral("%d [%t] %-5p %c - %m%n");
    d.format.encoding = Encoding::Utf16LE;
    d.format.sourceZone.kind = ZoneChoice::Kind::Utc;
    d.format.timeDisplay = TimeDisplay::RunSeconds;
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
    v.dockName = QStringLiteral("docView-abc");
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
    QCOMPARE(od.format.pattern, d.format.pattern);
    QCOMPARE(int(od.format.encoding), int(Encoding::Utf16LE));
    QCOMPARE(int(od.format.sourceZone.kind), int(ZoneChoice::Kind::Utc));
    QCOMPARE(int(od.format.timeDisplay), int(TimeDisplay::RunSeconds));
    QCOMPARE(od.filters.value(QStringLiteral("priorityEnabled")).toBool(), true);

    // The view's own state: dock identity, columns and wrap mode (§5).
    QCOMPARE(out.views.size(), 1);
    const SessionView &ov = out.views.first();
    QCOMPARE(ov.documentIndex, 0);
    QCOMPARE(ov.dockName, QStringLiteral("docView-abc"));
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
    QVERIFY(out.active() == nullptr);
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
    a.format.pattern = QStringLiteral("A%n");
    SessionDocument b;
    b.path = QStringLiteral("/b.log");
    b.format.pattern = QStringLiteral("B%n");
    in.documents = {a, b};
    SessionView va;
    va.documentIndex = 0;
    va.dockName = QStringLiteral("docView-a");
    SessionView vb;
    vb.documentIndex = 1;
    vb.dockName = QStringLiteral("docView-b");
    in.views = {va, vb};
    {
        QSettings s(ini, QSettings::IniFormat);
        SessionStore::save(s, in);
    }

    QSettings s(ini, QSettings::IniFormat);
    const Session out = SessionStore::load(s);
    QCOMPARE(out.documents.size(), 2);
    QCOMPARE(out.documents.at(0).format.pattern, QStringLiteral("A%n"));
    QCOMPARE(out.documents.at(1).format.pattern, QStringLiteral("B%n"));
    QCOMPARE(out.documentFor(*out.active())->path, QStringLiteral("/b.log"));
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
    first.dockName = QStringLiteral("docView-1");
    first.columnState = QByteArrayLiteral("WIDE");
    first.wrapMode = 0;
    SessionView second;
    second.documentIndex = 0;
    second.dockName = QStringLiteral("docView-2");
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
    QCOMPARE(out.active()->dockName, QStringLiteral("docView-2"));
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
    v1.dockName = QStringLiteral("docView-1");
    SessionView v2;
    v2.dockName = QStringLiteral("docView-2");
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
    QCOMPARE(out.views.first().dockName, QStringLiteral("docView-1"));
}

void TestSession::v1SessionMigrates()
{
    // A session written by the pre-tabs release must still open the user's file
    // rather than being silently discarded. Its windowState is deliberately dropped:
    // it describes a window with a central widget and no document docks.
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
    QVERIFY(!out.views.first().dockName.isEmpty());
}

void TestSession::legacyDisplayZoneKeyMigrates()
{
    // The display axis was a ZoneChoice under the key "displayZone" until the
    // timestamp header menu subsumed it (SPEC.md §4). A session written back then
    // must keep the user's UTC choice rather than silently reverting to "as written".
    QTemporaryDir dir;
    const QString ini = dir.filePath(QStringLiteral("s.ini"));
    {
        QSettings s(ini, QSettings::IniFormat);
        s.beginGroup(QStringLiteral("session"));
        s.setValue(QStringLiteral("schemaVersion"), SessionStore::kSchemaVersion);
        s.beginWriteArray(QStringLiteral("documents"), 1);
        s.setArrayIndex(0);
        s.setValue(QStringLiteral("path"), QStringLiteral("/logs/old.log"));
        s.setValue(QStringLiteral("displayZone"), QStringLiteral("utc")); // and no timeDisplay
        s.endArray();
        s.endGroup();
        s.sync();
    }

    QSettings s(ini, QSettings::IniFormat);
    const Session out = SessionStore::load(s);
    QCOMPARE(out.documents.size(), 1);
    QCOMPARE(int(out.documents.first().format.timeDisplay), int(TimeDisplay::Utc));

    // Adding the key did NOT bump the schema: it is additive within v2 and readable
    // both ways, and a bump would discard every existing session (load() accepts
    // only kSchemaVersion and 1).
    QCOMPARE(SessionStore::kSchemaVersion, 2);

    // The legacy spellings that meant "as written" for display land on AsWritten.
    for (const QString &legacy : {QStringLiteral("default"), QStringLiteral("offset:7200")}) {
        {
            QSettings w(ini, QSettings::IniFormat);
            w.beginGroup(QStringLiteral("session"));
            w.beginWriteArray(QStringLiteral("documents"), 1);
            w.setArrayIndex(0);
            w.setValue(QStringLiteral("displayZone"), legacy);
            w.endArray();
            w.endGroup();
            w.sync();
        }
        QSettings r(ini, QSettings::IniFormat);
        QCOMPARE(int(SessionStore::load(r).documents.first().format.timeDisplay),
                 int(TimeDisplay::AsWritten));
    }
}

void TestSession::runSelectionRoundTrip()
{
    QTemporaryDir dir;
    const QString ini = dir.filePath(QStringLiteral("s.ini"));

    Session in;
    SessionDocument d;
    d.path = QStringLiteral("/logs/multi.log");
    d.format.pattern = QStringLiteral("%d [%t] %-5p %c - %m%n");
    // The run-start axis rides in FormatSettings (persisted like the format, §3a).
    d.format.runStartPattern = QStringLiteral("Application starting");
    d.format.runStartIsRegex = true;
    d.format.runStartCaseSensitive = true;
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
    const SessionDocument &od = SessionStore::load(s).documents.first();

    QCOMPARE(od.format.runStartPattern, QStringLiteral("Application starting"));
    QCOMPARE(od.format.runStartIsRegex, true);
    QCOMPARE(od.format.runStartCaseSensitive, true);
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
    d.format.pattern = QStringLiteral("%m%n");
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
        for (const char *k : {"runStartPattern", "runStartRegex", "runStartCase", "runAll",
                              "selectedRunOffset", "selectedRunTs"})
            s.remove(QLatin1String(k));
        s.endArray();
        s.endGroup();
    }

    QSettings s(ini, QSettings::IniFormat);
    const Session out = SessionStore::load(s);
    QCOMPARE(out.documents.size(), 1); // still loads (schema unchanged)
    const SessionDocument &od = out.documents.first();
    QVERIFY(od.format.runStartPattern.isEmpty());
    QCOMPARE(od.runAll, false);
    QCOMPARE(od.selectedRunStartOffset, qint64(-1));
}

QTEST_APPLESS_MAIN(TestSession)
#include "tst_session.moc"
