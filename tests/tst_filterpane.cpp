#include <QtTest>

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QGroupBox>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QStyleOptionGroupBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QFile>
#include <QJsonArray>
#include <QTemporaryFile>

#include "AxisEditor.h"
#include "Document.h"
#include "FilterPane.h"
#include "Filter.h"
#include "LogFormat.h"
#include "Priority.h"
#include "SectionBox.h"
#include "RecordIndex.h"

using namespace loftail;

// The subsystem axis is ENABLED by default (SPEC.md §6) so its controls act on the
// first click instead of silently doing nothing until a master checkbox is also ticked.
// Priority was the second such axis and is no longer: its lowest offered level is DEBUG
// now that TRACE is gone, so it has no setting that is both ticked and harmless, and it
// ships off. That default is only safe if three things hold, and each of them is a way
// the change could have gone wrong:
//
//   1. An enabled-but-all-inclusive axis narrows nothing AND costs nothing — it must
//      leave FilteredIndex on its allocation-free identity path (ARCHITECTURE §7.2).
//   2. A subsystem discovered later in the scan arrives CHECKED, or an untouched
//      view would start dropping records as indexing progresses (SPEC.md §6: the
//      list is discovered as the file is scanned).
//   3. Rebinding the pane to another document forgets what it had seen, or the next
//      file opens with everything unchecked and the view comes up empty.
//
// Runs under an offscreen QApplication; FilterPane is a QWidget.
class TestFilterPane : public QObject
{
    Q_OBJECT

private:
    static constexpr auto kPattern = "%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n";

    static bool writeLog(QTemporaryFile &file, const QByteArray &bytes)
    {
        if (!file.open())
            return false;
        file.write(bytes);
        file.flush();
        return true;
    }

    static bool openLog(Document &doc, QTemporaryFile &file, const QByteArray &bytes)
    {
        return writeLog(file, bytes)
            && doc.open(file.fileName(), QString::fromLatin1(kPattern),
                        Encoding::Utf8, QTimeZone::utc());
    }

    // Each of the FIVE axes is a checkable QGroupBox whose title row IS the enable
    // control — priority included, which used to be a bare checkbox and combo on a
    // row of its own. Found by OBJECT NAME, never by the title it shows — a visible
    // string is a translator's to change (CLAUDE.md), and these names are the test
    // contract precisely because they are not.
    static QGroupBox *axis(FilterPane &pane, const char *name)
    {
        return pane.findChild<QGroupBox *>(QString::fromLatin1(name));
    }

    static QGroupBox *priorityEnable(FilterPane &pane)
    {
        return axis(pane, "priorityGroup");
    }

    // "Others" — the discovery rule as the FIRST ROW of each value list. Ticked, a
    // value the scan turns up later arrives ticked; unticked, the list is a
    // restriction (MatchCriteria::loggerRestrictive). Found by the role AxisEditor
    // marks it with, never by its label: it says translated prose, and nothing in
    // tests/ identifies a widget by the text it shows (ARCHITECTURE.md §9.1).
    static QListWidgetItem *othersRow(QListWidget *list)
    {
        QListWidgetItem *item = list ? list->item(0) : nullptr;
        return AxisEditor::isOthersRow(item) ? item : nullptr;
    }

    static bool discovers(QListWidget *list)
    {
        QListWidgetItem *item = othersRow(list);
        return item && item->checkState() == Qt::Checked;
    }

    // Everything below row 0 is a value, so a test that counts subsystems counts from
    // there. The row itself is covered on its own, by the cases naming it.
    static int valueCount(const QListWidget *list)
    {
        return list->count() - AxisEditor::kFirstValueRow;
    }

    // The subsystem list is the one holding the logger names.
    static QListWidget *loggerList(FilterPane &pane, const QString &anyName)
    {
        const QList<QListWidget *> lists = pane.findChildren<QListWidget *>();
        for (QListWidget *l : lists)
            for (int i = 0; i < l->count(); ++i)
                if (l->item(i)->text() == anyName)
                    return l;
        return nullptr;
    }

    static constexpr auto kTwoLoggers =
        "2026-07-21 12:00:00,000 [main] INFO  net.socket - a\n"
        "2026-07-21 12:00:01,000 [main] WARN  db.pool - b\n";

    // "The scan continues and finds another subsystem" — appending and re-indexing is
    // how these cases reproduce discovery without a live watcher.
    static bool appendAndReindex(Document &doc, QTemporaryFile &file, const char *line)
    {
        QFile appended(file.fileName());
        if (!appended.open(QIODevice::Append))
            return false;
        appended.write(line);
        appended.close();
        return doc.open(file.fileName(), QString::fromLatin1(kPattern), Encoding::Utf8,
                        QTimeZone::utc());
    }

    static Qt::CheckState stateOf(QListWidget *list, const QString &name)
    {
        for (int i = 0; i < list->count(); ++i)
            if (list->item(i)->text() == name)
                return list->item(i)->checkState();
        return Qt::PartiallyChecked; // "not listed at all"
    }

    static void setStateOf(QListWidget *list, const QString &name, Qt::CheckState state)
    {
        for (int i = 0; i < list->count(); ++i)
            if (list->item(i)->text() == name)
                list->item(i)->setCheckState(state);
    }

    // The two context spinners (M15). They are the pane's own widgets, reparented
    // into the message-text axis's body by AxisEditor::addTextExtra() — which is
    // where context belongs, since it widens that axis and no other (SPEC.md §6).
    static QSpinBox *contextSpin(FilterPane &pane, const char *name)
    {
        return pane.findChild<QSpinBox *>(QString::fromLatin1(name));
    }

private slots:
    void metadataAxesAreOnByDefault();
    void allInclusiveAxesStayInactive();
    void narrowingActivatesTheAxis();
    void subsystemDiscoveredLaterArrivesChecked();
    void rebindingForgetsSeenNames();

    // The record menu's edits (SPEC.md §5) land here, on the same controls a hand
    // edit uses — so the cases that matter are about what each edit MEANS, and above
    // all about the one place it contradicts the discovery rule above.
    void showOnlyRestrictsToOneSubsystem();
    void showOnlyDoesNotWidenWhenTheScanFindsMore();
    void aHandEditLeavesTheDiscoveryRuleAlone();
    void theListButtonsCarryTheDiscoveryRule();
    void ctrlClickingAValueShowsOnlyIt();
    void theOthersRowIsNotASubsystem();
    void hideLeavesTheRestAloneAndKeepsDiscovering();
    void priorityFloorComesFromTheRecord();
    void timeBoundKeepsTheRangeNonEmpty();
    void restrictionSurvivesASavedState();

    // M15 — filter context. The spinners are the pane's own controls, so what has to
    // hold is that they behave like every other control on it: one notification per
    // edit, straight into the document, and absent from a state that does not use them.
    void contextLivesInsideTheMessageAxis();
    void contextSpinnersReachTheDocument();
    void contextRidesTheSavedStateOnlyWhenSet();

    // The pane has to fit a dock and say what it is doing. Each of these pins a fix
    // for something that was silently wrong rather than a preference about layout.
    void timeRangeOpensOnTheFilesOwnSpan();
    void aTimeBoundSetByHandSurvivesTheScan();
    void timeBoundsAreAskedForInTheColumnsOwnUnits();
    void switchingTheDisplayModeKeepsTheBoundsInstant();
    void aGapColumnStillAsksForAWallClockBound();
    void runSecondsBoundsCountFromTheSelectedRun();
    void priorityComboFollowsItsCheckbox();
    void theLevelsOfferedStartAtDebugAndDefaultToInfo();
    void aRestoredTraceFloorBecomesAnUntickedAxis();
    void switchedOffAxesStayVisibleAndGreyed();
    void anAxisTheFormatLacksIsNotShownAtAll();
    void typingAnUnlistedNameOffersToAddIt();
    void listButtonsOwnUpToWhatTheNarrowingHides();
    void clearAllReturnsToAnUnfilteredView();
    void activityTracksTheResolvedSetNotTheTicks();
    void theAxesAreLinesNotFrames();
    void theAxisEnableControlsSitAtTheLeftEdge();
    void theContextRowLaysOutWithoutOverlapOrClipping();
    void theAxesAreInReadingOrder();
    void theValueListsTakeTheSpareHeight();
};

