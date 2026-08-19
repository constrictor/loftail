#include <QtTest>

#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QLineEdit>
#include <QGroupBox>
#include <QFile>
#include <QLabel>
#include <QMainWindow>
#include <QSpinBox>
#include <QPushButton>
#include <QTabBar>
#include <QTableWidget>
#include <QTabWidget>
#include <QTemporaryDir>

#include <QListWidget>

#include "Document.h"
#include "DocumentContext.h"
#include "DocumentView.h"
#include "FilterPane.h"
#include "Highlight.h"
#include "HighlighterPane.h"
#include "MainWindow.h"
#include "PaneTitleStyle.h"
#include "RunPane.h"

using namespace loftail;

// The side panes' title-bar chrome (SPEC.md §8, ARCHITECTURE.md §12.2).
//
// The panes ship TABBED TOGETHER, so Qt's dock title bar prints each pane's name a
// second time directly under the tab that already carries it. PaneTitleStyle suppresses
// that text while a pane is tabbed and restores it when the pane is alone — the only
// place its name would otherwise appear.
//
// Two of these cases guard measured Qt behaviour rather than loftail's own, and they
// are here because the whole design rests on them: a dock can be dragged ONLY by Qt's
// real title bar, which is why the bar is repainted rather than hidden or replaced. If
// a future Qt makes tab-dragging undock a pane, `titleBarCannotSimplyBeHidden` starts
// failing and the cheaper design becomes available.
class TestPaneChrome : public QObject
{
    Q_OBJECT

private:
    static QDockWidget *paneDock(const MainWindow &w, const QString &name)
    {
        return w.findChild<QDockWidget *>(name);
    }

    // Which panes this build has, in the order they are tabbed. Presets are a build
    // option and off by default (SPEC.md §9), so the shipped group is three panes and a
    // presets build's is four — and the cases below turn on the COUNT as well as the
    // names, so neither may be written out.
    static QStringList paneDockNames()
    {
        QStringList names{QStringLiteral("filtersDock"), QStringLiteral("highlightersDock")};
#if defined(LOFTAIL_HAVE_PRESETS)
        names << QStringLiteral("presetsDock");
#endif
        names << QStringLiteral("runsDock");
        return names;
    }

    // The same list as the tab bar spells it. Asserting on visible text is the exception
    // here rather than the usual mistake: that the tab carries the name is the very thing
    // making the title bar's copy redundant, so it is the subject of the test.
    static QStringList paneTabTitles()
    {
        QStringList titles{QStringLiteral("Filters"), QStringLiteral("Highlighters")};
#if defined(LOFTAIL_HAVE_PRESETS)
        titles << QStringLiteral("Presets");
#endif
        titles << QStringLiteral("Runs");
        return titles;
    }

private slots:
    void tabbedPanesSuppressTheirTitleText();
    void aPaneAloneKeepsItsTitleText();
    void titleBarCannotSimplyBeHidden();
    void theFiltersTabIsMarkedWhileFiltersAreInForce();
    void theClearFiltersItemIsLiveOnlyWhileThereIsSomethingToClear();
    void theClearFiltersItemFollowsTheTabInFront();
    void theHighlightersTabIsMarkedOnlyOnceTheRulesAreNotTheSeededOnes();
    void aRunClickOrATimestampModeChangeIsNotAnEditToTheRules();
};

void TestPaneChrome::tabbedPanesSuppressTheirTitleText()
{
    MainWindow w;
    w.resize(1000, 700);
    w.show();
    QTest::qWait(50);

    // The shipped arrangement: every pane this build has, in one tab group on the right.
    const QStringList names = paneDockNames();
    for (const QString &name : names) {
        QDockWidget *dock = paneDock(w, name);
        QVERIFY2(dock, qPrintable(name));
        QVERIFY2(PaneTitleStyle::isTabbedWithAnother(dock), qPrintable(name));
        // The NAME ITSELF is untouched — only the painting of it is suppressed. This is
        // load-bearing: the dock tab bar takes its label from windowTitle(), so clearing
        // the title to hide the duplicate would blank the tab instead.
        QVERIFY2(!dock->windowTitle().isEmpty(), qPrintable(name));
    }

    // And the tab bar really is carrying those names, which is what makes the title bar
    // text redundant in the first place.
    QStringList tabs;
    for (QTabBar *bar : w.findChildren<QTabBar *>()) {
        if (bar->count() != names.size())
            continue;
        for (int i = 0; i < bar->count(); ++i)
            tabs << bar->tabText(i);
        break;
    }
    QCOMPARE(tabs, paneTabTitles());
}

