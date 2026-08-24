#include <QtTest>

#include <QApplication>
#include <QCheckBox>
#include <QFile>
#include <QFontDatabase>
#include <QImage>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTemporaryFile>

#include "Document.h"
#include "Highlight.h"
#include "Palette.h"
#include "Priority.h"
#include "DocumentContext.h"
#include "DocumentView.h"
#include "LogFormat.h"
#include "LogFileStore.h"
#include "LogSettingsStore.h"
#include "MainWindow.h"
#include "RunPane.h"
#include "UiColors.h"

using namespace loftail;

// The Runs pane lists the runs it detected (SPEC.md §3a). A LIST and not a drop-down:
// the pane's whole job is to show what the run-start pattern found, and a combo shows
// exactly one of those at a time — the user has to open it to learn whether the pattern
// split the file the way they meant. The list is also the one thing in the pane that
// GROWS, for the same reason the value lists are in the Filters pane: everything else
// here is a fixed number of rows, and how many runs a log holds is unknown when the
// pane is built and changes while it scans.
//
// Runs under an offscreen QApplication; RunPane is a QWidget.
class TestRunPane : public QObject
{
    Q_OBJECT

private:
    static constexpr auto kPattern = "%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n";

    // Three runs plus a leading preamble record, so the list has six rows counting
    // the two mode rows, and no run's row coincides with either of those.
    static constexpr auto kLog =
        "2026-01-01 10:00:00,000 [main] INFO  boot - preamble line\n"
        "2026-01-01 10:00:01,000 [main] INFO  boot - RUN START one\n"
        "2026-01-01 10:00:02,000 [main] INFO  app - working\n"
        "2026-01-01 10:00:03,000 [main] INFO  boot - RUN START two\n"
        "2026-01-01 10:00:04,000 [main] INFO  app - working\n"
        "2026-01-01 10:00:05,000 [main] INFO  boot - RUN START three\n"
        "2026-01-01 10:00:06,000 [main] INFO  app - working\n";

    // The same three runs, with something outstanding in two of them and nothing in the
    // third: run 1 has an ERROR and two WARNs, run 2 a FATAL, run 3 nothing above INFO.
    static constexpr auto kNoisyLog =
        "2026-01-01 10:00:00,000 [main] INFO  boot - preamble line\n"
        "2026-01-01 10:00:01,000 [main] INFO  boot - RUN START one\n"
        "2026-01-01 10:00:02,000 [main] WARN  app - careful\n"
        "2026-01-01 10:00:03,000 [main] ERROR app - broken\n"
        "2026-01-01 10:00:04,000 [main] WARN  app - careful again\n"
        "2026-01-01 10:00:05,000 [main] INFO  boot - RUN START two\n"
        "2026-01-01 10:00:06,000 [main] FATAL app - gone\n"
        "2026-01-01 10:00:07,000 [main] INFO  boot - RUN START three\n"
        "2026-01-01 10:00:08,000 [main] INFO  app - working\n";

    static bool openLog(Document &doc, QTemporaryFile &file, const char *text = kLog)
    {
        if (!file.open())
            return false;
        file.write(text);
        file.flush();
        if (!doc.open(file.fileName(), QString::fromLatin1(kPattern),
                      Encoding::Utf8, QTimeZone::utc()))
            return false;
        doc.setRunStart(QStringLiteral("RUN START"), false, Qt::CaseInsensitive);
        return doc.runs().size() == 4; // preamble + three
    }

    // By object name, never by the text a row shows: a run label is built from the
    // log's own first line and "All runs" is translated prose (CLAUDE.md §9.1).
    static QListWidget *runList(RunPane &pane)
    {
        return pane.findChild<QListWidget *>(QStringLiteral("runList"));
    }

    static QLabel *label(RunPane &pane, const char *name)
    {
        return pane.findChild<QLabel *>(QString::fromLatin1(name));
    }

    // The three cases below are WINDOW-level, and have to be: what they claim is that
    // ticking a box reaches nothing, and the only place an emission would have an effect
    // is where it is connected to MainWindow::onRunStartChanged() — which re-splits the
    // log, persists the pattern into the settings tree and retargets the run selection.
    // A pane on its own can only show that no signal left it.
    QTemporaryDir m_dir;

    // A real file carrying the same log the pane cases use, for MainWindow to open.
    bool writeLog(const QString &path) const
    {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return false;
        f.write(kLog);
        f.close();
        return true;
    }

    static QTabWidget *tabs(const MainWindow &w)
    {
        return w.findChild<QTabWidget *>(QStringLiteral("documentTabs"));
    }

