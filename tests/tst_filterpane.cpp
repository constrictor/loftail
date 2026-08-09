#include <QtTest>

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QGroupBox>
#include <QLineEdit>
#include <QListWidget>
#include <QSpinBox>
#include <QFile>
#include <QTemporaryFile>

#include "Document.h"
#include "FilterPane.h"
#include "Filter.h"
#include "LogFormat.h"
#include "Priority.h"
#include "RecordIndex.h"

using namespace loftail;

// The subsystem and priority axes are ENABLED by default (SPEC.md §6) so their
// controls act on the first click instead of silently doing nothing until a master
// checkbox is also ticked. That default is only safe if three things hold, and each
// of them is a way the change could have gone wrong:
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

    // The four group axes: each is a checkable QGroupBox whose title row IS the
    // enable control. Found by OBJECT NAME, never by the title it shows — a visible
    // string is a translator's to change (CLAUDE.md), and these names are the test
    // contract precisely because they are not.
    static QGroupBox *axis(FilterPane &pane, const char *name)
    {
        return pane.findChild<QGroupBox *>(QString::fromLatin1(name));
    }

    // Priority is the exception: one checkbox and one combo on a single row, with no
    // group box to be the enable control.
    static QCheckBox *priorityEnable(FilterPane &pane)
    {
        return pane.findChild<QCheckBox *>(QStringLiteral("priorityEnable"));
    }

    // "New" — the discovery rule as a control, under each value axis's
    // All / None / Invert column. Ticked, a value the scan turns up later arrives
    // ticked; unticked, the list is a restriction (MatchCriteria::loggerRestrictive).
    static QCheckBox *newValuesBox(FilterPane &pane, const char *name)
    {
        return pane.findChild<QCheckBox *>(QString::fromLatin1(name));
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
    void priorityComboFollowsItsCheckbox();
    void switchedOffAxesStayVisibleAndGreyed();
    void anAxisTheFormatLacksSaysSo();
    void typingAnUnlistedNameOffersToAddIt();
    void listButtonsOwnUpToWhatTheNarrowingHides();
    void clearAllReturnsToAnUnfilteredView();
    void activityTracksTheResolvedSetNotTheTicks();
};

void TestFilterPane::metadataAxesAreOnByDefault()
{
    FilterPane pane;
    QVERIFY(priorityEnable(pane)->isChecked());
    QVERIFY(axis(pane, "subsystemGroup")->isChecked());
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
    QCOMPARE(list->count(), 2);
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
    QCOMPARE(list->count(), 2);

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
    QCOMPARE(list->count(), 3);
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
    QCOMPARE(list->count(), 2);
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
    QCOMPARE(list->count(), 3);
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
// now that the rule is a checkbox in the pane: a box that unticks itself when the user
// ticks something ELSE is worse than no box, and the whole point of showing the rule is
// that the user can see which one is in force and say otherwise.
void TestFilterPane::aHandEditLeavesTheDiscoveryRuleAlone()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file, kTwoLoggers), qPrintable(doc.lastError()));

    FilterPane pane;
    pane.setDocument(&doc);
    pane.showOnlyValue(ValueAxis::Subsystem, QStringLiteral("db.pool"));
    QCheckBox *newValues = newValuesBox(pane, "subsystemNewValues");
    QVERIFY(newValues);
    QVERIFY(!newValues->isChecked()); // the restriction, stated where it can be seen

    QListWidget *list = loggerList(pane, QStringLiteral("db.pool"));
    QVERIFY(list);
    setStateOf(list, QStringLiteral("net.socket"), Qt::Checked);
    QVERIFY(!newValues->isChecked()); // ...and one more tick is not a retraction of it

    QVERIFY2(appendAndReindex(doc, file, "2026-07-21 12:00:02,000 [main] INFO  ui.window - c\n"),
             qPrintable(doc.lastError()));
    pane.refreshDiscoveredLists();
    list = loggerList(pane, QStringLiteral("ui.window"));
    QVERIFY(list);
    QCOMPARE(stateOf(list, QStringLiteral("ui.window")), Qt::Unchecked);

    // Ticking the box IS the retraction, and it applies to the next value to appear —
    // not retroactively to the one already listed and already unticked, which the user
    // can see and tick for themselves.
    newValues->setChecked(true);
    QVERIFY2(appendAndReindex(doc, file, "2026-07-21 12:00:03,000 [main] INFO  fs.cache - d\n"),
             qPrintable(doc.lastError()));
    pane.refreshDiscoveredLists();
    list = loggerList(pane, QStringLiteral("fs.cache"));
    QVERIFY(list);
    QCOMPARE(stateOf(list, QStringLiteral("fs.cache")), Qt::Checked);
}

