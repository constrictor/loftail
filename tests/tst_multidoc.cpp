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

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QDockWidget>
#include <QFrame>
#include <QHeaderView>
#include <QFile>
#include <QFontDatabase>
#include <QSettings>
#include <QSignalSpy>
#include <QStatusBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QToolButton>
#include <QImage>
#include <QLabel>
#include <QListWidget>
#include <QAbstractButton>
#include <QDialog>
#include <QPushButton>
#include <QTimer>

#include <functional>
#include <utility>

#include "ConfigReset.h"
#include "AxisEditor.h"
#include "CopyHighlightersDialog.h"
#include "HighlighterPane.h"
#include "Document.h"
#include "DocumentContext.h"
#include "DocumentView.h"
#include "FilterPane.h"
#include "Highlight.h"
#include "LiveController.h"
#include "LogFormat.h"
#include "LogModel.h"
#include "Fonts.h"
#include "LogView.h"
#include "MainWindow.h"
#include "CommandLine.h"

#include "FakeFetcher.h"
#include "MatchCriteria.h"

using namespace loftail;

// activeDocumentChanged carries a Document*, which QSignalSpy can only record once
// the type is known to the meta-object system (Document is not a QObject).
Q_DECLARE_METATYPE(loftail::Document *)

// M9 — several logs open at once as tabs, and several views onto one log (SPEC.md
// §3, §5a; ARCHITECTURE.md §12). Drives the REAL MainWindow under the offscreen
// platform, like tst_sessiongui and tst_openflow.
//
// The distinctions these cases exist to pin down:
//   * opening a second file ADDS a tab — it does not replace the first;
//   * switching between two FILES rebinds the panes, switching between two VIEWS of
//     one file does not (they share the filters those panes edit);
//   * a file closes with its LAST view, not its first;
//   * the whole tab arrangement round-trips through quit-and-relaunch.
class TestMultiDoc : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QString       m_a;
    QString       m_b;
    // Two logs with ONE basename, in sibling directories — the case a tab bar cannot
    // report by name alone.
    QString       m_svcA;
    QString       m_svcB;

    static void writeLog(const QString &path, const char *subsystem, int lines)
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        for (int i = 0; i < lines; ++i) {
            // The app's default log4cplus pattern, so opening needs no dialog.
            f.write(QStringLiteral("2026-07-21 10:00:%1,000 [main] INFO  %2 - line %3\n")
                        .arg(i % 60, 2, 10, QLatin1Char('0'))
                        .arg(QLatin1String(subsystem))
                        .arg(i)
                        .toUtf8());
        }
        f.close();
    }

    // The document well. Open files are its pages; the side panes are docks and
    // therefore not in it, which is the point of the separation.
    static QTabWidget *tabs(const MainWindow &w)
    {
        return w.findChild<QTabWidget *>(QStringLiteral("documentTabs"));
    }

    static int tabCount(const MainWindow &w)
    {
        QTabWidget *t = tabs(w);
        return t ? t->count() : -1;
    }

    // The strip above the document well: what an open that made no tab says for itself
    // (SPEC.md §3). By object name, never by its text — the text is what is asserted.
    static QLabel *noticeLabel(const MainWindow &w)
    {
        return w.findChild<QLabel *>(QStringLiteral("openNoticeText"));
    }
    static QString noticeText(const MainWindow &w)
    {
        QLabel *label = noticeLabel(w);
        return label ? label->text() : QString();
    }

    static void trigger(const MainWindow &w, const char *actionName)
    {
        QAction *a = w.findChild<QAction *>(QLatin1String(actionName));
        QVERIFY(a);
        QVERIFY(a->isEnabled());
        a->trigger();
    }

    // Indexing runs on a worker thread, and a tab shows its progress in its title
    // while it does. Waiting for every title to settle is the observable "all files
    // are indexed" condition, which the record-level assertions below need.
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

private slots:
    void initTestCase();
    // Each case builds its own MainWindow, whose constructor restores whatever
    // session is stored — so start every one from a clean store.
    void init();
    void documentsAndPanesLiveInSeparateShells();
    void aPaneDragMovesThatPaneAlone();
    void oneOpenFileIsEnoughToEnableTheFileActions();
    void secondFileOpensAsAnotherTab();
    void reopeningAnOpenFileRaisesItInsteadOfDuplicating();
    void newViewSharesOneDocumentAndModel();
    void switchingViewsOfOneFileDoesNotRebindPanes();
    void closingATabLeavesTheOtherFileOpen();
    void fileClosesWithItsLastViewOnly();
    void closingEverythingUnbindsThePanes();

    // Timestamp display modes (SPEC.md §4). The mode is per FILE and the menu lives
    // on the timestamp column's header, so scope and persistence are the things a
    // window-level test can pin that the core-level rendering tests cannot.
    void timestampModeIsPerFileNotPerView();
    void timestampModeSharedAcrossViewsOfOneFile();
    void timestampModeSurvivesRestart();
    void theGapColumnRendersTheIntervalBetweenTheRowsAsShown();

    // M19 — the tab marker. A rule carrying HighlightAction::Tab marks its tab when a
    // match arrives while that log is not the one on screen (SPEC.md §7). Asserted on
    // the BACKGROUND tab throughout: the active tab's other half of the gate is
    // isActiveWindow(), which is not reliable under the offscreen platform.
    void aMatchInABackgroundTabMarksIt();
    void aRuleWithoutTheTabActionMarksNothing();
    void returningToTheTabClearsTheMark();
    void theTabMarkerIsNotTheWordIndexing();

    // Column widths (SPEC.md §5). The seed that matters is the one taken when the SCAN
    // finishes and the intern tables are complete, which only a real window runs — and
    // the two header-menu commands are actions on that window like any other.
    void theSubsystemColumnWidensOnceTheScanHasSeenEveryName();
    void theScanCompletionSeedLeavesTheMessageColumnOnScreen();
    void theHeaderMenuFitsAndResetsTheColumnWidths();
    // And the seed nobody ran: a log that opened WAITING has no columns to seed until
    // its format settles, which happens on a resume rather than on a scan (bugs.md 29).
    void aLogThatOpensWaitingGetsItsHeaderAndItsWidthsWhenItArrives();

    // SEVERAL logs asked for in ONE gesture (SPEC.md §3). Dropping files on the window
    // had always opened all of them; the command line took the first and dropped the
    // rest, and File ▸ Open could name only one. All three now go through openFiles(),
    // so what happens to the tab bar and to a refusal is one answer, not three.
    void severalFilesOpenedAtOnceBecomeSeveralTabs();
    void aRefusedLogAmongSeveralLeavesTheOthersOpenAndIsNamedWithThem();
    void aRefusalOutlivesTheTicksAndTabSwitchesThatFollowItUntilDismissed();
    void anAddressWithNoFileNamePartIsStillNamedInTheStrip();
    void aWaitingLogWithNoFileNamePartStillHasALabelATitleAndAStatus();

    // What a tab's stashed filter state MEANS when its log had not been indexed yet
    // (SPEC.md §6). The panes are stashed on the way out of a tab, and opening the
    // next log leaves that tab immediately — so the state stashed for every log but
    // the last one lists no subsystems at all, and reading that back as a selection
    // left every one of those tabs showing none of its records. The two cases below
    // must NOT wait for indexing between the opens: that is the whole condition, and
    // it is why the multi-open cases above are green with the bug in place.
    void everyTabKeepsItsRecordsWhenTheOpensRunTogether();
    void aSecondLogOpenedByHandLeavesTheFirstItsRecords();
    void anEmptySelectionTheUserChoseSurvivesATabSwitchAndARelaunch();

    // Tab labels (SPEC.md §3). What a log is called depends on which OTHER logs are
    // open, so the claims are about the set: two app.logs say which is which, closing
    // one shortens the survivor again, and the view numbering composes with both. The
    // rule itself, over every shape of address, is tst_tablabels'.
    void twoLogsWithOneBasenameEachShowTheirOwnDirectory();
    void closingOneOfThemShortensTheSurvivorBackToItsPlainName();
    void theViewNumberingComposesWithTheDistinguisher();

    // --- The two high-frequency gestures (SPEC.md §5) ---------------------------
    // Wrap has a key now, and log text has a size. Driven through the real window,
    // because what both cases are about is that ONE path does the work: the wrap key
    // triggers the menu's own mode action, and the size is the application's rather
    // than any view's.
    void theWrapKeyTogglesBetweenOffAndAlwaysOnThroughTheMenusOwnAction();
    void theWrapKeyFromSelectedRecordOnlyWrapsEverything();
    void zoomingResizesEveryOpenViewIncludingTheDigestStrip();
    void theZoomStopsAtItsBoundsAndComesBackOnReset();
    void theChosenTextSizeSurvivesARestart();
    void theCommandLineTakesEveryFileNamedAndOnePatternForThemAll();

    // A log replaced behind the application says so, in the status BAR's own temporary
    // message and for the ACTIVE tab only (SPEC.md §3). Both halves need a real window
    // with two tabs, which is what this file is: the classification itself is tst_tail's
    // (core, POSIX-only), and what is left to pin here is that the sentence reaches the
    // bar at all, and that a background tab's rotation does not spend the one status bar
    // on a log nobody is looking at. The second case is the one a broken implementation
    // satisfies silently — a notice that always fires passes every assertion about a
    // notice that fires.
    void aReplacedLogSaysSoInTheStatusBar();
    void aRotationInABackgroundTabSaysNothing();

    // Taking another open log's whole rule list (SPEC.md §7). The window's half: which
    // logs are on offer, what the two entry points are live in, and that the copy is
    // remembered for the log like any other rule edit.
    void copyingHighlightersFromAnotherTabReplacesThisLogsRules();
    void cancellingTheCopyLeavesTheRulesAlone();
    void bothEntryPointsRunTheSameGesture();
    void theCopyCommandIsDeadWithOnlyOneLogOpen();
    void aLogOpenInTwoTabsIsOfferedOnce();
    void copiedRulesSurviveClosingAndReopeningTheLog();
};