void TestPaneChrome::aPaneAloneKeepsItsTitleText()
{
    MainWindow w;
    w.resize(1000, 700);
    w.show();
    QTest::qWait(50);

    QDockWidget *filters = paneDock(w, "filtersDock");
    QVERIFY(filters);
    QVERIFY(PaneTitleStyle::isTabbedWithAnother(filters));

    // Pulled out to the other side, it is the only pane in its area: no tab bar carries
    // its name, so the title bar has to.
    w.removeDockWidget(filters);
    w.addDockWidget(Qt::LeftDockWidgetArea, filters);
    filters->show();
    QTest::qWait(50);
    QVERIFY(!PaneTitleStyle::isTabbedWithAnother(filters));

    // Floating counts as alone too — a floating pane has a title bar and nothing else.
    w.addDockWidget(Qt::RightDockWidgetArea, filters);
    w.tabifyDockWidget(paneDock(w, "runsDock"), filters);
    QTest::qWait(50);
    QVERIFY(PaneTitleStyle::isTabbedWithAnother(filters));
    filters->setFloating(true);
    QVERIFY(!PaneTitleStyle::isTabbedWithAnother(filters));
    filters->setFloating(false);

    // A group whose other panes have all been CLOSED shows no tab bar either, so the
    // survivor needs its name back even though Qt still calls them tabified.
    QDockWidget *runs = paneDock(w, "runsDock");
    QVERIFY(runs);
    for (const QString &name : paneDockNames().mid(1)) { // every pane but Filters
        if (QDockWidget *d = paneDock(w, name))
            d->hide();
    }
    QTest::qWait(50);
    QVERIFY(!PaneTitleStyle::isTabbedWithAnother(filters));
    runs->show(); // and back again once one returns
    QTest::qWait(50);
    QVERIFY(PaneTitleStyle::isTabbedWithAnother(filters));
}

void TestPaneChrome::titleBarCannotSimplyBeHidden()
{
    // WHY the redundant title bar is repainted instead of removed. Qt starts a dock drag
    // only from its own title bar: dragging a TAB reorders it and never undocks, so a
    // pane with no title bar could not be moved at all — and since the panes ship
    // tabbed, that is the default state. Plain Qt widgets, no loftail involved, because
    // it is Qt's behaviour that is being pinned.
    QMainWindow w;
    w.resize(900, 600);
    w.setCentralWidget(new QLabel(QStringLiteral("centre")));
    auto *a = new QDockWidget(QStringLiteral("Alpha"), &w);
    a->setObjectName(QStringLiteral("a"));
    a->setWidget(new QLabel(QStringLiteral("A")));
    auto *b = new QDockWidget(QStringLiteral("Beta"), &w);
    b->setObjectName(QStringLiteral("b"));
    b->setWidget(new QLabel(QStringLiteral("B")));
    w.addDockWidget(Qt::RightDockWidgetArea, a);
    w.addDockWidget(Qt::RightDockWidgetArea, b);
    w.tabifyDockWidget(a, b);
    a->raise();
    w.show();
    QTest::qWait(100);

    const auto dragFrom = [](QWidget *target, const QPoint &start) {
        QTest::mousePress(target, Qt::LeftButton, {}, start);
        for (int dx = 5; dx <= 260; dx += 15)
            QTest::mouseMove(target, start + QPoint(-dx, dx / 3));
        QTest::qWait(60);
        const bool floated = qobject_cast<QDockWidget *>(target)
            ? qobject_cast<QDockWidget *>(target)->isFloating()
            : false;
        QTest::mouseRelease(target, Qt::LeftButton, {}, start + QPoint(-260, 80));
        QTest::qWait(60);
        return floated;
    };

    // 1. The dock TAB BAR cannot undock: dragging a tab only reorders it.
    QTabBar *bar = nullptr;
    for (QTabBar *t : w.findChildren<QTabBar *>()) {
        if (t->count() == 2) {
            bar = t;
            break;
        }
    }
    QVERIFY(bar);
    const QPoint tab = bar->tabRect(0).center();
    QTest::mousePress(bar, Qt::LeftButton, {}, tab);
    for (int dx = 5; dx <= 260; dx += 15)
        QTest::mouseMove(bar, tab + QPoint(-dx, dx / 3));
    QTest::mouseRelease(bar, Qt::LeftButton, {}, tab + QPoint(-260, 80));
    QTest::qWait(150);
    QVERIFY2(!a->isFloating(), "a dock tab drag undocked the pane — the title bar could now be hidden");

    // 2. A CUSTOM title bar never receives the drag either, however slim.
    auto *slim = new QWidget(a);
    slim->setFixedHeight(10);
    a->setTitleBarWidget(slim);
    QTest::qWait(50);
    QVERIFY2(!dragFrom(a, QPoint(a->width() / 2, 5)),
             "a custom title bar started a drag — a slimmer bar is now possible");

    // 3. The CONTROL: Qt's own title bar does undock, which is why it is what loftail
    //    keeps. Without this the two negatives above could just mean a broken harness.
    a->setTitleBarWidget(nullptr);
    QTest::qWait(50);
    QVERIFY2(dragFrom(a, QPoint(a->width() / 2, 8)),
             "the default title bar did not undock — this harness proves nothing");
}