    static Document *documentOf(const MainWindow &w)
    {
        auto *view = qobject_cast<DocumentView *>(tabs(w)->widget(0));
        DocumentContext *ctx = view ? view->context() : nullptr;
        return ctx ? ctx->doc.get() : nullptr;
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

    // What this log's own stored record says its run-start pattern is — read back through
    // a second store over the same directory, so it is the file on disk being asked and
    // not the copy the window is holding. Empty when the log has no record at all, which
    // is what "Apply has not been pressed" looks like.
    static QString storedRunStart(const QString &path)
    {
        LogFileStore store(LogFileStore::defaultDir());
        store.load();
        const LogFileSettings s = store.read(path);
        return s.profile ? s.profile->format.runStartPattern : QString();
    }

    // Every box is reached by object name and clicked as a user would: the labels are
    // translated prose, and setChecked() would drive the connections without the widget.
    static void tick(QCheckBox *box)
    {
        QTest::mouseClick(box, Qt::LeftButton, Qt::KeyboardModifiers(), box->rect().center());
    }

private slots:
    void theRunsAreAListWithTheSelectedRunOnIt();
    void theFollowEntryIsLastAndIsNotARun();
    void aLogWithNoRunsStillOffersLastRun();
    void theRunListTakesTheSpareHeight();
    void arrowingThroughTheListSelectsARun();
    void repopulatingTheListIsNotAChoice();
    void thePaneSaysThatThePatternWaitsForApply();
    void aTypedPatternMakesTheNoteAskForApply();
    void tickingABoxOnlyAsksForApply();
    void tickingABoxNeitherResplitsTheLogNorPersistsThePattern();
    void tickingABoxLeavesAPinnedRunPinned();
    void withNoLogOpenTheListStillExplainsItself();

    // A run row is three lines — its name, the span it covers, what is outstanding in
    // it (SPEC.md §3a) — and none of that is reachable through the item's own text, so
    // the parts are asserted where the delegate reads them and the chips are read back
    // off rendered pixels, which is the only place a colour exists at all.
    void aRunRowCarriesItsNameItsSpanAndItsCounts();
    void theTwoModeRowsSayWhatTheyWillShow();
    void aRunRowIsGivenThreeLinesOfHeight();
    void theCountsAreDrawnInTheDefaultLevelColours();
    void theRowsAlternateInABandThatIsVisibleOnThisTheme();
};

void TestRunPane::theRunsAreAListWithTheSelectedRunOnIt()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY2(openLog(doc, file), qPrintable(doc.lastError()));

    RunPane pane;
    pane.setDocument(&doc);

    QListWidget *list = runList(pane);
    QVERIFY(list);
    QCOMPARE(list->count(), 6);          // "All runs" + preamble + three + "Follow the last"
    // The newest run is what is on screen, but what is SELECTED is the standing
    // instruction that put it there — the pane reads the mode off the document rather
    // than inferring it from the selection being the last ordinal.
    QCOMPARE(list->currentRow(), pane.followRow());
    QVERIFY(doc.followingLastRun());
    QCOMPARE(doc.selectedRun(), 3);

    // Every row is readable at a dock's width without widening the pane past it: the
    // labels elide and the full text is on the tooltip, rather than the view growing a
    // horizontal scrollbar and pushing the pane's minimum width out.
    QCOMPARE(list->horizontalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);
    QCOMPARE(list->textElideMode(), Qt::ElideRight);
    QVERIFY(!list->item(5)->toolTip().isEmpty());
}

void TestRunPane::theFollowEntryIsLastAndIsNotARun()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY2(openLog(doc, file), qPrintable(doc.lastError()));

    RunPane pane;
    pane.setDocument(&doc);
    QListWidget *list = runList(pane);
    QVERIFY(list);
    QSignalSpy spy(&pane, &RunPane::runSelected);

    // The list reads in file order and the standing instruction sits at the BOTTOM,
    // beside the newest run it currently resolves to — so the two ends of the list are
    // the two rows that are not runs, "All runs" at the top and this one under them.
    QCOMPARE(RunPane::kAllRunsRow, 0);
    QCOMPARE(pane.followRow(), list->count() - 1);
    QVERIFY(pane.followRow() > RunPane::kFirstRunRow + 3); // ...below every run row

    // It carries a sentinel and not the ordinal it currently resolves to: the whole
    // point of the entry is that the ordinal changes underneath it. Selecting the run
    // that happens to be last today is therefore a DIFFERENT gesture, and the two rows
    // are now adjacent, which is exactly why they must not answer alike.
    // (The pane only reports; MainWindow is what turns these into Document calls.)
    list->setCurrentRow(RunPane::kFirstRunRow + 3); // pin the run that is last today
    QCOMPARE(spy.size(), 1);
    QCOMPARE(spy.takeFirst().at(0).toInt(), 3);

    list->setCurrentRow(pane.followRow());
    QCOMPARE(spy.size(), 1);
    QCOMPARE(spy.takeFirst().at(0).toInt(), RunPane::kLastRun);

    // ...and a pinned run puts the current row back on that run, never on the follow
    // row — the two are indistinguishable by selectedRun() alone, which is why the pane
    // asks the document for the mode instead.
    doc.selectRun(3);
    pane.refresh();
    QCOMPARE(list->currentRow(), RunPane::kFirstRunRow + 3);
    doc.selectLastRun();
    pane.refresh();
    QCOMPARE(list->currentRow(), pane.followRow());
    QCOMPARE(spy.size(), 0); // neither repopulation was a choice
}