void TestFilterPane::metadataAxesAreOnByDefault()
{
    FilterPane pane;
    // Subsystem alone. Priority ships off now: its lowest offered level is DEBUG, so
    // there is no longer a level at which "ticked" and "hides nothing" are both true,
    // and shipping it ticked would hide every DEBUG record in every log on open.
    QVERIFY(axis(pane, "subsystemGroup")->isChecked());
    QVERIFY(!priorityEnable(pane)->isChecked());
    // The axes the user did not ask for keep their old default.
    QVERIFY(!axis(pane, "threadGroup")->isChecked());
    QVERIFY(!axis(pane, "messageGroup")->isChecked());
    QVERIFY(!axis(pane, "timeGroup")->isChecked());
}

void TestFilterPane::allInclusiveAxesStayInactive()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file, kTwoLoggers), qPrintable(doc.lastError()));

    FilterPane pane;
    pane.setDocument(&doc);

    // Both boxes are ticked and every subsystem is listed and checked...
    QListWidget *list = loggerList(pane, QStringLiteral("db.pool"));
    QVERIFY(list);
    QCOMPARE(valueCount(list), 2);
    for (int i = 0; i < list->count(); ++i)
        QCOMPARE(list->item(i)->checkState(), Qt::Checked);

    // ...so the FilterSet written to the Document excludes nothing, and the view
    // stays on the identity path rather than materializing a compact copy.
    QVERIFY(!doc.filters().anyActive());
    QVERIFY(!doc.filters().loggerEnabled);
    QVERIFY(!doc.filters().priorityEnabled);
    doc.applyFilters();
    QVERIFY(!doc.filtered().active());
    QCOMPARE(doc.filtered().recordCount(), 2);
}

void TestFilterPane::narrowingActivatesTheAxis()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file, kTwoLoggers), qPrintable(doc.lastError()));

    FilterPane pane;
    pane.setDocument(&doc);
    QListWidget *list = loggerList(pane, QStringLiteral("db.pool"));
    QVERIFY(list);

    // Unticking one subsystem acts immediately — no second click on an enable box.
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->text() == QStringLiteral("db.pool"))
            list->item(i)->setCheckState(Qt::Unchecked);

    QVERIFY(doc.filters().loggerEnabled);
    QVERIFY(doc.filters().anyActive());
    doc.applyFilters();
    QVERIFY(doc.filtered().active());
    QCOMPARE(doc.filtered().recordCount(), 1);
}

// Subsystems appear as the file is scanned, so the list is repopulated repeatedly
// during one document's life. A name arriving in a later round has no prior checked
// state; before the axis shipped enabled it did not matter, and now it decides
// whether that subsystem's records are visible. It must arrive checked — while a
// name the user deliberately unticked must STAY unticked across the same rounds.
void TestFilterPane::subsystemDiscoveredLaterArrivesChecked()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file,
                     "2026-07-21 12:00:00,000 [main] INFO  net.socket - a\n"
                     "2026-07-21 12:00:01,000 [main] INFO  db.pool - b\n"),
             qPrintable(doc.lastError()));

    FilterPane pane;
    pane.setDocument(&doc);
    QListWidget *list = loggerList(pane, QStringLiteral("net.socket"));
    QVERIFY(list);
    QCOMPARE(valueCount(list), 2);

    // The user rules one subsystem out.
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->text() == QStringLiteral("db.pool"))
            list->item(i)->setCheckState(Qt::Unchecked);

    // The scan continues and turns up a third subsystem.
    QFile appended(file.fileName());
    QVERIFY(appended.open(QIODevice::Append));
    appended.write("2026-07-21 12:00:02,000 [main] INFO  ui.window - c\n");
    appended.close();
    QVERIFY2(doc.open(file.fileName(), QString::fromLatin1(kPattern),
                      Encoding::Utf8, QTimeZone::utc()),
             qPrintable(doc.lastError()));
    pane.refreshDiscoveredLists();

    list = loggerList(pane, QStringLiteral("ui.window"));
    QVERIFY(list);
    QCOMPARE(valueCount(list), 3);
    for (int i = 0; i < list->count(); ++i) {
        const QString name = list->item(i)->text();
        const Qt::CheckState want =
            (name == QStringLiteral("db.pool")) ? Qt::Unchecked : Qt::Checked;
        QVERIFY2(list->item(i)->checkState() == want, qPrintable(name));
    }

    // And the record from the newly-discovered subsystem is visible.
    doc.applyFilters();
    QCOMPARE(doc.filtered().recordCount(), 2); // net.socket + ui.window, db.pool hidden
}

void TestFilterPane::rebindingForgetsSeenNames()
{
    QTemporaryFile fileA;
    Document docA;
    QVERIFY2(openLog(docA, fileA, kTwoLoggers), qPrintable(docA.lastError()));

    FilterPane pane;
    pane.setDocument(&docA);
    QVERIFY(loggerList(pane, QStringLiteral("db.pool")));

    // A second file, sharing one subsystem name with the first. Binding to it must
    // present every subsystem checked: the pane's memory of what it had already
    // shown belongs to the previous document.
    QTemporaryFile fileB;
    Document docB;
    QVERIFY2(openLog(docB, fileB,
                     "2026-07-21 12:00:00,000 [main] INFO  db.pool - a\n"
                     "2026-07-21 12:00:01,000 [main] INFO  ui.window - b\n"),
             qPrintable(docB.lastError()));
    pane.setDocument(&docB);

    QListWidget *list = loggerList(pane, QStringLiteral("ui.window"));
    QVERIFY(list);
    QCOMPARE(valueCount(list), 2);
    for (int i = 0; i < list->count(); ++i)
        QVERIFY2(list->item(i)->checkState() == Qt::Checked,
                 qPrintable(list->item(i)->text()));

    QVERIFY(!docB.filters().anyActive());
    docB.applyFilters();
    QCOMPARE(docB.filtered().recordCount(), 2);
}

// --- Filtering by pointing (the record menu, SPEC.md §5) -------------------
//
// The menu itself is assembled in MainWindow (tst_recordmenu); what lands here is
// the edit. These cases pin the two things a caller cannot see: that "show only"
// means the axis and nothing else, and that it survives the file growing.

void TestFilterPane::showOnlyRestrictsToOneSubsystem()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file, kTwoLoggers), qPrintable(doc.lastError()));

    FilterPane pane;
    pane.setDocument(&doc);
    pane.showOnlyValue(ValueAxis::Subsystem, QStringLiteral("db.pool"));

    QListWidget *list = loggerList(pane, QStringLiteral("db.pool"));
    QVERIFY(list);
    QCOMPARE(stateOf(list, QStringLiteral("db.pool")), Qt::Checked);
    QCOMPARE(stateOf(list, QStringLiteral("net.socket")), Qt::Unchecked);

    QVERIFY(doc.filters().loggerEnabled);
    doc.applyFilters();
    QCOMPARE(doc.filtered().recordCount(), 1);
}

// THE case this feature turns on. The subsystem list arrives checked for a name
// nobody has seen yet, on purpose (subsystemDiscoveredLaterArrivesChecked above) —
// which is exactly wrong for a selection made by pointing at one record: a filter
// meant as "only db.pool" would silently grow a second subsystem, and on a tailing
// log it would keep growing for as long as the file is open.
void TestFilterPane::showOnlyDoesNotWidenWhenTheScanFindsMore()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file, kTwoLoggers), qPrintable(doc.lastError()));

    FilterPane pane;
    pane.setDocument(&doc);
    pane.showOnlyValue(ValueAxis::Subsystem, QStringLiteral("db.pool"));

    QVERIFY2(appendAndReindex(doc, file, "2026-07-21 12:00:02,000 [main] INFO  ui.window - c\n"),
             qPrintable(doc.lastError()));
    pane.refreshDiscoveredLists();

    QListWidget *list = loggerList(pane, QStringLiteral("ui.window"));
    QVERIFY(list);
    QCOMPARE(valueCount(list), 3);
    QCOMPARE(stateOf(list, QStringLiteral("ui.window")), Qt::Unchecked);
    QCOMPARE(stateOf(list, QStringLiteral("db.pool")), Qt::Checked);

    doc.applyFilters();
    QCOMPARE(doc.filtered().recordCount(), 1); // still just db.pool
}

