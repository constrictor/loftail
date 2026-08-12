#include <QtTest>

#include <QApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QAction>
#include <QTemporaryDir>

#include <QListWidget>
#include <QLineEdit>
#include <QPushButton>

#include "FilterPane.h"
#include "HighlighterPane.h"
#include "LogView.h"
#include "MainWindow.h"
#include "RunPane.h"

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
        // M15 — filter context. Two additive keys in the SAME object, which is why
        // this rides a session written at schema 3 with no version bump.
        o.insert(QStringLiteral("contextBefore"), 3);
        o.insert(QStringLiteral("contextAfter"), 1);
        return o;
    }

private slots:
    void initTestCase();
    void roundTripRestoresFileFiltersHighlighters();
    void missingLastFileRestoresAsWaiting();
    void runSelectionThroughUiAndPersists();
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
        QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1); // the file opened
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
        // Exactly one: restoring one saved file must not resurrect a second view.
        QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1); // the last file reopened
        QTest::qWait(300);

        auto *hp = w.findChild<HighlighterPane *>();
        QVERIFY(hp);
        const QJsonArray rules = hp->saveState().value(QStringLiteral("rules")).toArray();
        QCOMPARE(rules.size(), 1);
        const QJsonObject rule = rules.first().toObject();
        // highlightRulesJson() is deliberately in the ORIGINAL two-axis rule shape, so
        // this also exercises the backward-compatible read through the real session
        // path; it comes back out in the current nested form.
        const QJsonObject match = rule.value(QStringLiteral("match")).toObject();
        QVERIFY(match.value(QStringLiteral("priorityEnabled")).toBool());
        QCOMPARE(match.value(QStringLiteral("minPriorityIndex")).toInt(), 4); // ERROR
        QCOMPARE(rule.value(QStringLiteral("background")).toInt(), 0); // palette index, not RGB

        auto *fp = w.findChild<FilterPane *>();
        QVERIFY(fp);
        const QJsonObject ff = fp->saveState();
        QCOMPARE(ff.value(QStringLiteral("priorityEnabled")).toBool(), true);
        QCOMPARE(ff.value(QStringLiteral("minPriorityIndex")).toInt(), 4);
        QCOMPARE(ff.value(QStringLiteral("textEnabled")).toBool(), true);
        QCOMPARE(ff.value(QStringLiteral("text")).toString(), QStringLiteral("boom"));
        QCOMPARE(ff.value(QStringLiteral("contextBefore")).toInt(), 3);
        QCOMPARE(ff.value(QStringLiteral("contextAfter")).toInt(), 1);

        w.close(); // re-saves the session pointing at m_sample
    }
}

void TestSessionGui::missingLastFileRestoresAsWaiting()
{
    // The session now points at m_sample; delete it and relaunch. Restore must not
    // error, crash or raise a dialog (SPEC.md §10).
    //
    // M13: it comes back as a WAITING tab rather than as no tab at all. The old
    // behaviour dropped the file, and since saveSession() writes only the files that
    // are open, that forgot it permanently at the next quit.
    QVERIFY(QFile::remove(m_sample));

    MainWindow w;
    w.show();
    QTest::qWait(100);
    QCOMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1); // the tab is there, and waiting
    auto *view = w.findChild<LogView *>(QStringLiteral("logView"));
    QVERIFY(view);
    QCOMPARE(view->recordCount(), 0);
    QVERIFY(!view->placeholderText().isEmpty()); // it says why, in the view itself

    // Close it so the session this test leaves behind names no file — the cases after
    // this one open their own, and a waiting tab would now persist across all of them.
    auto *closeAll = w.findChild<QAction *>(QStringLiteral("closeAllAction"));
    QVERIFY(closeAll);
    closeAll->trigger();
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 0);
    w.close();
}

void TestSessionGui::runSelectionThroughUiAndPersists()
{
    // Drive the real Run pane through MainWindow: type a run-start regexp, Apply, and
    // confirm the run list populates and the newest run is selected — then relaunch
    // and confirm the run-start pattern was remembered per-file (§3a). Self-contained
    // (its own file), and last, so it doesn't disturb the session-pointing tests.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("multi.log"));
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        for (int r = 0; r < 3; ++r) {
            f.write(QStringLiteral("2026-07-21 10:0%1:00,000 [main] INFO  app - RUN START %1\n")
                        .arg(r).toUtf8());
            f.write(QStringLiteral("2026-07-21 10:0%1:01,000 [work] INFO  svc - work\n")
                        .arg(r).toUtf8());
        }
        f.close();
    }

    // --- Round 1: open, type the pattern, apply -----------------------------
    {
        MainWindow w;
        w.resize(900, 600);
        w.show();
        w.openFile(path);
        QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);
        QTest::qWait(300); // let indexing finish

        auto *rp = w.findChild<RunPane *>();
        QVERIFY(rp);
        auto *edit = rp->findChild<QLineEdit *>();
        auto *apply = rp->findChild<QPushButton *>();
        auto *runs = rp->findChild<QListWidget *>(QStringLiteral("runList"));
        QVERIFY(edit && apply && runs);

        edit->setText(QStringLiteral("RUN START"));
        apply->click();
        QTest::qWait(50);

        QCOMPARE(runs->count(), 4);       // "All runs" + 3 detected runs
        QCOMPARE(runs->currentRow(), 3);  // newest selected by default

        w.close(); // saves the session incl. the run-start pattern + selection
    }

    // --- Round 2: relaunch, the pattern is remembered per-file --------------
    {
        MainWindow w;
        w.show();
        QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);
        QTest::qWait(300);

        auto *rp = w.findChild<RunPane *>();
        QVERIFY(rp);
        auto *edit = rp->findChild<QLineEdit *>();
        auto *runs = rp->findChild<QListWidget *>(QStringLiteral("runList"));
        QVERIFY(edit && runs);
        QCOMPARE(edit->text(), QStringLiteral("RUN START")); // restored
        QCOMPARE(runs->count(), 4);                          // runs re-detected
        w.close();
    }
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