void TestRunPane::aLogWithNoRunsStillOffersLastRun()
{
    // No run-start pattern: nothing is detected, so the two mode rows are the only rows
    // and both mean the whole file. The entry is offered all the same — it is the pane's
    // default, and a pattern typed a moment later fills it in. This is also the one
    // layout where the follow row is row 1 rather than "somewhere past the runs", which
    // is why the pane tests it FIRST when a row change comes in: read as a run ordinal
    // it would select run 0 of a log that has none.
    Document doc;
    QTemporaryFile file;
    QVERIFY(file.open());
    file.write(kLog);
    file.flush();
    QVERIFY2(doc.open(file.fileName(), QString::fromLatin1(kPattern),
                      Encoding::Utf8, QTimeZone::utc()),
             qPrintable(doc.lastError()));

    RunPane pane;
    pane.setDocument(&doc);
    QListWidget *list = runList(pane);
    QVERIFY(list);
    QCOMPARE(list->count(), 2);
    QCOMPARE(list->currentRow(), pane.followRow());
    QVERIFY(doc.followingLastRun());
    QCOMPARE(doc.selectedRun(), -1);
    QVERIFY(!doc.viewRestricted()); // ...which is the whole file, as it always was
}

void TestRunPane::theRunListTakesTheSpareHeight()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY2(openLog(doc, file), qPrintable(doc.lastError()));

    RunPane pane;
    pane.setDocument(&doc);
    pane.show();
    QVERIFY(QTest::qWaitForWindowExposed(&pane));

    auto settle = [&pane](int h) {
        pane.resize(360, h);
        for (int i = 0; i < 8; ++i)
            QApplication::processEvents();
    };

    settle(400);
    QListWidget *list = runList(pane);
    QVERIFY(list);
    const int list0 = list->height();
    // The run-start editor is a fixed number of rows and must stay one.
    const auto boxes = pane.findChildren<QGroupBox *>();
    QCOMPARE(boxes.size(), 2);
    const int editor0 = boxes.at(0)->height();
    QVERIFY(list0 > 0);

    settle(800);
    const int list1 = list->height();

    // 400 px more pane is 400 px more list, and none of it anyone else's — which is
    // what a trailing addStretch(1) under the run box would silently undo.
    QVERIFY2(list1 >= list0 + 380,
             qPrintable(QString("list %1 -> %2").arg(list0).arg(list1)));
    QCOMPARE(boxes.at(0)->height(), editor0);
}

void TestRunPane::arrowingThroughTheListSelectsARun()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY2(openLog(doc, file), qPrintable(doc.lastError()));

    RunPane pane;
    pane.setDocument(&doc);
    pane.show();
    QVERIFY(QTest::qWaitForWindowExposed(&pane));

    QListWidget *list = runList(pane);
    QVERIFY(list);
    QSignalSpy spy(&pane, &RunPane::runSelected);

    // A list is walked with the arrow keys as much as it is clicked, so the selection
    // travels on currentRowChanged rather than on a click signal.
    list->setCurrentRow(RunPane::kFirstRunRow + 3); // the newest run, pinned
    QCOMPARE(spy.size(), 1);
    spy.clear();
    list->setFocus();
    QTest::keyClick(list, Qt::Key_Up); // one row up == the run before it
    QCOMPARE(spy.size(), 1);
    QCOMPARE(spy.takeFirst().at(0).toInt(), 2);

    // Row 1 is "All runs", which is run index -1 and not a run.
    list->setCurrentRow(RunPane::kAllRunsRow);
    QCOMPARE(spy.size(), 1);
    QCOMPARE(spy.takeFirst().at(0).toInt(), RunPane::kAllRuns);
}

void TestRunPane::repopulatingTheListIsNotAChoice()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY2(openLog(doc, file), qPrintable(doc.lastError()));

    RunPane pane;
    pane.setDocument(&doc);
    QListWidget *list = runList(pane);
    QVERIFY(list);
    QSignalSpy spy(&pane, &RunPane::runSelected);

    // refresh() runs on every live append, and it rebuilds the list — which clears it
    // (current row -1) and then sets the row back. Neither is the user choosing a run:
    // reporting one would yank the view to another run while the log was merely growing.
    pane.refresh();
    QCOMPARE(spy.size(), 0);
    QCOMPARE(list->currentRow(), pane.followRow());

    pane.setDocument(nullptr);
    QCOMPARE(spy.size(), 0);
}

void TestRunPane::thePaneSaysThatThePatternWaitsForApply()
{
    // This is the one pane whose edits are not live — the Filters and Highlighters panes
    // act as the user types — so the difference is stated next to the button that is it,
    // rather than left for the reader to discover by pressing nothing and waiting.
    Document doc;
    QTemporaryFile file;
    QVERIFY2(openLog(doc, file), qPrintable(doc.lastError()));

    RunPane pane;
    pane.setDocument(&doc);

    QLabel *note = label(pane, "runApplyNote");
    QVERIFY(note);
    QVERIFY(!note->text().isEmpty());
    // Muted from the palette, so it reads on a dark theme as well as a light one, and
    // reads as an aside rather than as something that has gone wrong.
    QCOMPARE(note->styleSheet(),
             QStringLiteral("color: %1;").arg(mutedColor(pane.palette()).name()));
}