// ...and the restriction is not permanent — but the way out of it is the control that
// states it, not a side effect of some other edit.
//
// A hand tick used to clear the restriction, on the reasoning that whatever the user
// is building now is a statement about the file. That was defensible while the
// discovery rule was invisible and showOnlyValue() was the only way into it. It is not
// now that the rule is the "Others" row at the top of the very list being ticked: a row
// that unticks itself when the user ticks another one is worse than no row, and the
// whole point of showing the rule is that the user can see which one is in force and
// say otherwise.
void TestFilterPane::aHandEditLeavesTheDiscoveryRuleAlone()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file, kTwoLoggers), qPrintable(doc.lastError()));

    FilterPane pane;
    pane.setDocument(&doc);
    pane.showOnlyValue(ValueAxis::Subsystem, QStringLiteral("db.pool"));

    QListWidget *list = loggerList(pane, QStringLiteral("db.pool"));
    QVERIFY(list);
    QVERIFY(othersRow(list));
    QVERIFY(!discovers(list)); // the restriction, stated where it can be seen

    setStateOf(list, QStringLiteral("net.socket"), Qt::Checked);
    QVERIFY(!discovers(list)); // ...and one more tick is not a retraction of it

    QVERIFY2(appendAndReindex(doc, file, "2026-07-21 12:00:02,000 [main] INFO  ui.window - c\n"),
             qPrintable(doc.lastError()));
    pane.refreshDiscoveredLists();
    list = loggerList(pane, QStringLiteral("ui.window"));
    QVERIFY(list);
    QCOMPARE(stateOf(list, QStringLiteral("ui.window")), Qt::Unchecked);

    // Ticking the row IS the retraction, and it applies to the next value to appear —
    // not retroactively to the one already listed and already unticked, which the user
    // can see and tick for themselves.
    othersRow(list)->setCheckState(Qt::Checked);
    QVERIFY2(appendAndReindex(doc, file, "2026-07-21 12:00:03,000 [main] INFO  fs.cache - d\n"),
             qPrintable(doc.lastError()));
    pane.refreshDiscoveredLists();
    list = loggerList(pane, QStringLiteral("fs.cache"));
    QVERIFY(list);
    QCOMPARE(stateOf(list, QStringLiteral("fs.cache")), Qt::Checked);
}

// All / None / Invert carry the same answer to the values that have NOT turned up yet.
// "Everything" and "nothing" are claims about the axis, not about the handful of names
// that happen to be listed a third of the way through a scan — and the top row of the
// list is where that claim shows.
//
// Invert is the one with a way to go wrong now that the rule lives IN the list: it
// flips the values and then reads the rule back to flip that too, so a sweep that took
// row 0 with it would flip the rule twice and leave it exactly where it started.
void TestFilterPane::theListButtonsCarryTheDiscoveryRule()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file, kTwoLoggers), qPrintable(doc.lastError()));

    FilterPane pane;
    pane.setDocument(&doc);
    QListWidget *list = loggerList(pane, QStringLiteral("db.pool"));
    auto *none = pane.findChild<QAbstractButton *>(QStringLiteral("subsystemNone"));
    auto *all = pane.findChild<QAbstractButton *>(QStringLiteral("subsystemAll"));
    auto *invert = pane.findChild<QAbstractButton *>(QStringLiteral("subsystemInvert"));
    QVERIFY(list && othersRow(list) && none && all && invert);

    QVERIFY(discovers(list)); // discovery is the default (SPEC.md §6)
    none->click();
    QVERIFY(!discovers(list));
    invert->click();
    QVERIFY(discovers(list));
    invert->click();
    QVERIFY(!discovers(list));
    all->click();
    QVERIFY(discovers(list));

    // And "None" really does mean nothing, including what turns up next.
    none->click();
    QVERIFY2(appendAndReindex(doc, file, "2026-07-21 12:00:02,000 [main] INFO  ui.window - c\n"),
             qPrintable(doc.lastError()));
    pane.refreshDiscoveredLists();
    QCOMPARE(stateOf(list, QStringLiteral("ui.window")), Qt::Unchecked);
    doc.applyFilters();
    QCOMPARE(doc.filtered().recordCount(), 0);
}

// Ctrl+click on a row is the record menu's "show only this" reached from the list
// itself — the edit a list of ticks is most often wanted for, and otherwise two clicks
// (None, then the value) or a trip to a record that happens to carry the name.
//
// It has to be a real click through the viewport, because the whole mechanism is that
// the press is TAKEN: left to the view, the same press would toggle the tick under the
// cursor (on the indicator) or sweep the selection (anywhere else), and either lands on
// top of the exclusive edit rather than beside it. Calling checkOnly() directly would
// prove nothing about that.
void TestFilterPane::ctrlClickingAValueShowsOnlyIt()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file, kTwoLoggers), qPrintable(doc.lastError()));

    FilterPane pane;
    pane.setDocument(&doc);
    pane.resize(320, 700);
    pane.show();
    QVERIFY(QTest::qWaitForWindowExposed(&pane));
    QApplication::processEvents();

    QListWidget *list = loggerList(pane, QStringLiteral("db.pool"));
    QVERIFY(list && othersRow(list));
    QVERIFY(discovers(list));

    const QList<QListWidgetItem *> hits =
        list->findItems(QStringLiteral("db.pool"), Qt::MatchExactly);
    QCOMPARE(hits.size(), 1);
    const QRect rect = list->visualItemRect(hits.first());
    QVERIFY(rect.isValid());
    // Deliberately NOT on the check indicator: the chord acts on the row, wherever in
    // it the click lands, and the far side of the label is where a plain click would
    // only have moved the selection.
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::ControlModifier,
                      QPoint(rect.right() - 2, rect.center().y()));

    QCOMPARE(stateOf(list, QStringLiteral("db.pool")), Qt::Checked);
    QCOMPARE(stateOf(list, QStringLiteral("net.socket")), Qt::Unchecked);
    // ...and the same statement about what has not been found yet that the record menu
    // makes: "only this one" is a restriction, or the next subsystem to appear joins it.
    QVERIFY(!discovers(list));
    doc.applyFilters();
    QCOMPARE(doc.filtered().recordCount(), 1);

    // A plain click is untouched — it toggles the row it lands on and nothing else.
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier,
                      QPoint(rect.right() - 2, rect.center().y()));
    QCOMPARE(stateOf(list, QStringLiteral("db.pool")), Qt::Checked);

    // Ctrl+clicking "Others" is the same rule with the other row as its target: only
    // what the scan has not turned up yet, which is every listed value unticked and the
    // discovery rule back on.
    const QRect othersRect = list->visualItemRect(othersRow(list));
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::ControlModifier,
                      QPoint(othersRect.right() - 2, othersRect.center().y()));
    QVERIFY(discovers(list));
    QCOMPARE(stateOf(list, QStringLiteral("db.pool")), Qt::Unchecked);
    QCOMPARE(stateOf(list, QStringLiteral("net.socket")), Qt::Unchecked);
    QVERIFY2(appendAndReindex(doc, file,
                              "2026-07-21 12:00:02,000 [main] INFO  ui.window - c\n"),
             qPrintable(doc.lastError()));
    pane.refreshDiscoveredLists();
    QCOMPARE(stateOf(list, QStringLiteral("ui.window")), Qt::Checked);
}

// The row is a rule, not a value, so its label must never reach the selection —
// otherwise every preset and every saved session would carry a subsystem nothing is
// ever logged under, and "All" would resolve to one more name than the list shows.
void TestFilterPane::theOthersRowIsNotASubsystem()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file, kTwoLoggers), qPrintable(doc.lastError()));

    FilterPane pane;
    pane.setDocument(&doc);
    QListWidget *list = loggerList(pane, QStringLiteral("db.pool"));
    QVERIFY(list && othersRow(list));

    auto *all = pane.findChild<QAbstractButton *>(QStringLiteral("subsystemAll"));
    QVERIFY(all);
    all->click(); // everything ticked, "Others" included

    const QJsonArray checked =
        pane.saveState().value(QStringLiteral("loggerChecked")).toArray();
    QCOMPARE(checked.size(), 2);
    QVERIFY(checked.contains(QStringLiteral("db.pool")));
    QVERIFY(checked.contains(QStringLiteral("net.socket")));

    // And narrowing never takes it off screen: it is not one of the names being
    // searched, and it matters most while a selection is being built.
    auto *narrow = pane.findChild<QLineEdit *>(QStringLiteral("subsystemNarrow"));
    QVERIFY(narrow);
    narrow->setText(QStringLiteral("db."));
    QVERIFY(!othersRow(list)->isHidden());
    const QList<QListWidgetItem *> narrowedOut =
        list->findItems(QStringLiteral("net.socket"), Qt::MatchExactly);
    QCOMPARE(narrowedOut.size(), 1);
    QVERIFY(narrowedOut.first()->isHidden()); // a value the narrowing does hide
}

