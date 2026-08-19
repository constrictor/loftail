#include <QtTest>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QFile>
#include <QFontDatabase>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QToolButton>

#include "DocumentView.h"
#include "Filter.h"
#include "FindBar.h"
#include "LogView.h"
#include "MainWindow.h"

using namespace loftail;

// What the Find bar SAYS (SPEC.md §5). Finding a match used to report nothing at all —
// the status label was set to an empty string on success — so a search that landed and a
// search that had not run yet looked identical, and the wrap that F3 performs at the last
// match was a silent teleport to the top.
//
// The three outcomes are pinned here through a real MainWindow: which match of how many,
// that a search came back round, and that nothing matched. The label is found by object
// name; only its reported TEXT is compared, which is the whole subject of the test.
//
// The bounded half of the count — the "47+" a log too large to count in the moment gets,
// and the position-less "match" when the match lies past where counting stopped — is
// Find::tally's own contract and is pinned in tst_filter, where a bound can be made small
// instead of a log made huge.
class TestFind : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QString       m_log;

    // Six records, three of which carry "alpha" — in the message only, so no timestamp,
    // thread, level or subsystem cell can match it and the count is the messages'.
    enum Row { kAlphaOne = 0, kAlphaTwo = 2, kAlphaThree = 4 };

    static void writeLog(const QString &path)
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("2026-07-21 10:00:00,000 [main] INFO  net.io - alpha one\n"
                "2026-07-21 10:00:01,000 [main] INFO  net.io - bravo\n"
                "2026-07-21 10:00:02,000 [main] WARN  db.pool - alpha two\n"
                "2026-07-21 10:00:03,000 [main] INFO  net.io - charlie\n"
                "2026-07-21 10:00:04,000 [main] ERROR net.io - alpha three\n"
                "2026-07-21 10:00:05,000 [main] INFO  net.io - delta\n");
        f.close();
    }

    static QTabWidget *tabs(const MainWindow &w)
    {
        return w.findChild<QTabWidget *>(QStringLiteral("documentTabs"));
    }

    static DocumentView *activeView(const MainWindow &w)
    {
        QTabWidget *t = tabs(w);
        return t ? qobject_cast<DocumentView *>(t->currentWidget()) : nullptr;
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

    static QLineEdit *queryField(const MainWindow &w)
    {
        DocumentView *view = activeView(w);
        return view ? view->findBar()->findChild<QLineEdit *>(QStringLiteral("findEdit")) : nullptr;
    }

    // The report AS GIVEN. The label renders an elided form of it into a cell whose
    // width comes from the bar and never from the text (the case at the bottom of this
    // file), so the label's own text is a rendering and this is the report.
    static QString reported(const MainWindow &w)
    {
        DocumentView *view = activeView(w);
        return view ? view->findBar()->status() : QStringLiteral("<no view>");
    }

    // The query the table is MARKING, or a null string when it is marking nothing. The
    // view holds the matcher itself rather than a list of positions, so this is the whole
    // of the state the marking has (ARCHITECTURE.md §7.1.4).
    static QString marked(const MainWindow &w)
    {
        DocumentView *view = activeView(w);
        if (!view)
            return QStringLiteral("<no view>");
        const TextMatcher &m = view->logView()->findMatcher();
        return m.isEmpty() ? QString() : m.pattern();
    }

    static int cursorRow(const MainWindow &w)
    {
        DocumentView *view = activeView(w);
        return view ? view->logView()->currentRecord() : -1;
    }

    static void findNext(const MainWindow &w)
    {
        w.findChild<QAction *>(QStringLiteral("findNextAction"))->trigger();
    }

    static void findPrevious(const MainWindow &w)
    {
        w.findChild<QAction *>(QStringLiteral("findPreviousAction"))->trigger();
    }

    // The three Edit-menu entries, by object name. Their enabled state is the subject of
    // the three cases at the bottom of this file.
    static void checkFindItemsEnabled(const MainWindow &w, bool expected)
    {
        for (const char *name : {"findAction", "findNextAction", "findPreviousAction"}) {
            QAction *a = w.findChild<QAction *>(QLatin1String(name));
            QVERIFY2(a, name);
            QVERIFY2(a->isEnabled() == expected, name);
        }
    }

    // Whether the bar is ON SCREEN. Every case in this file used to assert on status()
    // alone, and a report written into a hidden bar satisfies that exactly as well as one
    // the reader can read — which is how F3 shipped answering into nothing.
    static bool barVisible(const MainWindow &w)
    {
        DocumentView *view = activeView(w);
        return view && view->findBar()->isVisible();
    }

    // The FIND-NEXT GESTURE, not the menu item. trigger() on the action starts halfway
    // through the story: the subject here is what the window does with F3 pressed while
    // the reader is looking at the table, so the key has to be a real one.
    static void pressFindNext(MainWindow &w, Qt::KeyboardModifiers mods = Qt::NoModifier)
    {
        QTest::keyClick(&w, Qt::Key_F3, mods);
    }

    // The focus widget WITHIN the window. QWidget::focusWidget() and not
    // QApplication::focusWidget(), because the latter answers null unless the window is
    // active, which an offscreen window need not be.
    static QWidget *focusIn(const MainWindow &w) { return w.focusWidget(); }

    // The gesture itself: a real key press at the query field, found by object name.
    // `key` is Qt::Key_Return (the main keyboard's) or Qt::Key_Enter (the keypad's) —
    // both reach the same field and both must mean the same thing.
    static void pressAtQueryField(const MainWindow &w, int key, Qt::KeyboardModifiers mods)
    {
        QLineEdit *query = queryField(w);
        QVERIFY(query);
        QTest::keyClick(query, Qt::Key(key), mods);
    }

private slots:
    void initTestCase();
    void init();
    void theBarSaysWhichMatchOfHowMany();
    void findNextPastTheLastMatchSaysItWrappedToTheTop();
    void findPreviousPastTheFirstMatchSaysItWrappedToTheBottom();
    void aQueryThatMatchesNothingSaysSo();
    void enterInTheQueryFieldSearchesForwards();
    void shiftEnterInTheQueryFieldSearchesBackwards();
    void shiftEnterPastTheFirstMatchWrapsToTheBottomAndSaysSo();
    void theKeypadsEnterMeansWhatTheKeyboardsDoes();
    void emptyingTheQueryClearsWhatTheBarSaid();
    void reopeningTheBarDoesNotLeaveTheLastResultBehind();
    void theQueryThatFoundAMatchIsHandedToTheTableToMark();
    void changingTheQueryChangesWhatIsMarkedWithIt();
    void aQueryThatMatchesNothingLeavesNothingMarked();
    void closingTheBarTakesTheMarksWithIt();
    void theFindItemsAreDeadUntilThereIsALogToSearch();
    void theFindItemsFollowTheTabInFrontAndDieWithTheLastOne();
    void anExplicitFindOnAnEmptyQuerySaysThereIsNothingToFind();
    void findNextWithTheBarClosedPutsItOnScreenToAnswerInto();
    void steppingThroughMatchesAfterEscapeIsNotDoneBehindTheReadersBack();
    void escapeStillClosesTheBarThatFindNextRevealed();
    void aQueryThatMatchesNothingLeavesTheFocusWhereItWas();
    void theReopenedBarClaimsNothingItIsNotShowing();
    void theControlsDoNotMoveWhenTheStatusTextChanges();
};

