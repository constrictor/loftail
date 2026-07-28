#include <QtTest>

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDockWidget>
#include <QFile>
#include <QHeaderView>
#include <QSettings>
#include <QTemporaryDir>

#include "LogView.h"
#include "MainWindow.h"

using namespace loftail;

// M9 — the dock layout round-trip (SPEC.md §10, ARCHITECTURE.md §12). Two MainWindow
// instances over one isolated settings store model quit-and-relaunch: the first opens
// two files, adds a second view onto one of them and splits it out of the tab group;
// the second must come back with the same docks in the same places.
//
// This is the milestone's riskiest mechanism. Document docks do not exist when
// restoreState() runs — they are created as the files reopen — so each one claims its
// saved slot afterwards via restoreDockWidget(), keyed by an object name persisted
// with the session. A regression here degrades silently to "everything stacked in one
// area", which is exactly what these assertions catch.
class TestDockSession : public QObject
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

    static QList<QDockWidget *> documentDocks(const MainWindow &w)
    {
        QList<QDockWidget *> out;
        for (QDockWidget *d : w.findChildren<QDockWidget *>())
            if (d->objectName().startsWith(QStringLiteral("docView-")))
                out.append(d);
        return out;
    }

    static QDockWidget *dockTitled(const MainWindow &w, const QString &title)
    {
        for (QDockWidget *d : documentDocks(w))
            if (d->windowTitle() == title)
                return d;
        return nullptr;
    }

    // A tab's title carries its file's indexing progress while the worker runs, so
    // the plain-name lookups below only hold once every scan has finished.
    static void waitUntilIndexed(const MainWindow &w)
    {
        QTRY_VERIFY([&w]() {
            for (QDockWidget *d : documentDocks(w))
                if (d->windowTitle().contains(QStringLiteral("indexing")))
                    return false;
            return !documentDocks(w).isEmpty();
        }());
    }

private slots:
    void initTestCase();
    // Every case here writes a session and then relaunches into it, so each must
    // start from a clean store or it would restore the previous case's windows.
    void init();
    void tabsSplitsAndPerViewStateRestore();
    void missingFileIsSkippedAndTheRestStillRestores();
};

void TestDockSession::init()
{
    QSettings s;
    s.remove(QStringLiteral("session"));
    s.sync();
}

void TestDockSession::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_a = m_dir.filePath(QStringLiteral("a.log"));
    m_b = m_dir.filePath(QStringLiteral("b.log"));
    writeLog(m_a, 30);
    writeLog(m_b, 20);
}

void TestDockSession::tabsSplitsAndPerViewStateRestore()
{
    QString splitDockName;
    QByteArray splitColumns;

    // --- Round 1: two files, a second view on b.log, split to the bottom --------
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
        QCOMPARE(documentDocks(w).size(), 3);
        waitUntilIndexed(w);

        QDockWidget *second = dockTitled(w, QStringLiteral("b.log [2]"));
        QVERIFY(second);
        splitDockName = second->objectName();
        QVERIFY(!splitDockName.isEmpty());

        // Move it out of the tab group into its own area — the programmatic stand-in
        // for dragging its tab to the window edge.
        w.addDockWidget(Qt::BottomDockWidgetArea, second);
        second->show();
        QCOMPARE(w.dockWidgetArea(second), Qt::BottomDockWidgetArea);

        // Give this view a column layout of its own, so the round-trip has something
        // per-view to prove (the other views keep the format's defaults).
        auto *view = second->findChild<LogView *>();
        QVERIFY(view);
        view->header()->resizeSection(0, 321);
        view->setWrapMode(LogView::WrapMode::AlwaysOn);
        splitColumns = view->saveColumnState();

        QCloseEvent ev; // drives saveSession()
        QCoreApplication::sendEvent(&w, &ev);
    }

    // --- Round 2: relaunch ------------------------------------------------------
    MainWindow w;
    w.show();
    QTRY_COMPARE(w.findChildren<LogView *>().size(), 3);
    waitUntilIndexed(w);

    const QList<QDockWidget *> docks = documentDocks(w);
    QCOMPARE(docks.size(), 3); // two files, one of them in two views — not five

    // Both files came back, one of them twice.
    QVERIFY(dockTitled(w, QStringLiteral("a.log")));
    QVERIFY(dockTitled(w, QStringLiteral("b.log [1]")));
    QVERIFY(dockTitled(w, QStringLiteral("b.log [2]")));

    // The split is back where it was, under the SAME dock name — this is the
    // restoreDockWidget() path doing its job.
    QDockWidget *second = dockTitled(w, QStringLiteral("b.log [2]"));
    QVERIFY(second);
    QCOMPARE(second->objectName(), splitDockName);
    QCOMPARE(w.dockWidgetArea(second), Qt::BottomDockWidgetArea);
    // ...and the other two are not down there with it.
    QCOMPARE(w.dockWidgetArea(dockTitled(w, QStringLiteral("a.log"))), Qt::LeftDockWidgetArea);
    QCOMPARE(w.dockWidgetArea(dockTitled(w, QStringLiteral("b.log [1]"))), Qt::LeftDockWidgetArea);

    // Per-view state: this view's columns and wrap mode, not the other views'.
    auto *view = second->findChild<LogView *>();
    QVERIFY(view);
    QCOMPARE(view->header()->sectionSize(0), 321);
    QCOMPARE(int(view->wrapMode()), int(LogView::WrapMode::AlwaysOn));
    auto *sibling = dockTitled(w, QStringLiteral("b.log [1]"))->findChild<LogView *>();
    QVERIFY(sibling);
    QVERIFY(sibling->header()->sectionSize(0) != 321); // independent of its twin
}

void TestDockSession::missingFileIsSkippedAndTheRestStillRestores()
{
    // A file that has gone away between sessions must not take the others down with
    // it, and must not raise a dialog every launch (SPEC.md §10).
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
    QTRY_COMPARE(w.findChildren<LogView *>().size(), 1); // a.log survives alone
    waitUntilIndexed(w);
    QCOMPARE(documentDocks(w).size(), 1);
    QVERIFY(dockTitled(w, QStringLiteral("a.log")));
}

int main(int argc, char *argv[])
{
    QTemporaryDir configHome;
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
    qputenv("XDG_DATA_HOME", configHome.path().toUtf8());
    qputenv("HOME", configHome.path().toUtf8());
    qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("loftail-test-docksession"));
    QApplication::setApplicationName(QStringLiteral("loftail-test-docksession"));

    TestDockSession tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_docksession.moc"
