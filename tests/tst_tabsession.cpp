#include <QtTest>

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QFile>
#include <QHeaderView>
#include <QSettings>
#include <QTabBar>
#include <QTabWidget>
#include <QTemporaryDir>

#include "LogView.h"
#include "MainWindow.h"

using namespace loftail;

// M9 — the tab arrangement round-trip (SPEC.md §10, ARCHITECTURE.md §12). Two
// MainWindow instances over one isolated settings store model quit-and-relaunch: the
// first opens two files and adds a second view onto one of them, then reorders the
// tabs; the second must come back with the same tabs in the same order, each view
// keeping its own column layout and wrap mode.
//
// The tab order IS the saved view order — there is no separate layout blob to key
// off, since the document well replaced the per-view document docks — so a
// regression here degrades silently to "the tabs came back shuffled", which is what
// these assertions catch.
class TestTabSession : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QString       m_a;
    QString       m_b;

    static void writeLog(const QString &path, int lines)
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        for (int i = 0; i < lines; ++i) {
            f.write(QStringLiteral("2026-07-21 10:00:%1,000 [main] INFO  net.io - line %2\n")
                        .arg(i % 60, 2, 10, QLatin1Char('0'))
                        .arg(i)
                        .toUtf8());
        }
        f.close();
    }

    static QTabWidget *tabs(const MainWindow &w)
    {
        return w.findChild<QTabWidget *>(QStringLiteral("documentTabs"));
    }

    // The tab titles, left to right.
    static QStringList tabTitles(const MainWindow &w)
    {
        QStringList out;
        QTabWidget *t = tabs(w);
        for (int i = 0; t && i < t->count(); ++i)
            out.append(t->tabText(i));
        return out;
    }

    static LogView *viewInTab(const MainWindow &w, int index)
    {
        QTabWidget *t = tabs(w);
        return t && index >= 0 && index < t->count() ? t->widget(index)->findChild<LogView *>()
                                                     : nullptr;
    }

    // A tab's title carries its file's indexing progress while the worker runs, so
    // the title assertions below only hold once every scan has finished.
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
    // Every case here writes a session and then relaunches into it, so each must
    // start from a clean store or it would restore the previous case's windows.
    void init();
    void tabOrderAndPerViewStateRestore();
    void missingFileRestoresAsWaitingAndTheRestStillOpen();
};

void TestTabSession::init()
{
    QSettings s;
    s.remove(QStringLiteral("session"));
    s.sync();
}

void TestTabSession::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_a = m_dir.filePath(QStringLiteral("a.log"));
    m_b = m_dir.filePath(QStringLiteral("b.log"));
    writeLog(m_a, 30);
    writeLog(m_b, 20);
}