void TestFind::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_log = m_dir.filePath(QStringLiteral("find.log"));
    writeLog(m_log);
}

void TestFind::init()
{
    // Each case closes its window, which saves a session the next one would restore as
    // an extra tab. Start clean.
    QSettings settings;
    settings.remove(QStringLiteral("session"));
    settings.sync();
}

void TestFind::theBarSaysWhichMatchOfHowMany()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_log);
    waitUntilIndexed(w);

    QLineEdit *query = queryField(w);
    QVERIFY(query);
    query->setText(QStringLiteral("alpha")); // typing searches from the top

    QCOMPARE(reported(w), QStringLiteral("1 of 3"));
    QCOMPARE(cursorRow(w), int(kAlphaOne));

    findNext(w);
    QCOMPARE(reported(w), QStringLiteral("2 of 3"));
    QCOMPARE(cursorRow(w), int(kAlphaTwo));

    findNext(w);
    QCOMPARE(reported(w), QStringLiteral("3 of 3"));
    QCOMPARE(cursorRow(w), int(kAlphaThree));

    w.close();
}

void TestFind::findNextPastTheLastMatchSaysItWrappedToTheTop()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_log);
    waitUntilIndexed(w);

    queryField(w)->setText(QStringLiteral("alpha"));
    findNext(w);
    findNext(w);
    QCOMPARE(cursorRow(w), int(kAlphaThree)); // on the last match

    // The search still wraps (SPEC.md §5) — it just no longer does it in silence.
    findNext(w);
    QCOMPARE(cursorRow(w), int(kAlphaOne));
    QCOMPARE(reported(w), QStringLiteral("1 of 3, wrapped to the top"));

    // And an ordinary step afterwards says nothing about wrapping.
    findNext(w);
    QCOMPARE(reported(w), QStringLiteral("2 of 3"));

    w.close();
}