namespace {
// The model behind a view. LogView hands its LogModel to the QHeaderView, which is
// the only public route back to it.
LogModel *modelOf(LogView *view)
{
    return view ? qobject_cast<LogModel *>(view->header()->model()) : nullptr;
}

// Which timestamp mode the menu currently shows as chosen, by objectName.
QString checkedTimeDisplay(const MainWindow &w)
{
    for (const char *name : {"timeDisplayAsWrittenAction", "timeDisplayLocalAction",
                             "timeDisplayUtcAction", "timeDisplaySecondsAction",
                             "timeDisplayRunSecondsAction",
                             "timeDisplaySincePreviousAction"}) {
        QAction *a = w.findChild<QAction *>(QLatin1String(name));
        if (a && a->isChecked())
            return QLatin1String(name);
    }
    return QString();
}
} // namespace

void TestMultiDoc::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_a = m_dir.filePath(QStringLiteral("a.log"));
    m_b = m_dir.filePath(QStringLiteral("b.log"));
    writeLog(m_a, "net.io", 30);
    writeLog(m_b, "db.pool", 20);

    QDir d(m_dir.path());
    QVERIFY(d.mkpath(QStringLiteral("svc-a")));
    QVERIFY(d.mkpath(QStringLiteral("svc-b")));
    m_svcA = m_dir.filePath(QStringLiteral("svc-a/app.log"));
    m_svcB = m_dir.filePath(QStringLiteral("svc-b/app.log"));
    writeLog(m_svcA, "net.io", 12);
    writeLog(m_svcB, "net.io", 12);
}

void TestMultiDoc::init()
{
    QSettings s;
    s.remove(QStringLiteral("session"));
    s.sync();
    // AND EVERY STORE A LOG'S SETTINGS CAN BE IN. Since M21 a log's filters, its rules
    // and its run outlive the tab — that is the point of them — so a case that opens the
    // same path as an earlier one would otherwise inherit whatever that one left behind,
    // and pass or fail on the order QtTest happened to run them in.
    clearLogSettings();
}

void TestMultiDoc::documentsAndPanesLiveInSeparateShells()
{
    // The separation itself (ARCHITECTURE.md §12.2): open files are pages of the
    // central document well and the side panes are dock widgets, so neither Qt's dock
    // dragging nor the tab bar can put one inside the other. Dragging cannot be
    // exercised offscreen; the structure that forbids it can.
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_a);
    w.openFile(m_b);
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 2);

    QTabWidget *t = tabs(w);
    QVERIFY(t);
    // The well is the CENTRAL widget — the one region Qt's dock areas cannot claim.
    QVERIFY(w.centralWidget());
    QVERIFY(w.centralWidget() == t || w.centralWidget()->isAncestorOf(t));

    // Every open file is a page of it...
    QCOMPARE(t->count(), 2);
    for (int i = 0; i < t->count(); ++i)
        QVERIFY(t->widget(i)->findChild<LogView *>(QStringLiteral("logView")));

    // ...and the docks are the panes and nothing else: no log inside a dock, no dock
    // inside the well.
    const QList<QDockWidget *> docks = w.findChildren<QDockWidget *>();
    QVERIFY(!docks.isEmpty()); // the panes are still dockable among themselves
    for (QDockWidget *d : docks) {
        // Unnamed on purpose, unlike every other LogView lookup in this file: the claim
        // is that a dock holds NO log view of any kind, digest strip included.
        QVERIFY2(!d->findChild<LogView *>(), qPrintable(d->objectName()));
        QVERIFY2(!t->isAncestorOf(d), qPrintable(d->objectName()));
    }
}

void TestMultiDoc::aPaneDragMovesThatPaneAlone()
{
    // GroupedDragging makes a drag on ANY dock's title bar move that dock's whole
    // tab group — so pulling the Filters pane out took Highlighters, Presets and
    // Runs with it. The panes are tabbed together by default, which is exactly the
    // arrangement that turns it into a trap.
    MainWindow w;
    w.resize(900, 600);
    w.show();
    QVERIFY(!w.dockOptions().testFlag(QMainWindow::GroupedDragging));

    // And a pane belongs at one side or the other (SPEC.md §8), never as a strip
    // above or below the log. Restricting areas is only safe without GroupedDragging,
    // which is why the two assertions live together.
    const QList<QDockWidget *> docks = w.findChildren<QDockWidget *>();
    QVERIFY(!docks.isEmpty());
    for (QDockWidget *d : docks) {
        QCOMPARE(d->allowedAreas(),
                 Qt::DockWidgetAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea));
        QVERIFY2(d->features().testFlag(QDockWidget::DockWidgetMovable),
                 qPrintable(d->objectName()));
    }
}

void TestMultiDoc::oneOpenFileIsEnoughToEnableTheFileActions()
{
    // The per-file actions are enabled from the active view, and the FIRST tab added
    // becomes current the instant it is added — before the window has finished
    // recording it. Close All, which keys off the view list rather than the active
    // view, is the one that catches an ordering mistake there.
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_a);
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);

    for (const char *name : {"closeTabAction", "closeAllAction", "newViewAction"}) {
        QAction *a = w.findChild<QAction *>(QLatin1String(name));
        QVERIFY(a);
        QVERIFY2(a->isEnabled(), name);
    }
}

void TestMultiDoc::secondFileOpensAsAnotherTab()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();

    w.openFile(m_a);
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);

    w.openFile(m_b);
    // The point of the milestone: the first file is still open.
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 2);
    QCOMPARE(tabCount(w), 2);

    // The file just opened is the visible, active one — its name is in the title.
    QVERIFY(w.windowTitle().endsWith(QStringLiteral("b.log")));
}

void TestMultiDoc::reopeningAnOpenFileRaisesItInsteadOfDuplicating()
{
    MainWindow w;
    w.show();
    w.openFile(m_a);
    w.openFile(m_b);
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 2);

    w.openFile(m_a); // already open
    QCOMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 2); // no third view
    QVERIFY(w.windowTitle().endsWith(QStringLiteral("a.log"))); // but it is raised
}

void TestMultiDoc::newViewSharesOneDocumentAndModel()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_a);
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);

    trigger(w, "newViewAction");
    QCOMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 2);
    QCOMPARE(tabCount(w), 2);

    // Two views, ONE file: the index, filters and highlighters are shared, so the
    // status bar still reports a single document's record count.
    QVERIFY(w.windowTitle().endsWith(QStringLiteral("a.log")));

    // The views are independent in what they show: moving one leaves the other.
    waitUntilIndexed(w); // records must exist before a record can be selected
    const QList<LogView *> views = w.findChildren<LogView *>(QStringLiteral("logView"));
    views.at(0)->setCurrentRecord(0);
    views.at(1)->setCurrentRecord(20);
    QCOMPARE(views.at(0)->currentRecord(), 0);
    QCOMPARE(views.at(1)->currentRecord(), 20);
}

void TestMultiDoc::switchingViewsOfOneFileDoesNotRebindPanes()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_a);
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);

    QSignalSpy spy(&w, &MainWindow::activeDocumentChanged);

    // A second view onto the SAME file: the panes edit that file's filters and
    // highlighters, which both views share, so rebinding them would be wrong (and
    // would reset the filter pane's discovered-value state).
    trigger(w, "newViewAction");
    QCOMPARE(spy.count(), 0);

    // A second FILE does rebind them.
    w.openFile(m_b);
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 3);
    QCOMPARE(spy.count(), 1);
}

void TestMultiDoc::closingATabLeavesTheOtherFileOpen()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_a);
    w.openFile(m_b);
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 2);

    trigger(w, "closeTabAction"); // closes b.log, the active tab
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);
    QCOMPARE(tabCount(w), 1);
    QVERIFY(w.windowTitle().endsWith(QStringLiteral("a.log")));
}

void TestMultiDoc::fileClosesWithItsLastViewOnly()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_a);
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);
    trigger(w, "newViewAction");
    QCOMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 2);

    QSignalSpy spy(&w, &MainWindow::activeDocumentChanged);

    // Closing ONE of two views leaves the file open — nothing unbinds.
    trigger(w, "closeTabAction");
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);
    QVERIFY(w.windowTitle().endsWith(QStringLiteral("a.log")));
    // ...and the survivor drops the "[1]" it wore only to be told apart from its twin
    // (the title may still carry an indexing suffix, which is why this is not exact).
    QVERIFY2(!tabs(w)->tabText(0).contains(QLatin1Char('[')),
             qPrintable(tabs(w)->tabText(0)));
    for (const QList<QVariant> &args : spy)
        QVERIFY(args.at(0).value<Document *>() != nullptr); // never "no file open"

    // Closing the last one closes the file.
    trigger(w, "closeTabAction");
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 0);
    QCOMPARE(w.windowTitle(), QStringLiteral("loftail"));
}

void TestMultiDoc::closingEverythingUnbindsThePanes()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_a);
    w.openFile(m_b);
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 2);

    QSignalSpy spy(&w, &MainWindow::activeDocumentChanged);
    trigger(w, "closeAllAction");

    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 0);
    QCOMPARE(tabCount(w), 0);
    // The panes must be told there is no document (invariant #7).
    QVERIFY(spy.count() >= 1);
    QCOMPARE(spy.last().at(0).value<Document *>(), nullptr);
    auto *fp = w.findChild<FilterPane *>();
    QVERIFY(fp);
    QVERIFY(!fp->isEnabled()); // FilterPane::setDocument(nullptr) disables it
}

void TestMultiDoc::timestampModeIsPerFileNotPerView()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_a);
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);
    waitUntilIndexed(w);

    QCOMPARE(checkedTimeDisplay(w), QStringLiteral("timeDisplayAsWrittenAction"));
    trigger(w, "timeDisplayUtcAction");
    QCOMPARE(checkedTimeDisplay(w), QStringLiteral("timeDisplayUtcAction"));

    // A second FILE carries its own mode; choosing one for a.log must not leak.
    w.openFile(m_b);
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 2);
    waitUntilIndexed(w);
    QCOMPARE(checkedTimeDisplay(w), QStringLiteral("timeDisplayAsWrittenAction"));
    trigger(w, "timeDisplayRunSecondsAction");

    // Back to a.log: its own choice is intact and the checkmark follows the tab.
    w.openFile(m_a); // already open, so this raises it
    QTRY_COMPARE(checkedTimeDisplay(w), QStringLiteral("timeDisplayUtcAction"));
    w.openFile(m_b);
    QTRY_COMPARE(checkedTimeDisplay(w), QStringLiteral("timeDisplayRunSecondsAction"));
}

