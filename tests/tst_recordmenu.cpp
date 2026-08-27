// loftail — a desktop viewer for log4cplus logs.
// Copyright (C) 2026 Valentyn Pavliuchenko
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QtTest>

#include <QAction>
#include <QApplication>
#include <QFile>
#include <QGroupBox>
#include <QHeaderView>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QScrollBar>
#include <QSettings>
#include <QSpinBox>
#include <QTabWidget>
#include <QTemporaryDir>

#include "AxisEditor.h"
#include "ConfigReset.h"
#include "Document.h"
#include "DocumentContext.h"
#include "DocumentView.h"
#include "FilterPane.h"
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

    // How many records the active view is showing.
    //
    // ALWAYS READ THROUGH QTRY_COMPARE, never QCOMPARE, and the reason is not laziness
    // about timing. FilterPane::scheduleApply() applies SYNCHRONOUSLY only while the
    // last re-filter measured under 40 ms, and falls back to a 150 ms debounce
    // otherwise — an adaptive cadence that is right for typing and invisible from here.
    // So on an unloaded machine every edit lands before the next line runs, and on a
    // loaded one a single slow apply flips the pane into debounced mode and every
    // immediate read afterwards sees the PREVIOUS count.
    //
    // That is what this file used to do, and it was green for as long as the runners
    // happened to be quick: it failed as "3 instead of 2" on Windows in one CI run and
    // on Linux in the next, on code that did not touch filtering, and never once
    // locally. Waiting weakens nothing — a filter that never lands still fails, on the
    // QTRY timeout — and it removes an assumption the pane never made.
    static int visibleRecords(const MainWindow &w)
    {
        DocumentView *view = activeView(w);
        return view ? view->logView()->recordCount() : -1;
    }

    // A window big enough for four records and a header, shown, with the log open and
    // indexed. Shown because the double-click cases below aim at real coordinates, and
    // a viewport that was never laid out has none.
    static void openShown(MainWindow &w, const QString &path,
                          const QString &pattern = QString())
    {
        w.resize(1200, 800);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.openFile(path, pattern);
        waitUntilIndexed(w);
        // Every fixture record is a single line and a log opens with wrap off, so a view
        // row is one line tall and cellCentre()'s y is arithmetic.
        QCOMPARE(activeView(w)->logView()->wrapMode(), LogView::WrapMode::Off);

        // Every cell aimed at below has to BE in the viewport, and a column opens at a
        // width measured from the font (SPEC.md §5) — where no font resolves (Windows
        // offscreen ships none) each character is charged the 8 px floor, which is wide
        // enough to push the Subsystem column clean off the right edge of the document
        // area. Narrowing every section to an equal share of the viewport puts the whole
        // row on screen, so cellCentre() answers about the layout and not about the
        // platform's metrics.
        LogView *log = activeView(w)->logView();
        QHeaderView *header = log->header();
        header->setMinimumSectionSize(10); // the style's floor is font-derived too
        const int columns = qMax(1, header->count());
        const int share = qMax(10, (log->viewport()->width() - 8) / columns);
        for (int c = 0; c < header->count(); ++c)
            header->resizeSection(c, share);
        QVERIFY(header->length() <= log->viewport()->width());
    }

    // The centre of one cell, in the log view's VIEWPORT coordinates — which is what
    // QTest::mouseDClick on the viewport takes, and the space the header's own section
    // positions are in (LogView::layoutHeader aligns the two).
    static QPoint cellCentre(const MainWindow &w, int viewRow, FieldRole role)
    {
        LogView *log = activeView(w)->logView();
        const int column = columnOf(w, role);
        if (column < 0)
            return QPoint(-1, -1);
        QHeaderView *h = log->header();
        // Any x INSIDE the section and inside the viewport identifies the cell, which is
        // what the clamp below picks — a section wider than what is left of the viewport
        // has its midpoint off screen.
        const int lo = qMax(h->sectionViewportPosition(column), 0);
        const int hi = qMin(h->sectionViewportPosition(column) + h->sectionSize(column) - 1,
                            log->viewport()->width() - 1);
        if (lo > hi)
            return QPoint(-1, -1);
        const int lh = qMax(1, log->fontMetrics().height()); // LogView::lineHeight()'s floor
        return QPoint(qBound(lo, h->sectionViewportPosition(column) + h->sectionSize(column) / 2,
                             hi),
                      (viewRow - log->verticalScrollBar()->value()) * lh + lh / 2);
    }

    static void doubleClickCell(const MainWindow &w, int viewRow, FieldRole role)
    {
        LogView *log = activeView(w)->logView();
        const QPoint pos = cellCentre(w, viewRow, role);
        QVERIFY(pos.x() >= 0 && pos.y() >= 0);
        QVERIFY(log->viewport()->rect().contains(pos));
        QTest::mouseDClick(log->viewport(), Qt::LeftButton, Qt::KeyboardModifiers(), pos);
    }

    // One press-and-release on a cell with whatever modifiers are held — the two filter
    // chords, and the plain, Ctrl and Shift clicks they must leave alone.
    static void clickCell(const MainWindow &w, int viewRow, FieldRole role,
                          Qt::KeyboardModifiers mods)
    {
        LogView *log = activeView(w)->logView();
        const QPoint pos = cellCentre(w, viewRow, role);
        QVERIFY(pos.x() >= 0 && pos.y() >= 0);
        QVERIFY(log->viewport()->rect().contains(pos));
        QTest::mouseClick(log->viewport(), Qt::LeftButton, mods, pos);
    }

    static constexpr Qt::KeyboardModifiers kShowOnlyChord =
        Qt::ControlModifier | Qt::AltModifier;
    static constexpr Qt::KeyboardModifiers kHideChord = Qt::AltModifier;

    static int selectedRecords(const MainWindow &w)
    {
        DocumentView *v = activeView(w);
        return v ? v->logView()->selectionModel()->selectedRows(0).size() : -1;
    }

    // The interned ids an axis is narrowed to, as names, so a case can say what the
    // filter came out as rather than what its bit pattern is (invariant #4).
    static QStringList filteredNames(const MainWindow &w, bool logger)
    {
        const Document *doc = activeView(w)->context()->doc.get();
        const InternTable &table = logger ? doc->index().loggers : doc->index().threads;
        const QSet<quint32> &ids =
            logger ? doc->filters().loggerIds : doc->filters().threadIds;
        QStringList out;
        for (quint32 id : ids)
            out << table.name(id);
        out.sort();
        return out;
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
    void selectAllTakesTheActiveViewsVisibleRecordsAndNothingElse();

    // M15 — a context row is a real row: pointing at it must read ITS record, not the
    // match it was pulled in beside. Nothing in the menu changed for this, which is
    // exactly why it is worth pinning.
    void aContextRowOffersItsOwnRecord();

    // The one double-click gesture (SPEC.md §5). Driven with real mouse events at the
    // viewport, because the gesture is the whole of what was added: every case here
    // passes against activateRecordColumn() called directly.
    void doubleClickingASubsystemCellShowsOnlyThatSubsystem();
    void doubleClickingAThreadCellShowsOnlyThatThread();
    void doubleClickingAnyOtherColumnDoesNothingAtAll();
    void doubleClickingACellTheRecordCannotAnswerForDoesNothing();
    void doubleClickingTheSameCellAgainLeavesTheFilterWhereItIs();

    // The two filter chords (SPEC.md §5): Ctrl+Alt+click is *Show Only*, Alt+click is
    // *Hide*. Driven with real mouse events at the viewport for the reason the
    // double-click cases are — the modifier gating is the whole of what was added, and
    // every one of these passes against applyRecordFilter() called directly.
    void ctrlAltClickingASubsystemCellShowsOnlyThatSubsystem();
    void ctrlAltClickingAThreadCellShowsOnlyThatThread();
    void altClickingASubsystemCellHidesItAndKeepsDiscovering();
    void altClickingAThreadCellHidesIt();
    void ctrlAltClickingAPriorityCellSetsTheFloor();
    void altClickingAPriorityCellDoesNothing();
    void theChordsDoNothingOnTheOtherColumns();
    void aCellTheRecordCannotAnswerForIsInertUnderBothChords();
    void theChordsLeaveTheSelectionExactlyWhereItWas();
    void theOrdinarySelectionChordsAreUntouched();
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
    s.sync();
    // AND EVERY STORE A LOG'S SETTINGS CAN BE IN. `formatCache` was the QSettings key
    // this used to clear and has not existed since M20; what matters now is that since
    // M21 a log's FILTERS outlive the tab, so the "show only net.http" one case applies
    // would come back for every later case that opens the same log — and the record menu
    // is built from the record at a VIEW ROW, which a restored filter silently makes a
    // different record.
    clearLogSettings();
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
    QTRY_COMPARE(visibleRecords(w), 4);

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
    QTRY_COMPARE(second->logView()->recordCount(), 3);
    DocumentView *first = qobject_cast<DocumentView *>(tabs(w)->widget(0));
    QVERIFY(first);
    QTRY_COMPARE(first->logView()->recordCount(), 3);
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
    QTRY_COMPARE(visibleRecords(w), 3);
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

    QTRY_COMPARE(visibleRecords(w), 2); // the ERROR record + the unparsed line
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
    QTRY_COMPARE(visibleRecords(w), 3);

    // Now close the other end on what is row 1 of the FILTERED view — the same
    // [worker] record, since the plain line still leads.
    {
        QMenu menu;
        w.buildRecordMenu(&menu, view, 1, -1);
        QAction *end = item(menu, "recordTimeEnd");
        QVERIFY(end);
        end->trigger();
    }
    QTRY_COMPARE(visibleRecords(w), 2); // one timestamped record left, and the plain line
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
    QTRY_COMPARE(visibleRecords(w), 3);
}