void TestFind::findPreviousPastTheFirstMatchSaysItWrappedToTheBottom()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_log);
    waitUntilIndexed(w);

    queryField(w)->setText(QStringLiteral("alpha"));
    QCOMPARE(cursorRow(w), int(kAlphaOne));

    findPrevious(w);
    QCOMPARE(cursorRow(w), int(kAlphaThree));
    QCOMPARE(reported(w), QStringLiteral("3 of 3, wrapped to the bottom"));

    findPrevious(w);
    QCOMPARE(reported(w), QStringLiteral("2 of 3"));

    w.close();
}

void TestFind::aQueryThatMatchesNothingSaysSo()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_log);
    waitUntilIndexed(w);

    queryField(w)->setText(QStringLiteral("zulu"));
    QCOMPARE(reported(w), QStringLiteral("no match"));

    // Pressing F3 on a query that matches nothing keeps saying so rather than falling
    // back to silence.
    findNext(w);
    QCOMPARE(reported(w), QStringLiteral("no match"));

    w.close();
}

// Enter in the box is Find Next and Shift+Enter is Find Previous (SPEC.md §5). Enter was
// hardwired forward: QLineEdit emits returnPressed() whatever modifiers are held, so
// Shift+Enter searched forward like a plain one. The direction each gesture actually took
// is what these three cases assert — through the cursor, not through the bar's wording.
void TestFind::enterInTheQueryFieldSearchesForwards()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_log);
    waitUntilIndexed(w);

    queryField(w)->setText(QStringLiteral("alpha"));
    QCOMPARE(cursorRow(w), int(kAlphaOne));

    pressAtQueryField(w, Qt::Key_Return, Qt::NoModifier);
    QCOMPARE(cursorRow(w), int(kAlphaTwo));
    QCOMPARE(reported(w), QStringLiteral("2 of 3"));

    pressAtQueryField(w, Qt::Key_Return, Qt::NoModifier);
    QCOMPARE(cursorRow(w), int(kAlphaThree));
    QCOMPARE(reported(w), QStringLiteral("3 of 3"));

    w.close();
}

void TestFind::shiftEnterInTheQueryFieldSearchesBackwards()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_log);
    waitUntilIndexed(w);

    queryField(w)->setText(QStringLiteral("alpha"));
    findNext(w);
    findNext(w);
    QCOMPARE(cursorRow(w), int(kAlphaThree)); // on the last match, so backwards has room

    pressAtQueryField(w, Qt::Key_Return, Qt::ShiftModifier);
    QCOMPARE(cursorRow(w), int(kAlphaTwo));
    QCOMPARE(reported(w), QStringLiteral("2 of 3"));

    pressAtQueryField(w, Qt::Key_Return, Qt::ShiftModifier);
    QCOMPARE(cursorRow(w), int(kAlphaOne));
    QCOMPARE(reported(w), QStringLiteral("1 of 3"));

    // And the gesture next to it still goes the other way.
    pressAtQueryField(w, Qt::Key_Return, Qt::NoModifier);
    QCOMPARE(cursorRow(w), int(kAlphaTwo));

    w.close();
}