// Hiding one value says nothing about the next value to appear, so it must NOT
// restrict: the discovery rule stays in force and only the named subsystem is out.
void TestFilterPane::hideLeavesTheRestAloneAndKeepsDiscovering()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file, kTwoLoggers), qPrintable(doc.lastError()));

    FilterPane pane;
    pane.setDocument(&doc);
    pane.hideValue(ValueAxis::Subsystem, QStringLiteral("db.pool"));

    QListWidget *list = loggerList(pane, QStringLiteral("net.socket"));
    QVERIFY(list);
    QCOMPARE(stateOf(list, QStringLiteral("db.pool")), Qt::Unchecked);
    QCOMPARE(stateOf(list, QStringLiteral("net.socket")), Qt::Checked);
    doc.applyFilters();
    QCOMPARE(doc.filtered().recordCount(), 1);

    QVERIFY2(appendAndReindex(doc, file, "2026-07-21 12:00:02,000 [main] INFO  ui.window - c\n"),
             qPrintable(doc.lastError()));
    pane.refreshDiscoveredLists();
    doc.applyFilters();
    QCOMPARE(doc.filtered().recordCount(), 2); // net.socket + ui.window, db.pool still out
}

void TestFilterPane::priorityFloorComesFromTheRecord()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file, kTwoLoggers), qPrintable(doc.lastError()));

    FilterPane pane;
    pane.setDocument(&doc);
    pane.setMinimumPriority(Priority::Warn);

    QVERIFY(doc.filters().priorityEnabled);
    QCOMPARE(doc.filters().minPriority, Priority::Warn);
    doc.applyFilters();
    QCOMPARE(doc.filtered().recordCount(), 1); // the WARN record; the INFO one is out
}

// The time editors always hold SOME wall clock — there is no "no bound" to show —
// so setting one bound has to answer for the other. Setting the start must not
// leave an end that hides the record just pointed at, which is what an unseeded
// end (the year 2000) would do.
void TestFilterPane::timeBoundKeepsTheRangeNonEmpty()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file, kTwoLoggers), qPrintable(doc.lastError()));
    const qint64 first = doc.index().records.at(0).timestamp;
    const qint64 second = doc.index().records.at(1).timestamp;

    FilterPane pane;
    pane.setDocument(&doc);
    pane.setTimeBound(TimeBound::Start, first);

    QVERIFY(doc.filters().timeEnabled);
    QCOMPARE(doc.filters().startMs, first);
    QVERIFY(doc.filters().endMs >= second);
    doc.applyFilters();
    QCOMPARE(doc.filtered().recordCount(), 2);

    // Closing the other end from the first record leaves exactly that record.
    pane.setTimeBound(TimeBound::End, first);
    doc.applyFilters();
    QCOMPARE(doc.filtered().recordCount(), 1);
}

// A restriction is part of what the selection MEANS, so it has to travel with it
// into a preset or a session — otherwise a restored "only db.pool" would start
// widening the moment the restored file turned up a subsystem the original lacked.
void TestFilterPane::restrictionSurvivesASavedState()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file, kTwoLoggers), qPrintable(doc.lastError()));

    FilterPane source;
    source.setDocument(&doc);
    // A state nobody restricted carries no such key at all, which is what keeps
    // every preset written before this existed loading byte-identically.
    QVERIFY(!source.saveState().contains(QStringLiteral("loggerRestrictive")));
    source.showOnlyValue(ValueAxis::Subsystem, QStringLiteral("db.pool"));
    const QJsonObject state = source.saveState();
    QCOMPARE(state.value(QStringLiteral("loggerRestrictive")).toBool(), true);

    FilterPane restored;
    restored.setDocument(&doc);
    restored.restoreState(state);

    QVERIFY2(appendAndReindex(doc, file, "2026-07-21 12:00:02,000 [main] INFO  ui.window - c\n"),
             qPrintable(doc.lastError()));
    restored.refreshDiscoveredLists();

    QListWidget *list = loggerList(restored, QStringLiteral("ui.window"));
    QVERIFY(list);
    QCOMPARE(stateOf(list, QStringLiteral("ui.window")), Qt::Unchecked);
    doc.applyFilters();
    QCOMPARE(doc.filtered().recordCount(), 1);
}

void TestFilterPane::contextLivesInsideTheMessageAxis()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file, kTwoLoggers), qPrintable(doc.lastError()));

    FilterPane pane;
    pane.setDocument(&doc);

    QGroupBox *message = axis(pane, "messageGroup");
    QVERIFY(message);

    // Ancestry, not a layout position. Context widens the message-text axis and no
    // other (SPEC.md §6), so it belongs inside that axis rather than beside the five
    // of them — and being inside it is what makes the rest of this true.
    QVERIFY(message->isAncestorOf(contextSpin(pane, "contextBefore")));
    QVERIFY(message->isAncestorOf(contextSpin(pane, "contextAfter")));

    // With the axis off, context has nothing to widen: every record the other axes
    // admit is a match, so no record is left to be context. The checkable group says
    // so by greying the spinners, without a line of code here to keep in step.
    QVERIFY(!message->isChecked());
    QVERIFY(!contextSpin(pane, "contextBefore")->isEnabled());
    QVERIFY(!contextSpin(pane, "contextAfter")->isEnabled());

    message->setChecked(true);
    QVERIFY(contextSpin(pane, "contextBefore")->isEnabled());
    QVERIFY(contextSpin(pane, "contextAfter")->isEnabled());
}

void TestFilterPane::contextSpinnersReachTheDocument()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file, kTwoLoggers), qPrintable(doc.lastError()));

    FilterPane pane;
    pane.setDocument(&doc);
    QCOMPARE(doc.contextBefore(), 0);
    QCOMPARE(doc.contextAfter(), 0);

    QSpinBox *before = contextSpin(pane, "contextBefore");
    QSpinBox *after = contextSpin(pane, "contextAfter");
    QVERIFY(before);
    QVERIFY(after);
    QCOMPARE(before->maximum(), Document::kMaxContext);

    // One filtersChanged() per edit, exactly as a tick in the axis list produces:
    // MainWindow turns that signal into the model-reset + applyFilters() recompute,
    // so a context edit and a filter edit take the same route.
    QSignalSpy changed(&pane, &FilterPane::filtersChanged);
    QVERIFY(changed.isValid());

    before->setValue(3);
    QCOMPARE(changed.count(), 1);
    QCOMPARE(doc.contextBefore(), 3);

    after->setValue(2);
    QCOMPARE(changed.count(), 2);
    QCOMPARE(doc.contextAfter(), 2);
}

void TestFilterPane::contextRidesTheSavedStateOnlyWhenSet()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file, kTwoLoggers), qPrintable(doc.lastError()));

    FilterPane source;
    source.setDocument(&doc);
    // A pane with no context configured writes neither key, so every preset and
    // session stored before the feature existed round-trips byte-identically and
    // neither store's schema version has to move.
    QVERIFY(!source.saveState().contains(QStringLiteral("contextBefore")));
    QVERIFY(!source.saveState().contains(QStringLiteral("contextAfter")));

    contextSpin(source, "contextBefore")->setValue(4);
    const QJsonObject state = source.saveState();
    QCOMPARE(state.value(QStringLiteral("contextBefore")).toInt(), 4);
    QVERIFY(!state.contains(QStringLiteral("contextAfter"))); // still zero

    // Restoring puts it back on the controls AND on the document, and — like every
    // other restore — reports itself exactly once.
    Document other;
    QTemporaryFile otherFile;
    QVERIFY2(openLog(other, otherFile, kTwoLoggers), qPrintable(other.lastError()));

    FilterPane restored;
    restored.setDocument(&other);
    QSignalSpy changed(&restored, &FilterPane::filtersChanged);
    restored.restoreState(state);

    QCOMPARE(changed.count(), 1);
    QCOMPARE(contextSpin(restored, "contextBefore")->value(), 4);
    QCOMPARE(contextSpin(restored, "contextAfter")->value(), 0);
    QCOMPARE(other.contextBefore(), 4);
    QCOMPARE(other.contextAfter(), 0);

    // A state from before the feature existed means zero, which is what it meant then.
    QJsonObject legacy = state;
    legacy.remove(QStringLiteral("contextBefore"));
    restored.restoreState(legacy);
    QCOMPARE(other.contextBefore(), 0);
}

// ---------------------------------------------------------------------------
// Fitting a dock, and saying what is going on
// ---------------------------------------------------------------------------