void TestRunPane::aTypedPatternMakesTheNoteAskForApply()
{
    // A note that always says the same thing is read once and never again, so it is
    // quiet until the field disagrees with the pattern actually in force — which is the
    // only moment the pane owes the user an instruction.
    Document doc;
    QTemporaryFile file;
    QVERIFY2(openLog(doc, file), qPrintable(doc.lastError()));

    RunPane pane;
    pane.setDocument(&doc);

    QLabel *note = label(pane, "runApplyNote");
    auto *edit = pane.findChild<QLineEdit *>(QStringLiteral("runStartPattern"));
    QVERIFY(note && edit);
    const QString quiet = note->text();

    edit->setText(QStringLiteral("RUN START o"));
    const QString asking = note->text();
    QVERIFY(asking != quiet);
    QCOMPARE(note->styleSheet(),
             QStringLiteral("color: %1;").arg(warningColor(pane.palette()).name()));

    // Applying is MainWindow's: it reconfigures the document and refreshes the pane.
    // The note goes quiet because the field and the matcher agree again, never because
    // something remembered that a button was pressed.
    doc.setRunStart(edit->text(), false, Qt::CaseInsensitive);
    pane.refresh();
    QCOMPARE(note->text(), quiet);

    // ...and it is measured against the matcher, so a pattern that changes from under
    // the pane — a rebind, a session restore — asks again with nothing typed.
    doc.setRunStart(QStringLiteral("RUN START"), false, Qt::CaseInsensitive);
    pane.refresh();
    QCOMPARE(note->text(), asking);
    // ...and the rebuild did not clobber what was typed, which is why it can disagree.
    QCOMPARE(edit->text(), QStringLiteral("RUN START o"));
}

void TestRunPane::tickingABoxOnlyAsksForApply()
{
    // Apply and Return are the only route out of this pane. Both boxes were once wired
    // to emitPattern as well, so ticking Regex applied whatever was standing in the
    // field — which is the one thing this pane exists to make deliberate (SPEC.md §3a).
    Document doc;
    QTemporaryFile file;
    QVERIFY2(openLog(doc, file), qPrintable(doc.lastError()));

    RunPane pane;
    pane.setDocument(&doc);
    pane.show();
    QVERIFY(QTest::qWaitForWindowExposed(&pane));

    auto *edit = pane.findChild<QLineEdit *>(QStringLiteral("runStartPattern"));
    auto *regex = pane.findChild<QCheckBox *>(QStringLiteral("runStartRegex"));
    auto *caseBox = pane.findChild<QCheckBox *>(QStringLiteral("runStartCase"));
    QLabel *note = label(pane, "runApplyNote");
    QVERIFY(edit && regex && caseBox && note);

    edit->setText(QStringLiteral("RUN START o")); // half typed, never applied
    const QString asking = note->text();
    QSignalSpy spy(&pane, &RunPane::runStartChanged);

    tick(regex);
    QVERIFY(regex->isChecked()); // the box itself moves...
    QCOMPARE(spy.size(), 0);     // ...and reports to nobody
    tick(caseBox);
    QVERIFY(caseBox->isChecked());
    QCOMPARE(spy.size(), 0);

    // The note is the whole of what a tick does, so it must still be asking: a box that
    // changes nothing and says nothing would be a control that had stopped working.
    QCOMPARE(note->text(), asking);
    QCOMPARE(note->styleSheet(),
             QStringLiteral("color: %1;").arg(warningColor(pane.palette()).name()));

    // Apply is what emits, and it carries the boxes as they now stand.
    auto *apply = pane.findChild<QPushButton *>(QStringLiteral("runApply"));
    QVERIFY(apply);
    apply->click();
    QCOMPARE(spy.size(), 1);
    const QList<QVariant> args = spy.takeFirst();
    QCOMPARE(args.at(0).toString(), QStringLiteral("RUN START o"));
    QVERIFY(args.at(1).toBool());
    QVERIFY(args.at(2).toBool());
}

