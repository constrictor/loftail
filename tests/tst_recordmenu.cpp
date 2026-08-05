#include <QtTest>

#include <QAction>
#include <QApplication>
#include <QFile>
#include <QMenu>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>

#include "Document.h"
#include "DocumentContext.h"
#include "DocumentView.h"
#include "Highlight.h"
#include "LogFormat.h"
#include "LogView.h"
#include "MainWindow.h"

using namespace loftail;

// The record context menu (SPEC.md §5): right-clicking a record turns that record's
// own field values into filter and highlight criteria.
//
// What a window-level test can pin that the pane-level cases (tst_filterpane) cannot
// is the ASSEMBLY — which items a given record offers, and that triggering one
// reaches the same per-file state a click in the pane would:
//
//   * an axis the record cannot speak for is OMITTED, not greyed: an unparsed
//     plain-text line has no subsystem, thread, level or timestamp (§4), and a
//     pattern with no %t has none for any record (§6);
//   * the clicked column reorders the menu and never changes its contents;
//   * filtering from the menu is filtering the FILE (invariant #7), so a second view
//     of the same log sees it too;
//   * a highlight item ADDS a rule rather than replacing the list, and behind the
//     first-match-wins order (§7) rather than in front of it.
//
// Drives the real MainWindow offscreen, like tst_multidoc and tst_sessiongui. The
// menu is built through MainWindow::buildRecordMenu rather than by posting a context
// menu event, because exec() on a real popup blocks the test.
class TestRecordMenu : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QString       m_log;      // the default pattern: %d [%t] %p %c - %m
    QString       m_noThread; // a pattern with no %t and no %d

    // The unparsed line comes FIRST on purpose: a non-matching line after a matched
    // one is a CONTINUATION of it (invariant #2), so a leading one is the only way to
    // get a record with no fields at all — which is the record the omission rule is
    // about.
    enum Row { kPlain = 0, kMain = 1, kWorker = 2, kError = 3 };

    static void writeDefaultLog(const QString &path)
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("a plain line no pattern will ever match\n"
                "2026-07-21 10:00:00,000 [main] INFO  net.io - one\n"
                "2026-07-21 10:00:01,000 [worker] WARN  db.pool - two\n"
                "2026-07-21 10:00:02,000 [main] ERROR net.io - three\n");
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

    // The logical column carrying a role, so a case can say "right-click the
    // subsystem cell" without hardcoding the pattern's field order.
    static int columnOf(const MainWindow &w, FieldRole role)
    {
        DocumentView *view = activeView(w);
        if (!view || !view->context() || !view->context()->doc)
            return -1;
        const QVector<Field> &fields = view->context()->doc->format().fields;
        for (int i = 0; i < fields.size(); ++i)
            if (fields.at(i).role == role)
                return i;
        return -1;
    }

    static QStringList itemNames(const QMenu &menu)
    {
        QStringList out;
        for (const QAction *a : menu.actions())
            if (!a->objectName().isEmpty())
                out << a->objectName();
        return out;
    }

    static QAction *item(QMenu &menu, const char *name)
    {
        for (QAction *a : menu.actions())
            if (a->objectName() == QLatin1String(name))
                return a;
        return nullptr;
    }

    static int visibleRecords(const MainWindow &w)
    {
        DocumentView *view = activeView(w);
        return view ? view->logView()->recordCount() : -1;
    }

private slots:
    void initTestCase();
    void init();

    void aParsedRecordOffersEveryAxisItCarries();
    void anUnparsedLineOffersNothingToFilterBy();
    void aFormatWithoutThreadOrTimeOmitsThoseAxes();
    void theClickedColumnReordersButDoesNotRestrict();
    void showOnlySubsystemFiltersTheFile();
    void hideThreadLeavesTheOthers();
    void priorityFloorTakesTheRecordsOwnLevel();
    void timeBoundsNarrowFromBothEnds();
    void aSelectionOfTwoRecordsOffersItsOwnRange();
    void highlightingAppendsARuleAndKeepsTheOthers();
    void copyActionsAreOnTheMenu();
};