void TestFind::shiftEnterPastTheFirstMatchWrapsToTheBottomAndSaysSo()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_log);
    waitUntilIndexed(w);

    queryField(w)->setText(QStringLiteral("alpha"));
    QCOMPARE(cursorRow(w), int(kAlphaOne)); // on the first match

    // Same walk Find Previous performs, so the wrap note is produced for the gesture too.
    pressAtQueryField(w, Qt::Key_Return, Qt::ShiftModifier);
    QCOMPARE(cursorRow(w), int(kAlphaThree));
    QCOMPARE(reported(w), QStringLiteral("3 of 3, wrapped to the bottom"));

    w.close();
}

void TestFind::theKeypadsEnterMeansWhatTheKeyboardsDoes()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_log);
    waitUntilIndexed(w);

    queryField(w)->setText(QStringLiteral("alpha"));
    QCOMPARE(cursorRow(w), int(kAlphaOne));

    // Key_Enter carries Qt::KeypadModifier on a real keyboard, which says nothing about
    // direction and must not defeat the Shift test.
    pressAtQueryField(w, Qt::Key_Enter, Qt::ShiftModifier | Qt::KeypadModifier);
    QCOMPARE(cursorRow(w), int(kAlphaThree)); // backwards, wrapping

    pressAtQueryField(w, Qt::Key_Enter, Qt::KeypadModifier);
    QCOMPARE(cursorRow(w), int(kAlphaOne)); // forwards, wrapping

    w.close();
}

void TestFind::emptyingTheQueryClearsWhatTheBarSaid()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_log);
    waitUntilIndexed(w);

    queryField(w)->setText(QStringLiteral("alpha"));
    QVERIFY(!reported(w).isEmpty());

    queryField(w)->clear();
    QCOMPARE(reported(w), QString()); // no query, nothing to report about one

    w.close();
}

void TestFind::reopeningTheBarDoesNotLeaveTheLastResultBehind()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_log);
    waitUntilIndexed(w);

    queryField(w)->setText(QStringLiteral("alpha"));
    QCOMPARE(reported(w), QStringLiteral("1 of 3"));

    // Ctrl+F selects the old query for replacement, so what the old query found is about
    // to stop being true.
    activeView(w)->activateFind();
    QCOMPARE(reported(w), QString());

    w.close();
}

int main(int argc, char *argv[])
{
    QTemporaryDir configHome;
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
    qputenv("XDG_DATA_HOME", configHome.path().toUtf8());
    qputenv("HOME", configHome.path().toUtf8());
    qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("loftail-test-find"));
    QApplication::setApplicationName(QStringLiteral("loftail-test-find"));

    TestFind tc;
    return QTest::qExec(&tc, argc, argv);
}

// --- what was found is marked where it was found (SPEC.md §5) ----------------
//
// Find selects a record; the mark says WHERE in it. The view is handed the matcher the
// search itself ran with — never a list of positions — so these cases turn on that one
// piece of state: which query the table is marking, and when it is marking none.
// Whether the marks are actually PAINTED, and where, is tst_logview's question.

void TestFind::theQueryThatFoundAMatchIsHandedToTheTableToMark()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_log);
    waitUntilIndexed(w);

    QVERIFY2(marked(w).isNull(), "something was marked before anything was searched for");

    queryField(w)->setText(QStringLiteral("alpha"));
    QCOMPARE(reported(w), QStringLiteral("1 of 3"));
    QCOMPARE(marked(w), QStringLiteral("alpha"));

    // Stepping to the next match leaves the same query marked: what is marked is the
    // search, not the record it happens to be sitting on.
    findNext(w);
    QCOMPARE(cursorRow(w), int(kAlphaTwo));
    QCOMPARE(marked(w), QStringLiteral("alpha"));

    // The digest strip is deliberately never armed: Find walks the table's rows, and a
    // mark down there would say the search had landed in it.
    QVERIFY(activeView(w)->digestView()->findMatcher().isEmpty());

    w.close();
}