void TestRunPane::tickingABoxNeitherResplitsTheLogNorPersistsThePattern()
{
    // The damage the pane's Apply gate exists to prevent, driven through a real window:
    // an unapplied pattern re-read over the whole log, and written into its settings
    // node so that it outlives the session that never asked for it.
    const QString path = m_dir.filePath(QStringLiteral("unapplied.log"));
    QVERIFY(writeLog(path));

    MainWindow w;
    w.resize(1000, 700);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    QVERIFY(w.openFile(path));
    waitUntilIndexed(w);

    Document *doc = documentOf(w);
    auto *edit = w.findChild<QLineEdit *>(QStringLiteral("runStartPattern"));
    auto *regex = w.findChild<QCheckBox *>(QStringLiteral("runStartRegex"));
    auto *apply = w.findChild<QPushButton *>(QStringLiteral("runApply"));
    auto *note = w.findChild<QLabel *>(QStringLiteral("runApplyNote"));
    QVERIFY(doc && edit && regex && apply && note);
    QCOMPARE(doc->runs().size(), 0);
    QVERIFY(storedRunStart(path).isEmpty());

    edit->setText(QStringLiteral("RUN START")); // typed, and NOT applied
    const QString asking = note->text();

    tick(regex);
    QVERIFY(regex->isChecked());
    QCOMPARE(doc->runs().size(), 0);              // the log was not re-read...
    QVERIFY(storedRunStart(path).isEmpty());      // ...and nothing was written for it
    QCOMPARE(note->text(), asking);               // ...and the pane still asks

    // Pressing Apply is what does both, so neither is being reported missing because
    // the wiring below the pane went with the connections.
    apply->click();
    QCOMPARE(doc->runs().size(), 4);              // preamble + three
    QCOMPARE(storedRunStart(path), QStringLiteral("RUN START"));
    QVERIFY(note->text() != asking);
}

void TestRunPane::tickingABoxLeavesAPinnedRunPinned()
{
    // A pinned run moves only when the user moves it (SPEC.md §3a). Applying a pattern
    // re-detects and defaults back to the newest run, so a tick that applied one threw
    // away the pin as well — silently, and for a gesture about something else.
    const QString path = m_dir.filePath(QStringLiteral("pinned.log"));
    QVERIFY(writeLog(path));

    MainWindow w;
    w.resize(1000, 700);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    QVERIFY(w.openFile(path));
    waitUntilIndexed(w);

    auto *edit = w.findChild<QLineEdit *>(QStringLiteral("runStartPattern"));
    auto *caseBox = w.findChild<QCheckBox *>(QStringLiteral("runStartCase"));
    auto *list = w.findChild<QListWidget *>(QStringLiteral("runList"));
    QVERIFY(edit && caseBox && list);

    edit->setText(QStringLiteral("RUN START"));
    QTest::keyClick(edit, Qt::Key_Return); // Return applies, exactly as Apply does

    Document *doc = documentOf(w);
    QVERIFY(doc);
    QCOMPARE(doc->runs().size(), 4);
    list->setCurrentRow(RunPane::kFirstRunRow + 1); // pin run #1, which is not the last
    QVERIFY(!doc->followingLastRun());
    QCOMPARE(doc->selectedRun(), 1);

    edit->setText(QStringLiteral("RUN START o")); // an edit the user has not applied
    tick(caseBox);

    QVERIFY(caseBox->isChecked());
    QVERIFY(!doc->followingLastRun()); // the pin is still the user's
    QCOMPARE(doc->selectedRun(), 1);
    QCOMPARE(doc->runs().size(), 4);
    QCOMPARE(list->currentRow(), RunPane::kFirstRunRow + 1);
}

void TestRunPane::withNoLogOpenTheListStillExplainsItself()
{
    // The list keeps its two italic mode rows with no document bound, so it reads as
    // something to act on; the status line under the pattern field is what says there is
    // nothing behind them, and it must not be the one thing that falls silent here.
    RunPane pane;
    pane.setDocument(nullptr);

    QLabel *info = label(pane, "runInfo");
    QVERIFY(info);
    QVERIFY(!info->text().isEmpty());
    QCOMPARE(runList(pane)->count(), 2);
}

