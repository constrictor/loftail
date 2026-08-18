#include <QtTest>

#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QHeaderView>
#include <QFile>
#include <QFontDatabase>
#include <QSettings>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QLabel>

#include "Document.h"
#include "DocumentContext.h"
#include "DocumentView.h"
#include "FilterPane.h"
#include "Highlight.h"
#include "LiveController.h"
#include "LogFormat.h"
#include "LogModel.h"
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
    void theHeaderMenuFitsAndResetsTheColumnWidths();

    // SEVERAL logs asked for in ONE gesture (SPEC.md §3). Dropping files on the window
    // had always opened all of them; the command line took the first and dropped the
    // rest, and File ▸ Open could name only one. All three now go through openFiles(),
    // so what happens to the tab bar and to a refusal is one answer, not three.
    void severalFilesOpenedAtOnceBecomeSeveralTabs();
    void aRefusedLogAmongSeveralLeavesTheOthersOpenAndIsNamedWithThem();
    void theCommandLineTakesEveryFileNamedAndOnePatternForThemAll();
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
                             "timeDisplayRunSecondsAction"}) {
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
}

void TestMultiDoc::init()
{
    QSettings s;
    s.remove(QStringLiteral("session"));
    // The per-file format cache is keyed by path and outlives a window, so a
    // timestamp mode chosen in one case would otherwise reappear in the next one
    // that opens the same file.
    s.remove(QStringLiteral("formatCache"));
    s.sync();
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
    QVERIFY2(view->header()->sectionSize(logger)
                 >= view->fontMetrics().horizontalAdvance(QLatin1String(longName)),
             "the Subsystem column did not take the widest interned name");
}

// The two commands the column header menu offers besides the visibility list. They are
// window-owned actions with object names, so a test drives them exactly as it drives the
// timestamp modes — without opening a modal menu.
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
    // (SPEC.md §3, M17). Two of them, deliberately: each writes its own reason into the
    // status bar as it happens, so with one message per failure the reader would be
    // left with the last one's reason and no hint that a second file had even been
    // asked for. They are reported TOGETHER instead, the way session restore lists the
    // logs it could not reopen.
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

    auto *status = w.findChild<QLabel *>(QStringLiteral("statusLabel"));
    QVERIFY(status);
    QVERIFY2(status->text().contains(QStringLiteral("one.log (web1)")),
             qPrintable(status->text()));
    QVERIFY2(status->text().contains(QStringLiteral("two.log (web2)")),
             qPrintable(status->text()));
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

#include "tst_multidoc.moc"