void TestRecordMenu::highlightingAppendsARuleAndKeepsTheOthers()
{
    MainWindow w;
    w.openFile(m_log);
    waitUntilIndexed(w);
    Document *doc = activeView(w)->context()->doc.get();

    // A freshly opened log arrives with the level colours already in the list (SPEC.md
    // §7), and "keeps the others" is exactly the claim here — so the menu's rules are
    // counted from the end of the seed rather than from zero.
    const int seeded = doc->highlighters().rules.size();
    QVERIFY(seeded > 0);

    QMenu first;
    w.buildRecordMenu(&first, activeView(w), kMain, -1);
    item(first, "recordHighlightSubsystem")->trigger();
    QCOMPARE(doc->highlighters().rules.size(), seeded + 1);
    QCOMPARE(doc->highlighters().rules.at(seeded).match.loggerNames,
             QStringList{QStringLiteral("net.io")});
    QVERIFY(doc->highlighters().rules.at(seeded).match.loggerEnabled);

    QMenu second;
    w.buildRecordMenu(&second, activeView(w), kWorker, -1);
    item(second, "recordHighlightThread")->trigger();
    QCOMPARE(doc->highlighters().rules.size(), seeded + 2);
    // Appended, so the rule that was there keeps its first-match-wins precedence...
    QCOMPARE(doc->highlighters().rules.at(seeded).match.loggerNames,
             QStringList{QStringLiteral("net.io")});
    // ...and the two are told apart by color rather than both landing on slot 0.
    QVERIFY(doc->highlighters().rules.at(seeded).background
            != doc->highlighters().rules.at(seeded + 1).background);

    // Highlighting removes nothing: every record is still there (SPEC.md §7).
    QTRY_COMPARE(visibleRecords(w), 4);
}