// The gap mode end to end: the menu entry, applySettings and a repaint (SPEC.md §4).
// A TimeDisplay change may never reach a rescan or a reparse, so the records the view
// is holding must be exactly the ones it had — what moves is the digits in one column.
void TestMultiDoc::theGapColumnRendersTheIntervalBetweenTheRowsAsShown()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_a);
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);
    waitUntilIndexed(w);

    LogModel *model = modelOf(w.findChildren<LogView *>(QStringLiteral("logView")).first());
    QVERIFY(model);
    const int rows = model->rowCount();
    QCOMPARE(rows, 30); // the fixture, one record a second

    trigger(w, "timeDisplaySincePreviousAction");
    QCOMPARE(checkedTimeDisplay(w), QStringLiteral("timeDisplaySincePreviousAction"));
    QCOMPARE(model->rowCount(), rows);

    auto cell = [&](int r) { return model->data(model->index(r, 0)).toString(); };
    QCOMPARE(cell(0), QString()); // nothing above the first row to measure from
    QCOMPARE(cell(1), QStringLiteral("1.000"));
    QCOMPARE(cell(rows - 1), QStringLiteral("1.000"));
}

void TestMultiDoc::timestampModeSharedAcrossViewsOfOneFile()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_a);
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);
    waitUntilIndexed(w);

    trigger(w, "newViewAction");
    QCOMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 2);

    const QList<LogView *> views = w.findChildren<LogView *>(QStringLiteral("logView"));
    LogModel *m0 = modelOf(views.at(0));
    LogModel *m1 = modelOf(views.at(1));
    QVERIFY(m0);
    // One LogModel backs all of a file's views (ARCHITECTURE.md §12.1), which is
    // exactly why the mode cannot be per view.
    QCOMPARE(m0, m1);

    const QString before = m0->data(m0->index(0, 0)).toString();
    QCOMPARE(before, QStringLiteral("2026-07-21 10:00:00,000"));

    trigger(w, "timeDisplaySecondsAction");
    const QString after = m0->data(m0->index(0, 0)).toString();
    QVERIFY(after != before);
    // The file's pattern has %q, so seconds render with milliseconds.
    QVERIFY2(after.endsWith(QStringLiteral(".000")), qPrintable(after));
    // Both views render identically — there is only one model to render from.
    QCOMPARE(m1->data(m1->index(0, 0)).toString(), after);
}

void TestMultiDoc::timestampModeSurvivesRestart()
{
    {
        MainWindow w;
        w.resize(900, 600);
        w.show();
        w.openFile(m_a);
        QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);
        waitUntilIndexed(w);
        trigger(w, "timeDisplayRunSecondsAction");
        w.close(); // saves the session
    }

    // The mode rides the same persistence path Preferences uses, so a
    // relaunch restores it along with the rest of the file's format.
    MainWindow w;
    w.show();
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);
    waitUntilIndexed(w);
    QCOMPARE(checkedTimeDisplay(w), QStringLiteral("timeDisplayRunSecondsAction"));

    LogModel *m = modelOf(w.findChildren<LogView *>(QStringLiteral("logView")).at(0));
    QVERIFY(m);
    QCOMPARE(m->data(m->index(0, 0)).toString(), QStringLiteral("0.000"));

    w.close();
}

// --- M19: the tab marker -----------------------------------------------------

namespace {

// The class's own tabs() is private, and these helpers are free functions so the
// case bodies below read as prose. Same lookup, same object name.
QTabWidget *docTabs(const MainWindow &w)
{
    return w.findChild<QTabWidget *>(QStringLiteral("documentTabs"));
}

// Which column carries a field, since the format decides the order. By ROLE and never
// by the caption: a caption is translated prose, and nothing in tests/ identifies
// anything by its visible text.
int columnOfRole(const Document &doc, FieldRole role)
{
    const QVector<Field> &fields = doc.format().fields;
    for (int c = 0; c < fields.size(); ++c)
        if (fields.at(c).role == role)
            return c;
    return -1;
}

// Add a rule to a file's Document directly, the way session restore does. The pane
// holds one file's rules at a time and these cases care about the file that is NOT on
// screen, so going through the pane would be the wrong route as well as a longer one.
void addRule(MainWindow &w, int tabIndex, HighlightActions actions, const char *needle)
{
    auto *view = qobject_cast<DocumentView *>(docTabs(w)->widget(tabIndex));
    QVERIFY(view);
    Document *doc = view->context()->doc.get();

    HighlightRule r;
    r.actions = actions;
    r.match.text.enabled = true;
    r.match.text.matcher.set(QString::fromLatin1(needle), /*regex=*/false,
                             Qt::CaseInsensitive);
    doc->highlighters().rules.append(r);
    doc->refreshHighlighting();
}

void appendLine(const QString &path, const char *text)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::Append));
    f.write(QStringLiteral("2026-07-21 11:00:00,000 [main] ERROR svc - %1\n")
                .arg(QLatin1String(text))
                .toUtf8());
    f.close();
}

// Drive one live tick on the file behind `tabIndex`, deterministically — the watcher's
// own poll would make these cases wait on a timer for no reason.
void tick(MainWindow &w, int tabIndex)
{
    auto *view = qobject_cast<DocumentView *>(docTabs(w)->widget(tabIndex));
    QVERIFY(view);
    LiveController *live = view->context()->live;
    QVERIFY(live);
    live->checkNow();
}

bool tabIsMarked(const MainWindow &w, int index)
{
    return docTabs(w)->tabText(index).startsWith(QStringLiteral("● "));
}

} // namespace

void TestMultiDoc::aMatchInABackgroundTabMarksIt()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_a);
    w.openFile(m_b);
    waitUntilIndexed(w);
    QCOMPARE(tabCount(w), 2);

    // b is the active tab (opened last); mark a rule on a, which is behind it.
    tabs(w)->setCurrentIndex(1);
    addRule(w, 0, HighlightAction::Tab, "boom");
    QVERIFY(!tabIsMarked(w, 0));

    appendLine(m_a, "boom");
    tick(w, 0);

    // The background tab is the only case the marker exists for — which is why the
    // handling sits ABOVE the ingested handler's `ctx != activeContext()` early return.
    QVERIFY(tabIsMarked(w, 0));
    QVERIFY(!tabIsMarked(w, 1));

    w.close();
}

void TestMultiDoc::aRuleWithoutTheTabActionMarksNothing()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_a);
    w.openFile(m_b);
    waitUntilIndexed(w);

    tabs(w)->setCurrentIndex(1);
    addRule(w, 0, HighlightAction::Color, "boom"); // colours, and only colours

    appendLine(m_a, "boom");
    tick(w, 0);

    // Every action is opt-in per rule: an ordinary colouring rule must never start
    // marking tabs because the machinery to do so now exists.
    QVERIFY(!tabIsMarked(w, 0));

    w.close();
}

void TestMultiDoc::returningToTheTabClearsTheMark()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_a);
    w.openFile(m_b);
    waitUntilIndexed(w);

    tabs(w)->setCurrentIndex(1);
    addRule(w, 0, HighlightAction::Tab, "boom");
    appendLine(m_a, "boom");
    tick(w, 0);
    QVERIFY(tabIsMarked(w, 0));

    // Arriving at a log is what "seen" means. Under offscreen the window may not report
    // itself active, so activate it explicitly — the gate is deliberately BOTH halves
    // (the right tab AND the window in front), and a test that could only prove one of
    // them would be asserting less than the feature promises.
    w.activateWindow();
    tabs(w)->setCurrentIndex(0);
    if (!w.isActiveWindow())
        QSKIP("the offscreen platform does not report window activation");
    QVERIFY(!tabIsMarked(w, 0));

    w.close();
}

void TestMultiDoc::theTabMarkerIsNotTheWordIndexing()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_a);
    w.openFile(m_b);
    waitUntilIndexed(w);

    tabs(w)->setCurrentIndex(1);
    addRule(w, 0, HighlightAction::Tab, "boom");
    appendLine(m_a, "boom");
    tick(w, 0);
    QVERIFY(tabIsMarked(w, 0));

    // waitUntilIndexed() above polls for the ABSENCE of "indexing" in every tab title,
    // so a marker containing that word would deadlock every case in this file that uses
    // it. Pinned directly rather than left to be rediscovered as a hang.
    for (int i = 0; i < tabCount(w); ++i)
        QVERIFY(!tabs(w)->tabText(i).contains(QStringLiteral("indexing")));

    w.close();
}

int main(int argc, char *argv[])
{
    // Isolate all persistent state under a throwaway config home, exactly as
    // tst_sessiongui does, so these runs never touch the developer's settings and
    // never inherit a session from a previous test binary.
    QTemporaryDir configHome;
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
    qputenv("XDG_DATA_HOME", configHome.path().toUtf8());
    qputenv("HOME", configHome.path().toUtf8());
    qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    qRegisterMetaType<loftail::Document *>();
    QApplication::setOrganizationName(QStringLiteral("loftail-test-multidoc"));
    QApplication::setApplicationName(QStringLiteral("loftail-test-multidoc"));

    TestMultiDoc tc;
    return QTest::qExec(&tc, argc, argv);
}

// The Subsystem column is seeded twice: once at construction, from the caption and a
// typical-value allowance, and once when the scan finishes, from the widest name the
// intern table actually holds (invariant #4). Only the second one needs a window.
void TestMultiDoc::theSubsystemColumnWidensOnceTheScanHasSeenEveryName()
{
    if (QFontDatabase::families().isEmpty())
        QSKIP("no fonts available to this platform plugin; nothing measures");

    const QString wide = m_dir.filePath(QStringLiteral("wide.log"));
    const char *longName = "com.example.deeply.nested.subsystem";
    writeLog(wide, longName, 40);

    MainWindow w;
    // Wide, and deliberately so: the seed is bounded by what is on screen (bugs.md 19),
    // so the widest interned name is what the column takes only where there is room for
    // it beside a readable message column. Wider than any window this case needs, because
    // what a document area is worth depends on the font the PANES are labelled in. The
    // narrow case, which derives its width rather than choosing one, is the sibling below.
    w.resize(2400, 600);
    w.show();
    w.openFile(wide);
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);
    waitUntilIndexed(w);

    auto *page = qobject_cast<DocumentView *>(docTabs(w)->widget(0));
    QVERIFY(page);
    LogView *view = page->logView();
    const int logger = columnOfRole(*page->context()->doc, FieldRole::Logger);
    QVERIFY(logger >= 0);
    QVERIFY2(view->header()->sectionSize(logger)
                 >= view->fontMetrics().horizontalAdvance(QLatin1String(longName)),
             "the Subsystem column did not take the widest interned name");
}