// The one that was actually broken. The time editors were seeded ONCE, from
// setDocument(), which runs at open time — before the scan has produced a record. So
// observedSpan() failed, the editors kept QDateTimeEdit's year-2000 default, and
// ticking Time range on a freshly opened log applied 2000-01-01 .. 2000-01-01 and hid
// every record. It only ever worked when switching back to an already-indexed tab.
void TestFilterPane::timeRangeOpensOnTheFilesOwnSpan()
{
    Document doc;
    QTemporaryFile file;
    FilterPane pane;
    // Bind BEFORE there is anything to see, which is the order MainWindow uses.
    pane.setDocument(&doc);
    QVERIFY2(openLog(doc, file, kTwoLoggers), qPrintable(doc.lastError()));
    pane.refreshDiscoveredLists(); // what onIndexFinished() calls

    axis(pane, "timeGroup")->setChecked(true);

    const auto *start = pane.findChild<QDateTimeEdit *>(QStringLiteral("timeStart"));
    const auto *end = pane.findChild<QDateTimeEdit *>(QStringLiteral("timeEnd"));
    QVERIFY(start && end);
    QCOMPARE(start->dateTime().date().year(), 2026);
    QCOMPARE(end->dateTime().date().year(), 2026);
    QVERIFY(start->dateTime() <= end->dateTime());

    // And the point of all that: switching the axis on hides nothing.
    doc.applyFilters();
    QCOMPARE(doc.filtered().recordCount(), doc.index().records.size());
}

// The seed keeps tracking a growing file, so it must never take back a bound the user
// set. Same distinction m_loggerRestrictive draws between a hand edit and a repopulation.
void TestFilterPane::aTimeBoundSetByHandSurvivesTheScan()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY2(openLog(doc, file, kTwoLoggers), qPrintable(doc.lastError()));
    FilterPane pane;
    pane.setDocument(&doc);

    auto *start = pane.findChild<QDateTimeEdit *>(QStringLiteral("timeStart"));
    QVERIFY(start);
    const QDateTime chosen(QDate(2020, 1, 2), QTime(3, 4, 5));
    start->setDateTime(chosen);

    // The scan finds more, and every repopulation re-seeds — except over this.
    QVERIFY(appendAndReindex(doc, file, "2026-07-21 12:00:02,000 [main] INFO  net.http - c\n"));
    pane.refreshDiscoveredLists();

    QCOMPARE(start->dateTime(), chosen);
}

// A bound is asked for in the units the timestamp column is showing (SPEC.md §6). With
// the column rendering seconds, being handed a calendar is a question about a quantity
// the log never displayed — and answering it means converting by hand from a baseline
// only the pane knows.
void TestFilterPane::timeBoundsAreAskedForInTheColumnsOwnUnits()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY2(openLog(doc, file, kTwoLoggers), qPrintable(doc.lastError()));

    FilterPane pane;
    pane.show();
    QVERIFY(QTest::qWaitForWindowExposed(&pane));
    pane.setDocument(&doc);
    pane.refreshDiscoveredLists();

    auto *date = pane.findChild<QDateTimeEdit *>(QStringLiteral("timeStart"));
    auto *secs = pane.findChild<QDoubleSpinBox *>(QStringLiteral("timeStartSeconds"));
    QVERIFY(date && secs);

    // The default display mode renders the file's own date, so the pane asks for one.
    QVERIFY(date->isVisible());
    QVERIFY(!secs->isVisible());

    doc.setTimeDisplay(TimeDisplay::EpochSeconds);
    pane.refreshTimeBounds(); // what MainWindow calls on a display-mode change
    QVERIFY(!date->isVisible());
    QVERIFY(secs->isVisible());

    // And it is seeded in those units: the file's first record, in epoch seconds.
    const qint64 firstMs = doc.index().records.first().timestamp;
    QCOMPARE(secs->value(), double(firstMs) / 1000.0);

    // Millisecond precision exactly where the file's own %d has it — ".000" under a
    // format without %q would invent precision the log does not carry.
    QCOMPARE(secs->decimals(), 3);
}

// Switching how the column reads must not move a bound. The digits change; the instant
// they name does not — in both directions, and through a bound the user typed.
// "Seconds since the previous record" reads as seconds and is NOT one of the modes the
// bound editors follow (SPEC.md §4, §6): those seconds are an interval between two
// rows, and a bound is one instant, so there is no baseline that would turn a typed
// number into one. The axis therefore keeps the wall clock — and keeps the bound.
void TestFilterPane::aGapColumnStillAsksForAWallClockBound()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY2(openLog(doc, file, kTwoLoggers), qPrintable(doc.lastError()));

    FilterPane pane;
    pane.setDocument(&doc);
    pane.refreshDiscoveredLists();
    axis(pane, "timeGroup")->setChecked(true);

    auto *date = pane.findChild<QDateTimeEdit *>(QStringLiteral("timeStart"));
    auto *secs = pane.findChild<QDoubleSpinBox *>(QStringLiteral("timeStartSeconds"));
    QVERIFY(date && secs);
    date->setDateTime(QDateTime(QDate(2026, 7, 21), QTime(12, 0, 1)));
    const qint64 chosen = doc.filters().startMs;

    doc.setTimeDisplay(TimeDisplay::SincePrevious);
    pane.refreshTimeBounds();

    // isHidden(), not isVisible() — this pane is never shown.
    QVERIFY(!date->isHidden());
    QVERIFY(secs->isHidden());
    QCOMPARE(doc.filters().startMs, chosen);
    QCOMPARE(date->dateTime(), QDateTime(QDate(2026, 7, 21), QTime(12, 0, 1)));
}

void TestFilterPane::switchingTheDisplayModeKeepsTheBoundsInstant()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY2(openLog(doc, file, kTwoLoggers), qPrintable(doc.lastError()));

    FilterPane pane;
    pane.setDocument(&doc);
    pane.refreshDiscoveredLists();
    axis(pane, "timeGroup")->setChecked(true);

    // A bound that admits the second record and not the first.
    auto *date = pane.findChild<QDateTimeEdit *>(QStringLiteral("timeStart"));
    auto *secs = pane.findChild<QDoubleSpinBox *>(QStringLiteral("timeStartSeconds"));
    QVERIFY(date && secs);
    date->setDateTime(QDateTime(QDate(2026, 7, 21), QTime(12, 0, 1)));
    const qint64 chosen = doc.filters().startMs;
    doc.applyFilters();
    QCOMPARE(doc.filtered().recordCount(), 1);

    doc.setTimeDisplay(TimeDisplay::EpochSeconds);
    pane.refreshTimeBounds();
    QCOMPARE(secs->value(), double(chosen) / 1000.0);
    QCOMPARE(doc.filters().startMs, chosen);

    // Typed in seconds now, and read back as the same kind of bound.
    secs->setValue(secs->value() + 1.0);
    QCOMPARE(doc.filters().startMs, chosen + 1000);
    doc.applyFilters();
    QCOMPARE(doc.filtered().recordCount(), 0);

    // And back: the date editor holds what the seconds editor was saying. isHidden(),
    // not isVisible() — this pane is never shown, so nothing on it is visible.
    doc.setTimeDisplay(TimeDisplay::AsWritten);
    pane.refreshTimeBounds();
    QVERIFY(!date->isHidden());
    QVERIFY(secs->isHidden());
    QCOMPARE(doc.filters().startMs, chosen + 1000);
}

// "Seconds from run start" is per-record in the column — each record counts from its
// own run — and a bound has to name ONE instant. The selected run's baseline is the one
// that agrees with what is on screen, since the run selection is already what restricts
// the view, so moving the selection re-renders the digits rather than moving the bound.
void TestFilterPane::runSecondsBoundsCountFromTheSelectedRun()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY2(openLog(doc, file,
                     "2026-07-21 12:00:00,000 [main] INFO  boot - starting\n"
                     "2026-07-21 12:00:01,000 [main] INFO  net.socket - a\n"
                     "2026-07-21 12:00:10,000 [main] INFO  boot - starting\n"
                     "2026-07-21 12:00:12,000 [main] INFO  net.socket - b\n"),
             qPrintable(doc.lastError()));
    doc.setRunStart(QStringLiteral("starting"), /*regex=*/false, Qt::CaseSensitive);
    QCOMPARE(doc.runs().size(), 2);

    FilterPane pane;
    pane.setDocument(&doc);
    pane.refreshDiscoveredLists();
    doc.setTimeDisplay(TimeDisplay::RunSeconds);
    pane.refreshTimeBounds();

    auto *secs = pane.findChild<QDoubleSpinBox *>(QStringLiteral("timeStartSeconds"));
    QVERIFY(secs);

    doc.selectRun(0);
    pane.refreshTimeBounds();
    const qint64 chosen = doc.index().records.at(1).timestamp; // 12:00:01
    secs->setValue(1.0);                                       // 1 s into run 0
    axis(pane, "timeGroup")->setChecked(true);
    QCOMPARE(doc.filters().startMs, chosen);

    // Run 1 starts at 12:00:10, so the same instant is now "-9" seconds. The bound is
    // where it was; only the way it is written down moved.
    doc.selectRun(1);
    pane.refreshTimeBounds();
    QCOMPARE(secs->value(), -9.0);
    QCOMPARE(doc.filters().startMs, chosen);
}

