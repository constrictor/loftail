#include <QtTest>

#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QHeaderView>
#include <QFile>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "Document.h"
#include "FilterPane.h"
#include "LogModel.h"
#include "LogView.h"
#include "MainWindow.h"

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
//   * the whole tab/split arrangement round-trips through quit-and-relaunch.
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

    // Every dock hosting a document view (as opposed to a side pane).
    static QList<QDockWidget *> documentDocks(const MainWindow &w)
    {
        QList<QDockWidget *> out;
        for (QDockWidget *d : w.findChildren<QDockWidget *>())
            if (d->objectName().startsWith(QStringLiteral("docView-")))
                out.append(d);
        return out;
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
            for (QDockWidget *d : documentDocks(w))
                if (d->windowTitle().contains(QStringLiteral("indexing")))
                    return false;
            return !documentDocks(w).isEmpty();
        }());
    }

private slots:
    void initTestCase();
    // Each case builds its own MainWindow, whose constructor restores whatever
    // session is stored — so start every one from a clean store.
    void init();
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

void TestMultiDoc::secondFileOpensAsAnotherTab()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();

    w.openFile(m_a);
    QTRY_COMPARE(w.findChildren<LogView *>().size(), 1);

    w.openFile(m_b);
    // The point of the milestone: the first file is still open.
    QTRY_COMPARE(w.findChildren<LogView *>().size(), 2);
    QCOMPARE(documentDocks(w).size(), 2);

    // The file just opened is the visible, active one — its name is in the title.
    QVERIFY(w.windowTitle().endsWith(QStringLiteral("b.log")));
}

void TestMultiDoc::reopeningAnOpenFileRaisesItInsteadOfDuplicating()
{
    MainWindow w;
    w.show();
    w.openFile(m_a);
    w.openFile(m_b);
    QTRY_COMPARE(w.findChildren<LogView *>().size(), 2);

    w.openFile(m_a); // already open
    QCOMPARE(w.findChildren<LogView *>().size(), 2); // no third view
    QVERIFY(w.windowTitle().endsWith(QStringLiteral("a.log"))); // but it is raised
}

void TestMultiDoc::newViewSharesOneDocumentAndModel()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_a);
    QTRY_COMPARE(w.findChildren<LogView *>().size(), 1);

    trigger(w, "newViewAction");
    QCOMPARE(w.findChildren<LogView *>().size(), 2);
    QCOMPARE(documentDocks(w).size(), 2);

    // Two views, ONE file: the index, filters and highlighters are shared, so the
    // status bar still reports a single document's record count.
    QVERIFY(w.windowTitle().endsWith(QStringLiteral("a.log")));

    // The views are independent in what they show: moving one leaves the other.
    waitUntilIndexed(w); // records must exist before a record can be selected
    const QList<LogView *> views = w.findChildren<LogView *>();
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
    QTRY_COMPARE(w.findChildren<LogView *>().size(), 1);

    QSignalSpy spy(&w, &MainWindow::activeDocumentChanged);

    // A second view onto the SAME file: the panes edit that file's filters and
    // highlighters, which both views share, so rebinding them would be wrong (and
    // would reset the filter pane's discovered-value state).
    trigger(w, "newViewAction");
    QCOMPARE(spy.count(), 0);

    // A second FILE does rebind them.
    w.openFile(m_b);
    QTRY_COMPARE(w.findChildren<LogView *>().size(), 3);
    QCOMPARE(spy.count(), 1);
}

void TestMultiDoc::closingATabLeavesTheOtherFileOpen()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_a);
    w.openFile(m_b);
    QTRY_COMPARE(w.findChildren<LogView *>().size(), 2);

    trigger(w, "closeTabAction"); // closes b.log, the active tab
    QTRY_COMPARE(w.findChildren<LogView *>().size(), 1);
    QCOMPARE(documentDocks(w).size(), 1);
    QVERIFY(w.windowTitle().endsWith(QStringLiteral("a.log")));
}

void TestMultiDoc::fileClosesWithItsLastViewOnly()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_a);
    QTRY_COMPARE(w.findChildren<LogView *>().size(), 1);
    trigger(w, "newViewAction");
    QCOMPARE(w.findChildren<LogView *>().size(), 2);

    QSignalSpy spy(&w, &MainWindow::activeDocumentChanged);

    // Closing ONE of two views leaves the file open — nothing unbinds.
    trigger(w, "closeTabAction");
    QTRY_COMPARE(w.findChildren<LogView *>().size(), 1);
    QVERIFY(w.windowTitle().endsWith(QStringLiteral("a.log")));
    for (const QList<QVariant> &args : spy)
        QVERIFY(args.at(0).value<Document *>() != nullptr); // never "no file open"

    // Closing the last one closes the file.
    trigger(w, "closeTabAction");
    QTRY_COMPARE(w.findChildren<LogView *>().size(), 0);
    QCOMPARE(w.windowTitle(), QStringLiteral("loftail"));
}

void TestMultiDoc::closingEverythingUnbindsThePanes()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_a);
    w.openFile(m_b);
    QTRY_COMPARE(w.findChildren<LogView *>().size(), 2);

    QSignalSpy spy(&w, &MainWindow::activeDocumentChanged);
    trigger(w, "closeAllAction");

    QTRY_COMPARE(w.findChildren<LogView *>().size(), 0);
    QCOMPARE(documentDocks(w).size(), 0);
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
    QTRY_COMPARE(w.findChildren<LogView *>().size(), 1);
    waitUntilIndexed(w);

    QCOMPARE(checkedTimeDisplay(w), QStringLiteral("timeDisplayAsWrittenAction"));
    trigger(w, "timeDisplayUtcAction");
    QCOMPARE(checkedTimeDisplay(w), QStringLiteral("timeDisplayUtcAction"));

    // A second FILE carries its own mode; choosing one for a.log must not leak.
    w.openFile(m_b);
    QTRY_COMPARE(w.findChildren<LogView *>().size(), 2);
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
    QTRY_COMPARE(w.findChildren<LogView *>().size(), 1);
    waitUntilIndexed(w);

    trigger(w, "newViewAction");
    QCOMPARE(w.findChildren<LogView *>().size(), 2);

    const QList<LogView *> views = w.findChildren<LogView *>();
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
        QTRY_COMPARE(w.findChildren<LogView *>().size(), 1);
        waitUntilIndexed(w);
        trigger(w, "timeDisplayRunSecondsAction");
        w.close(); // saves the session
    }

    // The mode rides the same persistence path the Log Format dialog uses, so a
    // relaunch restores it along with the rest of the file's format.
    MainWindow w;
    w.show();
    QTRY_COMPARE(w.findChildren<LogView *>().size(), 1);
    waitUntilIndexed(w);
    QCOMPARE(checkedTimeDisplay(w), QStringLiteral("timeDisplayRunSecondsAction"));

    LogModel *m = modelOf(w.findChildren<LogView *>().at(0));
    QVERIFY(m);
    QCOMPARE(m->data(m->index(0, 0)).toString(), QStringLiteral("0.000"));

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

#include "tst_multidoc.moc"
