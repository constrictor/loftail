#include <QtTest>

#include <QApplication>
#include <QAction>
#include <QComboBox>
#include <QFile>
#include <QGroupBox>
#include <QLineEdit>
#include <QListWidget>
#include <QScrollBar>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>

#include "Document.h"
#include "DocumentContext.h"
#include "DocumentView.h"
#include "FilterPane.h"
#include "LogView.h"
#include "MainWindow.h"
#include "Priority.h"
#include "RunPane.h"

using namespace loftail;

// Changing a filter must not move the reader (SPEC.md §6). The mechanism is pinned at
// the view level by tst_logview; what only a WINDOW-level test can pin is that the real
// routes into a re-apply go through it — the pane's own controls, the View menu's Clear
// Filters — that EVERY view of a file keeps its own place (invariant #7), and that the
// caller which positions its views itself (a run selection) still lands where it means to.
//
// Drives a real MainWindow offscreen, like tst_recordmenu and tst_multidoc. Widgets are
// found by OBJECT NAME, never by visible text.
class TestFilterAnchor : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QString       m_log;
    QString       m_runs;

    // 400 single-line records, every 4th of them ERROR: under a WARN floor exactly
    // every 4th survives, and with one physical line each a scroll value is a view row.
    static void writeLog(const QString &path)
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        for (int i = 0; i < 400; ++i) {
            f.write("2026-07-21 10:00:00,000 [main] ");
            f.write(i % 4 == 0 ? "ERROR " : "INFO  ");
            f.write("net.io - record ");
            f.write(QByteArray::number(i).rightJustified(4, '0'));
            f.write("\n");
        }
        f.close();
    }

    // Two runs, so the Run pane has something to switch between.
    static void writeRunsLog(const QString &path)
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        for (int run = 0; run < 2; ++run) {
            f.write("2026-07-21 10:00:00,000 [main] INFO  net.io - Starting up\n");
            for (int i = 0; i < 100; ++i) {
                f.write("2026-07-21 10:00:00,000 [main] ");
                f.write(i % 4 == 0 ? "ERROR " : "INFO  ");
                f.write("net.io - record\n");
            }
        }
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

    static FilterPane *filterPane(const MainWindow &w)
    {
        return w.findChild<FilterPane *>();
    }

    // Tick the priority axis at WARN through the pane's own controls, which is the
    // route a user takes and the one that ends in MainWindow::applyActiveFilters().
    static void applyWarnFloor(MainWindow &w, bool on)
    {
        FilterPane *pane = filterPane(w);
        QVERIFY(pane);
        auto *group = pane->findChild<QGroupBox *>(QStringLiteral("priorityGroup"));
        auto *combo = pane->findChild<QComboBox *>(QStringLiteral("priorityCombo"));
        QVERIFY(group);
        QVERIFY(combo);
        if (on) {
            const int row = combo->findData(int(Priority::Warn));
            QVERIFY(row >= 0);
            combo->setCurrentIndex(row);
        }
        group->setChecked(on);
    }

    static int sourceOfViewRow(const DocumentView *v, int viewRow)
    {
        return v->context()->doc->filtered().sourceRow(viewRow);
    }

private slots:
    void initTestCase();
    void init();

    void changingAFilterInThePaneKeepsTheViewWhereItWas();
    void everyViewOfOneFileKeepsItsOwnPlace();
    void clearFiltersFromTheViewMenuKeepsThePlaceToo();
    void pickingAnEarlierRunStillLandsAtItsTop();
};

void TestFilterAnchor::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_log = m_dir.filePath(QStringLiteral("app.log"));
    writeLog(m_log);
    m_runs = m_dir.filePath(QStringLiteral("runs.log"));
    writeRunsLog(m_runs);
}

void TestFilterAnchor::init()
{
    QSettings s;
    s.remove(QStringLiteral("session"));
    s.remove(QStringLiteral("formatCache"));
    s.sync();
}

void TestFilterAnchor::changingAFilterInThePaneKeepsTheViewWhereItWas()
{
    MainWindow w;
    w.resize(1200, 800);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    w.openFile(m_log);
    waitUntilIndexed(w);

    DocumentView *v = activeView(w);
    QVERIFY(v);
    LogView *log = v->logView();
    log->setWrapMode(LogView::WrapMode::Off);

    QScrollBar *sb = log->verticalScrollBar();
    sb->setValue(100);
    log->setCurrentRecord(104); // on screen, and a survivor of the WARN floor
    const int offsetBefore = 104 - sb->value();
    QCOMPARE(offsetBefore, 4); // it really is mid-viewport, not pinned to an edge

    applyWarnFloor(w, true);

    QCOMPARE(v->context()->doc->filters().minPriority, Priority::Warn);
    QCOMPARE(sourceOfViewRow(v, log->currentRecord()), 104);
    QCOMPARE(log->currentRecord() - sb->value(), offsetBefore);
}

