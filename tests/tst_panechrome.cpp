#include <QtTest>

#include <QApplication>
#include <QDockWidget>
#include <QLineEdit>
#include <QGroupBox>
#include <QFile>
#include <QLabel>
#include <QMainWindow>
#include <QTabBar>
#include <QTemporaryDir>

#include "FilterPane.h"
#include "MainWindow.h"
#include "PaneTitleStyle.h"

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