void TestFind::changingTheQueryChangesWhatIsMarkedWithIt()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_log);
    waitUntilIndexed(w);

    queryField(w)->setText(QStringLiteral("alpha"));
    QCOMPARE(marked(w), QStringLiteral("alpha"));

    // A second query replaces the first — a mark left over from a query nobody is
    // searching for any more points at nothing.
    queryField(w)->setText(QStringLiteral("bravo"));
    QCOMPARE(marked(w), QStringLiteral("bravo"));

    // Emptying the box marks nothing, exactly as it reports nothing: an empty query
    // matches every record, and marking everything is not a mark.
    queryField(w)->setText(QString());
    QVERIFY(marked(w).isNull());

    // A regex that will not compile has nothing to mark either.
    w.findChild<QCheckBox *>(QStringLiteral("findRegex"))->setChecked(true);
    queryField(w)->setText(QStringLiteral("alpha("));
    QCOMPARE(reported(w), QStringLiteral("bad regex"));
    QVERIFY(marked(w).isNull());

    w.close();
}

void TestFind::aQueryThatMatchesNothingLeavesNothingMarked()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_log);
    waitUntilIndexed(w);

    queryField(w)->setText(QStringLiteral("alpha"));
    QCOMPARE(marked(w), QStringLiteral("alpha"));

    // Typing on past the last match: nothing matches, so nothing is marked — the marks
    // from the query's shorter, matching prefix must not survive it.
    queryField(w)->setText(QStringLiteral("alphabet"));
    QCOMPARE(reported(w), QStringLiteral("no match"));
    QVERIFY(marked(w).isNull());

    w.close();
}

void TestFind::closingTheBarTakesTheMarksWithIt()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_log);
    waitUntilIndexed(w);

    queryField(w)->setText(QStringLiteral("alpha"));
    QCOMPARE(marked(w), QStringLiteral("alpha"));

    // The bar is gone from the screen, so a mark still in the table would be a claim
    // about a search the reader can no longer see.
    activeView(w)->findBar()->findChild<QToolButton *>(QStringLiteral("findClose"))->click();
    QVERIFY(marked(w).isNull());

    // And reopening it does not bring them back on its own; the next search does.
    activeView(w)->activateFind();
    QVERIFY(marked(w).isNull());
    findNext(w);
    QCOMPARE(marked(w), QStringLiteral("alpha"));

    w.close();
}

#include "tst_find.moc"

// --- when the three Edit-menu items are live (bugs.md) -----------------------
//
// Find, Find Next and Find Previous were the only Edit-menu entries updateActionStates()
// had never been told about, so all three were offered with no file open — where Find
// opened nothing, because its handler tests m_activeView and returns. They now track the
// active tab exactly as Copy, Copy as Columns and Select All beside them do.
//
// They track NOTHING ELSE, and the query in particular: F3 and Shift+F3 are shortcuts, a
// disabled QAction swallows its shortcut without a word, and the answer for an empty
// query belongs in the bar that holds it. The third case is that answer.

void TestFind::theFindItemsAreDeadUntilThereIsALogToSearch()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();

    checkFindItemsEnabled(w, false); // nothing open: nothing to search

    w.openFile(m_log);
    waitUntilIndexed(w);
    checkFindItemsEnabled(w, true);

    w.close();
}

void TestFind::theFindItemsFollowTheTabInFrontAndDieWithTheLastOne()
{
    const QString other = m_dir.filePath(QStringLiteral("find-other.log"));
    writeLog(other);

    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_log);
    w.openFile(other);
    waitUntilIndexed(w);
    QCOMPARE(tabs(w)->count(), 2);
    checkFindItemsEnabled(w, true);

    // Switching between two logs leaves them live — the condition is read from the
    // INCOMING view, and setActiveView() has already set it before it calls
    // updateActionStates() (the ordering trap 3bacaca recorded for the panes).
    tabs(w)->setCurrentIndex(0);
    checkFindItemsEnabled(w, true);

    // Closing one of two leaves a log to search; closing the last one does not.
    QAction *closeTab = w.findChild<QAction *>(QStringLiteral("closeTabAction"));
    QVERIFY(closeTab);
    closeTab->trigger();
    QCOMPARE(tabs(w)->count(), 1);
    checkFindItemsEnabled(w, true);

    closeTab->trigger();
    QCOMPARE(tabs(w)->count(), 0);
    checkFindItemsEnabled(w, false);

    w.close();
}