// The Filters pane no longer says "Filtering" inside itself — that word and the Clear
// button beside it are gone. The dock TITLE is what reports it now, and since the panes
// ship tabified that title is a tab label, which is the case the word could never cover:
// filters are usually in force while the pane is behind Highlighters, Presets or Runs.
// So this marker is the whole of the feature now rather than a second copy of it.
void TestPaneChrome::theFiltersTabIsMarkedWhileFiltersAreInForce()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("marked.log"));
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        // The app's default log4cplus pattern, so opening needs no dialog.
        for (int i = 0; i < 8; ++i)
            f.write(QStringLiteral("2026-07-21 10:00:0%1,000 [main] INFO  sub.%2 - line\n")
                        .arg(i).arg(i % 2).toUtf8());
    }

    MainWindow w;
    w.resize(1000, 700);
    w.show();
    w.openFile(path);
    QTest::qWait(50);

    QDockWidget *filters = paneDock(w, "filtersDock");
    QVERIFY(filters);
    auto *pane = w.findChild<FilterPane *>();
    QVERIFY(pane);

    // Unfiltered on open — every axis default excludes nothing, so no marker.
    const QString plain = filters->windowTitle();
    QVERIFY2(!plain.contains(QChar(0x2022)), qPrintable(plain));

    // Something that actually narrows: a message search no record answers.
    auto *messageGroup = pane->findChild<QGroupBox *>(QStringLiteral("messageGroup"));
    auto *messageText = pane->findChild<QLineEdit *>(QStringLiteral("messageText"));
    QVERIFY(messageGroup && messageText);
    messageGroup->setChecked(true);
    messageText->setText(QStringLiteral("line"));
    QTRY_VERIFY(filters->windowTitle().contains(QChar(0x2022)));

    // And back, so the marker tracks the state rather than latching on the first filter.
    pane->clearAll();
    QTRY_COMPARE(filters->windowTitle(), plain);
}

// A log that is written in the application's default log4cplus pattern, so opening it
// needs no format dialog.
static bool writeDefaultLog(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    for (int i = 0; i < 8; ++i) {
        f.write(QStringLiteral("2026-07-21 10:00:0%1,000 [main] INFO  sub.%2 - line\n")
                    .arg(i).arg(i % 2).toUtf8());
    }
    return true;
}

