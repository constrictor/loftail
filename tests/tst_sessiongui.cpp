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

#include <QDockWidget>

#include "FilterPane.h"
#include "Highlight.h"
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
    // A second log, for the seeded-rules pair below. Its own file, because those two
    // cases hand a session to each other and must not disturb the ones above.
    QTemporaryDir m_seedDir;
    QString       m_seedLog;

    // Drop whatever the constructor restored, so a case that opens its own file leaves
    // a session naming only that file. The preceding cases' logs live in temporary
    // directories that are gone by now, so what they restore is a waiting tab.
    static void closeEverything(MainWindow &w)
    {
        auto *closeAll = w.findChild<QAction *>(QStringLiteral("closeAllAction"));
        QVERIFY(closeAll);
        closeAll->trigger();
        QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 0);
    }

    static QJsonArray paneRules(MainWindow &w)
    {
        auto *hp = w.findChild<HighlighterPane *>();
        return hp ? hp->saveState().value(QStringLiteral("rules")).toArray() : QJsonArray();
    }

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

    // The level colours a log with nothing saved for it starts with (SPEC.md §7), and
    // the half of that promise that is easy to break: they are a SEED, not a floor.
    void aFreshlyOpenedLogArrivesWithTheLevelColours();
    void aDeletedDefaultRuleStaysDeletedAcrossARelaunch();
    void aRunClickAndAZoneChangeLeaveTheSeedRestorable();
};

void TestSessionGui::initTestCase()
{
    QVERIFY(m_logDir.isValid());
    QVERIFY(m_seedDir.isValid());
    m_seedLog = m_seedDir.filePath(QStringLiteral("seeded.log"));
    {
        QFile f(m_seedLog);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("2026-07-21 11:00:00,000 [main] INFO  net.io - fine\n");
        f.write("2026-07-21 11:00:01,000 [main] ERROR net.io - broken\n");
        f.close();
    }
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

        QCOMPARE(runs->count(), 5);      // "Last run" + "All runs" + 3 detected runs
        // Not row 4 — the newest run is what is SHOWN, but what is selected is the
        // standing "Last run" instruction that put it there (SPEC.md §3a).
        QCOMPARE(runs->currentRow(), RunPane::kLastRunRow);

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
        QCOMPARE(runs->count(), 5);                          // runs re-detected
        // "Last run" comes back as itself and not as the run it happened to resolve to
        // last time: saving that run would restore the session pinned to a run the
        // application has since finished, which is the one thing it exists not to do.
        QCOMPARE(runs->currentRow(), RunPane::kLastRunRow);
        w.close();
    }
}

void TestSessionGui::aFreshlyOpenedLogArrivesWithTheLevelColours()
{
    // A log loftail has never been told anything about opens with FATAL, ERROR and WARN
    // already coloured — the three a reader opens a log to find, which rendered exactly
    // like TRACE before (SPEC.md §7). Driven through the real MainWindow because that is
    // where the seed is applied: it is the one place that can ask whether anything has
    // ever been stored for this file.
    MainWindow w;
    w.resize(900, 600);
    w.show();
    closeEverything(w);
    w.openFile(m_seedLog);
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);
    QTest::qWait(300);

    const QJsonArray rules = paneRules(w);
    QCOMPARE(rules.size(), 3);
    for (const QJsonValue &v : rules) {
        const QJsonObject rule = v.toObject();
        QVERIFY(rule.value(QStringLiteral("enabled")).toBool());
        // Colour alone, which serializes as NO "actions" key at all — the same shape a
        // rule written before actions existed has, and the reason no schema version
        // moves to ship these.
        QVERIFY(!rule.contains(QStringLiteral("actions")));
        QVERIFY(rule.value(QStringLiteral("match")).toObject()
                    .value(QStringLiteral("priorityEnabled"))
                    .toBool());
    }

    w.close(); // saves a session naming m_seedLog, which the next case relaunches into
}