void TestFind::anExplicitFindOnAnEmptyQuerySaysThereIsNothingToFind()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_log);
    waitUntilIndexed(w);

    // F3 before anything has been typed used to report nothing whatsoever, which is
    // indistinguishable from a search that ran and had nothing to say — and then, once
    // it did report, it reported into a bar that was still hidden. Both halves are
    // asserted: what was said, and that there is somewhere to read it.
    QVERIFY(!barVisible(w));
    findNext(w);
    QCOMPARE(reported(w), QStringLiteral("no search text"));
    QVERIFY(barVisible(w));
    findPrevious(w);
    QCOMPARE(reported(w), QStringLiteral("no search text"));
    QVERIFY(barVisible(w));

    // Deleting the query is the other empty-query case and stays quiet: the reader just
    // removed their own text, and telling them so is a nag rather than an answer.
    queryField(w)->setText(QStringLiteral("alpha"));
    QCOMPARE(reported(w), QStringLiteral("1 of 3"));
    queryField(w)->clear();
    QCOMPARE(reported(w), QString());

    // But asking again, deliberately, still gets an answer — in a bar still on screen.
    findNext(w);
    QCOMPARE(reported(w), QStringLiteral("no search text"));
    QVERIFY(barVisible(w));

    w.close();
}

// F3 is a window shortcut and the bar is a DocumentView child that starts hidden, so the
// two used to meet only by luck: a reader who opened a log and pressed F3 got
// `no search text` written into a widget that was never on screen — exactly the silence
// the branch was added to remove. The gesture is a REAL key press here, because that is
// the whole of what is being asked: trigger() on the menu action would answer a question
// about the action rather than about the key the reader pressed.
void TestFind::findNextWithTheBarClosedPutsItOnScreenToAnswerInto()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_log);
    waitUntilIndexed(w);

    QVERIFY(!barVisible(w)); // nothing has opened it: no Ctrl+F, no typing
    pressFindNext(w);

    QVERIFY(barVisible(w));
    QCOMPARE(reported(w), QStringLiteral("no search text"));
    // And the label carrying that sentence is on screen too, which is the claim the
    // bar's own visibility only implies.
    auto *label = activeView(w)->findBar()->findChild<QLabel *>(QStringLiteral("findStatus"));
    QVERIFY(label);
    QVERIFY(label->isVisible());

    w.close();
}

// The wider half of the same defect, and the one that shows the state machine
// contradicting itself. Escape closes the bar and takes the marks with it (SPEC.md §5),
// on the argument that a mark is a claim about a search the reader can no longer see —
// and the very next F3 used to re-arm exactly those marks, move the cursor and write the
// wrap note into the hidden label. So the log was searched behind the reader's back, and
// the one report that would have explained the jump was the report they could not read.
void TestFind::steppingThroughMatchesAfterEscapeIsNotDoneBehindTheReadersBack()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_log);
    waitUntilIndexed(w);

    activeView(w)->activateFind();
    queryField(w)->setText(QStringLiteral("alpha"));
    QCOMPARE(reported(w), QStringLiteral("1 of 3"));
    QCOMPARE(cursorRow(w), int(kAlphaOne));

    // Escape: the bar goes, the marks go, the query stays (it is the reader's).
    QTest::keyClick(focusIn(w), Qt::Key_Escape);
    QVERIFY(!barVisible(w));
    QVERIFY(marked(w).isNull());
    QCOMPARE(queryField(w)->text(), QStringLiteral("alpha"));

    // F3 still searches — and now says so somewhere the reader can see.
    pressFindNext(w);
    QVERIFY(barVisible(w));
    QCOMPARE(cursorRow(w), int(kAlphaTwo));
    QCOMPARE(reported(w), QStringLiteral("2 of 3"));
    QCOMPARE(marked(w), QStringLiteral("alpha"));

    // Including the wrap, which is the report this file exists for: the cursor teleports
    // to the top and the sentence explaining it must not be written into a hidden bar.
    pressFindNext(w);
    pressFindNext(w);
    QCOMPARE(cursorRow(w), int(kAlphaOne));
    QVERIFY(barVisible(w));
    QCOMPARE(reported(w), QStringLiteral("1 of 3, wrapped to the top"));

    w.close();
}

