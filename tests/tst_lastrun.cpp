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

#include <QApplication>
#include <QFile>
#include <QLineEdit>
#include <QListWidget>
#include <QTabWidget>
#include <QScrollBar>
#include <QTemporaryDir>

#include <QTableWidget>

#include "Document.h"
#include "DocumentContext.h"
#include "DocumentView.h"
#include "LiveController.h"
#include "LogModel.h"
#include "LogView.h"
#include "Highlight.h"
#include "HighlighterPane.h"
#include "MainWindow.h"
#include "RunPane.h"

using namespace loftail;

// "Follow the last" (SPEC.md §3a): the Runs pane's bottom entry and its default, which is not a
// run but a standing instruction to show whichever run is last. The document half — the
// sticky flag, the retarget, and the append still freezing at the boundary — is pinned
// core-side by tst_runselect. What only a WINDOW-level test can pin is the wiring that
// makes the instruction MEAN anything: that a live append which starts a new run
// re-applies the view onto it, that it does so for a tab in the BACKGROUND (the case the
// handler sits above the early return for), and that a run the user pinned is left alone
// by exactly the same tick.
//
// Ticks are driven through LiveController::checkNow(), like tst_multidoc: the watcher's
// own poll would make these wait on a timer for nothing. Widgets are found by OBJECT
// NAME, never by visible text.
// "Follow the last" is the BOTTOM row of the Runs pane's list (SPEC.md §3a), so its
// index is a function of how many runs the log turned out to hold rather than a
// constant — RunPane::followRow() is the same answer where the pane itself is in hand.
static int followRow(const QListWidget *list) { return list ? list->count() - 1 : -1; }

class TestLastRun : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;

    static constexpr auto kMarker = "Starting up";

    static QByteArray banner() { return "2026-07-21 10:00:00,000 [main] INFO  app - Starting up\n"; }
    static QByteArray line(const char *text)
    {
        return QByteArray("2026-07-21 10:00:00,000 [main] INFO  app - ") + text + "\n";
    }

    // One run of three records: a banner and two lines.
    static void writeOneRun(const QString &path)
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(banner());
        f.write(line("a0"));
        f.write(line("a1"));
        f.close();
    }

    // Two runs long enough that neither fits a viewport, so where a run OPENS is a
    // question with an observable answer: the first record is off screen at the end and
    // the last one is off screen at the start.
    static void writeTwoLongRuns(const QString &path)
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        for (int run = 0; run < 2; ++run) {
            f.write(banner());
            for (int i = 0; i < 80; ++i)
                f.write(line(qPrintable(QStringLiteral("r%1 line %2").arg(run).arg(i))));
        }
        f.close();
    }

    // The application restarts: a second run, of a banner and one line.
    static void appendNewRun(const QString &path)
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::Append));
        f.write(banner());
        f.write(line("b0"));
        f.close();
    }

    static QTabWidget *tabs(const MainWindow &w)
    {
        return w.findChild<QTabWidget *>(QStringLiteral("documentTabs"));
    }

    static DocumentContext *contextAt(const MainWindow &w, int tab)
    {
        auto *view = qobject_cast<DocumentView *>(tabs(w)->widget(tab));
        return view ? view->context() : nullptr;
    }

    static void waitUntilIndexed(const MainWindow &w)
    {
        QTRY_VERIFY([&w]() {
            QTabWidget *t = tabs(w);
            if (!t || t->count() == 0)
                return false;
            for (int i = 0; i < t->count(); ++i)
                if (t->tabText(i).contains(QStringLiteral("indexing")))
                    return false;
            return true;
        }());
    }

    // Split the ACTIVE file into runs through the pane's own field, which is the route
    // a user has and the one that ends in MainWindow::onRunStartChanged().
    static void typeRunPattern(MainWindow &w)
    {
        auto *pattern = w.findChild<QLineEdit *>(QStringLiteral("runStartPattern"));
        QVERIFY(pattern);
        pattern->setText(QString::fromLatin1(kMarker));
        QTest::keyClick(pattern, Qt::Key_Return);
    }

    static QListWidget *runList(const MainWindow &w)
    {
        return w.findChild<QListWidget *>(QStringLiteral("runList"));
    }

    static void tick(const MainWindow &w, int tab)
    {
        DocumentContext *ctx = contextAt(w, tab);
        QVERIFY(ctx && ctx->live);
        ctx->live->checkNow();
    }

private slots:
    void aNewRunMovesTheViewOntoIt();
    void aBackgroundTabFollowsTheNewRunToo();
    void aPinnedRunIsLeftWhereItIs();
    void aRestartDoesNotRewriteTheSeededHighlightRules();
    void aPinnedRunOpensAtItsEnd();
    void theLastRunOpensAtItsEndAndKeepsFollowing();
};