void TestSessionGui::aDeletedDefaultRuleStaysDeletedAcrossARelaunch()
{
    // THE ONE THAT MATTERS. A default that comes back after being deleted is a bug the
    // user cannot get rid of, so once the rules have been touched — including emptied —
    // the user's answer is what persists. It works because the seed asks whether the
    // session said ANYTHING about this file's rules, never whether what it said was
    // empty (the contains()-not-isEmpty() rule, three stores deep now).
    {
        MainWindow w;
        w.resize(900, 600);
        w.show();
        QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);
        QTest::qWait(300);
        // Restored, not re-seeded — a saved rule list comes back as itself.
        QCOMPARE(paneRules(w).size(), 3);

        auto *hp = w.findChild<HighlighterPane *>();
        QVERIFY(hp);
        auto *clear = hp->findChild<QPushButton *>(QStringLiteral("ruleClear"));
        QVERIFY(clear);
        clear->click(); // the user's own gesture: Clear removes every rule
        QCOMPARE(paneRules(w).size(), 0);

        w.close();
    }

    {
        MainWindow w;
        w.show();
        QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);
        QTest::qWait(300);
        QCOMPARE(paneRules(w).size(), 0);

        // Leave no file in the session: the log below this one lives in a QTemporaryDir
        // that dies with this object.
        closeEverything(w);
        w.close();
    }
}

void TestSessionGui::aRunClickAndAZoneChangeLeaveTheSeedRestorable()
{
    // The persistence half of bugs.md #5. Picking a run, and switching the timestamp
    // column between zones, both run MainWindow through HighlighterPane::
    // refreshTimeBounds() — which used to read the whole of the axis editor back into
    // whichever rule was selected. The rule then differed from the seed in two fields
    // nothing on screen showed, commit() put it on the Document, and the session wrote
    // it: the log came back marked on the next launch, for ever, with no gesture that
    // could put it back.
    //
    // Compared against HighlighterSet::defaults() rather than against a rule count or
    // the marker: the corruption never changed the number of rules, and a fix that
    // repaired only the time bounds would still leave loggerCoversAll rewritten.
    QJsonArray seeded;
    for (const HighlightRule &r : HighlighterSet::defaults().rules)
        seeded.append(r.toJson());

    {
        MainWindow w;
        w.resize(900, 600);
        w.show();
        closeEverything(w);
        w.openFile(m_sample);
        QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);
        QTest::qWait(300);
        QCOMPARE(paneRules(w), seeded);

        // A run row. "All runs" needs no run-start pattern and reaches exactly the same
        // MainWindow::onRunSelected() a detected run does.
        auto *runs = w.findChild<QListWidget *>(QStringLiteral("runList"));
        QVERIFY(runs);
        runs->setCurrentRow(RunPane::kAllRunsRow);
        QTest::qWait(50);
        QCOMPARE(paneRules(w), seeded);

        // ...and the timestamp menu, including the two entries that really do move the
        // display zone. The seeded rules name no time bound, so there is nothing for a
        // zone change to re-express and nothing may be written.
        for (const char *name : {"timeDisplayUtcAction", "timeDisplayLocalAction",
                                 "timeDisplaySecondsAction", "timeDisplayAsWrittenAction"}) {
            auto *action = w.findChild<QAction *>(QString::fromLatin1(name));
            QVERIFY2(action, name);
            action->trigger();
            QTest::qWait(20);
            QVERIFY2(paneRules(w) == seeded, name);
        }

        w.close(); // saves a session naming m_sample and its rules
    }

    {
        MainWindow w;
        w.show();
        QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);
        QTest::qWait(300);
        // What the reader sees on the next launch: the same three rules, and a
        // Highlighters tab with nothing to report about them.
        QCOMPARE(paneRules(w), seeded);
        auto *hp = w.findChild<HighlighterPane *>();
        QVERIFY(hp && !hp->hasCustomRules());
        auto *dock = w.findChild<QDockWidget *>(QStringLiteral("highlightersDock"));
        QVERIFY(dock);
        QVERIFY2(!dock->windowTitle().contains(QChar(0x2022)),
                 qPrintable(dock->windowTitle()));

        closeEverything(w);
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