// View ▸ Clear Filters is the ONLY way back to an unfiltered view, the pane having no
// Clear button of its own — so whether it is live is the one signal the window gives
// about it, and it has to mean something. It is asked of the same
// FilterPane::hasActiveFilters() the Filters tab's marker is drawn from, which is why
// the two are checked together here: a second definition of "filters are in force"
// would let the menu item and the dot drift apart.
void TestPaneChrome::theClearFiltersItemIsLiveOnlyWhileThereIsSomethingToClear()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("clearable.log"));
    QVERIFY(writeDefaultLog(path));

    MainWindow w;
    w.resize(1000, 700);
    w.show();
    QTest::qWait(50);

    auto *clear = w.findChild<QAction *>(QStringLiteral("clearFiltersAction"));
    QVERIFY(clear);
    // Nothing open: there is no view to unfilter, so the item says so.
    QVERIFY(!clear->isEnabled());

    w.openFile(path);
    QTest::qWait(50);
    // Open but unfiltered. Every axis's default excludes nothing — including the two
    // that ship ticked — so an enabled item here would be an invitation to clear a
    // filter the reader never set.
    QVERIFY(!clear->isEnabled());

    auto *pane = w.findChild<FilterPane *>();
    QVERIFY(pane);
    auto *messageGroup = pane->findChild<QGroupBox *>(QStringLiteral("messageGroup"));
    auto *messageText = pane->findChild<QLineEdit *>(QStringLiteral("messageText"));
    QVERIFY(messageGroup && messageText);
    messageGroup->setChecked(true);
    messageText->setText(QStringLiteral("line"));
    // QTRY, not QCOMPARE: the pane coalesces an edit once a pass has proved slow enough
    // to be worth deferring, so the item follows the DEBOUNCED signal and not the
    // keystroke — which is the whole reason it hangs off activityChanged().
    QTRY_VERIFY(clear->isEnabled());

    // And the item does what being live promised, which is also how it goes out again.
    clear->trigger();
    QTRY_VERIFY(!clear->isEnabled());
    QCOMPARE(messageText->text(), QString());

    // Context alone counts. It is not part of the FilterSet — it is two ints on the
    // Document — and with no text axis in force it hides nothing, so nothing on screen
    // changes. It is still a setting the user made and still a setting clearAll() zeroes,
    // so a grey item would be refusing to undo something it undoes.
    auto *contextBefore = pane->findChild<QSpinBox *>(QStringLiteral("contextBefore"));
    QVERIFY(contextBefore);
    contextBefore->setValue(2);
    QTRY_VERIFY(clear->isEnabled());
    clear->trigger();
    QTRY_VERIFY(!clear->isEnabled());
    QCOMPARE(contextBefore->value(), 0);
}

// Filters belong to the FILE (invariant #7), so the item answers for whichever log is in
// front — the case a check written only against one open document would miss entirely.
void TestPaneChrome::theClearFiltersItemFollowsTheTabInFront()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString filtered = dir.filePath(QStringLiteral("filtered.log"));
    const QString plain = dir.filePath(QStringLiteral("plain.log"));
    QVERIFY(writeDefaultLog(filtered));
    QVERIFY(writeDefaultLog(plain));

    MainWindow w;
    w.resize(1000, 700);
    w.show();
    w.openFile(filtered);
    QTest::qWait(50);

    auto *clear = w.findChild<QAction *>(QStringLiteral("clearFiltersAction"));
    auto *pane = w.findChild<FilterPane *>();
    auto *tabs = w.findChild<QTabWidget *>(QStringLiteral("documentTabs"));
    QVERIFY(clear && pane && tabs);

    auto *messageGroup = pane->findChild<QGroupBox *>(QStringLiteral("messageGroup"));
    auto *messageText = pane->findChild<QLineEdit *>(QStringLiteral("messageText"));
    QVERIFY(messageGroup && messageText);
    messageGroup->setChecked(true);
    messageText->setText(QStringLiteral("line"));
    QTRY_VERIFY(clear->isEnabled());

    // The second log opens in front of the first and carries no filters of its own.
    w.openFile(plain);
    QTest::qWait(50);
    QCOMPARE(tabs->count(), 2);
    QVERIFY(!clear->isEnabled());

    // Back to the filtered one and the item is live again, with the tab's marker
    // agreeing — the pane's state travels with the file, and so does this.
    tabs->setCurrentIndex(0);
    QTest::qWait(50);
    QVERIFY(clear->isEnabled());
    QDockWidget *filtersDock = paneDock(w, "filtersDock");
    QVERIFY(filtersDock);
    QVERIFY(filtersDock->windowTitle().contains(QChar(0x2022)));

    tabs->setCurrentIndex(1);
    QTest::qWait(50);
    QVERIFY(!clear->isEnabled());
    QVERIFY(!filtersDock->windowTitle().contains(QChar(0x2022)));
}

