#include <QtTest>

#include <QAction>
#include <QApplication>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>

#include "DocumentView.h"
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

private slots:
    void initTestCase();
    void init();
    void theBarSaysWhichMatchOfHowMany();
    void findNextPastTheLastMatchSaysItWrappedToTheTop();
    void findPreviousPastTheFirstMatchSaysItWrappedToTheBottom();
    void aQueryThatMatchesNothingSaysSo();
    void emptyingTheQueryClearsWhatTheBarSaid();
    void reopeningTheBarDoesNotLeaveTheLastResultBehind();
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

#include "tst_find.moc"