// The two commands the column header menu offers besides the visibility list. They are
// window-owned actions with object names, so a test drives them exactly as it drives the
// timestamp modes — without opening a modal menu.
// The other side of the same seed, and the defect it had (bugs.md 19). The widths above
// are measured from the font and the data and know nothing about how wide the view is,
// which is right per column and wrong for the SUM: a 40-character subsystem beside a
// 40-character thread name pushes the message column's origin to or past the right edge
// of an ordinary window — at the exact moment the scan finishes and this seed runs. A log
// that was readable while it was indexing loses its messages when it finishes loading.
//
// Driven through the real window because the scan-completion seed is MainWindow's, and
// the pixels are read back because "the message is off the edge" is not visible in any
// value the view reports: the geometry stays perfectly self-consistent either way.
//
// Every width here is DERIVED, never written down. What the seed asks for depends on the
// font, and what a document area is worth depends on the font the panes beside it are
// labelled in — the two move independently, and a window size chosen against one of them
// says nothing on a platform with different fonts. So the case measures what the seed
// wants, then sizes the window until the document area is exactly that: the one width at
// which the old behaviour leaves precisely nothing of the message on screen.
void TestMultiDoc::theScanCompletionSeedLeavesTheMessageColumnOnScreen()
{
    if (QFontDatabase::families().isEmpty())
        QSKIP("no fonts available to this platform plugin; nothing measures");

    // Two copies: the first settles the panes at the width these names give them, so the
    // document area does not move under the second one's scan-completion seed.
    const QString warmup = m_dir.filePath(QStringLiteral("wide-names-a.log"));
    const QString path = m_dir.filePath(QStringLiteral("wide-names-b.log"));
    for (const QString &p : {warmup, path}) {
        QFile f(p);
        QVERIFY(f.open(QIODevice::WriteOnly));
        for (int i = 0; i < 40; ++i)
            f.write(QStringLiteral("2026-07-21 10:00:%1,000 [worker-thread-number-seventeen"
                                   "-of-many] INFO  com.example.deeply.nested.subsystem.of"
                                   ".some.service - %2\n")
                        .arg(i % 60, 2, 10, QLatin1Char('0'))
                        .arg(QStringLiteral("message body number ").repeated(6))
                        .toUtf8());
        f.close();
    }

    // What the seed asks for with nothing bounding it — the widths that shipped. A view
    // that has never been laid out has no viewport to consult, which is the one state in
    // which the bound is inert by construction.
    Document probe;
    QVERIFY2(probe.open(path, QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"),
                        Encoding::Utf8, QTimeZone::utc()),
             qPrintable(probe.lastError()));
    LogModel probeModel(&probe);
    LogView probeView(&probe, &probeModel);
    const int message = columnOfRole(probe, FieldRole::Message);
    QVERIFY(message >= 0);
    int unbounded = 0;
    for (int c = 0; c < probeModel.columnCount(); ++c)
        if (c != message)
            unbounded += probeView.header()->sectionSize(c);
    const int em = qMax(1, probeView.fontMetrics().horizontalAdvance(QStringLiteral("0")));

    MainWindow w;
    w.resize(1400, 700);
    w.show();
    w.openFile(warmup);
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);
    waitUntilIndexed(w);
    auto *first = qobject_cast<DocumentView *>(docTabs(w)->widget(0));
    QVERIFY(first);

    // Narrow the window until the document area is the width the seed wants, less a
    // couple of characters: too narrow for the seeded columns, wide enough that the
    // message column's own share is there to be protected.
    const int target = unbounded - 4 * em;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const int have = first->logView()->viewport()->width();
        if (qAbs(have - target) <= 2)
            break;
        w.resize(w.width() + (target - have), w.height());
        QCoreApplication::processEvents();
    }
    const int well = first->logView()->viewport()->width();
    if (qAbs(well - target) > 2 * em)
        QSKIP("the window could not be sized to the width this case is about");

    w.openFile(path);
    QTRY_COMPARE(docTabs(w)->count(), 2);
    waitUntilIndexed(w);
    auto *page = qobject_cast<DocumentView *>(docTabs(w)->widget(1));
    QVERIFY(page);
    LogView *view = page->logView();
    QCOMPARE(columnOfRole(*page->context()->doc, FieldRole::Message), message);

    const int origin = view->header()->sectionViewportPosition(message);
    const int visible = view->viewport()->width() - origin;
    QVERIFY2(visible >= 20 * em,
             qPrintable(QStringLiteral("the message column starts at %1 px of a %2 px "
                                       "viewport (the seed asks for %3 px before it): "
                                       "%4 px of it are on screen")
                            .arg(origin)
                            .arg(view->viewport()->width())
                            .arg(unbounded)
                            .arg(visible)));

    // And there is message text in those pixels, which is the thing the reader lost.
    QImage img(view->viewport()->size(), QImage::Format_ARGB32);
    img.fill(Qt::transparent);
    view->viewport()->render(&img);
    QHash<QRgb, int> tally;
    for (int y = 0; y < img.height(); ++y)
        for (int x = origin; x < img.width(); ++x)
            ++tally[img.pixel(x, y)];
    QRgb background = 0;
    int most = -1;
    for (auto it = tally.cbegin(); it != tally.cend(); ++it) {
        if (it.value() > most) {
            most = it.value();
            background = it.key();
        }
    }
    int ink = 0;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = origin; x < img.width(); ++x) {
            const QRgb px = img.pixel(x, y);
            if (qAbs(qRed(px) - qRed(background)) > 30
                || qAbs(qGreen(px) - qGreen(background)) > 30
                || qAbs(qBlue(px) - qBlue(background)) > 30)
                ++ink;
        }
    }
    QVERIFY2(ink > 200, qPrintable(QStringLiteral("only %1 painted pixels right of the "
                                                  "message column's origin at %2")
                                       .arg(ink)
                                       .arg(origin)));
}

void TestMultiDoc::theHeaderMenuFitsAndResetsTheColumnWidths()
{
    if (QFontDatabase::families().isEmpty())
        QSKIP("no fonts available to this platform plugin; nothing measures");

    const QString wide = m_dir.filePath(QStringLiteral("fit.log"));
    // Longer than the 40 characters a SEED will open a column to, so the seeded width
    // and the fitted width are tellable apart.
    const char *longName = "com.example.a.name.longer.than.any.seed.allowance.at.all";
    writeLog(wide, longName, 40);

    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(wide);
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);
    waitUntilIndexed(w);

    auto *page = qobject_cast<DocumentView *>(docTabs(w)->widget(0));
    QVERIFY(page);
    LogView *view = page->logView();
    const int logger = columnOfRole(*page->context()->doc, FieldRole::Logger);
    QVERIFY(logger >= 0);
    const int full = view->fontMetrics().horizontalAdvance(QLatin1String(longName));
    const int seeded = view->header()->sectionSize(logger);
    QVERIFY(seeded < full); // the seed clamps a pathological name; a fit does not

    trigger(w, "fitColumnsAction");
    QVERIFY(view->header()->sectionSize(logger) >= full);

    // Reset forgets the fit along with every dragged width and seeds the lot again.
    trigger(w, "resetColumnWidthsAction");
    QCOMPARE(view->header()->sectionSize(logger), seeded);
}

// The seed that had no caller (bugs.md 29). A document that opens WAITING has no compiled
// format, so LogModel::columnCount() is 0 for the whole of the wait: layoutChrome()
// reserves no band for a header with no sections and never gives it a geometry, and the
// construction seed has nothing to measure. The count goes 0 → n on the RESUME — which
// builds no IndexController, so the scan-completion seed never runs either — and until
// applyFormatChange() said so, nothing was watching: the tab came back with no header at
// all (no dividers, and a right-click where the captions belong offering no column menu)
// and every section at Qt's default size, which saveColumnState() then wrote into the
// session and restoreColumnState() marked as the reader's own layout.
//
// Stated as a RELATION against a second tab holding the same content opened the ordinary
// way, never in pixels: what a column is seeded to depends on the font, and what a
// document area is worth depends on the font the panes beside it are labelled in — the
// two move independently under another style or face (the rule tst_highlighterpane's
// inset case records). The waiting log is the BACKGROUND tab when it arrives, which is
// also the ordinary shape of `loftail app.log other.log` before the service starts.
void TestMultiDoc::aLogThatOpensWaitingGetsItsHeaderAndItsWidthsWhenItArrives()
{
    if (QFontDatabase::families().isEmpty())
        QSKIP("no fonts available to this platform plugin; nothing measures");

    const QString later = m_dir.filePath(QStringLiteral("waiting-header.log"));
    QFile::remove(later);
    QVERIFY(!QFile::exists(later));
    const QString present = m_dir.filePath(QStringLiteral("present-header.log"));
    // Few enough records that neither view grows a vertical scrollbar: a bar appearing
    // re-lays the viewport, which is exactly what used to hide this defect from anyone
    // whose log was longer than a screen.
    const char *subsystem = "com.example.waiting.subsystem";
    writeLog(present, subsystem, 12);

    MainWindow w;
    w.resize(1200, 700);
    w.show();

    QVERIFY(w.openFile(later)); // waiting is a state, not a failure (SPEC.md §3)
    QVERIFY(w.openFile(present));
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 2);
    waitUntilIndexed(w);

    auto *waited = qobject_cast<DocumentView *>(docTabs(w)->widget(0));
    auto *reference = qobject_cast<DocumentView *>(docTabs(w)->widget(1));
    QVERIFY(waited && reference);
    QVERIFY(waited->context()->doc->isWaiting());
    // The state the defect was latched in, and the reason the construction seed and
    // layoutChrome() were both no-ops for this view.
    QCOMPARE(modelOf(waited->logView())->columnCount(), 0);

    // The log turns up, with the same content. The real watcher and poll timer bring it
    // in, exactly as they do for the user — no reopening.
    writeLog(later, subsystem, 12);
    QTRY_VERIFY_WITH_TIMEOUT(waited->logView()->recordCount() == 12, 5000);

    LogView *arrived = waited->logView();
    LogView *ordinary = reference->logView();
    QCOMPARE(modelOf(arrived)->columnCount(), modelOf(ordinary)->columnCount());
    QVERIFY(modelOf(ordinary)->columnCount() > 0);

    // (a) The header band, which is a viewport top margin and a geometry — neither of
    // which any value the view reports would have contradicted, since the geometry stays
    // perfectly self-consistent with no header in it.
    QVERIFY2(ordinary->viewport()->y() > ordinary->frameWidth(),
             "the reference reserved no header band either; nothing is being tested");
    QCOMPARE(arrived->viewport()->y(), ordinary->viewport()->y());
    QCOMPARE(arrived->header()->geometry(), ordinary->header()->geometry());

    // (b) The widths. Same content, same window and same font, so the seed has the same
    // answer for both — and the reference has to differ from Qt's default section size
    // somewhere, or a pair of unseeded views would satisfy this.
    bool anythingSeeded = false;
    for (int c = 0; c < modelOf(ordinary)->columnCount(); ++c) {
        QCOMPARE(arrived->header()->sectionSize(c), ordinary->header()->sectionSize(c));
        anythingSeeded = anythingSeeded
            || ordinary->header()->sectionSize(c) != ordinary->header()->defaultSectionSize();
    }
    QVERIFY2(anythingSeeded,
             "every reference column sits at the default section size; nothing is tested");
}