// The Highlighters tab's marker, which is NOT "are there rules". Every log opens with
// the three seeded level colours (HighlighterSet::defaults), so a dot meaning "rules
// exist" was on for every file from the moment it opened — truthful, and useless, a
// marker being worth a glance only while it is sometimes absent. It now means "these
// are not the rules loftail seeded", which is the whole list compared IN ORDER
// (HighlighterPane::hasCustomRules).
//
// Driven through a real MainWindow because that is the only place the seeding, the
// pane and the dock title meet: the pane alone would prove the comparison and none of
// what the reader actually sees.
void TestPaneChrome::theHighlightersTabIsMarkedOnlyOnceTheRulesAreNotTheSeededOnes()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("highlighted.log"));
    QVERIFY(writeDefaultLog(path));

    MainWindow w;
    w.resize(1000, 700);
    w.show();
    w.openFile(path);
    QTest::qWait(50);

    QDockWidget *highlighters = paneDock(w, "highlightersDock");
    QVERIFY(highlighters);
    auto *pane = w.findChild<HighlighterPane *>();
    QVERIFY(pane);
    auto *table = pane->findChild<QTableWidget *>(QStringLiteral("ruleTable"));
    QVERIFY(table);
    auto *newBtn = pane->findChild<QPushButton *>(QStringLiteral("ruleNew"));
    auto *removeBtn = pane->findChild<QPushButton *>(QStringLiteral("ruleRemove"));
    auto *downBtn = pane->findChild<QPushButton *>(QStringLiteral("ruleDown"));
    auto *upBtn = pane->findChild<QPushButton *>(QStringLiteral("ruleUp"));
    QVERIFY(newBtn && removeBtn && downBtn && upBtn);

    const int seeded = HighlighterSet::defaults().rules.size();
    QCOMPARE(table->rowCount(), seeded);

    // The case the marker used to get wrong: three rules are colouring this log and
    // nobody asked for them, so the tab says nothing.
    const QString plain = highlighters->windowTitle();
    QVERIFY2(!plain.contains(QChar(0x2022)), qPrintable(plain));

    // A rule the reader added.
    newBtn->click();
    QCOMPARE(table->rowCount(), seeded + 1);
    QTRY_VERIFY(highlighters->windowTitle().contains(QChar(0x2022)));

    // ...and taking it away again leaves the seed, which is not a state to report.
    removeBtn->click();
    QCOMPARE(table->rowCount(), seeded);
    QTRY_COMPARE(highlighters->windowTitle(), plain);

    // A seeded rule SWITCHED OFF is a difference — the list is still three rules long,
    // so a count could never see it, and the log has visibly stopped colouring FATAL.
    QTableWidgetItem *tick = table->item(0, HighlighterPane::kColEnabled);
    QVERIFY(tick);
    tick->setCheckState(Qt::Unchecked);
    QTRY_VERIFY(highlighters->windowTitle().contains(QChar(0x2022)));
    tick->setCheckState(Qt::Checked);
    QTRY_COMPARE(highlighters->windowTitle(), plain);

    // A seeded rule MOVED is a difference too, and for a reason the reader can see:
    // first-match-wins is per action, so FATAL under ERROR hands a FATAL record the
    // ERROR colour. Same three rules, same ticks, same colours — only the order.
    table->setCurrentCell(0, HighlighterPane::kColRule);
    downBtn->click();
    QTRY_VERIFY(highlighters->windowTitle().contains(QChar(0x2022)));

    // Put back exactly, and the marker goes out: the answer is the list in front of the
    // reader, never a latch on having once edited it.
    upBtn->click();
    QTRY_COMPARE(highlighters->windowTitle(), plain);

    // Throughout which the Filters tab has stayed unmarked, its axes' defaults excluding
    // nothing — the property the Highlighters tab had lost and has now got back.
    QDockWidget *filters = paneDock(w, "filtersDock");
    QVERIFY(filters);
    QVERIFY(!filters->windowTitle().contains(QChar(0x2022)));
}