// The reveal moves the focus, and that is not decoration. Escape is handled in
// FindBar::keyPressEvent, and the bar is a SIBLING of the table under DocumentView, not
// an ancestor — so an Escape pressed with the caret still in the table propagates
// LogView -> DocumentView -> MainWindow and never reaches the bar at all. A reveal
// written as a bare show() therefore ships a bar closable only with the mouse, which no
// assertion about status(), visibility or marks can see.
void TestFind::escapeStillClosesTheBarThatFindNextRevealed()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_log);
    waitUntilIndexed(w);

    activeView(w)->logView()->setFocus();
    QCOMPARE(focusIn(w), static_cast<QWidget *>(activeView(w)->logView()));

    pressFindNext(w);
    QVERIFY(barVisible(w));
    // The caret came with the bar: it is in the query field, which is what makes the
    // next line's Escape reach FindBar::keyPressEvent.
    QCOMPARE(focusIn(w), static_cast<QWidget *>(queryField(w)));

    QTest::keyClick(focusIn(w), Qt::Key_Escape);
    QVERIFY(!barVisible(w));

    w.close();
}

// The other side of the same rule: a reveal is for a bar that is not there. On one that
// is, it must touch nothing — a `no match` on a real query leaves the focus inside the
// bar so the next F3 goes on being typed at, and the query stays unselected so that
// keystroke does not delete the text being stepped through.
void TestFind::aQueryThatMatchesNothingLeavesTheFocusWhereItWas()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_log);
    waitUntilIndexed(w);

    activeView(w)->activateFind();
    QLineEdit *query = queryField(w);
    QVERIFY(query);
    QTest::keyClicks(query, QStringLiteral("zulu")); // matches nothing in this log
    QCOMPARE(reported(w), QStringLiteral("no match"));

    findNext(w);
    QCOMPARE(reported(w), QStringLiteral("no match"));
    QVERIFY(barVisible(w));
    QCOMPARE(focusIn(w), static_cast<QWidget *>(query));
    QCOMPARE(query->text(), QStringLiteral("zulu"));
    QVERIFY(!query->hasSelectedText()); // nothing re-selected it under the reader

    w.close();
}

// Ctrl+F on a bar that was closed with a query still in it: the box shows that query
// again, and the table marks nothing. That is not the hidden-search state above and must
// not become it — Ctrl+F asks for a box to type into, so it may not move the cursor, and
// a mark whose search never ran is a mark that disagrees with the hit (ARCHITECTURE.md
// §7.1.4). What makes the state honest is that it claims nothing: no report, no marks,
// and the standing query selected for replacement. One gesture then puts all three into
// agreement at once.
void TestFind::theReopenedBarClaimsNothingItIsNotShowing()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_log);
    waitUntilIndexed(w);

    activeView(w)->activateFind();
    queryField(w)->setText(QStringLiteral("alpha"));
    QCOMPARE(reported(w), QStringLiteral("1 of 3"));
    const int cursorBefore = cursorRow(w);

    QTest::keyClick(focusIn(w), Qt::Key_Escape);
    QVERIFY(!barVisible(w));

    activeView(w)->activateFind();
    QVERIFY(barVisible(w));
    QCOMPARE(queryField(w)->text(), QStringLiteral("alpha")); // the reader's, kept
    QVERIFY(queryField(w)->hasSelectedText());                // and offered for replacement
    QCOMPARE(reported(w), QString());                         // claiming nothing
    QVERIFY(marked(w).isNull());
    QCOMPARE(cursorRow(w), cursorBefore); // reopening searched nothing

    // And the next find is the one that makes the box, the marks and the report agree.
    pressFindNext(w);
    QVERIFY(barVisible(w));
    QCOMPARE(marked(w), QStringLiteral("alpha"));
    QCOMPARE(reported(w), QStringLiteral("2 of 3"));

    w.close();
}

