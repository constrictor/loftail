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
#include <QComboBox>
#include <QFile>
#include <QGroupBox>
#include <QJsonObject>
#include <QLineEdit>
#include <QListWidget>
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
#include "FilterUndoStack.h"
#include "FindBar.h"
#include "LogView.h"
#include "MainWindow.h"
#include "Priority.h"

using namespace loftail;

// A filter change is undoable, per log, with Esc and Shift+Esc (SPEC.md §6).
//
// Two halves, and both are needed. The rule itself — what counts as one entry — is
// value logic with no widgets in it, so the first cases drive FilterUndoStack directly:
// a run of typed edits is ONE entry and a tick is another, which is the whole reason
// the stack takes a `continuous` flag rather than pushing whatever it is handed.
//
// The rest needs a real MainWindow, because the things most easily got wrong are all
// about the window: the history is per LOG and survives a tab round trip; a pane REBIND
// must not look like an edit; an undo's own result must not go back on the stack; and
// Escape must still belong to the Find bar, which is what a QAction shortcut would have
// taken away with nothing in the pane or the FilterSet to show for it.
//
// Widgets and actions are found by OBJECT NAME, never by visible text.
class TestFilterUndo : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QString       m_a;
    QString       m_b;

    // 200 records over two subsystems, every 4th an ERROR: a WARN floor, a subsystem
    // tick and a message query each narrow it by a different amount, so which filter
    // is in force can be read off the record count alone.
    static void writeLog(const QString &path, const char *word)
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        for (int i = 0; i < 200; ++i) {
            f.write("2026-07-21 10:00:00,000 [main] ");
            f.write(i % 4 == 0 ? "ERROR " : "INFO  ");
            f.write(i % 2 == 0 ? "net.io" : "db.pool");
            f.write(" - ");
            f.write(word);
            f.write(" record ");
            f.write(QByteArray::number(i).rightJustified(4, '0'));
            f.write("\n");
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

    // From the FILTERS pane and never from the window: the Highlighters pane embeds an
    // AxisEditor of its own, so every one of these object names exists twice over and
    // findChild() on the window answers with whichever it reaches first.
    static FilterPane *filterPane(const MainWindow &w) { return w.findChild<FilterPane *>(); }

    template <typename T>
    static T *paneChild(const MainWindow &w, const char *name)
    {
        FilterPane *pane = filterPane(w);
        return pane ? pane->findChild<T *>(QString::fromLatin1(name)) : nullptr;
    }

    static int recordCount(const MainWindow &w)
    {
        const DocumentView *v = activeView(w);
        return v ? v->logView()->recordCount() : -1;
    }

    // Tick the priority axis at WARN through the pane's own controls, which is the route
    // a user takes and the one that ends in MainWindow::applyActiveFilters().
    //
    // TWO gestures, deliberately — the combo and then the group's tick — so it is also
    // what theHistoryIsOneEntryPerControlTouched() is built on. Moving the combo while
    // the axis is off narrows nothing, and it is still an entry: what an entry records is
    // the pane the reader can see, not the resolved FilterSet, or Esc would silently skip
    // over controls they had moved.
    static void applyWarnFloor(MainWindow &w)
    {
        auto *group = paneChild<QGroupBox>(w, "priorityGroup");
        auto *combo = paneChild<QComboBox>(w, "priorityCombo");
        QVERIFY(group);
        QVERIFY(combo);
        const int row = combo->findData(int(Priority::Warn));
        QVERIFY(row >= 0);
        combo->setCurrentIndex(row);
        group->setChecked(true);
    }

    // ONE gesture: untick one subsystem in the value list. Half the records name it, so
    // the count halves — and unlike applyWarnFloor above this touches a single control,
    // which is what most of the cases below want when they are counting Esc presses.
    static void hideSubsystem(MainWindow &w, const QString &name)
    {
        auto *list = paneChild<QListWidget>(w, "subsystemList");
        QVERIFY(list);
        for (int row = AxisEditor::kFirstValueRow; row < list->count(); ++row) {
            if (list->item(row)->text() == name) {
                list->item(row)->setCheckState(Qt::Unchecked);
                return;
            }
        }
        QFAIL("the subsystem is not in the list");
    }

    // Type into the message box the way a person does — one key at a time, through the
    // widget, with the focus in it. Typing is exactly what has to collapse to one entry,
    // so setText() would not exercise the thing under test.
    static void typeQuery(MainWindow &w, const QString &text)
    {
        auto *group = paneChild<QGroupBox>(w, "messageGroup");
        auto *edit = paneChild<QLineEdit>(w, "messageText");
        QVERIFY(group);
        QVERIFY(edit);
        group->setChecked(true);
        edit->setFocus();
        QVERIFY(edit->hasFocus());
        for (const QChar c : text)
            QTest::keyClicks(edit, QString(c));
    }

    static void pressEscape(MainWindow &w, Qt::KeyboardModifiers mods = Qt::NoModifier)
    {
        QTest::keyClick(&w, Qt::Key_Escape, mods);
    }

    // The pane's own snapshot, which is what an entry IS.
    static QJsonObject paneState(const MainWindow &w)
    {
        FilterPane *pane = filterPane(w);
        return pane ? pane->saveState() : QJsonObject();
    }

private slots:
    void initTestCase();
    void init();

    // The rule, with no window in it.
    void theFirstStateRecordedIsABaselineAndNotAnEntry();
    void arunOfTypedEditsIsOneEntryAndATickIsAnother();
    void abrokenRunStartsAFreshEntry();
    void anyEditClearsTheForwardPath();
    void theHistoryIsBoundedAndDropsItsOldest();

    // The window.
    void escapeTakesBackTheLastFilterAndShiftEscapePutsItAgain();
    void aTypedQueryComesBackInOnePress();
    void theHistoryIsOneEntryPerControlTouched();
    void escapeStillClosesTheFindBar();
    void escapeWithNothingToUndoIsNotSwallowed();
    void eachLogUndoesItsOwnFiltersAcrossATabRoundTrip();
    void undoingAndRedoingDoesNotDriftThePaneState();
    void theHistoryDiesWithTheTab();
    void theMenuItemsFollowTheTabInFront();
};