// All / None / Invert carry the same answer to the values that have NOT turned up yet.
// "Everything" and "nothing" are claims about the axis, not about the handful of names
// that happen to be listed a third of the way through a scan — and the checkbox under
// the three buttons is where that claim shows.
void TestFilterPane::theListButtonsCarryTheDiscoveryRule()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file, kTwoLoggers), qPrintable(doc.lastError()));

    FilterPane pane;
    pane.setDocument(&doc);
    QCheckBox *newValues = newValuesBox(pane, "subsystemNewValues");
    auto *none = pane.findChild<QAbstractButton *>(QStringLiteral("subsystemNone"));
    auto *all = pane.findChild<QAbstractButton *>(QStringLiteral("subsystemAll"));
    auto *invert = pane.findChild<QAbstractButton *>(QStringLiteral("subsystemInvert"));
    QVERIFY(newValues && none && all && invert);

    QVERIFY(newValues->isChecked()); // discovery is the default (SPEC.md §6)
    none->click();
    QVERIFY(!newValues->isChecked());
    invert->click();
    QVERIFY(newValues->isChecked());
    invert->click();
    QVERIFY(!newValues->isChecked());
    all->click();
    QVERIFY(newValues->isChecked());

    // And "None" really does mean nothing, including what turns up next.
    none->click();
    QVERIFY2(appendAndReindex(doc, file, "2026-07-21 12:00:02,000 [main] INFO  ui.window - c\n"),
             qPrintable(doc.lastError()));
    pane.refreshDiscoveredLists();
    QListWidget *list = loggerList(pane, QStringLiteral("ui.window"));
    QVERIFY(list);
    QCOMPARE(stateOf(list, QStringLiteral("ui.window")), Qt::Unchecked);
    doc.applyFilters();
    QCOMPARE(doc.filtered().recordCount(), 0);
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

// Priority is the one axis with no QGroupBox, so Qt was not greying its body for it:
// with the box unticked the combo stayed bright and spinnable and did nothing. The
// pane also collapses a switched-off axis now, so the combo is hidden as well —
// but the enabled state is what makes it right in a pane that does not collapse.
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
    QVERIFY(priorityEnable(pane)->isChecked());
    QVERIFY(combo->isEnabled());

    priorityEnable(pane)->setChecked(false);
    QVERIFY(!combo->isEnabled());

    priorityEnable(pane)->setChecked(true);
    QVERIFY(combo->isEnabled());
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

// A format with no %t greys the thread axis — but a preset or a restored session can
// leave it greyed AND TICKED, at which point resolve() drops it and the pane shows a
// selection that is not in force. The title carries the reason, because Qt delivers no
// mouse events to a disabled widget and so never shows its tooltip.
void TestFilterPane::anAxisTheFormatLacksSaysSo()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY(writeLog(file, "2026-07-21 12:00:00,000 INFO  net.socket - a\n"));
    QVERIFY2(doc.open(file.fileName(),
                      QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} %-5p %c - %m%n"),
                      Encoding::Utf8, QTimeZone::utc()),
             qPrintable(doc.lastError()));

    FilterPane pane;
    pane.setDocument(&doc);
    QGroupBox *thread = axis(pane, "threadGroup");
    QVERIFY(thread);
    QVERIFY(!thread->isEnabled());
    // Not asserting the wording — only that it stopped being the bare axis name, so
    // the greyed box is no longer silent about why.
    QVERIFY(thread->title() != QStringLiteral("Thread"));
    QVERIFY(thread->title().contains(QStringLiteral("Thread")));

    // The axis that IS in the format keeps its plain name.
    QCOMPARE(axis(pane, "subsystemGroup")->title(), QStringLiteral("Subsystem"));
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
    QVERIFY(priorityEnable(pane)->isChecked());
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

    QVERIFY(priorityEnable(pane)->isChecked()); // on, and excluding nothing
    QVERIFY(axis(pane, "subsystemGroup")->isChecked());
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

QTEST_MAIN(TestFilterPane)
#include "tst_filterpane.moc"