void TestRecordMenu::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_log = m_dir.filePath(QStringLiteral("app.log"));
    writeDefaultLog(m_log);

    m_noThread = m_dir.filePath(QStringLiteral("bare.log"));
    QFile bare(m_noThread);
    QVERIFY(bare.open(QIODevice::WriteOnly));
    bare.write("INFO  net.io - one\n"
               "WARN  db.pool - two\n");
    bare.close();
}

void TestRecordMenu::init()
{
    QSettings s;
    s.remove(QStringLiteral("session"));
    s.remove(QStringLiteral("formatCache"));
    s.sync();
}

void TestRecordMenu::aParsedRecordOffersEveryAxisItCarries()
{
    MainWindow w;
    w.openFile(m_log);
    waitUntilIndexed(w);

    QMenu menu;
    w.buildRecordMenu(&menu, activeView(w), kMain, -1);
    const QStringList names = itemNames(menu);

    QVERIFY(names.contains(QStringLiteral("recordShowOnlySubsystem")));
    QVERIFY(names.contains(QStringLiteral("recordHideSubsystem")));
    QVERIFY(names.contains(QStringLiteral("recordShowOnlyThread")));
    QVERIFY(names.contains(QStringLiteral("recordHideThread")));
    QVERIFY(names.contains(QStringLiteral("recordPriorityFloor")));
    QVERIFY(names.contains(QStringLiteral("recordTimeStart")));
    QVERIFY(names.contains(QStringLiteral("recordTimeEnd")));
    QVERIFY(names.contains(QStringLiteral("recordHighlightSubsystem")));
    QVERIFY(names.contains(QStringLiteral("recordHighlightThread")));
    QVERIFY(names.contains(QStringLiteral("recordHighlightPriority")));

    // The values are named in the items, not hidden behind "this": the point of the
    // menu is that the user sees what they are about to filter to.
    QVERIFY(item(menu, "recordShowOnlySubsystem")->text().contains(QStringLiteral("net.io")));
    QVERIFY(item(menu, "recordHideThread")->text().contains(QStringLiteral("main")));
    QVERIFY(item(menu, "recordPriorityFloor")->text().contains(QStringLiteral("INFO")));

    // One record names one instant, so there is no range item until a selection
    // names two (see timeBoundsNarrowFromBothEnds).
    QVERIFY(!names.contains(QStringLiteral("recordTimeRange")));
}

// A plain-text line has no subsystem, thread, level or timestamp, and SPEC.md §4
// promises it stays visible anyway. There is nothing to filter it BY, so the menu
// says nothing rather than offering four dead entries.
void TestRecordMenu::anUnparsedLineOffersNothingToFilterBy()
{
    MainWindow w;
    w.openFile(m_log);
    waitUntilIndexed(w);
    QCOMPARE(visibleRecords(w), 4);

    QMenu menu;
    w.buildRecordMenu(&menu, activeView(w), kPlain, -1);
    const QStringList names = itemNames(menu);

    QVERIFY(!names.contains(QStringLiteral("recordShowOnlySubsystem")));
    QVERIFY(!names.contains(QStringLiteral("recordHideThread")));
    QVERIFY(!names.contains(QStringLiteral("recordPriorityFloor")));
    QVERIFY(!names.contains(QStringLiteral("recordTimeStart")));
    QVERIFY(!names.contains(QStringLiteral("recordHighlightSubsystem")));
    // The clipboard items are about the record's text, which it has like any other.
    QVERIFY(!menu.actions().isEmpty());
}