// The pane's own reading of a run: what the three lines are built from. Asserted at the
// item-data seam rather than on a label, because there is no label — the delegate
// composes the row from these and the DisplayRole string is a one-line fallback for
// type-ahead and accessibility.
void TestRunPane::aRunRowCarriesItsNameItsSpanAndItsCounts()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY2(openLog(doc, file, kNoisyLog), qPrintable(doc.lastError()));

    RunPane pane;
    pane.setDocument(&doc);
    QListWidget *list = runList(pane);
    QVERIFY(list);
    QCOMPARE(list->count(), 6); // "All runs" + preamble + three + "Follow the last"

    // The preamble is named for what it is; every other row is named by its ORDINAL,
    // which is what runSelected() carries and what the session stores.
    QCOMPARE(list->item(RunPane::kFirstRunRow + 1)->data(RunPane::kRunTitleRole).toString(),
             QStringLiteral("Run 1"));

    QListWidgetItem *run1 = list->item(RunPane::kFirstRunRow + 1);
    QCOMPARE(run1->data(RunPane::kRunErrorRole).toInt(), 1);
    QCOMPARE(run1->data(RunPane::kRunWarnRole).toInt(), 2);
    QCOMPARE(run1->data(RunPane::kRunFatalRole).toInt(), 0);

    QListWidgetItem *run2 = list->item(RunPane::kFirstRunRow + 2);
    QCOMPARE(run2->data(RunPane::kRunFatalRole).toInt(), 1);
    QCOMPARE(run2->data(RunPane::kRunErrorRole).toInt(), 0);

    // Nothing outstanding is zeros and not an absent role: the delegate distinguishes a
    // run row from a mode row by the TITLE role, so a run with a quiet log is still a
    // three-line row.
    QListWidgetItem *run3 = list->item(RunPane::kFirstRunRow + 3);
    QVERIFY(run3->data(RunPane::kRunTitleRole).isValid());
    QCOMPARE(run3->data(RunPane::kRunFatalRole).toInt(), 0);
    QCOMPARE(run3->data(RunPane::kRunErrorRole).toInt(), 0);
    QCOMPARE(run3->data(RunPane::kRunWarnRole).toInt(), 0);

    // The span is the run's own first and last instant, rendered in the DISPLAY zone
    // (invariant #10 — the stored value is UTC ms and the digits are a rendering of it).
    // Run 1 spans 10:00:01 to 10:00:04; the second stamp drops its date, which is the
    // same date, and that is what keeps the line inside a dock's width.
    const QString times = run1->data(RunPane::kRunTimesRole).toString();
    QVERIFY2(times.contains(QStringLiteral("2026-01-01 10:00:01")), qPrintable(times));
    QVERIFY2(times.endsWith(QStringLiteral("10:00:04")), qPrintable(times));
    QVERIFY2(!times.contains(QStringLiteral("2026-01-01 10:00:04")), qPrintable(times));

    // Whatever the row cannot fit is on the tooltip, counts included.
    QVERIFY(run1->toolTip().contains(QStringLiteral("1 ERROR")));
    QVERIFY(run1->toolTip().contains(QStringLiteral("2 WARN")));
}

void TestRunPane::theTwoModeRowsSayWhatTheyWillShow()
{
    // "All runs" and "Follow the last" resolve to a stretch of this log, so a reader picking
    // one is asking exactly what they ask of a run: what does it cover, and is there
    // anything wrong in it. They carry the same three lines, and what each one reports
    // is what it will actually SHOW — the whole file for "All runs", whichever run is
    // last for "Follow the last" — the difference between the two, and the whole reason
    // both rows exist. They stay ITALIC, because saying what a row will show does not
    // make it a run: a log may well start a run with a line that reads like either name.
    Document doc;
    QTemporaryFile file;
    QVERIFY2(openLog(doc, file, kNoisyLog), qPrintable(doc.lastError()));

    RunPane pane;
    pane.setDocument(&doc);
    QListWidget *list = runList(pane);
    QVERIFY(list);

    QListWidgetItem *lastRun = list->item(pane.followRow());
    QListWidgetItem *allRuns = list->item(RunPane::kAllRunsRow);
    for (QListWidgetItem *item : { lastRun, allRuns }) {
        QVERIFY(item->data(RunPane::kRunTitleRole).isValid());
        QVERIFY(!item->data(RunPane::kRunTimesRole).toString().isEmpty());
        QVERIFY(item->font().italic());
        QVERIFY(!item->text().isEmpty());
    }

    // "All runs" is the fold over the file: the ERROR and the two WARNs of run 1 and the
    // FATAL of run 2, which is also the sum of the run rows below it — one fold behind
    // both, so the row cannot claim a total the rows under it do not add up to.
    QCOMPARE(allRuns->data(RunPane::kRunFatalRole).toInt(), 1);
    QCOMPARE(allRuns->data(RunPane::kRunErrorRole).toInt(), 1);
    QCOMPARE(allRuns->data(RunPane::kRunWarnRole).toInt(), 2);
    const QString allTimes = allRuns->data(RunPane::kRunTimesRole).toString();
    QVERIFY2(allTimes.contains(QStringLiteral("2026-01-01 10:00:00")), qPrintable(allTimes));
    QVERIFY2(allTimes.endsWith(QStringLiteral("10:00:08")), qPrintable(allTimes));

    // "Follow the last" is run 3, which has nothing above INFO — so the two rows say DIFFERENT
    // things, which is what makes this a test of the mapping and not of "some numbers".
    QCOMPARE(lastRun->data(RunPane::kRunFatalRole).toInt(), 0);
    QCOMPARE(lastRun->data(RunPane::kRunErrorRole).toInt(), 0);
    QCOMPARE(lastRun->data(RunPane::kRunWarnRole).toInt(), 0);
    QCOMPARE(lastRun->data(RunPane::kRunTimesRole).toString(),
             list->item(RunPane::kFirstRunRow + 3)->data(RunPane::kRunTimesRole).toString());

    // ...and it MOVES with the mode: pinning a run does not change what the follow row says,
    // but a new run arriving does. Selecting run 1 leaves the row reporting run 3.
    doc.selectRun(1);
    pane.refresh();
    QCOMPARE(list->item(pane.followRow())->data(RunPane::kRunWarnRole).toInt(), 0);

    // With NO document there is nothing to report, so both rows fall back to the
    // one-line italic entries the pane opens with — a span and a count made of nothing
    // is worse than neither, and the absence of the title role is exactly how the
    // delegate leaves such a row to the base class.
    pane.setDocument(nullptr);
    for (int row : { pane.followRow(), RunPane::kAllRunsRow }) {
        QVERIFY(!list->item(row)->data(RunPane::kRunTitleRole).isValid());
        QVERIFY(!list->item(row)->data(RunPane::kRunTimesRole).isValid());
        QVERIFY(list->item(row)->font().italic());
        QVERIFY(!list->item(row)->text().isEmpty());
        QVERIFY(!list->item(row)->toolTip().isEmpty());
    }
}