// --- Several logs in one gesture (SPEC.md §3) ------------------------------

void TestMultiDoc::severalFilesOpenedAtOnceBecomeSeveralTabs()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();

    // One call, two tabs — what `loftail a.log b.log` and a multi-select in File ▸ Open
    // both reach. Neither used to: the command line took the first file and dropped the
    // rest without a word, and the dialog could not name a second one.
    QVERIFY(w.openFiles({m_a, m_b}));
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 2);
    QCOMPARE(tabCount(w), 2);

    // In the order given, and the LAST one is the tab left in front — the same answer
    // opening them one at a time gives, and the same one a drop of both gives.
    QTabWidget *t = tabs(w);
    QVERIFY(t->tabText(0).contains(QStringLiteral("a.log")));
    QVERIFY(t->tabText(1).contains(QStringLiteral("b.log")));
    QVERIFY(w.windowTitle().endsWith(QStringLiteral("b.log")));
}

void TestMultiDoc::aRefusedLogAmongSeveralLeavesTheOthersOpenAndIsNamedWithThem()
{
    // A refusal decided with no I/O — the one kind that still opens no tab at all
    // (SPEC.md §3, M17). Two of them, deliberately: reported one at a time the reader
    // would be left with the last one's reason and no hint that a second file had even
    // been asked for. They are reported TOGETHER instead, in the notice strip, each
    // named WITH the reason it was refused for.
    FakeRemoteFarm farm;
    const QString bad1 = QStringLiteral("ssh://deploy@web1/var/log/one.log");
    const QString bad2 = QStringLiteral("ssh://deploy@web2/var/log/two.log");
    farm.at(bad1)->setStartFailure(QStringLiteral("Authentication to deploy@web1 failed."));
    farm.at(bad2)->setStartFailure(QStringLiteral("Authentication to deploy@web2 failed."));

    MainWindow w;
    w.resize(900, 600);
    w.show();

    QVERIFY(!w.openFiles({m_a, bad1, bad2}));
    // The one that could open did, and is the tab in front.
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);
    QCOMPARE(tabCount(w), 1);
    QVERIFY(w.windowTitle().endsWith(QStringLiteral("a.log")));

    const QString notice = noticeText(w);
    QVERIFY2(notice.contains(QStringLiteral("one.log (web1)")), qPrintable(notice));
    QVERIFY2(notice.contains(QStringLiteral("two.log (web2)")), qPrintable(notice));
    // The reason is the half the reader actually needs, so it travels with each name
    // rather than being dropped when several are listed.
    QCOMPARE(notice.count(QStringLiteral("Authentication")), 2);
}

// An address with no file-name part was named "" everywhere, and the strip is where
// it showed worst: "Cannot open : Not a valid remote log address: ssh://" with nothing
// at all before the colon, and — in the multi form, which is a bare "%1: %2" per line —
// a list of lines each beginning with a colon and no way to tell which address was
// which. Every address has a name now (RemoteLocation.h).
void TestMultiDoc::anAddressWithNoFileNamePartIsStillNamedInTheStrip()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();

    QVERIFY(!w.openFile(QStringLiteral("ssh://")));
    QCOMPARE(tabCount(w), 0);
    QString notice = noticeText(w);
    QVERIFY2(!notice.contains(QStringLiteral("open :")), qPrintable(notice));
    QVERIFY2(notice.contains(QStringLiteral("ssh")), qPrintable(notice));

    // The multi form, where the name is the only thing telling the lines apart.
    QVERIFY(!w.openFiles({QStringLiteral("ssh://"), QStringLiteral("sftp://")}));
    QCOMPARE(tabCount(w), 0);
    notice = noticeText(w);
    const QStringList lines = notice.split(u'\n', Qt::SkipEmptyParts);
    QCOMPARE(lines.size(), 3); // the caption and one line each
    for (int i = 1; i < lines.size(); ++i)
        QVERIFY2(!lines.at(i).startsWith(u':'), qPrintable(lines.at(i)));
    QVERIFY2(lines.at(1).startsWith(QStringLiteral("ssh:")), qPrintable(lines.at(1)));
    QVERIFY2(lines.at(2).startsWith(QStringLiteral("sftp:")), qPrintable(lines.at(2)));
}

// `loftail /mnt/share/logs/` with the share not mounted — the M13 case, and a
// SUCCESSFUL open: the path is well-formed, so it waits. Which is exactly why the empty
// name reached further here than in the strip, into three places at once.
void TestMultiDoc::aWaitingLogWithNoFileNamePartStillHasALabelATitleAndAStatus()
{
    const QString absent = m_dir.filePath(QStringLiteral("share")) + u'/';

    MainWindow w;
    w.resize(900, 600);
    w.show();

    QVERIFY(w.openFile(absent)); // waiting is a state, not a failure (SPEC.md §3)
    QCOMPARE(tabCount(w), 1);

    // The hollow marker AND a name — the marker on its own says a log is not there
    // without saying which log.
    QTRY_COMPARE(tabs(w)->tabText(0), QStringLiteral("◦ share"));
    QCOMPARE(w.windowTitle(), QStringLiteral("loftail — share"));

    auto *status = w.findChild<QLabel *>(QStringLiteral("statusLabel"));
    QVERIFY(status);
    QVERIFY2(status->text().startsWith(QStringLiteral("share ")), qPrintable(status->text()));
}

void TestMultiDoc::aRefusalOutlivesTheTicksAndTabSwitchesThatFollowItUntilDismissed()
{
    // The whole point of the strip. A refusal used to be written into the status
    // label, which updateStatus() rewrites from the active document on every ingest
    // tick and every tab switch — so beside one live log the reason for an open that
    // did not happen was gone within a second, with no tab anywhere to ask.
    FakeRemoteFarm farm;
    const QString bad = QStringLiteral("ssh://deploy@web1/var/log/one.log");
    farm.at(bad)->setStartFailure(QStringLiteral("Authentication to deploy@web1 failed."));

    MainWindow w;
    w.resize(900, 600);
    w.show();

    QVERIFY(!w.openFiles({m_a, bad, m_b}));
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 2);
    QVERIFY(noticeLabel(w));
    QVERIFY(noticeLabel(w)->isVisible());
    QVERIFY(noticeText(w).contains(QStringLiteral("one.log (web1)")));

    // Let the two live logs tick, then switch tabs — both of which go through
    // updateStatus(), which is what used to take the message away.
    QTest::qWait(300);
    tabs(w)->setCurrentIndex(0);
    QTest::qWait(300);

    auto *status = w.findChild<QLabel *>(QStringLiteral("statusLabel"));
    QVERIFY(status);
    QVERIFY2(status->text().contains(QStringLiteral("a.log")), qPrintable(status->text()));
    QVERIFY2(!status->text().contains(QStringLiteral("Authentication")),
             qPrintable(status->text())); // the label has indeed moved on

    QVERIFY(noticeLabel(w)->isVisible());
    QVERIFY2(noticeText(w).contains(QStringLiteral("one.log (web1)")),
             qPrintable(noticeText(w)));
    QVERIFY2(noticeText(w).contains(QStringLiteral("Authentication")),
             qPrintable(noticeText(w)));

    // It is laid out ABOVE the well and clear of its own dismiss button — the strip
    // spans the window, so a text half drawn under the button would be unreadable in
    // exactly the case it exists for (the precedent: tst_filterpane's context row).
    auto *notice = w.findChild<QFrame *>(QStringLiteral("openNotice"));
    QVERIFY(notice);
    auto *dismissButton = w.findChild<QToolButton *>(QStringLiteral("openNoticeDismiss"));
    QVERIFY(dismissButton);
    QVERIFY(noticeLabel(w)->width() > 0);
    QVERIFY(noticeLabel(w)->geometry().right() <= dismissButton->geometry().left());
    QVERIFY(noticeLabel(w)->height() <= notice->height());
    QVERIFY(notice->mapTo(&w, QPoint(0, notice->height())).y()
            <= tabs(w)->mapTo(&w, QPoint(0, 0)).y());

    // And the user is the only thing that takes it away.
    auto *dismiss = w.findChild<QToolButton *>(QStringLiteral("openNoticeDismiss"));
    QVERIFY(dismiss);
    dismiss->click();
    QVERIFY(!noticeLabel(w)->isVisible());
    QVERIFY(noticeText(w).isEmpty());
}