void TestRecordMenu::aContextRowOffersItsOwnRecord()
{
    MainWindow w;
    w.openFile(m_log);
    waitUntilIndexed(w);

    // Search the messages for "three" — the one axis context widens (SPEC.md §6) —
    // then ask for one record of lead-up. Visible becomes: the WARN as CONTEXT, the
    // ERROR that matched. The record menu has no message item of its own, so this
    // reaches for the pane's controls by object name.
    auto *pane = w.findChild<FilterPane *>();
    QVERIFY(pane);
    auto *messageGroup = pane->findChild<QGroupBox *>(QStringLiteral("messageGroup"));
    auto *messageText = pane->findChild<QLineEdit *>(QStringLiteral("messageText"));
    QVERIFY(messageGroup && messageText);
    messageGroup->setChecked(true);
    messageText->setText(QStringLiteral("three"));
    QTRY_COMPARE(visibleRecords(w), 1);

    auto *before = pane->findChild<QSpinBox *>(QStringLiteral("contextBefore"));
    QVERIFY(before);
    before->setValue(1);
    QTRY_COMPARE(visibleRecords(w), 2);

    // View row 0 is the context row, whose SOURCE record is the [worker] WARN from
    // db.pool. The menu must describe that record, not the ERROR below it.
    QMenu menu;
    w.buildRecordMenu(&menu, activeView(w), 0, -1);
    QAction *hide = item(menu, "recordHideThread");
    QVERIFY(hide);
    QVERIFY2(hide->text().contains(QStringLiteral("worker")), qPrintable(hide->text()));
    QAction *only = item(menu, "recordShowOnlySubsystem");
    QVERIFY(only);
    QVERIFY2(only->text().contains(QStringLiteral("db.pool")), qPrintable(only->text()));
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

// Edit ▸ Select All (SPEC.md §5). Driven through the real window because what the
// action has to get right is which view it lands on and what "all" means there: the
// records IN VIEW (invariant #6), which a filter narrows, on the ACTIVE view alone
// (invariant #7). And it is dead while no log is open, like the copy actions beside it.
void TestRecordMenu::selectAllTakesTheActiveViewsVisibleRecordsAndNothingElse()
{
    MainWindow w;
    auto *selectAll = w.findChild<QAction *>(QStringLiteral("selectAllAction"));
    QVERIFY(selectAll);
    QVERIFY(!selectAll->isEnabled());

    w.openFile(m_log);
    waitUntilIndexed(w);
    QVERIFY(selectAll->isEnabled());

    const auto selectedCount = [](DocumentView *v) {
        return v->logView()->selectionModel()->selectedRows(0).size();
    };

    selectAll->trigger();
    QCOMPARE(selectedCount(activeView(w)), 4);

    // A second log in a second tab: the action speaks for the tab in front, and the one
    // behind it keeps whatever selection it had.
    DocumentView *first = activeView(w);
    const auto firstBefore = selectedCount(first);
    // With its pattern, so the open cannot stop on the format prompt.
    w.openFile(m_noThread, QStringLiteral("%-5p %c - %m%n"));
    waitUntilIndexed(w);
    QVERIFY(activeView(w) != first);
    selectAll->trigger();
    QCOMPARE(selectedCount(activeView(w)), 2);
    QCOMPARE(selectedCount(first), firstBefore);

    // Filter that tab down to one record: "all" is what the filter left, not the file.
    auto *pane = w.findChild<FilterPane *>();
    QVERIFY(pane);
    auto *messageGroup = pane->findChild<QGroupBox *>(QStringLiteral("messageGroup"));
    auto *messageText = pane->findChild<QLineEdit *>(QStringLiteral("messageText"));
    QVERIFY(messageGroup && messageText);
    messageGroup->setChecked(true);
    messageText->setText(QStringLiteral("two"));
    QTRY_COMPARE(visibleRecords(w), 1);

    selectAll->trigger();
    QCOMPARE(selectedCount(activeView(w)), 1);
}

// --- double-clicking a cell (SPEC.md §5) --------------------------------------
//
// The gesture is the record menu's own *Show Only …*, reached without the menu, so what
// these pin is that it lands on the SAME per-file filter state — and that it is offered
// exactly where the menu offers the item and nowhere else.

void TestRecordMenu::doubleClickingASubsystemCellShowsOnlyThatSubsystem()
{
    MainWindow w;
    openShown(w, m_log);
    QTRY_COMPARE(visibleRecords(w), 4);

    // kWorker is db.pool, and it is NOT the record the view opened on — the gesture has
    // to read the record under the pointer, not the selection.
    doubleClickCell(w, kWorker, FieldRole::Logger);

    const Document *doc = activeView(w)->context()->doc.get();
    QTRY_VERIFY(doc->filters().loggerEnabled);
    QTRY_COMPARE(filteredNames(w, /*logger=*/true), QStringList{QStringLiteral("db.pool")});
    QVERIFY(!doc->filters().threadEnabled);
    // db.pool's one record, plus the unparsed line no subsystem filter may hide (§6).
    QTRY_COMPARE(visibleRecords(w), 2);
    // And the record acted on is the one that was double-clicked — it is still the
    // focused one afterwards, at whatever view row the narrowed subset gives it.
    QCOMPARE(doc->filtered().sourceRow(activeView(w)->logView()->currentRecord()),
             int(kWorker));
}

void TestRecordMenu::doubleClickingAThreadCellShowsOnlyThatThread()
{
    MainWindow w;
    openShown(w, m_log);

    doubleClickCell(w, kWorker, FieldRole::Thread);

    const Document *doc = activeView(w)->context()->doc.get();
    QTRY_VERIFY(doc->filters().threadEnabled);
    QTRY_COMPARE(filteredNames(w, /*logger=*/false), QStringList{QStringLiteral("worker")});
    QVERIFY(!doc->filters().loggerEnabled);
    QTRY_COMPARE(visibleRecords(w), 2);
}

// The item asks for ONE gesture. A Message, Time or Priority cell names no value a
// value axis holds — the priority axis is a minimum, not a set — so a double-click there
// does nothing rather than something invented.
void TestRecordMenu::doubleClickingAnyOtherColumnDoesNothingAtAll()
{
    MainWindow w;
    openShown(w, m_log);

    for (FieldRole role : {FieldRole::Message, FieldRole::Date, FieldRole::Priority}) {
        doubleClickCell(w, kWorker, role);
        QVERIFY2(!activeView(w)->context()->doc->filters().anyActive(),
                 "a double-click outside the two value columns filtered something");
        QTRY_COMPARE(visibleRecords(w), 4);
    }
}

// Nothing that cannot be answered: an unparsed plain-text line has no subsystem, so
// the menu offers no item and the double-click on its Subsystem cell is inert — with no
// gate of its own to fall out of step with the menu's.
void TestRecordMenu::doubleClickingACellTheRecordCannotAnswerForDoesNothing()
{
    MainWindow w;
    openShown(w, m_log);

    doubleClickCell(w, kPlain, FieldRole::Logger);
    QVERIFY(!activeView(w)->context()->doc->filters().loggerEnabled);
    QTRY_COMPARE(visibleRecords(w), 4);

    // The empty space below the last record answers "nothing" too, exactly as it does
    // for the menu: a gesture aimed there acts on no record.
    LogView *log = activeView(w)->logView();
    const int lh = qMax(1, log->fontMetrics().height());
    QTest::mouseDClick(log->viewport(), Qt::LeftButton, Qt::KeyboardModifiers(),
                       QPoint(cellCentre(w, 0, FieldRole::Logger).x(),
                              log->viewport()->height() - lh / 2));
    QVERIFY(!activeView(w)->context()->doc->filters().loggerEnabled);
    QTRY_COMPARE(visibleRecords(w), 4);
}

// Deliberately not a toggle: the second double-click re-applies the same "show only",
// which the pane already treats as idempotent. Taking a filter back is the pane's job,
// and it is where the user can see what there is to take back.
void TestRecordMenu::doubleClickingTheSameCellAgainLeavesTheFilterWhereItIs()
{
    MainWindow w;
    openShown(w, m_log);

    doubleClickCell(w, kMain, FieldRole::Logger); // net.io
    QTRY_COMPARE(filteredNames(w, /*logger=*/true), QStringList{QStringLiteral("net.io")});
    QTRY_COMPARE(visibleRecords(w), 3); // two net.io records + the unparsed line

    // The filtered view still holds that record at row 1, and it is still net.io's.
    doubleClickCell(w, 1, FieldRole::Logger);
    QTRY_COMPARE(filteredNames(w, /*logger=*/true), QStringList{QStringLiteral("net.io")});
    QTRY_COMPARE(visibleRecords(w), 3);
}

// --- the two filter chords (SPEC.md §5) ---------------------------------------
//
// Ctrl+Alt+click is the Filters pane's own Ctrl+click — "show only this one" — reached
// from the record, and Alt+click is unticking that value. Both are the record menu's own
// items, so what these pin is that the chord lands on the SAME per-file filter state, is
// offered exactly where the menu offers the item, and spends the click entirely on the
// filter: the selection must not move.

void TestRecordMenu::ctrlAltClickingASubsystemCellShowsOnlyThatSubsystem()
{
    MainWindow w;
    openShown(w, m_log);
    QTRY_COMPARE(visibleRecords(w), 4);

    clickCell(w, kWorker, FieldRole::Logger, kShowOnlyChord); // db.pool

    const Document *doc = activeView(w)->context()->doc.get();
    QTRY_VERIFY(doc->filters().loggerEnabled);
    QTRY_COMPARE(filteredNames(w, /*logger=*/true), QStringList{QStringLiteral("db.pool")});
    QVERIFY(!doc->filters().threadEnabled);
    // db.pool's one record, plus the unparsed line no subsystem filter may hide (§6).
    QTRY_COMPARE(visibleRecords(w), 2);
}

void TestRecordMenu::ctrlAltClickingAThreadCellShowsOnlyThatThread()
{
    MainWindow w;
    openShown(w, m_log);

    clickCell(w, kWorker, FieldRole::Thread, kShowOnlyChord); // worker

    const Document *doc = activeView(w)->context()->doc.get();
    QTRY_VERIFY(doc->filters().threadEnabled);
    QTRY_COMPARE(filteredNames(w, /*logger=*/false), QStringList{QStringLiteral("worker")});
    QVERIFY(!doc->filters().loggerEnabled);
    QTRY_COMPARE(visibleRecords(w), 2);
}

// Hide unticks ONE value and says nothing about the next name the scan turns up, which
// is the whole difference from Show Only — the discovery rule stays armed.
void TestRecordMenu::altClickingASubsystemCellHidesItAndKeepsDiscovering()
{
    MainWindow w;
    openShown(w, m_log);

    clickCell(w, kMain, FieldRole::Logger, kHideChord); // net.io

    const Document *doc = activeView(w)->context()->doc.get();
    QTRY_VERIFY(doc->filters().loggerEnabled);
    QTRY_COMPARE(filteredNames(w, /*logger=*/true), QStringList{QStringLiteral("db.pool")});
    // net.io's two records gone; db.pool's one and the unparsed line left.
    QTRY_COMPARE(visibleRecords(w), 2);

    auto *pane = w.findChild<FilterPane *>();
    QVERIFY(pane);
    auto *list = pane->findChild<QListWidget *>(QStringLiteral("subsystemList"));
    QVERIFY(list);
    QVERIFY(AxisEditor::isOthersRow(list->item(0)));
    QCOMPARE(list->item(0)->checkState(), Qt::Checked); // still discovering
}

void TestRecordMenu::altClickingAThreadCellHidesIt()
{
    MainWindow w;
    openShown(w, m_log);

    clickCell(w, kWorker, FieldRole::Thread, kHideChord); // worker

    const Document *doc = activeView(w)->context()->doc.get();
    QTRY_VERIFY(doc->filters().threadEnabled);
    QTRY_COMPARE(filteredNames(w, /*logger=*/false), QStringList{QStringLiteral("main")});
    QTRY_COMPARE(visibleRecords(w), 3); // the two main records + the unparsed line
}

// The priority axis is a MINIMUM level, so Show Only means "this level and above" — the
// menu's own recordPriorityFloor, and the one column double-click still leaves alone.
void TestRecordMenu::ctrlAltClickingAPriorityCellSetsTheFloor()
{
    MainWindow w;
    openShown(w, m_log);

    clickCell(w, kError, FieldRole::Priority, kShowOnlyChord);

    QTRY_COMPARE(visibleRecords(w), 2); // the ERROR record + the unparsed line
}

// And there is no "hide this level": a floor cannot exclude one rung, so the chord that
// would ask for it does nothing rather than something invented.
void TestRecordMenu::altClickingAPriorityCellDoesNothing()
{
    MainWindow w;
    openShown(w, m_log);

    clickCell(w, kError, FieldRole::Priority, kHideChord);

    QVERIFY(!activeView(w)->context()->doc->filters().anyActive());
    QTRY_COMPARE(visibleRecords(w), 4);
}

void TestRecordMenu::theChordsDoNothingOnTheOtherColumns()
{
    MainWindow w;
    openShown(w, m_log);

    for (FieldRole role : {FieldRole::Message, FieldRole::Date}) {
        for (Qt::KeyboardModifiers mods : {kShowOnlyChord, kHideChord}) {
            clickCell(w, kWorker, role, mods);
            QVERIFY2(!activeView(w)->context()->doc->filters().anyActive(),
                     "a chord outside the three filterable columns filtered something");
            QTRY_COMPARE(visibleRecords(w), 4);
        }
    }
}

// Nothing that cannot be answered: an unparsed plain-text line has no subsystem, so the
// menu offers no item and both chords are inert — with no gate of their own to fall out
// of step with the menu's. The empty space below the last record answers "nothing" too.
void TestRecordMenu::aCellTheRecordCannotAnswerForIsInertUnderBothChords()
{
    MainWindow w;
    openShown(w, m_log);

    for (Qt::KeyboardModifiers mods : {kShowOnlyChord, kHideChord}) {
        clickCell(w, kPlain, FieldRole::Logger, mods);
        QVERIFY(!activeView(w)->context()->doc->filters().loggerEnabled);
        QTRY_COMPARE(visibleRecords(w), 4);

        LogView *log = activeView(w)->logView();
        const int lh = qMax(1, log->fontMetrics().height());
        QTest::mouseClick(log->viewport(), Qt::LeftButton, mods,
                          QPoint(cellCentre(w, 0, FieldRole::Logger).x(),
                                 log->viewport()->height() - lh / 2));
        QVERIFY(!activeView(w)->context()->doc->filters().loggerEnabled);
        QTRY_COMPARE(visibleRecords(w), 4);
    }
}

// The press is TAKEN: the click is spent entirely on the filter and moves nothing.
//
// Read on a chord that filters NOTHING, which is the only place the claim is visible —
// where the chord does apply a filter, the re-filter's own model reset clears the
// selection (QItemSelectionModel clears itself from modelReset), exactly as the record
// menu's Show Only has always done, so a fall-through there is indistinguishable. Here
// it is not: a press that fell through would collapse a two-record selection onto the
// record it landed on, and both of these leave the filters untouched.
void TestRecordMenu::theChordsLeaveTheSelectionExactlyWhereItWas()
{
    MainWindow w;
    openShown(w, m_log);

    LogView *log = activeView(w)->logView();
    clickCell(w, kMain, FieldRole::Message, Qt::NoModifier);
    clickCell(w, kWorker, FieldRole::Message, Qt::ShiftModifier);
    QCOMPARE(selectedRecords(w), 2);
    QCOMPARE(log->currentRecord(), int(kWorker));

    // A Message cell: neither chord means anything there, and both land on a record
    // that is not in the selection.
    for (Qt::KeyboardModifiers mods : {kShowOnlyChord, kHideChord}) {
        clickCell(w, kError, FieldRole::Message, mods);
        QVERIFY(!activeView(w)->context()->doc->filters().anyActive());
        QCOMPARE(selectedRecords(w), 2);
        QCOMPARE(log->currentRecord(), int(kWorker));
    }

    // And the unparsed line's Subsystem cell, where the chord is aimed at a real axis
    // and the record simply cannot answer for it.
    for (Qt::KeyboardModifiers mods : {kShowOnlyChord, kHideChord}) {
        clickCell(w, kPlain, FieldRole::Logger, mods);
        QVERIFY(!activeView(w)->context()->doc->filters().anyActive());
        QCOMPARE(selectedRecords(w), 2);
        QCOMPARE(log->currentRecord(), int(kWorker));
    }
}

// Ctrl+Alt was chosen over the pane's bare Ctrl precisely because Ctrl+click here means
// something already, and it goes on meaning it — on the filterable columns too. The
// modifiers are matched by exact equality, so Ctrl+Alt+Shift is still Shift's.
void TestRecordMenu::theOrdinarySelectionChordsAreUntouched()
{
    MainWindow w;
    openShown(w, m_log);
    LogView *log = activeView(w)->logView();

    clickCell(w, kPlain, FieldRole::Logger, Qt::NoModifier);
    QCOMPARE(selectedRecords(w), 1);

    // Ctrl+click on a SUBSYSTEM cell still takes a record in, and filters nothing.
    clickCell(w, kWorker, FieldRole::Logger, Qt::ControlModifier);
    QCOMPARE(selectedRecords(w), 2);
    QVERIFY(!activeView(w)->context()->doc->filters().anyActive());

    // Ctrl+click again takes it back out.
    clickCell(w, kWorker, FieldRole::Logger, Qt::ControlModifier);
    QCOMPARE(selectedRecords(w), 1);

    // Ctrl+Alt+SHIFT is not the chord: Shift outranks it and extends, filtering nothing.
    clickCell(w, kError, FieldRole::Logger,
              Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier);
    QVERIFY(!activeView(w)->context()->doc->filters().anyActive());
    QCOMPARE(log->currentRecord(), int(kError));
    QVERIFY(selectedRecords(w) > 1);
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