void TestRunPane::aRunRowIsGivenThreeLinesOfHeight()
{
    // A height claim, so it needs a resolved font: Windows offscreen has no font
    // database at all and every advance there comes back 0 (CLAUDE.md).
    if (QFontDatabase::families().isEmpty())
        QSKIP("no fonts resolvable on this platform");

    Document doc;
    QTemporaryFile file;
    QVERIFY2(openLog(doc, file, kNoisyLog), qPrintable(doc.lastError()));

    RunPane pane;
    pane.setDocument(&doc);
    QListWidget *list = runList(pane);
    QVERIFY(list);
    list->resize(300, 420);

    // NOT uniform, and still not: Qt measures the FIRST item under that flag, and with no
    // document bound the first item is a one-line mode row — so a pane that is bound
    // after it is shown would give every run row a third of the height it needs.
    QVERIFY(!list->uniformItemSizes());

    const int oneLine = QFontMetrics(list->font()).height();
    const int modeRow = list->visualItemRect(list->item(pane.followRow())).height();
    const int runRow = list->visualItemRect(list->item(RunPane::kFirstRunRow + 1)).height();
    QVERIFY2(runRow >= 3 * oneLine,
             qPrintable(QStringLiteral("run row %1 px, one line %2 px").arg(runRow).arg(oneLine)));
    // The mode rows are three lines too now that they report what they will show, and
    // they are the same three lines — a list whose rows were a different height for the
    // same content would read as two lists.
    QCOMPARE(modeRow, runRow);

    // With no document they go back to one line, which is what the flag above is about.
    pane.setDocument(nullptr);
    const int unbound = list->visualItemRect(list->item(pane.followRow())).height();
    QVERIFY2(unbound < 2 * oneLine,
             qPrintable(QStringLiteral("unbound mode row %1 px, one line %2 px")
                            .arg(unbound).arg(oneLine)));
}

void TestRunPane::theCountsAreDrawnInTheDefaultLevelColours()
{
    // The claim is a COLOUR, which exists nowhere but in pixels: every widget here holds
    // the same values whatever the chips are painted in. What it pins is that the pane
    // and the log make one statement rather than two — a run's ERROR count wears the
    // colour HighlighterSet::defaults() gives an ERROR record (SPEC.md §7).
    if (QFontDatabase::families().isEmpty())
        QSKIP("no fonts resolvable on this platform");

    Document doc;
    QTemporaryFile file;
    QVERIFY2(openLog(doc, file, kNoisyLog), qPrintable(doc.lastError()));

    RunPane pane;
    pane.setDocument(&doc);
    // Shown and settled, because what is grabbed below is the list's LAID-OUT size: a
    // resize() on a child inside an unshown layout is redistributed the moment anything
    // activates that layout, and the grab would then be of a viewport too short to hold
    // the rows the rects are read from.
    pane.show();
    QVERIFY(QTest::qWaitForWindowExposed(&pane));
    pane.resize(360, 700);
    for (int i = 0; i < 8; ++i)
        QApplication::processEvents();

    QListWidget *list = runList(pane);
    QVERIFY(list);
    // Not the selected row: a selection fill is the style's and would be what is being
    // measured if the chip happened not to be drawn at all.
    list->setCurrentRow(pane.followRow());

    const bool dark = isDarkPalette(list->palette());
    QColor errorBg;
    QColor fatalBg;
    for (const HighlightRule &r : HighlighterSet::defaults().rules) {
        if (!r.match.priorityEnabled)
            continue;
        if (r.match.minPriority == Priority::Error)
            errorBg = HighlightPalette::color(r.background, dark);
        if (r.match.minPriority == Priority::Fatal)
            fatalBg = HighlightPalette::color(r.background, dark);
    }
    QVERIFY(errorBg.isValid() && fatalBg.isValid());
    QVERIFY(errorBg != fatalBg); // or the two assertions below prove nothing

    const QImage shot = list->grab().toImage().convertToFormat(QImage::Format_RGB32);

    const auto rowHolds = [&shot, list](int row, const QColor &c) {
        const QRect r = list->visualItemRect(list->item(row));
        // A row read against a viewport too short to hold it would answer "no colour"
        // for the most ordinary of reasons, so the rect being on screen IS the test's
        // precondition rather than something to quietly clip away.
        if (!shot.rect().contains(r))
            return false;
        for (int y = r.top(); y <= r.bottom(); ++y)
            for (int x = r.left(); x <= r.right(); ++x)
                if (shot.pixelColor(x, y).rgb() == c.rgb())
                    return true;
        return false;
    };

    for (int row = 0; row < list->count(); ++row)
        QVERIFY2(shot.rect().contains(list->visualItemRect(list->item(row))),
                 "the list is not tall enough to hold every row it is asked about");

    // Run 1 has the ERROR and run 2 the FATAL, and neither wears the other's colour —
    // which is what makes this a test of the mapping and not of "something red".
    QVERIFY(rowHolds(RunPane::kFirstRunRow + 1, errorBg));
    QVERIFY(!rowHolds(RunPane::kFirstRunRow + 1, fatalBg));
    QVERIFY(rowHolds(RunPane::kFirstRunRow + 2, fatalBg));
    QVERIFY(!rowHolds(RunPane::kFirstRunRow + 2, errorBg));

    // ...and a run with nothing outstanding wears neither.
    QVERIFY(!rowHolds(RunPane::kFirstRunRow + 3, errorBg));
    QVERIFY(!rowHolds(RunPane::kFirstRunRow + 3, fatalBg));
}