// The value rows of a value axis, "Others" (row 0) excluded — the tick states these
// three cases are about. Never found by the row's label, which is translated prose.
//
// Searched from the FILTERS pane and never from the window: the Highlighters pane
// embeds an AxisEditor of its own, so `subsystemList` and `subsystemNone` name a
// widget in each of them and findChild() on the window answers with whichever it
// reaches first — a rule list belonging to one highlight rule, whose ticks say
// nothing about what the table is showing.
namespace {
FilterPane *filterPane(const MainWindow &w)
{
    return w.findChild<FilterPane *>();
}

QListWidget *valueList(const MainWindow &w, const char *name)
{
    FilterPane *pane = filterPane(w);
    return pane ? pane->findChild<QListWidget *>(QLatin1String(name)) : nullptr;
}

int tickedValues(const QListWidget *list)
{
    int n = 0;
    for (int i = AxisEditor::kFirstValueRow; i < list->count(); ++i)
        if (list->item(i)->checkState() == Qt::Checked)
            ++n;
    return n;
}

int valueCount(const QListWidget *list)
{
    return list->count() - AxisEditor::kFirstValueRow;
}

LogModel *modelOfTab(const MainWindow &w, int index)
{
    QTabWidget *t = w.findChild<QTabWidget *>(QStringLiteral("documentTabs"));
    if (!t || index < 0 || index >= t->count())
        return nullptr;
    return modelOf(t->widget(index)->findChild<LogView *>(QStringLiteral("logView")));
}
} // namespace

void TestMultiDoc::everyTabKeepsItsRecordsWhenTheOpensRunTogether()
{
    // Its own pair of logs, because the cases above append to the shared ones and
    // these three count records.
    const QString first = m_dir.filePath(QStringLiteral("stash-a.log"));
    const QString second = m_dir.filePath(QStringLiteral("stash-b.log"));
    writeLog(first, "net.io", 30);
    writeLog(second, "db.pool", 20);

    MainWindow w;
    w.resize(900, 600);
    w.show();

    // One gesture, two logs, and NOTHING pumped between them — which is what
    // `loftail a.log b.log`, a multi-select in File ▸ Open and a drop of both files all
    // reach. Indexing is asynchronous, so the second open brings its own tab forward
    // while the first log's index still holds not one record, and what gets stashed for
    // a.log is an enabled Subsystem axis over an empty list. That is an axis excluding
    // nothing, and it must not come back reading as a selection of nothing.
    QVERIFY(w.openFiles({first, second}));
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 2);
    waitUntilIndexed(w);

    QTabWidget *t = tabs(w);
    QVERIFY(t);
    QCOMPARE(t->count(), 2);

    // The tab left in front was always fine: nothing was ever stashed for it, because
    // it was never switched away from.
    QCOMPARE(modelOfTab(w, 1)->rowCount(), 20);

    // The one behind is the case — every record of it, not "0 of 30".
    t->setCurrentIndex(0);
    QCOMPARE(modelOfTab(w, 0)->rowCount(), 30);

    // And the pane agrees with the table. With the bug the two contradicted each
    // other: the axis ticked, "Others" ticked, and every value discovered under them
    // unticked — a picture no click of the user's could have produced.
    FilterPane *fp = filterPane(w);
    QVERIFY(fp);
    QVERIFY(!fp->hasActiveFilters());
    QListWidget *loggers = valueList(w, "subsystemList");
    QVERIFY(loggers);
    QCOMPARE(valueCount(loggers), 1); // a.log logs under one subsystem
    QCOMPARE(tickedValues(loggers), valueCount(loggers));

    // The Thread axis ships OFF, which is the only reason it was not reported too: it
    // is stashed and reloaded by the same rule, and switching it on has to narrow
    // nothing rather than everything.
    QListWidget *threads = valueList(w, "threadList");
    QVERIFY(threads);
    QCOMPARE(tickedValues(threads), valueCount(threads));
    QVERIFY(valueCount(threads) > 0);

    w.close();
}

void TestMultiDoc::aSecondLogOpenedByHandLeavesTheFirstItsRecords()
{
    const QString first = m_dir.filePath(QStringLiteral("byhand-a.log"));
    const QString second = m_dir.filePath(QStringLiteral("byhand-b.log"));
    writeLog(first, "net.io", 30);
    writeLog(second, "db.pool", 20);

    // The same condition reached one file at a time: nothing about it needs the two
    // opens to be one gesture, only that the first log is still being indexed when the
    // second takes the window. Two openFile() calls back to back are exactly that, and
    // are what a user opening a second log a moment after the first does.
    MainWindow w;
    w.resize(900, 600);
    w.show();
    QVERIFY(w.openFile(first));
    QVERIFY(w.openFile(second));
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 2);
    waitUntilIndexed(w);

    tabs(w)->setCurrentIndex(0);
    QCOMPARE(modelOfTab(w, 0)->rowCount(), 30);
    w.close();
}

void TestMultiDoc::anEmptySelectionTheUserChoseSurvivesATabSwitchAndARelaunch()
{
    const QString first = m_dir.filePath(QStringLiteral("none-a.log"));
    const QString second = m_dir.filePath(QStringLiteral("none-b.log"));
    writeLog(first, "net.io", 30);
    writeLog(second, "db.pool", 20);

    // The other half of the same ambiguity, and the reason it cannot be settled by
    // reading an empty selection as "show everything": None is a click, and a tab
    // switch or a relaunch must not undo it.
    {
        MainWindow w;
        w.resize(900, 600);
        w.show();
        QVERIFY(w.openFiles({first, second}));
        QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 2);
        waitUntilIndexed(w);

        QTabWidget *t = tabs(w);
        t->setCurrentIndex(0);
        QVERIFY(filterPane(w));
        auto *none =
            filterPane(w)->findChild<QAbstractButton *>(QStringLiteral("subsystemNone"));
        QVERIFY(none);
        none->click();
        QTRY_COMPARE(modelOfTab(w, 0)->rowCount(), 0);
        QCOMPARE(tickedValues(valueList(w, "subsystemList")), 0);

        // Away and back, which stashes and hydrates it exactly as the case above does.
        t->setCurrentIndex(1);
        t->setCurrentIndex(0);
        QCOMPARE(modelOfTab(w, 0)->rowCount(), 0);
        QCOMPARE(tickedValues(valueList(w, "subsystemList")), 0);

        // Closed from the OTHER tab, so the relaunch resumes there. A pane hydrated
        // while its own log is still being scanned is discovering values rather than
        // loading them, and everything a scan discovers arrives ticked (SPEC.md §6) —
        // which is a separate rule and not this one.
        t->setCurrentIndex(1);
        w.close();
    }

    MainWindow w;
    w.resize(900, 600);
    w.show();
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 2);
    waitUntilIndexed(w);
    QTabWidget *t = tabs(w);
    QCOMPARE(t->count(), 2);
    QVERIFY(t->tabText(0).contains(QStringLiteral("none-a.log")));

    t->setCurrentIndex(0);
    QCOMPARE(modelOfTab(w, 0)->rowCount(), 0);
    QCOMPARE(tickedValues(valueList(w, "subsystemList")), 0);
    // And the log it was not applied to still shows everything.
    t->setCurrentIndex(1);
    QCOMPARE(modelOfTab(w, 1)->rowCount(), 20);
    w.close();
}

void TestMultiDoc::theCommandLineTakesEveryFileNamedAndOnePatternForThemAll()
{
    // The parsing half, without a process: main() hands openFiles() exactly what
    // files() returns, so this is where "only the first argument opened" would come
    // back. --pattern stays one value covering the lot; it says how a log is written,
    // and files named on one command line are files written alike.
    CommandLine cmd;
    QVERIFY(cmd.parse({QStringLiteral("loftail"), QStringLiteral("--pattern"),
                       QStringLiteral("%d %m%n"), m_a, m_b}));
    QCOMPARE(cmd.files(), QStringList({m_a, m_b}));
    QCOMPARE(cmd.pattern(), QStringLiteral("%d %m%n"));

    // An address is not a path, and this layer must not decide it is one: an ssh:// URL
    // and an in-archive address travel through as written, for openFile() to normalize.
    CommandLine addresses;
    QVERIFY(addresses.parse({QStringLiteral("loftail"),
                             QStringLiteral("ssh://deploy@web1/var/log/app.log"),
                             QStringLiteral("bundle.tar.gz/var/log/app.log")}));
    QCOMPARE(addresses.files().size(), 2);
    QCOMPARE(addresses.files().at(0), QStringLiteral("ssh://deploy@web1/var/log/app.log"));
    QCOMPARE(addresses.files().at(1), QStringLiteral("bundle.tar.gz/var/log/app.log"));

    // A bare launch names nothing, and openFiles() on it is a no-op window.
    CommandLine bare;
    QVERIFY(bare.parse({QStringLiteral("loftail")}));
    QVERIFY(bare.files().isEmpty());
    QVERIFY(bare.pattern().isEmpty());
}

void TestMultiDoc::twoLogsWithOneBasenameEachShowTheirOwnDirectory()
{
    // Both tabs used to read "app.log", with only the tooltip telling them apart —
    // which is the ordinary state of affairs for anyone tailing one service in two
    // deployments. Each now brackets on the directory that differs (TabLabels.h); the
    // common parts of the two paths are left out, because they tell nothing apart.
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_svcA);
    w.openFile(m_svcB);
    QTRY_COMPARE(tabCount(w), 2);
    waitUntilIndexed(w);

    QTabWidget *t = tabs(w);
    QCOMPARE(t->tabText(0), QStringLiteral("app.log (svc-a)"));
    QCOMPARE(t->tabText(1), QStringLiteral("app.log (svc-b)"));

    // The tooltip is untouched by any of this: it is the full address, which is what
    // makes a shortened label safe. Asserted as a suffix rather than as the string the
    // test wrote, since the address a window holds is the normalized one.
    QVERIFY(t->tabToolTip(0).endsWith(QStringLiteral("svc-a/app.log")));
    QVERIFY(t->tabToolTip(1).endsWith(QStringLiteral("svc-b/app.log")));
    QVERIFY(t->tabToolTip(0).size() > t->tabText(0).size()); // the whole path, not the label

    // And a log with a name of its own is unaffected by the pair beside it.
    w.openFile(m_a);
    QTRY_COMPARE(tabCount(w), 3);
    waitUntilIndexed(w);
    QCOMPARE(t->tabText(2), QStringLiteral("a.log"));
    QCOMPARE(t->tabText(0), QStringLiteral("app.log (svc-a)"));
}