// Priority used to be the one axis with no QGroupBox, so Qt was not greying its body
// for it: with the box unticked the combo stayed bright and spinnable and did nothing,
// and a hand-written setEnabled() had to stand in. It is a checkable group box like
// the other four now and Qt does it, which is what this still checks — the failure it
// guards against ("changing the minimum level did nothing") is the same either way.
void TestFilterPane::priorityComboFollowsItsCheckbox()
{
    // A document, because the editor disables itself wholesale without one and every
    // child would then report isEnabled() false for a reason that is not this one.
    Document doc;
    QTemporaryFile file;
    QVERIFY2(openLog(doc, file, kTwoLoggers), qPrintable(doc.lastError()));
    FilterPane pane;
    pane.setDocument(&doc);

    auto *combo = pane.findChild<QComboBox *>(QStringLiteral("priorityCombo"));
    QVERIFY(combo);
    // The axis ships OFF, so the combo ships greyed — and that greying is now the whole
    // reason the axis is allowed to ship off. It used to ship on so the combo would act
    // on the first click; what makes that unnecessary is that an untickable axis has an
    // unusable combo, so "I changed the level and nothing happened" cannot occur.
    QVERIFY(!priorityEnable(pane)->isChecked());
    QVERIFY(!combo->isEnabled());

    priorityEnable(pane)->setChecked(true);
    QVERIFY(combo->isEnabled());

    priorityEnable(pane)->setChecked(false);
    QVERIFY(!combo->isEnabled());
}

// TRACE is not offered: it is the lowest level the enum has, so it excludes nothing, and
// the no-op it named is reachable by unticking the axis instead. INFO is what a fresh
// pane holds.
void TestFilterPane::theLevelsOfferedStartAtDebugAndDefaultToInfo()
{
    FilterPane pane;
    auto *combo = pane.findChild<QComboBox *>(QStringLiteral("priorityCombo"));
    QVERIFY(combo);

    QCOMPARE(combo->count(), 5); // DEBUG, INFO, WARN, ERROR, FATAL — six levels less TRACE
    QCOMPARE(combo->findData(int(Priority::Trace)), -1);
    // QString(priorityName(p)), not priorityName(p) directly: it returns a
    // QLatin1StringView, which has no toUtf8() on the Qt 6.4 the reference build uses
    // (ARCHITECTURE.md §1) however well it compiles against a newer one. The explicit
    // QString is what every other caller of priorityName() in src/ already writes.
    for (Priority p : {Priority::Debug, Priority::Info, Priority::Warn, Priority::Error,
                       Priority::Fatal})
        QVERIFY2(combo->findData(int(p)) >= 0,
                 QString(priorityName(p)).toUtf8().constData());

    QCOMPARE(combo->currentData().toInt(), int(Priority::Info));

    // Every row's data agrees with its label, which is what stops the combo's own order
    // from being read as a PriorityChoice index — the two are no longer the same order,
    // and PriorityChoice is the on-disk format (MatchCriteria's minPriorityIndex).
    for (int i = 0; i < combo->count(); ++i)
        QCOMPARE(combo->itemText(i), priorityName(Priority(combo->itemData(i).toInt())));

    // And a saved INFO floor still reads back as INFO rather than one level over, which
    // is what dropping TRACE from PriorityChoice instead of from the combo would have
    // cost every preset and session ever written.
    MatchCriteria c;
    c.priorityEnabled = true;
    c.minPriority = Priority::Info;
    MatchCriteria back = MatchCriteria::fromJson(c.toJson());
    QCOMPARE(int(back.minPriority), int(Priority::Info));
}

// A TRACE floor can still arrive from a session or preset written while TRACE was on
// offer. It must not be promoted to DEBUG — that would turn a filter which showed
// everything into one that hides every TRACE record, silently, on restore. It means
// "do not narrow by level", so the axis comes back off.
void TestFilterPane::aRestoredTraceFloorBecomesAnUntickedAxis()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY2(openLog(doc, file, kTwoLoggers), qPrintable(doc.lastError()));
    FilterPane pane;
    pane.setDocument(&doc);

    // Through the real restore path, with the JSON a pre-change session actually holds:
    // minPriorityIndex 0 is TRACE in PriorityChoice, which is why that table could not
    // simply lose its first row.
    MatchCriteria old;
    old.priorityEnabled = true;
    old.minPriority = Priority::Trace;
    const QJsonObject saved = old.toJson();
    QCOMPARE(saved.value(QStringLiteral("minPriorityIndex")).toInt(), 0);
    pane.restoreState(saved);

    QVERIFY(!priorityEnable(pane)->isChecked());
    QVERIFY(!doc.filters().priorityEnabled);
    // Not silently promoted to the lowest level still offered.
    auto *combo = pane.findChild<QComboBox *>(QStringLiteral("priorityCombo"));
    QCOMPARE(combo->currentData().toInt(), int(Priority::Info));

    // The view is what it was before the level stopped being offered: everything.
    doc.applyFilters();
    QCOMPARE(doc.filtered().recordCount(), 2);
}

// A switched-off axis keeps its controls on screen, greyed. The pane briefly borrowed
// the Highlighters pane's setCollapsible(true) to save height, and height was the wrong
// thing to buy: an axis whose controls appear only once it is ticked cannot be read,
// only explored — the user has to switch a filter ON to find out whether it was the one
// they wanted, and the pane relays out under the pointer while they find out.
void TestFilterPane::switchedOffAxesStayVisibleAndGreyed()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file, kTwoLoggers), qPrintable(doc.lastError()));

    FilterPane pane;
    // Bound, because the editor disables itself wholesale without a document and the
    // question here is what an AXIS does to its own body.
    pane.setDocument(&doc);
    QGroupBox *message = axis(pane, "messageGroup");
    QVERIFY(message && !message->isChecked());
    // The body is everything under the title row; the title row is the check control.
    const auto *field = pane.findChild<QLineEdit *>(QStringLiteral("messageText"));
    QVERIFY(field);
    QVERIFY(field->isVisibleTo(message));
    // Qt greys a checkable group box's contents while it is unchecked, which is what
    // carries "not in force" now that nothing is hidden.
    QVERIFY(!field->isEnabled());

    message->setChecked(true);
    QVERIFY(field->isVisibleTo(message));
    QVERIFY(field->isEnabled());
}

// A format with no %t leaves the thread axis out of this pane entirely — it used to show
// it greyed with the reason in its title, which is a sentence read once in exchange for a
// section that stays for the session. It stays DISABLED as well as hidden, and that is
// the half that matters: a preset or a restored session can arrive with the axis ticked,
// at which point resolve() drops it, and nothing on screen may be able to tick it back.
void TestFilterPane::anAxisTheFormatLacksIsNotShownAtAll()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY(writeLog(file, "2026-07-21 12:00:00,000 INFO  net.socket - a\n"));
    QVERIFY2(doc.open(file.fileName(),
                      QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} %-5p %c - %m%n"),
                      Encoding::Utf8, QTimeZone::utc()),
             qPrintable(doc.lastError()));

    FilterPane pane;
    pane.show();
    QVERIFY(QTest::qWaitForWindowExposed(&pane));
    pane.setDocument(&doc);

    QGroupBox *thread = axis(pane, "threadGroup");
    QVERIFY(thread);
    QVERIFY(!thread->isVisible());
    QVERIFY(!thread->isEnabled());

    // The axes this format DOES carry are untouched by it, name included.
    QVERIFY(axis(pane, "subsystemGroup")->isVisible());
    QVERIFY(axis(pane, "timeGroup")->isVisible());
    QCOMPARE(axis(pane, "subsystemGroup")->title(), QStringLiteral("Subsystem"));

    // And rebinding to a log that has %t brings it back — the axis is hidden per
    // document, not switched off once for the pane.
    Document threaded;
    QTemporaryFile withThread;
    QVERIFY2(openLog(threaded, withThread, kTwoLoggers), qPrintable(threaded.lastError()));
    pane.setDocument(&threaded);
    QVERIFY(thread->isVisible());
    QVERIFY(thread->isEnabled());
}