void TestFilterAnchor::everyViewOfOneFileKeepsItsOwnPlace()
{
    MainWindow w;
    w.resize(1200, 800);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    w.openFile(m_log);
    waitUntilIndexed(w);

    // A second view of the SAME file. Scroll and selection are per-view state
    // (invariant #7), so one filter change has to anchor each of them separately.
    auto *newView = w.findChild<QAction *>(QStringLiteral("newViewAction"));
    QVERIFY(newView);
    newView->trigger();
    waitUntilIndexed(w);

    QTabWidget *t = tabs(w);
    QVERIFY(t);
    QVector<LogView *> views;
    for (int i = 0; i < t->count(); ++i) {
        if (auto *dv = qobject_cast<DocumentView *>(t->widget(i))) {
            const QList<LogView *> found =
                dv->findChildren<LogView *>(QStringLiteral("logView"));
            for (LogView *lv : found)
                views.append(lv);
        }
    }
    QCOMPARE(views.size(), 2);

    // Select first, THEN scroll: setCurrentRecord() scrolls the record into view, so
    // doing it the other way round would decide the offset instead of this test.
    const int kSel[2] = {104, 208};
    const int kOffset[2] = {4, 9};
    for (int i = 0; i < 2; ++i) {
        LogView *lv = views.at(i);
        lv->setWrapMode(LogView::WrapMode::Off);
        lv->setCurrentRecord(kSel[i]);
        lv->verticalScrollBar()->setValue(kSel[i] - kOffset[i]);
        QVERIFY2(lv->verticalScrollBar()->pageStep() > kOffset[i],
                 "the viewport must be tall enough to hold the selection at this offset");
        QCOMPARE(lv->currentRecord() - lv->verticalScrollBar()->value(), kOffset[i]);
    }

    applyWarnFloor(w, true);

    DocumentView *dv = activeView(w);
    QVERIFY(dv);
    for (int i = 0; i < 2; ++i) {
        LogView *lv = views.at(i);
        QCOMPARE(sourceOfViewRow(dv, lv->currentRecord()), kSel[i]);
        // Each kept ITS OWN offset rather than both landing on one place.
        QCOMPARE(lv->currentRecord() - lv->verticalScrollBar()->value(), kOffset[i]);
    }
}

void TestFilterAnchor::clearFiltersFromTheViewMenuKeepsThePlaceToo()
{
    MainWindow w;
    w.resize(1200, 800);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    w.openFile(m_log);
    waitUntilIndexed(w);

    DocumentView *v = activeView(w);
    QVERIFY(v);
    LogView *log = v->logView();
    log->setWrapMode(LogView::WrapMode::Off);

    applyWarnFloor(w, true);
    QScrollBar *sb = log->verticalScrollBar();
    sb->setValue(25);
    log->setCurrentRecord(26); // view row 26 == source 104
    QCOMPARE(sourceOfViewRow(v, 26), 104);
    const int offsetBefore = 26 - sb->value();
    QCOMPARE(offsetBefore, 1);

    auto *clear = w.findChild<QAction *>(QStringLiteral("clearFiltersAction"));
    QVERIFY(clear);
    clear->trigger();

    QVERIFY(!v->context()->doc->filters().anyActive());
    QCOMPARE(log->currentRecord(), 104); // identity view: the row IS the ordinal
    QCOMPARE(log->currentRecord() - sb->value(), offsetBefore);
}

void TestFilterAnchor::pickingAnEarlierRunStillLandsAtItsTop()
{
    // A run selection re-applies the filters and then positions every view itself
    // (KeepPosition::No). The claim here is only that the anchor did not get in the
    // way of that — the run still opens at its own first record, with follow detached.
    MainWindow w;
    w.resize(1200, 800);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    w.openFile(m_runs);
    waitUntilIndexed(w);

    DocumentView *v = activeView(w);
    QVERIFY(v);
    Document *doc = v->context()->doc.get();

    // Split the file into runs through the pane's own control, which is what a user has.
    auto *pattern = w.findChild<QLineEdit *>(QStringLiteral("runStartPattern"));
    QVERIFY(pattern);
    pattern->setText(QStringLiteral("Starting up"));
    QTest::keyClick(pattern, Qt::Key_Return);
    QCOMPARE(doc->runs().size(), 2);

    LogView *log = v->logView();
    log->setWrapMode(LogView::WrapMode::Off);
    log->verticalScrollBar()->setValue(log->verticalScrollBar()->maximum());

    auto *runList = w.findChild<QListWidget *>(QStringLiteral("runList"));
    QVERIFY(runList);
    QVERIFY(runList->count() >= 3);  // "All runs" + one row per run
    runList->setCurrentRow(1);       // row 0 is "All runs"; row 1 is the older run

    QCOMPARE(log->currentRecord(), 0);
    QCOMPARE(log->verticalScrollBar()->value(), 0);
    QVERIFY(!log->following()); // an earlier, finished run detaches follow (SPEC.md §3a)
}

int main(int argc, char *argv[])
{
    QTemporaryDir configHome;
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
    qputenv("XDG_DATA_HOME", configHome.path().toUtf8());
    qputenv("HOME", configHome.path().toUtf8());
    qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("loftail-test-filteranchor"));
    QApplication::setApplicationName(QStringLiteral("loftail-test-filteranchor"));

    TestFilterAnchor tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_filteranchor.moc"