void TestFilterUndo::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_a = m_dir.filePath(QStringLiteral("a.log"));
    writeLog(m_a, "alpha");
    m_b = m_dir.filePath(QStringLiteral("b.log"));
    writeLog(m_b, "beta");
}

void TestFilterUndo::init()
{
    QSettings s;
    s.remove(QStringLiteral("session"));
    s.sync();
    // A log's filters outlive its tab (M21), so a case that opens the same path as an
    // earlier one would inherit what that one left behind and pass or fail on the order
    // QtTest happened to run them in.
    clearLogSettings();
}

// ---------------------------------------------------------------------------
// The rule
// ---------------------------------------------------------------------------

namespace {
QJsonObject stateWith(const char *key, int value)
{
    QJsonObject o;
    o.insert(QString::fromLatin1(key), value);
    return o;
}
} // namespace

void TestFilterUndo::theFirstStateRecordedIsABaselineAndNotAnEntry()
{
    FilterUndoStack stack;
    QVERIFY(!stack.canUndo());
    // What the log's filters ARE, not a change to them — this is what a context's first
    // hydration supplies, and establishing it here is why nothing has to seed the stack.
    stack.record(stateWith("text", 1), /*continuous=*/false);
    QVERIFY(!stack.canUndo());
    QVERIFY(!stack.canRedo());

    // And a re-apply that moves nothing is not an entry either. Reached often: every
    // route that re-resolves the same criteria comes through the pane's one apply path.
    stack.record(stateWith("text", 1), false);
    QVERIFY(!stack.canUndo());

    stack.record(stateWith("text", 2), false);
    QVERIFY(stack.canUndo());
    QCOMPARE(stack.undo(), stateWith("text", 1));
}

void TestFilterUndo::arunOfTypedEditsIsOneEntryAndATickIsAnother()
{
    FilterUndoStack stack;
    stack.record(stateWith("text", 0), false); // the baseline

    // Three keystrokes. The first has to push — there would otherwise be nothing to come
    // back to — and the two after it merge into it.
    stack.record(stateWith("text", 1), /*continuous=*/true);
    stack.record(stateWith("text", 2), true);
    stack.record(stateWith("text", 3), true);
    // A tick is a gesture of its own whatever came before it.
    stack.record(stateWith("tick", 1), /*continuous=*/false);

    QCOMPARE(stack.undo(), stateWith("text", 3)); // the whole typed run, in one press
    QCOMPARE(stack.undo(), stateWith("text", 0));
    QVERIFY(!stack.canUndo());
}