// The "Add ... manually" row and its button are gone; the narrow field does both, and
// the "+" says which one it is about to do.
void TestFilterPane::typingAnUnlistedNameOffersToAddIt()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY2(openLog(doc, file, kTwoLoggers), qPrintable(doc.lastError()));
    FilterPane pane;
    pane.setDocument(&doc);

    auto *narrow = pane.findChild<QLineEdit *>(QStringLiteral("subsystemNarrow"));
    auto *add = pane.findChild<QAbstractButton *>(QStringLiteral("subsystemAdd"));
    QVERIFY(narrow && add);
    QVERIFY(!add->isVisibleTo(narrow->parentWidget())); // nothing typed, nothing to add

    // A name the file already has is a narrowing, not an addition.
    narrow->setText(QStringLiteral("net.socket"));
    QVERIFY(!add->isVisibleTo(narrow->parentWidget()));

    narrow->setText(QStringLiteral("net.http"));
    QVERIFY(add->isVisibleTo(narrow->parentWidget()));

    add->click();
    QListWidget *loggers = loggerList(pane, QStringLiteral("net.socket"));
    QVERIFY(loggers);
    QCOMPARE(stateOf(loggers, QStringLiteral("net.http")), Qt::Checked);
    QVERIFY(narrow->text().isEmpty()); // cleared, so the list un-narrows around it
}

// All / None / Invert act on the narrowed view, deliberately. The trap is that a
// hidden entry keeps its tick, so "None" over a narrowed list can leave the axis still
// letting records through while the list on screen reads as fully cleared.
void TestFilterPane::listButtonsOwnUpToWhatTheNarrowingHides()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY2(openLog(doc, file, kTwoLoggers), qPrintable(doc.lastError()));
    FilterPane pane;
    pane.setDocument(&doc);

    auto *narrow = pane.findChild<QLineEdit *>(QStringLiteral("subsystemNarrow"));
    auto *none = pane.findChild<QAbstractButton *>(QStringLiteral("subsystemNone"));
    QVERIFY(narrow && none);
    QVERIFY(!none->toolTip().contains(QStringLiteral("hidden")));

    narrow->setText(QStringLiteral("net")); // hides db.pool
    QVERIFY2(none->toolTip().contains(QStringLiteral("hidden")), qPrintable(none->toolTip()));

    // ...and the behaviour the tooltip is warning about is genuinely what happens.
    none->click();
    QListWidget *loggers = loggerList(pane, QStringLiteral("net.socket"));
    QVERIFY(loggers);
    QCOMPARE(stateOf(loggers, QStringLiteral("net.socket")), Qt::Unchecked);
    QCOMPARE(stateOf(loggers, QStringLiteral("db.pool")), Qt::Checked);

    narrow->clear();
    QVERIFY(!none->toolTip().contains(QStringLiteral("hidden")));
}

// One action back to an unfiltered view, from a state built out of every axis at once.
void TestFilterPane::clearAllReturnsToAnUnfilteredView()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY2(openLog(doc, file, kTwoLoggers), qPrintable(doc.lastError()));
    FilterPane pane;
    pane.setDocument(&doc);

    QListWidget *loggers = loggerList(pane, QStringLiteral("net.socket"));
    QVERIFY(loggers);
    setStateOf(loggers, QStringLiteral("db.pool"), Qt::Unchecked);
    axis(pane, "messageGroup")->setChecked(true);
    pane.findChild<QLineEdit *>(QStringLiteral("messageText"))->setText(QStringLiteral("a"));
    axis(pane, "timeGroup")->setChecked(true);
    contextSpin(pane, "contextBefore")->setValue(3);
    QVERIFY(pane.hasActiveFilters());

    QSignalSpy changed(&pane, &FilterPane::filtersChanged);
    pane.clearAll();

    // One notification for the whole reset, as a restore gives for its own.
    QCOMPARE(changed.count(), 1);
    QVERIFY(!pane.hasActiveFilters());
    QVERIFY(!doc.filters().anyActive());
    QCOMPARE(doc.contextBefore(), 0);
    QCOMPARE(doc.contextAfter(), 0);
    // Every value ticked again, and the axes back to the defaults a fresh pane has.
    QCOMPARE(stateOf(loggers, QStringLiteral("db.pool")), Qt::Checked);
    QVERIFY(!axis(pane, "messageGroup")->isChecked());
    QVERIFY(!axis(pane, "timeGroup")->isChecked());
    QVERIFY(!priorityEnable(pane)->isChecked());
    QVERIFY(axis(pane, "subsystemGroup")->isChecked());

    doc.applyFilters();
    QCOMPARE(doc.filtered().recordCount(), doc.index().records.size());
}

// The dock marker asks the RESOLVED FilterSet, not the ticks. Priority and subsystem
// ship ENABLED, so reading the checkboxes would put a marker on every file the moment
// it opened — which is exactly the no-op state NoOpAxes::Collapse exists to write away.
void TestFilterPane::activityTracksTheResolvedSetNotTheTicks()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY2(openLog(doc, file, kTwoLoggers), qPrintable(doc.lastError()));
    FilterPane pane;
    QSignalSpy activity(&pane, &FilterPane::activityChanged);
    pane.setDocument(&doc);

    QVERIFY(!priorityEnable(pane)->isChecked()); // off, so excluding nothing
    QVERIFY(axis(pane, "subsystemGroup")->isChecked()); // on, and excluding nothing
    QVERIFY(!pane.hasActiveFilters());

    QListWidget *loggers = loggerList(pane, QStringLiteral("net.socket"));
    QVERIFY(loggers);
    setStateOf(loggers, QStringLiteral("db.pool"), Qt::Unchecked);
    QVERIFY(pane.hasActiveFilters());
    QVERIFY(!activity.isEmpty());
    QCOMPARE(activity.last().first().toBool(), true);

    // Context alone counts: it changes what is shown, and it is a thing the user set.
    setStateOf(loggers, QStringLiteral("db.pool"), Qt::Checked);
    QVERIFY(!pane.hasActiveFilters());
    axis(pane, "messageGroup")->setChecked(true);
    pane.findChild<QLineEdit *>(QStringLiteral("messageText"))->setText(QStringLiteral("a"));
    contextSpin(pane, "contextBefore")->setValue(2);
    QVERIFY(pane.hasActiveFilters());
}

// An axis is a title row with a hairline, not a framed panel — in THIS pane as much as in
// the rule editor, since the axes are one shared widget and how an axis looks is not
// something the two panes may disagree about. Five framed panels stacked in one dock put
// five borders around things nothing else groups, and the dock is a frame already.
//
// Nothing here is optional any more: the flat look was a per-pane flag for exactly one
// milestone, and this is what notices if a frame ever comes back on one side only.
void TestFilterPane::theAxesAreLinesNotFrames()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file, kTwoLoggers), qPrintable(doc.lastError()));

    FilterPane pane;
    pane.setDocument(&doc);

    for (const char *name : {"priorityGroup", "messageGroup", "subsystemGroup",
                             "threadGroup", "timeGroup"}) {
        auto *box = pane.findChild<SectionBox *>(QString::fromLatin1(name));
        QVERIFY2(box, name);
        QVERIFY2(box->isFlat(), name);
        QVERIFY2(box->hasTitleDivider(), name);
        // A control, not a caption: centred and bold belongs to the rule editor's two
        // section headings and to nothing in this pane.
        QVERIFY2(!box->isHeading(), name);
        // And still a checkable group box, which is what keeps the title row the enable
        // control and lets Qt grey the body (switchedOffAxesStayVisibleAndGreyed).
        QVERIFY2(box->isCheckable(), name);
    }
}

void TestFilterPane::theAxisEnableControlsSitAtTheLeftEdge()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file, kTwoLoggers), qPrintable(doc.lastError()));

    FilterPane pane;
    pane.setDocument(&doc);
    pane.resize(320, 700);
    pane.show();
    QVERIFY(QTest::qWaitForWindowExposed(&pane));

    for (const char *name : {"priorityGroup", "messageGroup", "subsystemGroup",
                             "threadGroup", "timeGroup"}) {
        auto *box = pane.findChild<SectionBox *>(QString::fromLatin1(name));
        QVERIFY2(box, name);

        // Asked of the style, which is what actually decides this — the option is built
        // the way QGroupBox builds its own, minus the parts a rect query ignores.
        QStyleOptionGroupBox option;
        option.initFrom(box);
        option.text = box->title();
        option.textAlignment = box->alignment();
        option.subControls = QStyle::SC_GroupBoxFrame | QStyle::SC_GroupBoxLabel
                             | QStyle::SC_GroupBoxCheckBox;
        option.lineWidth = 1;
        const QRect indicator = box->style()->subControlRect(
            QStyle::CC_GroupBox, &option, QStyle::SC_GroupBoxCheckBox, box);
        QVERIFY2(indicator.isValid(), name);
        QVERIFY2(indicator.x() < box->width() / 4,
                 qPrintable(QStringLiteral("%1 enable control is at x=%2 of %3")
                                .arg(QString::fromLatin1(name))
                                .arg(indicator.x())
                                .arg(box->width())));

        // And the mechanism, because the check above cannot notice its loss under a style
        // that left-aligns anyway — which is every style the suite is likely to run on.
        QVERIFY2(box->styleSheet().contains(QStringLiteral("subcontrol-position")), name);
    }
}