void TestLastRun::aNewRunMovesTheViewOntoIt()
{
    const QString path = m_dir.filePath(QStringLiteral("active.log"));
    writeOneRun(path);

    MainWindow w;
    w.resize(1000, 700);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    w.openFile(path);
    waitUntilIndexed(w);

    typeRunPattern(w);
    DocumentContext *ctx = contextAt(w, 0);
    QVERIFY(ctx);
    Document *doc = ctx->doc.get();
    QCOMPARE(doc->runs().size(), 1);
    QVERIFY(doc->followingLastRun());
    QCOMPARE(ctx->model->rowCount(), 3);

    appendNewRun(path);
    tick(w, 0);

    // The whole feature in one assertion: nobody clicked anything, and the view is on
    // the run the application has just started.
    QCOMPARE(doc->runs().size(), 2);
    QCOMPARE(doc->selectedRun(), 1);
    QCOMPARE(ctx->model->rowCount(), 2); // the new banner and its one line
    QCOMPARE(doc->filtered().sourceRow(0), 3);

    // ...and the pane is still on "Follow the last", not the ordinal it has landed on: the next
    // restart has to move it again.
    QListWidget *list = runList(w);
    QVERIFY(list);
    QCOMPARE(list->count(), 4); // "All runs" + two runs + "Follow the last"
    QCOMPARE(list->currentRow(), followRow(list));
    QVERIFY(doc->followingLastRun());
}

void TestLastRun::aBackgroundTabFollowsTheNewRunToo()
{
    const QString first = m_dir.filePath(QStringLiteral("background.log"));
    const QString second = m_dir.filePath(QStringLiteral("other.log"));
    writeOneRun(first);
    writeOneRun(second);

    MainWindow w;
    w.resize(1000, 700);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    w.openFile(first);
    waitUntilIndexed(w);
    typeRunPattern(w); // while tab 0 is the one the pane is bound to

    w.openFile(second); // ...which sends tab 0 to the background
    waitUntilIndexed(w);
    QCOMPARE(tabs(w)->currentIndex(), 1);

    DocumentContext *ctx = contextAt(w, 0);
    QVERIFY(ctx);
    Document *doc = ctx->doc.get();
    QCOMPARE(doc->runs().size(), 1);

    appendNewRun(first);
    tick(w, 0);

    // A log that restarts while its tab is behind another one must not be left on the
    // finished run: the tab is switched to minutes later and shows what it shows.
    QCOMPARE(doc->selectedRun(), 1);
    QCOMPARE(ctx->model->rowCount(), 2);

    // The pane is bound to the OTHER file throughout and must not have been repainted
    // with this one's runs.
    QListWidget *list = runList(w);
    QVERIFY(list);
    QCOMPARE(list->count(), 2); // tab 1 has no run pattern: the two fixed rows only
}

void TestLastRun::aPinnedRunIsLeftWhereItIs()
{
    const QString path = m_dir.filePath(QStringLiteral("pinned.log"));
    writeOneRun(path);

    MainWindow w;
    w.resize(1000, 700);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    w.openFile(path);
    waitUntilIndexed(w);
    typeRunPattern(w);

    DocumentContext *ctx = contextAt(w, 0);
    QVERIFY(ctx);
    Document *doc = ctx->doc.get();

    // Picking the run that happens to be last is NOT the same gesture as following it,
    // and the difference is exactly what happens next.
    QListWidget *list = runList(w);
    QVERIFY(list);
    list->setCurrentRow(RunPane::kFirstRunRow);
    QVERIFY(!doc->followingLastRun());
    QCOMPARE(doc->selectedRun(), 0);

    appendNewRun(path);
    tick(w, 0);

    QCOMPARE(doc->runs().size(), 2); // listed...
    QCOMPARE(doc->selectedRun(), 0); // ...but not switched to
    QCOMPARE(ctx->model->rowCount(), 3);
    QCOMPARE(list->currentRow(), RunPane::kFirstRunRow);
}

// The tick above goes through MainWindow::followLastRunIfMoved(), which re-renders the
// two panes' time bounds because the run baseline the seconds modes count from has just
// moved. HighlighterPane::refreshTimeBounds() used to answer that by reading the whole
// axis editor back into the selected rule, so a log that simply RESTARTED rewrote its
// seeded highlight rules, marked the Highlighters tab and persisted both — with nobody
// having touched anything, and once per tick for as long as it kept restarting
// (bugs.md #5).
void TestLastRun::aRestartDoesNotRewriteTheSeededHighlightRules()
{
    const QString path = m_dir.filePath(QStringLiteral("seeded.log"));
    writeOneRun(path);

    MainWindow w;
    w.resize(1000, 700);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    w.openFile(path);
    waitUntilIndexed(w);
    typeRunPattern(w);

    DocumentContext *ctx = contextAt(w, 0);
    QVERIFY(ctx);
    Document *doc = ctx->doc.get();
    // A rule is selected in the pane, which is the state the write-back happened in.
    auto *table = w.findChild<QTableWidget *>(QStringLiteral("ruleTable"));
    QVERIFY(table);
    QCOMPARE(table->rowCount(), HighlighterSet::defaults().rules.size());
    table->setCurrentCell(0, HighlighterPane::kColRule);
    QCOMPARE(doc->highlighters().rules, HighlighterSet::defaults().rules);

    appendNewRun(path);
    tick(w, 0);

    QCOMPARE(doc->selectedRun(), 1); // the retarget really did happen...
    QCOMPARE(doc->highlighters().rules, HighlighterSet::defaults().rules); // ...and cost nothing
    auto *pane = w.findChild<HighlighterPane *>();
    QVERIFY(pane && !pane->hasCustomRules());
}

