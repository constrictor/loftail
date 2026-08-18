#include <QtTest>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QFile>
#include <QLabel>
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

    static QString reported(const MainWindow &w)
    {
        DocumentView *view = activeView(w);
        if (!view)
            return QStringLiteral("<no view>");
        auto *label = view->findBar()->findChild<QLabel *>(QStringLiteral("findStatus"));
        return label ? label->text() : QStringLiteral("<no label>");
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
