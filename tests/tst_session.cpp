#include <QtTest>

#include <QJsonArray>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>

#include "SessionStore.h"

using namespace loftail;

// M5 — session persistence (SPEC.md §10, ARCHITECTURE.md §12.4). The schema stores
// a `documents` ARRAY from day one even with a single element (invariant #7), so
// multi-file needs no migration later. Core-only (a QSettings ini in a temp dir).
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
};

void TestSession::documentsArrayRoundTrip()
{
    QTemporaryDir dir;
    const QString ini = dir.filePath(QStringLiteral("s.ini"));

    Session in;
    in.geometry = QByteArrayLiteral("GEOM");
    in.windowState = QByteArrayLiteral("STATE");
    in.activeDocument = 0;

    SessionDocument d;
    d.path = QStringLiteral("/logs/app.log");
    d.format.pattern = QStringLiteral("%d [%t] %-5p %c - %m%n");
    d.format.encoding = Encoding::Utf16LE;
    d.format.sourceZone.kind = ZoneChoice::Kind::Utc;
    d.format.displayZone.kind = ZoneChoice::Kind::Local;
    d.columnState = QByteArrayLiteral("COLS");
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

    {
        QSettings s(ini, QSettings::IniFormat);
        SessionStore::save(s, in);
    }

    QSettings s(ini, QSettings::IniFormat);
    const Session out = SessionStore::load(s);

    QCOMPARE(out.geometry, in.geometry);
    QCOMPARE(out.windowState, in.windowState);
    QCOMPARE(out.activeDocument, 0);
    QCOMPARE(out.documents.size(), 1); // stored as an array even with one element
    const SessionDocument &od = out.documents.first();
    QCOMPARE(od.path, d.path);
    QCOMPARE(od.format.pattern, d.format.pattern);
    QCOMPARE(int(od.format.encoding), int(Encoding::Utf16LE));
    QCOMPARE(int(od.format.sourceZone.kind), int(ZoneChoice::Kind::Utc));
    QCOMPARE(int(od.format.displayZone.kind), int(ZoneChoice::Kind::Local));
    QCOMPARE(od.columnState, d.columnState);
    QCOMPARE(od.filters.value(QStringLiteral("priorityEnabled")).toBool(), true);
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
    in.activeDocument = 1;
    SessionDocument a;
    a.path = QStringLiteral("/a.log");
    a.format.pattern = QStringLiteral("A%n");
    SessionDocument b;
    b.path = QStringLiteral("/b.log");
    b.format.pattern = QStringLiteral("B%n");
    in.documents = {a, b};
    {
        QSettings s(ini, QSettings::IniFormat);
        SessionStore::save(s, in);
    }

    QSettings s(ini, QSettings::IniFormat);
    const Session out = SessionStore::load(s);
    QCOMPARE(out.documents.size(), 2);
    QCOMPARE(out.documents.at(0).format.pattern, QStringLiteral("A%n"));
    QCOMPARE(out.documents.at(1).format.pattern, QStringLiteral("B%n"));
    QCOMPARE(out.active()->path, QStringLiteral("/b.log"));
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
    // added additively at schemaVersion 1, NOT by bumping the schema (which would
    // discard every saved session).
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
