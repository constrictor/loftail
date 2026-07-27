#include <QtTest>

#include <QApplication>
#include <QCheckBox>
#include <QListWidget>
#include <QFile>
#include <QTemporaryFile>

#include "Document.h"
#include "FilterPane.h"
#include "Filter.h"

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

    static QCheckBox *box(FilterPane &pane, const QString &label)
    {
        const QList<QCheckBox *> boxes = pane.findChildren<QCheckBox *>();
        for (QCheckBox *b : boxes)
            if (b->text() == label)
                return b;
        return nullptr;
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

private slots:
    void metadataAxesAreOnByDefault();
    void allInclusiveAxesStayInactive();
    void narrowingActivatesTheAxis();
    void subsystemDiscoveredLaterArrivesChecked();
    void rebindingForgetsSeenNames();
};

void TestFilterPane::metadataAxesAreOnByDefault()
{
    FilterPane pane;
    QVERIFY(box(pane, QStringLiteral("Filter by minimum priority"))->isChecked());
    QVERIFY(box(pane, QStringLiteral("Filter by subsystem"))->isChecked());
    // The axes the user did not ask for keep their old default.
    QVERIFY(!box(pane, QStringLiteral("Filter by thread"))->isChecked());
    QVERIFY(!box(pane, QStringLiteral("Filter by message text"))->isChecked());
    QVERIFY(!box(pane, QStringLiteral("Filter by time range"))->isChecked());
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

QTEST_MAIN(TestFilterPane)
#include "tst_filterpane.moc"
