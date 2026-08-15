#include <QtTest>

#include <QApplication>
#include <QFile>
#include <QLineEdit>
#include <QListWidget>
#include <QTabWidget>
#include <QTemporaryDir>

#include "Document.h"
#include "DocumentContext.h"
#include "DocumentView.h"
#include "LiveController.h"
#include "LogModel.h"
#include "LogView.h"
#include "MainWindow.h"
#include "RunPane.h"

using namespace loftail;

// "Last run" (SPEC.md §3a): the Runs pane's first entry and its default, which is not a
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

    // ...and the pane still says "Last run", not the ordinal it has landed on: the next
    // restart has to move it again.
    QListWidget *list = runList(w);
    QVERIFY(list);
    QCOMPARE(list->count(), 4); // "Last run" + "All runs" + two runs
    QCOMPARE(list->currentRow(), RunPane::kLastRunRow);
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