void TestFilterUndo::abrokenRunStartsAFreshEntry()
{
    FilterUndoStack stack;
    stack.record(stateWith("text", 0), false);
    stack.record(stateWith("text", 1), true);
    stack.record(stateWith("text", 2), true);
    // The caret left the box. Nothing else can see that a run ended: leaving and
    // returning moves focus twice with no filter change in between.
    stack.breakRun();
    stack.record(stateWith("text", 3), true);

    QCOMPARE(stack.undo(), stateWith("text", 2));
    QCOMPARE(stack.undo(), stateWith("text", 0));
}

void TestFilterUndo::anyEditClearsTheForwardPath()
{
    FilterUndoStack stack;
    stack.record(stateWith("text", 0), false);
    stack.record(stateWith("text", 1), false);
    stack.undo();
    QVERIFY(stack.canRedo());

    // Including one that MERGED rather than pushed: a redo after it would put back a
    // state that no longer follows from what is on screen.
    stack.record(stateWith("text", 5), true);
    QVERIFY(!stack.canRedo());
    stack.record(stateWith("text", 6), true);
    QVERIFY(!stack.canRedo());
}

void TestFilterUndo::theHistoryIsBoundedAndDropsItsOldest()
{
    FilterUndoStack stack;
    stack.record(stateWith("text", 0), false);
    for (int i = 1; i <= FilterUndoStack::kMaxDepth + 10; ++i)
        stack.record(stateWith("text", i), false);

    int steps = 0;
    QJsonObject last;
    while (stack.canUndo()) {
        last = stack.undo();
        ++steps;
    }
    QCOMPARE(steps, FilterUndoStack::kMaxDepth);
    // The oldest went, not the newest: the far end of a long history is what a reader
    // is least likely to want back.
    QCOMPARE(last, stateWith("text", 10));
}

// ---------------------------------------------------------------------------
// The window
// ---------------------------------------------------------------------------

void TestFilterUndo::escapeTakesBackTheLastFilterAndShiftEscapePutsItAgain()
{
    MainWindow w;
    w.resize(1200, 800);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    w.openFile(m_a);
    waitUntilIndexed(w);
    const int all = recordCount(w);
    QCOMPARE(all, 200);

    applyWarnFloor(w);
    QTRY_COMPARE(recordCount(w), 50); // every 4th record is an ERROR

    pressEscape(w);
    QTRY_COMPARE(recordCount(w), all);
    QVERIFY(!activeView(w)->context()->doc->filters().anyActive());
    // The pane shows it too — the widgets are the authoritative state, so a view that
    // widened while the axis stayed ticked would be the two disagreeing.
    QVERIFY(!paneChild<QGroupBox>(w, "priorityGroup")->isChecked());

    pressEscape(w, Qt::ShiftModifier);
    QTRY_COMPARE(recordCount(w), 50);
    QVERIFY(paneChild<QGroupBox>(w, "priorityGroup")->isChecked());
}

void TestFilterUndo::aTypedQueryComesBackInOnePress()
{
    MainWindow w;
    w.resize(1200, 800);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    w.openFile(m_a);
    waitUntilIndexed(w);

    // Ticking the axis on is one gesture; the four keystrokes after it are one more.
    typeQuery(w, QStringLiteral("0004"));
    QTRY_COMPARE(recordCount(w), 1);

    // ONE press for the whole word. Per keystroke this would take four, which is the
    // failure the `continuous` flag exists for — and the query box would come back
    // reading "000", then "00", then "0" on the way.
    pressEscape(w);
    QTRY_COMPARE(recordCount(w), 200); // an empty query narrows nothing
    QCOMPARE(paneChild<QLineEdit>(w, "messageText")->text(), QString());
    QVERIFY(paneChild<QGroupBox>(w, "messageGroup")->isChecked());

    // And the tick that armed the axis is the entry before it.
    pressEscape(w);
    QTRY_VERIFY(!paneChild<QGroupBox>(w, "messageGroup")->isChecked());
    QVERIFY(!w.findChild<QAction *>(QStringLiteral("undoFilterAction"))->isEnabled());
}