void TestTabSession::tabOrderAndPerViewStateRestore()
{
    // --- Round 1: two files, a second view on b.log, dragged to the front --------
    {
        MainWindow w;
        w.resize(1000, 700);
        w.show();
        w.openFile(m_a);
        w.openFile(m_b);
        QTRY_COMPARE(w.findChildren<LogView *>().size(), 2);

        QAction *newView = w.findChild<QAction *>(QStringLiteral("newViewAction"));
        QVERIFY(newView);
        newView->trigger(); // a second view onto b.log, the active file
        waitUntilIndexed(w);
        QCOMPARE(tabTitles(w),
                 QStringList({QStringLiteral("a.log"), QStringLiteral("b.log [1]"),
                              QStringLiteral("b.log [2]")}));

        // Give the second view of b.log a column layout of its own, so the round-trip
        // has something per-view to prove (the others keep the format's defaults).
        LogView *second = viewInTab(w, 2);
        QVERIFY(second);
        second->header()->resizeSection(0, 321);
        second->setWrapMode(LogView::WrapMode::AlwaysOn);

        // Drag that tab to the front — the programmatic stand-in for the user doing
        // it with the mouse. The saved order must follow the tab bar, not creation.
        tabs(w)->tabBar()->moveTab(2, 0);
        QCOMPARE(tabTitles(w),
                 QStringList({QStringLiteral("b.log [1]"), QStringLiteral("a.log"),
                              QStringLiteral("b.log [2]")}));
        tabs(w)->setCurrentIndex(0);

        QCloseEvent ev; // drives saveSession()
        QCoreApplication::sendEvent(&w, &ev);
    }

    // --- Round 2: relaunch ------------------------------------------------------
    MainWindow w;
    w.show();
    QTRY_COMPARE(w.findChildren<LogView *>().size(), 3);
    waitUntilIndexed(w);

    QVERIFY(tabs(w));
    QCOMPARE(tabs(w)->count(), 3); // two files, one of them in two views — not five

    // Both files came back, one of them twice, in the order the tabs were left in.
    QCOMPARE(tabTitles(w),
             QStringList({QStringLiteral("b.log [1]"), QStringLiteral("a.log"),
                          QStringLiteral("b.log [2]")}));
    QCOMPARE(tabs(w)->currentIndex(), 0); // ...and the active tab is the saved one

    // Per-view state: the moved view's columns and wrap mode, not its twin's.
    LogView *moved = viewInTab(w, 0);
    QVERIFY(moved);
    QCOMPARE(moved->header()->sectionSize(0), 321);
    QCOMPARE(int(moved->wrapMode()), int(LogView::WrapMode::AlwaysOn));
    LogView *sibling = viewInTab(w, 2);
    QVERIFY(sibling);
    QVERIFY(sibling->header()->sectionSize(0) != 321); // independent of its twin
}

void TestTabSession::missingFileRestoresAsWaitingAndTheRestStillOpen()
{
    // A file that has gone away between sessions must not take the others down with
    // it, and must not raise a dialog every launch (SPEC.md §10).
    //
    // M13 CHANGED WHAT "not taking the others down" MEANS, and the change is the point
    // of the case: the missing file used to be dropped from the restore, which — since
    // saveSession() writes only the files that are open — silently forgot it at the
    // next quit. Now it comes back as a WAITING tab and picks the log up if it returns.
    const QString doomed = m_dir.filePath(QStringLiteral("doomed.log"));
    writeLog(doomed, 10);

    {
        MainWindow w;
        w.resize(1000, 700);
        w.show();
        w.openFile(m_a);
        w.openFile(doomed);
        QTRY_COMPARE(w.findChildren<LogView *>().size(), 2);
        QCloseEvent ev;
        QCoreApplication::sendEvent(&w, &ev);
    }

    QVERIFY(QFile::remove(doomed));

    MainWindow w;
    w.show();
    QTRY_COMPARE(w.findChildren<LogView *>().size(), 2); // both tabs, one of them waiting
    waitUntilIndexed(w);
    // The waiting tab is marked, so the tab bar tells a log that is empty from one that
    // is not there.
    QCOMPARE(tabTitles(w),
             QStringList({QStringLiteral("a.log"), QStringLiteral("◦ doomed.log")}));
    QCOMPARE(tabs(w)->currentIndex(), 1); // the saved active view, still active

    // And it is genuinely waiting rather than merely empty: write the log again and it
    // fills in on the watch tick, with no reopening and no dialog.
    writeLog(doomed, 4);
    LogView *waiting = viewInTab(w, 1);
    QVERIFY(waiting);
    QTRY_VERIFY_WITH_TIMEOUT(waiting->recordCount() > 0, 5000);
    QCOMPARE(tabTitles(w),
             QStringList({QStringLiteral("a.log"), QStringLiteral("doomed.log")}));
}

int main(int argc, char *argv[])
{
    QTemporaryDir configHome;
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
    qputenv("XDG_DATA_HOME", configHome.path().toUtf8());
    qputenv("HOME", configHome.path().toUtf8());
    qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("loftail-test-tabsession"));
    QApplication::setApplicationName(QStringLiteral("loftail-test-tabsession"));

    TestTabSession tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_tabsession.moc"