// The gesture the marker used to be lit by, with nothing on screen changing: picking a
// run, and switching how the timestamp column reads. Both call
// HighlighterPane::refreshTimeBounds(), which used to read the whole axis editor back
// into the selected rule — and AxisEditor::criteria() is not the inverse of
// setCriteria(), so a seeded rule came back with 2000-01-01 bounds and a value axis
// claiming to cover nothing (bugs.md #5).
//
// The DOCUMENT's rules are what is asserted on, with the dock title beside them: the
// marker alone would pass a patch that repaired only the bounds, because the coverage
// flag is a difference all by itself.
void TestPaneChrome::aRunClickOrATimestampModeChangeIsNotAnEditToTheRules()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("unedited.log"));
    QVERIFY(writeDefaultLog(path));

    MainWindow w;
    w.resize(1000, 700);
    w.show();
    w.openFile(path);
    QTest::qWait(200);

    auto *tabs = w.findChild<QTabWidget *>(QStringLiteral("documentTabs"));
    QVERIFY(tabs && tabs->count() == 1);
    auto *view = qobject_cast<DocumentView *>(tabs->widget(0));
    QVERIFY(view && view->context());
    Document *doc = view->context()->doc.get();
    QVERIFY(doc);

    QDockWidget *highlighters = paneDock(w, "highlightersDock");
    QVERIFY(highlighters);
    const QString plain = highlighters->windowTitle();
    QCOMPARE(doc->highlighters().rules, HighlighterSet::defaults().rules);
    QVERIFY2(!plain.contains(QChar(0x2022)), qPrintable(plain));

    // "All runs" — the row that needs no run-start pattern, and the same
    // MainWindow::onRunSelected() a detected run's row reaches.
    auto *runs = w.findChild<QListWidget *>(QStringLiteral("runList"));
    QVERIFY(runs);
    runs->setCurrentRow(RunPane::kAllRunsRow);
    QTest::qWait(50);
    QCOMPARE(doc->highlighters().rules, HighlighterSet::defaults().rules);
    QCOMPARE(highlighters->windowTitle(), plain);

    runs->setCurrentRow(RunPane::kLastRunRow);
    QTest::qWait(50);
    QCOMPARE(doc->highlighters().rules, HighlighterSet::defaults().rules);
    QCOMPARE(highlighters->windowTitle(), plain);

    // Every entry of the timestamp menu, including the two that genuinely move the
    // display zone — the seeded rules name no time bound, so there is nothing to
    // re-express and nothing to write.
    for (const char *name : {"timeDisplaySecondsAction", "timeDisplayUtcAction",
                             "timeDisplayLocalAction", "timeDisplayRunSecondsAction",
                             "timeDisplayAsWrittenAction"}) {
        auto *action = w.findChild<QAction *>(QString::fromLatin1(name));
        QVERIFY2(action, name);
        action->trigger();
        QTest::qWait(20);
        QVERIFY2(doc->highlighters().rules == HighlighterSet::defaults().rules, name);
        QVERIFY2(highlighters->windowTitle() == plain, name);
    }
}

int main(int argc, char *argv[])
{
    QTemporaryDir configHome;
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
    qputenv("XDG_DATA_HOME", configHome.path().toUtf8());
    qputenv("HOME", configHome.path().toUtf8());
    qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("loftail-test-panechrome"));
    QApplication::setApplicationName(QStringLiteral("loftail-test-panechrome"));

    TestPaneChrome tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_panechrome.moc"