void TestMultiDoc::closingOneOfThemShortensTheSurvivorBackToItsPlainName()
{
    // The ambiguity is gone with the log that caused it, so the bracket goes too.
    // This falls out only because the labels are decided for the whole set on every
    // open and close, rather than fixed when a log opens.
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_svcA);
    w.openFile(m_svcB);
    QTRY_COMPARE(tabCount(w), 2);
    waitUntilIndexed(w);
    QCOMPARE(tabs(w)->tabText(0), QStringLiteral("app.log (svc-a)"));

    trigger(w, "closeTabAction"); // closes svc-b, the active tab
    QTRY_COMPARE(tabCount(w), 1);
    QCOMPARE(tabs(w)->tabText(0), QStringLiteral("app.log"));
}

void TestMultiDoc::theViewNumberingComposesWithTheDistinguisher()
{
    // Two mechanisms, two different questions: the bracket says WHICH LOG, the
    // number says which view of it. Both at once reads "app.log (svc-a) [1]".
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_svcA);
    QTRY_COMPARE(tabCount(w), 1);
    trigger(w, "newViewAction");
    QCOMPARE(tabCount(w), 2);
    w.openFile(m_svcB);
    QTRY_COMPARE(tabCount(w), 3);
    waitUntilIndexed(w);

    QTabWidget *t = tabs(w);
    QCOMPARE(t->tabText(0), QStringLiteral("app.log (svc-a) [1]"));
    QCOMPARE(t->tabText(1), QStringLiteral("app.log (svc-a) [2]"));
    QCOMPARE(t->tabText(2), QStringLiteral("app.log (svc-b)"));
}

// --- Line wrap by keyboard, and log text size (SPEC.md §5) -------------------

namespace {
// The wrap mode of the tab in front.
LogView::WrapMode activeWrap(const MainWindow &w)
{
    QTabWidget *t = w.findChild<QTabWidget *>(QStringLiteral("documentTabs"));
    return t->currentWidget()->findChild<LogView *>(QStringLiteral("logView"))->wrapMode();
}

bool actionChecked(const MainWindow &w, const char *name)
{
    QAction *a = w.findChild<QAction *>(QLatin1String(name));
    return a && a->isChecked();
}

// Puts the application-wide log text size back, whatever a case did to it: it outlives
// the window that changed it, and the cases below this one measure fonts.
struct FontSizeGuard
{
    ~FontSizeGuard()
    {
        resetLogFontPointSize();
        QSettings s;
        s.remove(QStringLiteral("logFontPointSize"));
        s.sync();
    }
};
} // namespace

void TestMultiDoc::theWrapKeyTogglesBetweenOffAndAlwaysOnThroughTheMenusOwnAction()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_a);
    QTRY_COMPARE(tabCount(w), 1);
    QCOMPARE(activeWrap(w), LogView::WrapMode::Off);

    // The key TRIGGERS the menu's Always On entry rather than setting the mode itself,
    // which is why the checkmark follows with nothing else wired up: one path sets wrap,
    // and it is the one that also remembers the choice for this log.
    trigger(w, "toggleWrapAction");
    QCOMPARE(activeWrap(w), LogView::WrapMode::AlwaysOn);
    QVERIFY(actionChecked(w, "wrapAlwaysOnAction"));
    QVERIFY(!actionChecked(w, "wrapOffAction"));

    trigger(w, "toggleWrapAction");
    QCOMPARE(activeWrap(w), LogView::WrapMode::Off);
    QVERIFY(actionChecked(w, "wrapOffAction"));
}

void TestMultiDoc::theWrapKeyFromSelectedRecordOnlyWrapsEverything()
{
    // The third mode is not on the way round: from it the key means "wrap it all",
    // which is the useful reading of a toggle pressed from a state it does not name.
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_a);
    QTRY_COMPARE(tabCount(w), 1);

    trigger(w, "wrapSelectedAction");
    QCOMPARE(activeWrap(w), LogView::WrapMode::SelectedRecordOnly);

    trigger(w, "toggleWrapAction");
    QCOMPARE(activeWrap(w), LogView::WrapMode::AlwaysOn);
}

void TestMultiDoc::zoomingResizesEveryOpenViewIncludingTheDigestStrip()
{
    FontSizeGuard guard;

    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_a);
    w.openFile(m_b);
    QTRY_COMPARE(tabCount(w), 2);

    const int was = logFontPointSize();
    trigger(w, "zoomInAction");
    QCOMPARE(logFontPointSize(), was + 1);

    // EVERY open view, not just the one in front — the size is the application's, so a
    // background tab that kept the old one would be a second answer to the same
    // question. The digest strip is in it too: its whole claim is that a row is
    // rendered exactly as it is in the log above it.
    const QList<LogView *> views = w.findChildren<LogView *>();
    QVERIFY(views.size() >= 4); // two tables and two strips
    for (LogView *v : views)
        QCOMPARE(qRound(v->font().pointSizeF()), was + 1);

    trigger(w, "zoomOutAction");
    QCOMPARE(logFontPointSize(), was);

    // A view opened AFTER a zoom opens at the size, with nobody pushing a font into it.
    trigger(w, "zoomInAction");
    trigger(w, "newViewAction");
    QTRY_COMPARE(tabCount(w), 3);
    for (LogView *v : w.findChildren<LogView *>())
        QCOMPARE(qRound(v->font().pointSizeF()), was + 1);
}

void TestMultiDoc::theZoomStopsAtItsBoundsAndComesBackOnReset()
{
    FontSizeGuard guard;

    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_a);
    QTRY_COMPARE(tabCount(w), 1);

    const int base = defaultLogFontPointSize();

    // Held down at either end it stops rather than running away, and the view stays
    // usable at both — which is the whole reason the bounds are there.
    for (int i = 0; i < 60; ++i)
        trigger(w, "zoomInAction");
    QCOMPARE(logFontPointSize(), kMaxLogFontPointSize);
    for (int i = 0; i < 60; ++i)
        trigger(w, "zoomOutAction");
    QCOMPARE(logFontPointSize(), kMinLogFontPointSize);

    trigger(w, "zoomResetAction");
    QCOMPARE(logFontPointSize(), base);
}

void TestMultiDoc::theChosenTextSizeSurvivesARestart()
{
    FontSizeGuard guard;

    int chosen = 0;
    {
        MainWindow w;
        w.resize(900, 600);
        w.show();
        w.openFile(m_a);
        QTRY_COMPARE(tabCount(w), 1);
        trigger(w, "zoomInAction");
        trigger(w, "zoomInAction");
        chosen = logFontPointSize();
    }

    // A preference, not window state: it is written when it changes rather than at
    // close, and it is read back before any view is built — so a restored tab is
    // constructed at the size instead of being re-fonted afterwards.
    resetLogFontPointSize();
    QVERIFY(logFontPointSize() != chosen);

    MainWindow again;
    again.resize(900, 600);
    again.show();
    QCOMPARE(logFontPointSize(), chosen);
    again.openFile(m_a);
    QTRY_COMPARE(tabCount(again), 1);
    QCOMPARE(qRound(tabs(again)->currentWidget()
                        ->findChild<LogView *>(QStringLiteral("logView"))
                        ->font()
                        .pointSizeF()),
             chosen);
}

void TestMultiDoc::aReplacedLogSaysSoInTheStatusBar()
{
    // Its own log: this case rewrites it twice, and the shared ones are appended to by
    // the cases above.
    const QString path = m_dir.filePath(QStringLiteral("rolled.log"));
    writeLog(path, "net.io", 20);

    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(path);
    waitUntilIndexed(w);
    QVERIFY(w.statusBar()->currentMessage().isEmpty());

    // A copytruncate: this same file, shorter. Read back off the status BAR and never
    // off m_statusLabel, which is where the sentence must NOT be — updateStatus()
    // rewrites that label from the active document on the very tick this fires from.
    writeLog(path, "net.io", 4);
    tick(w, 0);
    QCOMPARE(modelOfTab(w, 0)->rowCount(), 4);
    QCOMPARE(w.statusBar()->currentMessage(),
             QStringLiteral("rolled.log was truncated — reloaded"));

    // A rewrite in place that GROWS. Same file, same inode, more bytes — so it is a
    // replacement and not a truncation, which is SPEC.md §3's own vocabulary. The
    // wording is the whole content of the notice, which is why it is compared entire.
    writeLog(path, "db.pool", 30);
    tick(w, 0);
    QCOMPARE(modelOfTab(w, 0)->rowCount(), 30);
    QCOMPARE(w.statusBar()->currentMessage(),
             QStringLiteral("rolled.log was replaced — reloaded"));

    w.close();
}

void TestMultiDoc::aRotationInABackgroundTabSaysNothing()
{
    const QString behind = m_dir.filePath(QStringLiteral("behind.log"));
    writeLog(behind, "net.io", 20);

    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(behind);
    w.openFile(m_b);
    waitUntilIndexed(w);
    QCOMPARE(tabCount(w), 2);
    tabs(w)->setCurrentIndex(1); // b is in front; `behind` is the tab nobody is reading
    QVERIFY(w.statusBar()->currentMessage().isEmpty());

    writeLog(behind, "net.io", 3);
    tick(w, 0);

    // The reload itself DID happen — without this the case passes for the wrong reason,
    // and would go on passing if the signal were never emitted at all.
    QCOMPARE(modelOfTab(w, 0)->rowCount(), 3);
    // ...and it said nothing, because there is one status bar and it belongs to the log
    // in front. Unlike the tab marker, which exists FOR the background tab and is
    // therefore handled above the same guard (aMatchInABackgroundTabMarksIt), a notice
    // here would cover the record count of a log that had not moved.
    QVERIFY2(w.statusBar()->currentMessage().isEmpty(),
             qPrintable(w.statusBar()->currentMessage()));

    w.close();
}

// --- Taking another open log's rules (SPEC.md §7) -----------------------------------