void TestFilterUndo::theHistoryIsOneEntryPerControlTouched()
{
    MainWindow w;
    w.resize(1200, 800);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    w.openFile(m_a);
    waitUntilIndexed(w);

    // Two controls, so two presses back — even though the first of them moved no record
    // at all, the priority axis being off while the combo was set. The rule is one entry
    // per control the reader touched: an edit that Esc skipped over would leave the pane
    // holding a setting nobody could account for.
    applyWarnFloor(w);
    QTRY_COMPARE(recordCount(w), 50);
    auto *combo = paneChild<QComboBox>(w, "priorityCombo");
    const int warnRow = combo->currentIndex();

    pressEscape(w);
    QTRY_COMPARE(recordCount(w), 200);
    QVERIFY(!paneChild<QGroupBox>(w, "priorityGroup")->isChecked());
    QCOMPARE(combo->currentIndex(), warnRow); // the tick came back, the level has not

    pressEscape(w);
    QTRY_VERIFY(combo->currentIndex() != warnRow);
    QVERIFY(!w.findChild<QAction *>(QStringLiteral("undoFilterAction"))->isEnabled());
}

void TestFilterUndo::escapeStillClosesTheFindBar()
{
    MainWindow w;
    w.resize(1200, 800);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    w.openFile(m_a);
    waitUntilIndexed(w);

    applyWarnFloor(w);
    QTRY_COMPARE(recordCount(w), 50);

    DocumentView *v = activeView(w);
    QVERIFY(v);
    v->activateFind();
    QVERIFY(v->findBar()->isVisible());

    // THE CASE A QAction SHORTCUT FAILS. A window-scoped shortcut on Escape is
    // dispatched before the focus widget, so it would undo the filter and leave the bar
    // on screen — with nothing in FindBar::status() or in the marks able to see it.
    QTest::keyClick(v->findBar(), Qt::Key_Escape);
    QVERIFY(!v->findBar()->isVisible());
    QCOMPARE(recordCount(w), 50); // and the filter did not move
}

void TestFilterUndo::escapeWithNothingToUndoIsNotSwallowed()
{
    MainWindow w;
    w.resize(1200, 800);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    w.openFile(m_a);
    waitUntilIndexed(w);

    // Nothing has been filtered, so there is nothing to walk back to and nothing may
    // change. The event goes on to whoever else wants it: this key is not ours to
    // swallow, which is what the un-accepted branch of keyPressEvent is for.
    QCOMPARE(recordCount(w), 200);
    pressEscape(w);
    QCOMPARE(recordCount(w), 200);
    pressEscape(w, Qt::ShiftModifier);
    QCOMPARE(recordCount(w), 200);
    QVERIFY(!w.findChild<QAction *>(QStringLiteral("undoFilterAction"))->isEnabled());
    QVERIFY(!w.findChild<QAction *>(QStringLiteral("redoFilterAction"))->isEnabled());
}

void TestFilterUndo::eachLogUndoesItsOwnFiltersAcrossATabRoundTrip()
{
    MainWindow w;
    w.resize(1200, 800);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    w.openFile(m_a);
    waitUntilIndexed(w);
    hideSubsystem(w, QStringLiteral("db.pool"));
    QTRY_COMPARE(recordCount(w), 100);

    w.openFile(m_b);
    waitUntilIndexed(w);
    QTabWidget *t = tabs(w);
    QCOMPARE(t->count(), 2);
    QCOMPARE(recordCount(w), 200);
    hideSubsystem(w, QStringLiteral("net.io"));
    QTRY_COMPARE(recordCount(w), 100);

    // b's own history, not a's — and exactly one entry deep, so the pane rebind that put
    // b's filters on screen was not recorded as an edit of its own.
    pressEscape(w);
    QTRY_COMPARE(recordCount(w), 200);
    QVERIFY(!w.findChild<QAction *>(QStringLiteral("undoFilterAction"))->isEnabled());

    // And a's survived the round trip. This is what fails if the history hangs off the
    // pane instead of the context.
    t->setCurrentIndex(0);
    QTRY_COMPARE(recordCount(w), 100);
    QVERIFY(w.findChild<QAction *>(QStringLiteral("undoFilterAction"))->isEnabled());
    pressEscape(w);
    QTRY_COMPARE(recordCount(w), 200);
    QVERIFY(!w.findChild<QAction *>(QStringLiteral("undoFilterAction"))->isEnabled());
}