// The other half of the omission rule: the record is fully parsed, but its pattern
// carries no %t and no %d, so no record in the file can ever answer those axes
// (SPEC.md §6). The two that remain still work.
void TestRecordMenu::aFormatWithoutThreadOrTimeOmitsThoseAxes()
{
    MainWindow w;
    w.openFile(m_noThread, QStringLiteral("%-5p %c - %m%n"));
    waitUntilIndexed(w);

    QMenu menu;
    w.buildRecordMenu(&menu, activeView(w), 0, -1);
    const QStringList names = itemNames(menu);

    QVERIFY(names.contains(QStringLiteral("recordShowOnlySubsystem")));
    QVERIFY(names.contains(QStringLiteral("recordPriorityFloor")));
    QVERIFY(!names.contains(QStringLiteral("recordShowOnlyThread")));
    QVERIFY(!names.contains(QStringLiteral("recordHideThread")));
    QVERIFY(!names.contains(QStringLiteral("recordTimeStart")));
    QVERIFY(!names.contains(QStringLiteral("recordTimeEnd")));
    QVERIFY(!names.contains(QStringLiteral("recordHighlightThread")));
}

void TestRecordMenu::theClickedColumnReordersButDoesNotRestrict()
{
    MainWindow w;
    w.openFile(m_log);
    waitUntilIndexed(w);

    QMenu onSubsystem, onThread;
    w.buildRecordMenu(&onSubsystem, activeView(w), kMain, columnOf(w, FieldRole::Logger));
    w.buildRecordMenu(&onThread, activeView(w), kMain, columnOf(w, FieldRole::Thread));

    QStringList a = itemNames(onSubsystem);
    QStringList b = itemNames(onThread);
    QCOMPARE(a.first(), QStringLiteral("recordShowOnlySubsystem"));
    QCOMPARE(b.first(), QStringLiteral("recordShowOnlyThread"));

    // Same items, different order — a menu whose contents change with the column
    // cannot be learned.
    a.sort();
    b.sort();
    QCOMPARE(a, b);
}

void TestRecordMenu::showOnlySubsystemFiltersTheFile()
{
    MainWindow w;
    w.openFile(m_log);
    waitUntilIndexed(w);

    // A second view of the same log. Filtering is per FILE (invariant #7, §5a), so
    // the menu action in one view must be visible in the other.
    QAction *newView = w.findChild<QAction *>(QStringLiteral("newViewAction"));
    QVERIFY(newView);
    newView->trigger();
    QCOMPARE(tabs(w)->count(), 2);
    DocumentView *second = activeView(w);

    QMenu menu;
    w.buildRecordMenu(&menu, second, kMain, -1); // a net.io record
    QAction *showOnly = item(menu, "recordShowOnlySubsystem");
    QVERIFY(showOnly);
    showOnly->trigger();

    // net.io's two records, plus the unparsed line that no subsystem filter may hide.
    QCOMPARE(second->logView()->recordCount(), 3);
    DocumentView *first = qobject_cast<DocumentView *>(tabs(w)->widget(0));
    QVERIFY(first);
    QCOMPARE(first->logView()->recordCount(), 3);
}

void TestRecordMenu::hideThreadLeavesTheOthers()
{
    MainWindow w;
    w.openFile(m_log);
    waitUntilIndexed(w);

    QMenu menu;
    w.buildRecordMenu(&menu, activeView(w), kWorker, -1);
    QAction *hide = item(menu, "recordHideThread");
    QVERIFY(hide);
    QVERIFY(hide->text().contains(QStringLiteral("worker")));
    hide->trigger();

    // Both [main] records stay, and so does the unparsed line (§6: a record lacking
    // the field an axis tests is never hidden by it).
    QCOMPARE(visibleRecords(w), 3);
}

void TestRecordMenu::priorityFloorTakesTheRecordsOwnLevel()
{
    MainWindow w;
    w.openFile(m_log);
    waitUntilIndexed(w);

    QMenu menu;
    w.buildRecordMenu(&menu, activeView(w), kError, -1);
    QAction *floor = item(menu, "recordPriorityFloor");
    QVERIFY(floor);
    QVERIFY(floor->text().contains(QStringLiteral("ERROR")));
    floor->trigger();

    QCOMPARE(visibleRecords(w), 2); // the ERROR record + the unparsed line
}