// The zebra band (SPEC.md §3a). A run row is three lines and carries no separator, so
// without it a list of runs is one block of text in which the reader has to work out
// where one run stops and the next begins. Pixels are the only place it exists — every
// widget holds the same values banded or not — and the band must be read against the
// row's OWN fill rather than against a written-down colour: QPalette::AlternateBase is
// the role the style reads and nothing obliges a theme to make it differ from Base,
// which is the whole reason this pane does not simply switch alternating colours on and
// trust the theme (the log table's own band measured 1.00:1 on a white theme).
void TestRunPane::theRowsAlternateInABandThatIsVisibleOnThisTheme()
{
    if (QFontDatabase::families().isEmpty())
        QSKIP("no fonts resolvable on this platform");

    Document doc;
    QTemporaryFile file;
    QVERIFY2(openLog(doc, file), qPrintable(doc.lastError()));

    RunPane pane;
    pane.setDocument(&doc);
    pane.show();
    QVERIFY(QTest::qWaitForWindowExposed(&pane));
    pane.resize(360, 700);
    for (int i = 0; i < 8; ++i)
        QApplication::processEvents();

    QListWidget *list = runList(pane);
    QVERIFY(list);
    QVERIFY(list->alternatingRowColors());
    // Row 0 is the selected one, so the two rows compared below carry no selection fill:
    // a highlight is the style's own and would be what was measured otherwise.
    list->setCurrentRow(pane.followRow());
    for (int i = 0; i < 4; ++i)
        QApplication::processEvents();

    const QColor base = list->palette().color(QPalette::Base);
    const QColor band = list->palette().color(QPalette::AlternateBase);
    // The band is a colour the theme may never have supplied: it is derived here, and a
    // theme that leaves AlternateBase equal to Base would take the stripe away in
    // silence — which is exactly what this asserts did not happen.
    QCOMPARE(band, alternateRowColor(list->palette()));
    QVERIFY(band != base);

    const QImage shot = list->grab().toImage().convertToFormat(QImage::Format_RGB32);
    const auto fillOf = [&shot, list](int row) {
        const QRect r = list->visualItemRect(list->item(row));
        QMap<QRgb, int> counts;
        if (!shot.rect().contains(r))
            return QRgb(0);
        for (int y = r.top(); y <= r.bottom(); ++y)
            for (int x = r.left(); x <= r.right(); ++x)
                ++counts[shot.pixelColor(x, y).rgb()];
        // The row's FILL is whatever most of it is painted in — text, chips and the
        // frame are a minority of a three-line row by construction.
        QRgb best = 0;
        int most = 0;
        for (auto it = counts.cbegin(); it != counts.cend(); ++it)
            if (it.value() > most) { most = it.value(); best = it.key(); }
        return best;
    };

    // Row 1 ("All runs") is the alternate of row 0; row 2 is the first run row and is
    // back on the base fill. Both are asserted, so a band drawn over EVERY row would
    // fail this as surely as one drawn over none.
    QCOMPARE(fillOf(1), band.rgb());
    QCOMPARE(fillOf(2), base.rgb());
    // ...and the run rows themselves alternate, which is the case the band exists for:
    // those are the three-line rows.
    QCOMPARE(fillOf(3), band.rgb());
}

int main(int argc, char *argv[])
{
    // The three Apply-gate cases build a real MainWindow, which restores a session and
    // reads and writes logsettings.json — so every persistent store is isolated under a
    // throwaway config home, exactly as tst_lastrun and tst_sessiongui do. Without it
    // these cases would open the developer's own tabs and rewrite their settings tree.
    QTemporaryDir configHome;
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
    qputenv("XDG_DATA_HOME", configHome.path().toUtf8());
    qputenv("HOME", configHome.path().toUtf8());
    qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("loftail-test"));
    QApplication::setApplicationName(QStringLiteral("loftail-test-runpane"));

    TestRunPane tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_runpane.moc"