// Selecting a run opens it at its END (SPEC.md §3a). A run is picked because of how it
// went, and what went wrong is the last thing in it — a crash writes its stack and stops
// — so opening at the first record showed the one part nobody was looking for and left
// the reader scrolling the whole run to reach the part they were. The assertion has to be
// on the SCROLL POSITION as well as on the selected record: the record is what makes the
// view land there, and the position is the thing the reader actually sees.
void TestLastRun::aPinnedRunOpensAtItsEnd()
{
    const QString path = m_dir.filePath(QStringLiteral("tail-pinned.log"));
    writeTwoLongRuns(path);

    MainWindow w;
    w.resize(1000, 700);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    w.openFile(path);
    waitUntilIndexed(w);
    typeRunPattern(w);

    DocumentContext *ctx = contextAt(w, 0);
    QVERIFY(ctx);
    QCOMPARE(ctx->doc->runs().size(), 2);
    auto *view = qobject_cast<DocumentView *>(tabs(w)->widget(0));
    QVERIFY(view);
    LogView *lv = view->logView();
    QVERIFY(lv);

    QListWidget *list = runList(w);
    QVERIFY(list);
    list->setCurrentRow(RunPane::kFirstRunRow); // run 0: earlier, finished, pinned

    QCOMPARE(ctx->doc->selectedRun(), 0);
    const int rows = ctx->model->rowCount();
    QCOMPARE(rows, 81); // the banner and its 80 lines
    // The run does not fit, or there would be nothing to open AT.
    QVERIFY(lv->verticalScrollBar()->maximum() > 0);
    QCOMPARE(lv->currentRecord(), rows - 1);
    QCOMPARE(lv->verticalScrollBar()->value(), lv->verticalScrollBar()->maximum());
}

// The same for the two MODE rows — "All runs" and "Follow the last" — which is the same
// question ("what happened at the end?") asked of a moving target: both open on the last
// record of what they select and then FOLLOW it, so the end keeps moving under the
// reader. The selected record is asserted as well as the scroll position: scrolling to
// the end alone left the current record wherever the previous choice had put it, so the
// first arrow key jumped the reader back out of the tail they had just been given.
void TestLastRun::theLastRunOpensAtItsEndAndKeepsFollowing()
{
    const QString path = m_dir.filePath(QStringLiteral("tail-last.log"));
    writeTwoLongRuns(path);

    MainWindow w;
    w.resize(1000, 700);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    w.openFile(path);
    waitUntilIndexed(w);
    typeRunPattern(w);

    DocumentContext *ctx = contextAt(w, 0);
    QVERIFY(ctx);
    auto *view = qobject_cast<DocumentView *>(tabs(w)->widget(0));
    QVERIFY(view);
    LogView *lv = view->logView();
    QVERIFY(lv);

    QListWidget *list = runList(w);
    QVERIFY(list);
    const auto landsAtTheEnd = [lv]() {
        QVERIFY(lv->verticalScrollBar()->maximum() > 0); // ...or there is no end to land at
        QCOMPARE(lv->verticalScrollBar()->value(), lv->verticalScrollBar()->maximum());
        QCOMPARE(lv->currentRecord(), lv->recordCount() - 1);
        QVERIFY(lv->following());
    };

    list->setCurrentRow(RunPane::kFirstRunRow);       // away from the end...
    list->setCurrentRow(RunPane::kFirstRunRow + 1);   // ...and onto the newest run
    QCOMPARE(ctx->doc->selectedRun(), 1);
    QCOMPARE(ctx->model->rowCount(), 81);
    landsAtTheEnd();

    // "Follow the last" is a standing instruction rather than an ordinal, and "All runs" lifts
    // the restriction altogether — both still open on the end of what they show.
    list->setCurrentRow(RunPane::kFirstRunRow);
    list->setCurrentRow(followRow(list));
    QVERIFY(ctx->doc->followingLastRun());
    QCOMPARE(ctx->model->rowCount(), 81);
    landsAtTheEnd();

    list->setCurrentRow(RunPane::kFirstRunRow);
    list->setCurrentRow(RunPane::kAllRunsRow);
    QCOMPARE(ctx->model->rowCount(), 162); // both runs, whole file
    landsAtTheEnd();
}

int main(int argc, char *argv[])
{
    // Isolate every persistent store — the session, the settings tree — under a
    // throwaway config home, so these windows never restore or write the developer's.
    QTemporaryDir configHome;
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
    qputenv("XDG_DATA_HOME", configHome.path().toUtf8());
    qputenv("HOME", configHome.path().toUtf8());
    qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("loftail-test"));
    QApplication::setApplicationName(QStringLiteral("loftail-test-lastrun"));

    TestLastRun tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_lastrun.moc"