// The context row shares the message axis's toggle row, so it is the one row in the
// pane whose contents are squeezed from both ends. It once laid itself out on top of
// itself: the spin boxes carried QSizePolicy::Ignored (borrowed from the time editors,
// where it exists to shrink a box BELOW its size hint) on top of a setMaximumWidth.
// The two pull opposite ways — Ignored tells the layout to hand over every spare pixel,
// the cap stops the widget using them, and QLayout centres the short widget in the long
// cell. Each spin box drifted right, out from under the glyph naming it and into the
// NEXT glyph, and the last box was pushed past the row's own right edge and clipped.
//
// Neither the eye nor any other test caught it, because every widget still existed,
// still had the right value and still answered isVisible(). Only the rectangles were
// wrong. So this asserts on the rectangles, at several dock widths — the widest is where
// the drift was largest, and the narrowest is where the row has no slack at all.
void TestFilterPane::theContextRowLaysOutWithoutOverlapOrClipping()
{
    FilterPane pane;
    for (int width : {289, 300, 340, 424, 700}) {
        pane.resize(width, 900);
        pane.show();
        QVERIFY(QTest::qWaitForWindowExposed(&pane));
        QApplication::processEvents();

        QWidget *row = pane.findChild<QWidget *>(QStringLiteral("contextRow"));
        QVERIFY2(row, "the context row is gone from the message axis");

        // Children in visual order, so a gap is a positive number and an overlap a
        // negative one no matter which order the layout added them in.
        QList<QWidget *> kids;
        for (QObject *o : row->children())
            if (auto *w = qobject_cast<QWidget *>(o))
                kids.append(w);
        QVERIFY(kids.size() >= 5); // caption, two glyphs, two spin boxes
        std::sort(kids.begin(), kids.end(), [](QWidget *a, QWidget *b) {
            return a->geometry().left() < b->geometry().left();
        });

        const QByteArray at = QByteArrayLiteral("at dock width ") + QByteArray::number(width);
        for (int i = 1; i < kids.size(); ++i) {
            const QRect prev = kids.at(i - 1)->geometry();
            const QRect here = kids.at(i)->geometry();
            QVERIFY2(here.left() > prev.right(),
                     (QByteArrayLiteral("context row children overlap ") + at).constData());
        }
        // And nothing hangs off either end of the row, which is what clipped the second
        // spin box: the row does not clip its children, the group box above it does.
        for (QWidget *w : kids) {
            QVERIFY2(w->geometry().left() >= 0,
                     (QByteArrayLiteral("a context control starts left of the row ") + at)
                         .constData());
            QVERIFY2(w->geometry().right() <= row->width(),
                     (QByteArrayLiteral("a context control runs past the row ") + at)
                         .constData());
        }

        // Both spin boxes are whole — a clipped one still reports a width, so the check
        // that matters is that it got at least the floor it asked for.
        for (const QString &name : {QStringLiteral("contextBefore"), QStringLiteral("contextAfter")}) {
            auto *spin = pane.findChild<QSpinBox *>(name);
            QVERIFY(spin);
            QVERIFY2(spin->width() >= spin->minimumWidth(),
                     (name.toUtf8() + " was squeezed below its floor " + at).constData());
            QVERIFY2(spin->width() <= spin->maximumWidth(),
                     (name.toUtf8() + " grew past its cap " + at).constData());
        }
    }
}

// The five axes top to bottom. Priority and Subsystem say which records are being read;
// the free-text search comes under them, because you pick the stream and then search it.
// Message text used to sit second, above both value lists.
//
// A document is bound because the thread and time axes are HIDDEN without one — the
// format-lacks-an-axis rule — and a hidden widget is skipped by the layout entirely,
// which makes an unbound pane the wrong place to ask any question about order or height.
void TestFilterPane::theAxesAreInReadingOrder()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY2(openLog(doc, file, kTwoLoggers), qPrintable(doc.lastError()));
    FilterPane pane;
    pane.setDocument(&doc);
    pane.resize(424, 900);
    pane.show();
    QVERIFY(QTest::qWaitForWindowExposed(&pane));
    QApplication::processEvents();

    const QStringList expected{QStringLiteral("priorityGroup"), QStringLiteral("subsystemGroup"),
                               QStringLiteral("messageGroup"), QStringLiteral("threadGroup"),
                               QStringLiteral("timeGroup")};
    QList<QGroupBox *> boxes;
    for (const QString &name : expected) {
        QGroupBox *g = pane.findChild<QGroupBox *>(name);
        QVERIFY2(g, name.toUtf8().constData());
        QVERIFY2(g->isVisible(), name.toUtf8().constData());
        boxes.append(g);
    }
    std::sort(boxes.begin(), boxes.end(), [&pane](QGroupBox *a, QGroupBox *b) {
        return a->mapTo(&pane, QPoint(0, 0)).y() < b->mapTo(&pane, QPoint(0, 0)).y();
    });
    QStringList actual;
    for (QGroupBox *g : boxes)
        actual << g->objectName();
    QCOMPARE(actual, expected);
}

// The two value lists are the only things in the pane that grow. A subsystem list is as
// long as the log has subsystems — unknown when the pane is built, and still growing
// while it scans — where every other axis is a fixed number of rows, so spare height
// belongs to them. It used to go to a trailing addStretch(), which left both lists at
// their floor with an empty gap under Time range however tall the dock was.
void TestFilterPane::theValueListsTakeTheSpareHeight()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY2(openLog(doc, file, kTwoLoggers), qPrintable(doc.lastError()));
    FilterPane pane;
    pane.setDocument(&doc);
    pane.show();
    QVERIFY(QTest::qWaitForWindowExposed(&pane));

    auto settle = [&pane](int h) {
        pane.resize(424, h);
        for (int i = 0; i < 8; ++i)
            QApplication::processEvents();
    };
    auto listHeight = [&pane](const char *name) {
        auto *l = pane.findChild<QListWidget *>(QString::fromLatin1(name));
        return l ? l->height() : -1;
    };
    auto boxHeight = [&pane](const char *name) {
        auto *g = pane.findChild<QGroupBox *>(QString::fromLatin1(name));
        return g ? g->height() : -1;
    };

    settle(700);
    const int sub0 = listHeight("subsystemList"), thr0 = listHeight("threadList");
    const int msg0 = boxHeight("messageGroup"), pri0 = boxHeight("priorityGroup");
    QVERIFY(sub0 > 0 && thr0 > 0);

    settle(1300);
    const int sub1 = listHeight("subsystemList"), thr1 = listHeight("threadList");

    // 600 px more pane means 600 px more list, split between the two.
    QVERIFY2(sub1 > sub0 + 200, qPrintable(QString("subsystem %1 -> %2").arg(sub0).arg(sub1)));
    QVERIFY2(thr1 > thr0 + 200, qPrintable(QString("thread %1 -> %2").arg(thr0).arg(thr1)));

    // Evenly, and by the SAME factor rather than in proportion to how many values each
    // list happens to hold — a split that would otherwise shift under the reader every
    // time the scan turned up another subsystem.
    QVERIFY2(qAbs((sub1 - sub0) - (thr1 - thr0)) <= 4,
             qPrintable(QString("uneven: subsystem +%1, thread +%2")
                            .arg(sub1 - sub0).arg(thr1 - thr0)));

    // And nothing else grew: the three fixed axes are the same height in a pane twice
    // as tall, which is what makes the growth the LISTS' rather than everyone's.
    QCOMPARE(boxHeight("messageGroup"), msg0);
    QCOMPARE(boxHeight("priorityGroup"), pri0);

    // A pane too short for both floors scrolls rather than starving either list.
    settle(250);
    auto *scroll = pane.findChild<QScrollArea *>(QStringLiteral("filterScroll"));
    QVERIFY(scroll);
    QVERIFY(scroll->verticalScrollBar()->isVisible());
    QVERIFY(listHeight("subsystemList") >= 70);
    QVERIFY(listHeight("threadList") >= 70);
}

QTEST_MAIN(TestFilterPane)
#include "tst_filterpane.moc"
