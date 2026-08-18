#include <QtTest>

#include <QApplication>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QSignalSpy>
#include <QTemporaryFile>

#include "Document.h"
#include "LogFormat.h"
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
    // "Last run" and "All runs", and no run's row coincides with either of those.
    static constexpr auto kLog =
        "2026-01-01 10:00:00,000 [main] INFO  boot - preamble line\n"
        "2026-01-01 10:00:01,000 [main] INFO  boot - RUN START one\n"
        "2026-01-01 10:00:02,000 [main] INFO  app - working\n"
        "2026-01-01 10:00:03,000 [main] INFO  boot - RUN START two\n"
        "2026-01-01 10:00:04,000 [main] INFO  app - working\n"
        "2026-01-01 10:00:05,000 [main] INFO  boot - RUN START three\n"
        "2026-01-01 10:00:06,000 [main] INFO  app - working\n";

    static bool openLog(Document &doc, QTemporaryFile &file)
    {
        if (!file.open())
            return false;
        file.write(kLog);
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

private slots:
    void theRunsAreAListWithTheSelectedRunOnIt();
    void theLastRunEntryIsFirstAndIsNotARun();
    void aLogWithNoRunsStillOffersLastRun();
    void theRunListTakesTheSpareHeight();
    void arrowingThroughTheListSelectsARun();
    void repopulatingTheListIsNotAChoice();
    void thePaneSaysThatThePatternWaitsForApply();
    void aTypedPatternMakesTheNoteAskForApply();
    void withNoLogOpenTheListStillExplainsItself();
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
    QCOMPARE(list->count(), 6);          // "Last run" + "All runs" + preamble + three
    // The newest run is what is on screen, but what is SELECTED is the standing
    // instruction that put it there — the pane reads the mode off the document rather
    // than inferring it from the selection being the last ordinal.
    QCOMPARE(list->currentRow(), RunPane::kLastRunRow);
    QVERIFY(doc.followingLastRun());
    QCOMPARE(doc.selectedRun(), 3);

    // Every row is readable at a dock's width without widening the pane past it: the
    // labels elide and the full text is on the tooltip, rather than the view growing a
    // horizontal scrollbar and pushing the pane's minimum width out.
    QCOMPARE(list->horizontalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);
    QCOMPARE(list->textElideMode(), Qt::ElideRight);
    QVERIFY(!list->item(5)->toolTip().isEmpty());
}

void TestRunPane::theLastRunEntryIsFirstAndIsNotARun()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY2(openLog(doc, file), qPrintable(doc.lastError()));

    RunPane pane;
    pane.setDocument(&doc);
    QListWidget *list = runList(pane);
    QVERIFY(list);
    QSignalSpy spy(&pane, &RunPane::runSelected);

    // Row 0 carries a sentinel and not the ordinal it currently resolves to: the whole
    // point of the entry is that the ordinal changes underneath it.
    // (The pane only reports; MainWindow is what turns these into Document calls.)
    list->setCurrentRow(RunPane::kFirstRunRow + 3); // pin the run that is last today
    QCOMPARE(spy.size(), 1);
    QCOMPARE(spy.takeFirst().at(0).toInt(), 3);

    list->setCurrentRow(RunPane::kLastRunRow);
    QCOMPARE(spy.size(), 1);
    QCOMPARE(spy.takeFirst().at(0).toInt(), RunPane::kLastRun);

    // ...and a pinned run puts the current row back on that run, never on row 0 —
    // the two are indistinguishable by selectedRun() alone, which is why the pane asks.
    doc.selectRun(3);
    pane.refresh();
    QCOMPARE(list->currentRow(), RunPane::kFirstRunRow + 3);
    doc.selectLastRun();
    pane.refresh();
    QCOMPARE(list->currentRow(), RunPane::kLastRunRow);
    QCOMPARE(spy.size(), 0); // neither repopulation was a choice
}

void TestRunPane::aLogWithNoRunsStillOffersLastRun()
{
    // No run-start pattern: nothing is detected, so "Last run" and "All runs" are the
    // only rows and both mean the whole file. The entry is offered all the same —
    // it is the pane's default, and a pattern typed a moment later fills it in.
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
    QCOMPARE(list->currentRow(), RunPane::kLastRunRow);
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
    QCOMPARE(list->currentRow(), RunPane::kLastRunRow);

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

QTEST_MAIN(TestRunPane)
#include "tst_runpane.moc"