// The status text may not move the controls beside it — the bug that made stepping
// through matches with the mouse dangerous. The label used to sit at stretch 0 in a row
// whose only elastic item was the query box, so every pixel the wording grew by was
// taken from the box and ▲ ▼ Regex Case ✕ all slid left by exactly that much: with the
// pointer parked on ▼, the wrap note appearing at the last match walked the Case
// checkbox under it, and the click meant for "next match" restarted the search
// case-sensitively.
//
// So this asserts on GEOMETRY and not on text, in the shape of
// tst_filterpane::theContextRowLaysOutWithoutOverlapOrClipping and for the same reason:
// every widget existed, was visible and held the right value in the broken build — only
// its position was wrong, which no other kind of test can see.
void TestFind::theControlsDoNotMoveWhenTheStatusTextChanges()
{
    // Every wording the bar can produce, plus one nobody will see: the match count is
    // unbounded (`128 of 4096+`) and tr() puts translation on top, so no fix that sizes
    // the label from a "longest wording" constant survives this row.
    const QStringList reports = {
        QString(),
        QStringLiteral("2 of 2"),
        QStringLiteral("1 of 2, wrapped to the bottom"),
        QStringLiteral("128 of 4096+, wrapped to the bottom"),
    };
    const QStringList controls = {
        QStringLiteral("findPrevious"), QStringLiteral("findNext"),
        QStringLiteral("findRegex"),    QStringLiteral("findCase"),
        QStringLiteral("findClose"),    QStringLiteral("findEdit"),
    };

    FindBar bar;
    bar.show();

    // Two widths, because the defect was not an overflow effect: all the surplus went to
    // the stretch item however much of it there was, so it bit at every size.
    for (int width : {700, 1100}) {
        bar.resize(width, bar.sizeHint().height());
        bar.layout()->activate();

        QHash<QString, QRect> first;
        for (const QString &report : reports) {
            bar.setStatus(report);
            bar.layout()->activate();
            for (const QString &name : controls) {
                auto *widget = bar.findChild<QWidget *>(name);
                QVERIFY2(widget, qPrintable(name));
                if (!first.contains(name))
                    first.insert(name, widget->geometry());
                QVERIFY2(widget->geometry() == first.value(name),
                         qPrintable(QStringLiteral("%1 moved to %2 at width %3 under \"%4\"")
                                        .arg(name)
                                        .arg(QString::number(widget->geometry().x()))
                                        .arg(width)
                                        .arg(report)));
            }
        }
    }

    // What the cell cannot show is elided, and the whole report stays one hover away —
    // "the column elides; the tooltip does not". Font metrics decide whether the longest
    // wording fits, and a platform with no font database (Windows offscreen) answers 0
    // to every advance, so nothing would ever be cut short there.
    if (QFontDatabase::families().isEmpty())
        QSKIP("no font database: nothing elides, so there is nothing to hover over");

    auto *label = bar.findChild<QLabel *>(QStringLiteral("findStatus"));
    QVERIFY(label);
    bar.resize(700, bar.sizeHint().height());
    bar.layout()->activate();
    const QString longest = QStringLiteral("128 of 4096+, wrapped to the bottom");
    bar.setStatus(longest);
    QVERIFY(label->width() < label->fontMetrics().horizontalAdvance(longest)); // it is cut
    QVERIFY(label->text() != longest);
    QCOMPARE(label->toolTip(), longest);
    QCOMPARE(bar.status(), longest); // and the report itself is untouched by the cutting

    // A report that fits offers no tooltip: only a value actually cut short does.
    bar.setStatus(QStringLiteral("2 of 2"));
    QCOMPARE(label->text(), QStringLiteral("2 of 2"));
    QCOMPARE(label->toolTip(), QString());
}