void TestFilterUndo::undoingAndRedoingDoesNotDriftThePaneState()
{
    MainWindow w;
    w.resize(1200, 800);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    w.openFile(m_a);
    waitUntilIndexed(w);

    const QJsonObject opened = paneState(w);
    hideSubsystem(w, QStringLiteral("db.pool"));
    QTRY_COMPARE(recordCount(w), 100);
    const QJsonObject filtered = paneState(w);
    QVERIFY(filtered != opened);

    // Three round trips. AxisEditor::criteria() is not the inverse of setCriteria(), so a
    // stack that assumed the state it handed to restoreState() came back out unchanged
    // would drift a step per press and the counts would stop matching.
    for (int i = 0; i < 3; ++i) {
        pressEscape(w);
        QTRY_COMPARE(recordCount(w), 200);
        QCOMPARE(paneState(w), opened);
        pressEscape(w, Qt::ShiftModifier);
        QTRY_COMPARE(recordCount(w), 100);
        QCOMPARE(paneState(w), filtered);
    }
}

void TestFilterUndo::theHistoryDiesWithTheTab()
{
    MainWindow w;
    w.resize(1200, 800);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    w.openFile(m_a);
    waitUntilIndexed(w);
    applyWarnFloor(w);
    QTRY_COMPARE(recordCount(w), 50);

    w.findChild<QAction *>(QStringLiteral("closeTabAction"))->trigger();
    QTRY_COMPARE(tabs(w)->count(), 0);

    // The filter is remembered — it belongs to the log (M21) — but the history is not:
    // nothing about it is persisted, which is the whole of what "per tab" means here.
    w.openFile(m_a);
    waitUntilIndexed(w);
    QTRY_COMPARE(recordCount(w), 50);
    QVERIFY(!w.findChild<QAction *>(QStringLiteral("undoFilterAction"))->isEnabled());
    pressEscape(w);
    QCOMPARE(recordCount(w), 50);
}

void TestFilterUndo::theMenuItemsFollowTheTabInFront()
{
    MainWindow w;
    w.resize(1200, 800);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    auto *undo = w.findChild<QAction *>(QStringLiteral("undoFilterAction"));
    auto *redo = w.findChild<QAction *>(QStringLiteral("redoFilterAction"));
    QVERIFY(undo);
    QVERIFY(redo);
    QVERIFY(!undo->isEnabled()); // no log at all

    w.openFile(m_a);
    waitUntilIndexed(w);
    QVERIFY(!undo->isEnabled());
    hideSubsystem(w, QStringLiteral("db.pool"));
    // QTRY, not QCOMPARE: the pane coalesces an edit once a pass has proved slow enough
    // to be worth deferring, so the item follows the applied change and not the click.
    QTRY_VERIFY(undo->isEnabled());
    QVERIFY(!redo->isEnabled());

    // Triggered from the menu, the other way in.
    undo->trigger();
    QTRY_VERIFY(!undo->isEnabled());
    QVERIFY(redo->isEnabled());
    QCOMPARE(recordCount(w), 200);

    // A second log with no history of its own greys them both again, and coming back
    // gives the first one's history back — the items and the stacks cannot drift,
    // being one question asked in one place.
    w.openFile(m_b);
    waitUntilIndexed(w);
    QTRY_VERIFY(!undo->isEnabled());
    QVERIFY(!redo->isEnabled());
    tabs(w)->setCurrentIndex(0);
    QTRY_VERIFY(redo->isEnabled());
}

int main(int argc, char *argv[])
{
    QTemporaryDir configHome;
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
    qputenv("XDG_DATA_HOME", configHome.path().toUtf8());
    qputenv("HOME", configHome.path().toUtf8());
    qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("loftail-test-filterundo"));
    QApplication::setApplicationName(QStringLiteral("loftail-test-filterundo"));

    TestFilterUndo tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_filterundo.moc"