namespace {

// The picker is modal, so it has to be driven from a timer once it is up — the
// tst_archiveopen shape. Anything ELSE that goes modal is rejected on a repeating
// timer, because without that a regression does not fail the case, it HANGS it.
QTimer *rejectUnexpectedDialogs(bool *seen)
{
    auto *timer = new QTimer;
    timer->setInterval(200);
    QObject::connect(timer, &QTimer::timeout, [seen]() {
        for (QWidget *w : QApplication::topLevelWidgets()) {
            auto *dialog = qobject_cast<QDialog *>(w);
            if (!dialog || !dialog->isVisible())
                continue;
            if (qobject_cast<CopyHighlightersDialog *>(dialog))
                continue;
            *seen = true;
            dialog->reject();
        }
    });
    timer->start();
    return timer;
}

void whenPickerShown(std::function<void(CopyHighlightersDialog *)> act)
{
    QTimer::singleShot(0, [act = std::move(act)]() {
        for (QWidget *w : QApplication::topLevelWidgets()) {
            if (auto *dialog = qobject_cast<CopyHighlightersDialog *>(w)) {
                act(dialog);
                return;
            }
        }
    });
}

Document *docOfTab(const MainWindow &w, int index)
{
    auto *view = qobject_cast<DocumentView *>(docTabs(w)->widget(index));
    return view ? view->context()->doc.get() : nullptr;
}

// BY PATH, never by tab index, wherever a case relaunches: a MainWindow's constructor
// restores whatever session the last one saved, so a window that opens one log can come
// up holding two and the log this case means is not necessarily tab 0.
Document *docOfLog(const MainWindow &w, const QString &path)
{
    const QString wanted = QFileInfo(path).absoluteFilePath();
    for (int i = 0; i < docTabs(w)->count(); ++i) {
        Document *d = docOfTab(w, i);
        if (d && QFileInfo(d->path()).absoluteFilePath() == wanted)
            return d;
    }
    return nullptr;
}

// addRule() above writes straight into the Document, which is right for the tab-marker
// cases it was written for and one step short here: it leaves both swatches at
// *default*, and a rule that names no colour does not carry the Colour action — so
// adoptRules()' normalisation (the same one setDocument() and restoreState() apply)
// legitimately drops the flag and the copy would differ from its source in a way that
// says nothing about copying. A rule made in the application always names a colour;
// this is what one looks like.
void addColouredRule(MainWindow &w, int tabIndex, const char *needle, int background)
{
    addRule(w, tabIndex, HighlightActions(HighlightAction::Color), needle);
    QVector<HighlightRule> &rules = docOfTab(w, tabIndex)->highlighters().rules;
    rules.last().background = background;
    rules.last().foreground = HighlightPalette::readableTextSlot(background);
    docOfTab(w, tabIndex)->refreshHighlighting();
}

} // namespace

void TestMultiDoc::copyingHighlightersFromAnotherTabReplacesThisLogsRules()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_a);
    w.openFile(m_b);
    waitUntilIndexed(w);

    // a.log gets a rule of its own; b.log is left with the three it arrived with.
    addColouredRule(w, 0, "boom", 7);
    const QVector<HighlightRule> wanted = docOfTab(w, 0)->highlighters().rules;
    QCOMPARE(wanted.size(), HighlighterSet::defaults().rules.size() + 1);

    docTabs(w)->setCurrentIndex(1);
    QCOMPARE(docOfTab(w, 1)->highlighters().rules, HighlighterSet::defaults().rules);

    bool strayDialog = false;
    QTimer *guard = rejectUnexpectedDialogs(&strayDialog);
    // a.log is the only other log open, so it is the only row — and it is the one the
    // picker preselects, being the one whose rules are not the seed.
    whenPickerShown([](CopyHighlightersDialog *d) { d->accept(); });
    trigger(w, "copyHighlightersAction");
    guard->stop();
    delete guard;
    QVERIFY(!strayDialog);

    QCOMPARE(docOfTab(w, 1)->highlighters().rules, wanted);
    // ...and the log it came FROM is untouched. This is a copy, not a move.
    QCOMPARE(docOfTab(w, 0)->highlighters().rules, wanted);

    w.close();
}

void TestMultiDoc::cancellingTheCopyLeavesTheRulesAlone()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_a);
    w.openFile(m_b);
    waitUntilIndexed(w);

    addRule(w, 0, HighlightActions(HighlightAction::Color), "boom");
    docTabs(w)->setCurrentIndex(1);
    const QVector<HighlightRule> before = docOfTab(w, 1)->highlighters().rules;

    whenPickerShown([](CopyHighlightersDialog *d) { d->reject(); });
    trigger(w, "copyHighlightersAction");

    // Cancelling must change nothing, exactly as cancelling Preferences does — and
    // there is no undo for this gesture, which is why it has a Cancel worth trusting.
    QCOMPARE(docOfTab(w, 1)->highlighters().rules, before);

    w.close();
}

void TestMultiDoc::bothEntryPointsRunTheSameGesture()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_a);
    w.openFile(m_b);
    waitUntilIndexed(w);

    addColouredRule(w, 0, "boom", 7);
    const QVector<HighlightRule> wanted = docOfTab(w, 0)->highlighters().rules;
    docTabs(w)->setCurrentIndex(1);

    // The pane's button, not the menu item this time. They are one command and must be
    // live in the same states and do the same thing; two ways in, one gesture.
    auto *copy = w.findChild<QPushButton *>(QStringLiteral("ruleCopyFrom"));
    QVERIFY(copy);
    QVERIFY(copy->isEnabled());

    whenPickerShown([](CopyHighlightersDialog *d) { d->accept(); });
    copy->click();

    QCOMPARE(docOfTab(w, 1)->highlighters().rules, wanted);

    w.close();
}

void TestMultiDoc::theCopyCommandIsDeadWithOnlyOneLogOpen()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();

    QAction *item = w.findChild<QAction *>(QStringLiteral("copyHighlightersAction"));
    auto *copy = w.findChild<QPushButton *>(QStringLiteral("ruleCopyFrom"));
    QVERIFY(item && copy);

    // No log at all.
    QVERIFY(!item->isEnabled());
    QVERIFY(!copy->isEnabled());

    // One log: there is nowhere to copy FROM, which is not the same state as no log.
    w.openFile(m_a);
    waitUntilIndexed(w);
    QVERIFY2(!item->isEnabled(), "the item is live with nowhere to copy from");
    QVERIFY2(!copy->isEnabled(), "the button is live with nowhere to copy from");

    w.openFile(m_b);
    waitUntilIndexed(w);
    QVERIFY(item->isEnabled());
    QVERIFY(copy->isEnabled());

    // AND BACK AGAIN, closing the BACKGROUND tab — the case the relabelTabs() hook
    // exists for. The active view does not move, so the reap does not reach
    // updateActionStates(), and without that hook both surfaces stay live with nothing
    // left to offer.
    docTabs(w)->setCurrentIndex(1);
    QVERIFY(docTabs(w)->count() == 2);
    docTabs(w)->tabCloseRequested(0);
    QTRY_COMPARE(docTabs(w)->count(), 1);
    QVERIFY2(!item->isEnabled(), "the item stayed live after the other log closed");
    QVERIFY2(!copy->isEnabled(), "the button stayed live after the other log closed");

    w.close();
}

void TestMultiDoc::aLogOpenInTwoTabsIsOfferedOnce()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_a);
    waitUntilIndexed(w);
    trigger(w, "newViewAction"); // a second view of a.log
    w.openFile(m_b);
    waitUntilIndexed(w);
    QCOMPARE(docTabs(w)->count(), 3);

    // b.log is in front, and what is on offer is one entry per FILE and not per tab —
    // which is why the menu item says "Log". Asked of the accessor directly, with no
    // modal anywhere: that it is public is exactly so this can be stated.
    const QVector<DocumentContext *> offer = w.otherLogContexts();
    QCOMPARE(offer.size(), 1);
    QCOMPARE(offer.at(0)->doc->path(), docOfTab(w, 0)->path());

    w.close();
}

void TestMultiDoc::copiedRulesSurviveClosingAndReopeningTheLog()
{
    const QVector<HighlightRule> seeded = HighlighterSet::defaults().rules;
    QVector<HighlightRule> wanted;
    {
        MainWindow w;
        w.resize(900, 600);
        w.show();
        w.openFile(m_a);
        w.openFile(m_b);
        waitUntilIndexed(w);

        addColouredRule(w, 0, "boom", 7);
        wanted = docOfTab(w, 0)->highlighters().rules;
        docTabs(w)->setCurrentIndex(1);

        whenPickerShown([](CopyHighlightersDialog *d) { d->accept(); });
        trigger(w, "copyHighlightersAction");
        QCOMPARE(docOfTab(w, 1)->highlighters().rules, wanted);
        w.close();
    }

    // A copy is an ordinary rule edit, so it rides highlightersChanged into
    // persistFileSettings and is remembered for the LOG (SPEC.md §10) — nothing was
    // written for this feature, and this is what proves the free ride is real.
    {
        MainWindow w2;
        w2.resize(900, 600);
        w2.show();
        w2.openFile(m_b);
        waitUntilIndexed(w2);
        QCOMPARE(docOfLog(w2, m_b)->highlighters().rules, wanted);
        w2.close();
    }

    // ...and the same for an EMPTY list, which is the case a store reading emptiness as
    // silence gets wrong: a log deliberately left uncoloured must not come back wearing
    // the three level colours (presence, never emptiness).
    {
        MainWindow w3;
        w3.resize(900, 600);
        w3.show();
        w3.openFile(m_b);
        waitUntilIndexed(w3);
        auto *pane = w3.findChild<HighlighterPane *>();
        QVERIFY(pane);
        pane->adoptRules({}, docOfLog(w3, m_b)->displayZone());
        QVERIFY(docOfLog(w3, m_b)->highlighters().rules.isEmpty());
        w3.close();
    }
    {
        MainWindow w4;
        w4.resize(900, 600);
        w4.show();
        w4.openFile(m_b);
        waitUntilIndexed(w4);
        QVERIFY2(docOfLog(w4, m_b)->highlighters().rules.isEmpty(),
                 "an emptied rule list came back seeded");
        QVERIFY(docOfLog(w4, m_b)->highlighters().rules != seeded);
        w4.close();
    }
}

#include "tst_multidoc.moc"
