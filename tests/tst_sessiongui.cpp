#include <QtTest>

#include <QApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>

#include "FilterPane.h"
#include "HighlighterPane.h"
#include "LogView.h"
#include "MainWindow.h"

using namespace loftail;

// M5 — end-to-end session round-trip through the REAL MainWindow (SPEC.md §10). Two
// MainWindow instances in one process, sharing one isolated QSettings store, model
// "quit and relaunch": the first sets filters + highlighters and closes (saving the
// session); the second is constructed (restoring it) and must come back with the
// same file, filters and highlighters. A separate case proves a missing last file
// degrades to an empty view rather than an error. Runs under the offscreen platform.
class TestSessionGui : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_logDir;
    QString       m_sample;

    static QJsonObject highlightRulesJson()
    {
        QJsonObject rule;
        rule.insert(QStringLiteral("enabled"), true);
        rule.insert(QStringLiteral("matchPriority"), true);
        rule.insert(QStringLiteral("minPriority"), QStringLiteral("ERROR"));
        rule.insert(QStringLiteral("matchLogger"), false);
        rule.insert(QStringLiteral("background"), 0); // Red
        rule.insert(QStringLiteral("foreground"), -1);
        QJsonArray rules;
        rules.append(rule);
        QJsonObject o;
        o.insert(QStringLiteral("rules"), rules);
        return o;
    }

    static QJsonObject filterJson()
    {
        // Priority + text only, so the round-trip does not depend on discovered
        // subsystem timing (those are covered by tst_filter / tst_highlight).
        QJsonObject o;
        o.insert(QStringLiteral("priorityEnabled"), true);
        o.insert(QStringLiteral("minPriorityIndex"), 4); // ERROR
        o.insert(QStringLiteral("textEnabled"), true);
        o.insert(QStringLiteral("text"), QStringLiteral("boom"));
        return o;
    }

private slots:
    void initTestCase();
    void roundTripRestoresFileFiltersHighlighters();
    void missingLastFileIsGraceful();
};

void TestSessionGui::initTestCase()
{
    QVERIFY(m_logDir.isValid());
    m_sample = m_logDir.filePath(QStringLiteral("app.log"));
    QFile f(m_sample);
    QVERIFY(f.open(QIODevice::WriteOnly));
    // Matches the app's default log4cplus pattern, so open() needs no dialog.
    f.write("2026-07-21 10:00:00,000 [main] INFO  net.io - starting\n");
    f.write("2026-07-21 10:00:01,000 [work] ERROR db.pool - boom\n");
    f.write("2026-07-21 10:00:02,000 [work] WARN  db.pool - slow\n");
    f.close();
}

void TestSessionGui::roundTripRestoresFileFiltersHighlighters()
{
    // --- Round 1: open, configure, close (saves the session) ----------------
    {
        MainWindow w;
        w.resize(920, 640);
        w.show();
        w.openFile(m_sample);
        QTRY_VERIFY(w.findChild<LogView *>() != nullptr); // the file opened
        QTest::qWait(300);                                // let indexing finish

        auto *hp = w.findChild<HighlighterPane *>();
        QVERIFY(hp);
        hp->restoreState(highlightRulesJson());

        auto *fp = w.findChild<FilterPane *>();
        QVERIFY(fp);
        fp->restoreState(filterJson());

        w.close(); // triggers saveSession()
    }

    // --- Round 2: relaunch (restores the session) ---------------------------
    {
        MainWindow w; // constructor calls restoreSession()
        w.show();
        QTRY_VERIFY(w.findChild<LogView *>() != nullptr); // the last file reopened
        QTest::qWait(300);

        auto *hp = w.findChild<HighlighterPane *>();
        QVERIFY(hp);
        const QJsonArray rules = hp->saveState().value(QStringLiteral("rules")).toArray();
        QCOMPARE(rules.size(), 1);
        const QJsonObject rule = rules.first().toObject();
        QVERIFY(rule.value(QStringLiteral("matchPriority")).toBool());
        QCOMPARE(rule.value(QStringLiteral("minPriority")).toString(), QStringLiteral("ERROR"));
        QCOMPARE(rule.value(QStringLiteral("background")).toInt(), 0); // palette index, not RGB

        auto *fp = w.findChild<FilterPane *>();
        QVERIFY(fp);
        const QJsonObject ff = fp->saveState();
        QCOMPARE(ff.value(QStringLiteral("priorityEnabled")).toBool(), true);
        QCOMPARE(ff.value(QStringLiteral("minPriorityIndex")).toInt(), 4);
        QCOMPARE(ff.value(QStringLiteral("textEnabled")).toBool(), true);
        QCOMPARE(ff.value(QStringLiteral("text")).toString(), QStringLiteral("boom"));

        w.close(); // re-saves the session pointing at m_sample
    }
}

void TestSessionGui::missingLastFileIsGraceful()
{
    // The session now points at m_sample; delete it and relaunch. Restore must not
    // error or crash — it shows an empty view with an inline notice (SPEC.md §10).
    QVERIFY(QFile::remove(m_sample));

    MainWindow w;
    w.show();
    QTest::qWait(100);
    QVERIFY(w.findChild<LogView *>() == nullptr); // empty view, no file, no dialog
    w.close();
}

int main(int argc, char *argv[])
{
    // Isolate all persistent state (QSettings + AppConfigLocation presets) under a
    // throwaway config home so the round-trip runs against a clean, disposable store
    // and never touches the developer's real settings.
    QTemporaryDir configHome;
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
    qputenv("XDG_DATA_HOME", configHome.path().toUtf8());
    qputenv("HOME", configHome.path().toUtf8());
    qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("loftail-test"));
    QApplication::setApplicationName(QStringLiteral("loftail-test"));

    TestSessionGui tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_sessiongui.moc"
