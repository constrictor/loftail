#include <QtTest>

#include <QApplication>
#include <QGroupBox>
#include <QListWidget>
#include <QSignalSpy>
#include <QTemporaryFile>

#include "Document.h"
#include "LogFormat.h"
#include "RunPane.h"

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

    // Three runs plus a leading preamble record, so the list has five rows counting
    // "All runs" and the newest-run default is not row 1.
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

private slots:
    void theRunsAreAListWithTheSelectedRunOnIt();
    void theRunListTakesTheSpareHeight();
    void arrowingThroughTheListSelectsARun();
    void repopulatingTheListIsNotAChoice();
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
    QCOMPARE(list->count(), 5);          // "All runs" + preamble + three runs
    QCOMPARE(list->currentRow(), 4);     // newest run selected by setRunStart

    // Every row is readable at a dock's width without widening the pane past it: the
    // labels elide and the full text is on the tooltip, rather than the view growing a
    // horizontal scrollbar and pushing the pane's minimum width out.
    QCOMPARE(list->horizontalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);
    QCOMPARE(list->textElideMode(), Qt::ElideRight);
    QVERIFY(!list->item(4)->toolTip().isEmpty());
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
    list->setFocus();
    QTest::keyClick(list, Qt::Key_Up); // row 4 (newest) -> row 3
    QCOMPARE(spy.size(), 1);
    QCOMPARE(spy.takeFirst().at(0).toInt(), 2); // row 3 == runs()[2]

    // Row 0 is "All runs", which is run index -1 and not a run.
    list->setCurrentRow(0);
    QCOMPARE(spy.size(), 1);
    QCOMPARE(spy.takeFirst().at(0).toInt(), -1);
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
    QCOMPARE(list->currentRow(), 4);

    pane.setDocument(nullptr);
    QCOMPARE(spy.size(), 0);
}

QTEST_MAIN(TestRunPane)
#include "tst_runpane.moc"