void TestRecordMenu::timeBoundsNarrowFromBothEnds()
{
    MainWindow w;
    w.openFile(m_log);
    waitUntilIndexed(w);
    DocumentView *view = activeView(w);

    {
        QMenu menu;
        w.buildRecordMenu(&menu, view, kWorker, -1); // start at 10:00:01
        QAction *start = item(menu, "recordTimeStart");
        QVERIFY(start);
        start->trigger();
    }
    // The [worker] and ERROR records, plus the unparsed line: it has no timestamp, so
    // a time filter must not hide it either (§6).
    QCOMPARE(visibleRecords(w), 3);

    // Now close the other end on what is row 1 of the FILTERED view — the same
    // [worker] record, since the plain line still leads.
    {
        QMenu menu;
        w.buildRecordMenu(&menu, view, 1, -1);
        QAction *end = item(menu, "recordTimeEnd");
        QVERIFY(end);
        end->trigger();
    }
    QCOMPARE(visibleRecords(w), 2); // one timestamped record left, and the plain line
}

// One record names one instant, so the range item appears only when a selection
// names two — which is the gesture people actually make for "this stretch".
void TestRecordMenu::aSelectionOfTwoRecordsOffersItsOwnRange()
{
    MainWindow w;
    w.openFile(m_log);
    waitUntilIndexed(w);
    DocumentView *view = activeView(w);

    view->logView()->setCurrentRecord(kMain);
    view->logView()->setCurrentRecord(kWorker, /*extendSelection=*/true);

    QMenu menu;
    w.buildRecordMenu(&menu, view, kWorker, -1);
    QAction *range = item(menu, "recordTimeRange");
    QVERIFY(range);
    range->trigger();

    // 10:00:00 through 10:00:01 — the ERROR record at 10:00:02 is out, the plain
    // line stays because it has no timestamp to compare.
    QCOMPARE(visibleRecords(w), 3);
}

void TestRecordMenu::highlightingAppendsARuleAndKeepsTheOthers()
{
    MainWindow w;
    w.openFile(m_log);
    waitUntilIndexed(w);
    Document *doc = activeView(w)->context()->doc.get();

    QMenu first;
    w.buildRecordMenu(&first, activeView(w), kMain, -1);
    item(first, "recordHighlightSubsystem")->trigger();
    QCOMPARE(doc->highlighters().rules.size(), 1);
    QCOMPARE(doc->highlighters().rules.at(0).match.loggerNames,
             QStringList{QStringLiteral("net.io")});
    QVERIFY(doc->highlighters().rules.at(0).match.loggerEnabled);

    QMenu second;
    w.buildRecordMenu(&second, activeView(w), kWorker, -1);
    item(second, "recordHighlightThread")->trigger();
    QCOMPARE(doc->highlighters().rules.size(), 2);
    // Appended, so the rule that was there keeps its first-match-wins precedence...
    QCOMPARE(doc->highlighters().rules.at(0).match.loggerNames,
             QStringList{QStringLiteral("net.io")});
    // ...and the two are told apart by color rather than both landing on slot 0.
    QVERIFY(doc->highlighters().rules.at(0).background
            != doc->highlighters().rules.at(1).background);

    // Highlighting removes nothing: every record is still there (SPEC.md §7).
    QCOMPARE(visibleRecords(w), 4);
}

void TestRecordMenu::copyActionsAreOnTheMenu()
{
    MainWindow w;
    w.openFile(m_log);
    waitUntilIndexed(w);

    QMenu menu;
    w.buildRecordMenu(&menu, activeView(w), kMain, -1);
    QVERIFY(menu.actions().contains(w.findChild<QAction *>(QStringLiteral("copyAction"))));
    QVERIFY(menu.actions().contains(w.findChild<QAction *>(QStringLiteral("copyColumnsAction"))));
}

int main(int argc, char *argv[])
{
    QTemporaryDir configHome;
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
    qputenv("XDG_DATA_HOME", configHome.path().toUtf8());
    qputenv("HOME", configHome.path().toUtf8());
    qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("loftail-test-recordmenu"));
    QApplication::setApplicationName(QStringLiteral("loftail-test-recordmenu"));

    TestRecordMenu tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_recordmenu.moc"
