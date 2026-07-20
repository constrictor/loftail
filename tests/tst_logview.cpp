#include <QtTest>

#include <QApplication>
#include <QHeaderView>
#include <QTemporaryFile>

#include "Document.h"
#include "LogModel.h"
#include "LogView.h"
#include "Record.h"
#include "RecordIndex.h"

using namespace loftail;

// LogView coverage that does not need a shown window: the pure line<->record
// geometry mapping (exact mode, wrap off and selected-record-only), the
// copy-as-columns text builders, and the QHeaderView column hide/reorder state
// round-trip that backs "column layout is remembered" (SPEC.md §5). Runs under an
// offscreen QApplication (forced in main) so QWidget construction is legal.
class TestLogView : public QObject
{
    Q_OBJECT

private:
    // A synthetic index spanning several blocks with known line counts.
    static RecordIndex makeIndex(int n)
    {
        RecordIndex idx;
        idx.records.reserve(n);
        for (int i = 0; i < n; ++i) {
            Record r{};
            r.lineCount = quint16((i % 4) + 1); // 1..4 physical lines, under the cap
            idx.records.append(r);
        }
        idx.rebuildBlockSums();
        return idx;
    }

    static bool writeLog(QTemporaryFile &file, const QByteArray &bytes)
    {
        if (!file.open())
            return false;
        file.write(bytes);
        file.flush();
        return true;
    }

private slots:
    void wrapOffMappingMatchesBase();
    void selectedRecordWrapPatchesGeometry();
    void flattenAndTsvBuilders();
    void columnStateRoundTrips();
};

void TestLogView::wrapOffMappingMatchesBase()
{
    const RecordIndex idx = makeIndex(RecordIndex::kBlockSize * 2 + 9);
    const int n = idx.records.size();

    // Wrap off => selRecord == -1: the view mapping is exactly the base mapping.
    QCOMPARE(LogView::totalScrollLines(idx, -1, 0), idx.totalLines());

    qint64 acc = 0;
    for (int r = 0; r < n; ++r) {
        QCOMPARE(LogView::scrollLineOfRecord(idx, -1, 0, r), acc);
        QCOMPARE(LogView::recordAtScrollLine(idx, -1, 0, acc), r);
        const qint64 mid = acc + RecordIndex::displayLines(idx.records.at(r)) - 1;
        QCOMPARE(LogView::recordAtScrollLine(idx, -1, 0, mid), r);
        acc += RecordIndex::displayLines(idx.records.at(r));
    }
    QCOMPARE(acc, idx.totalLines());
}

void TestLogView::selectedRecordWrapPatchesGeometry()
{
    RecordIndex idx = makeIndex(50);
    const int sel = 20;
    const int base = RecordIndex::displayLines(idx.records.at(sel)); // its unwrapped height
    const int extra = 6;
    const int selWrap = base + extra; // the wrapped selected record is taller

    const qint64 selStart = idx.firstLineOfRecord(sel);

    // Total grows by exactly the extra lines.
    QCOMPARE(LogView::totalScrollLines(idx, sel, selWrap), idx.totalLines() + extra);

    // Records before the selection are unshifted; records after shift by `extra`.
    QCOMPARE(LogView::scrollLineOfRecord(idx, sel, selWrap, sel), selStart);
    QCOMPARE(LogView::scrollLineOfRecord(idx, sel, selWrap, sel - 1),
             idx.firstLineOfRecord(sel - 1));
    QCOMPARE(LogView::scrollLineOfRecord(idx, sel, selWrap, sel + 1),
             idx.firstLineOfRecord(sel + 1) + extra);

    // Every line inside the wrapped selected record maps back to it.
    QCOMPARE(LogView::recordAtScrollLine(idx, sel, selWrap, selStart), sel);
    QCOMPARE(LogView::recordAtScrollLine(idx, sel, selWrap, selStart + selWrap - 1), sel);
    // The first line past it is the next record.
    QCOMPARE(LogView::recordAtScrollLine(idx, sel, selWrap, selStart + selWrap), sel + 1);

    // Round-trip for a record after the selection.
    const qint64 after = LogView::scrollLineOfRecord(idx, sel, selWrap, 40);
    QCOMPARE(LogView::recordAtScrollLine(idx, sel, selWrap, after), 40);
}

void TestLogView::flattenAndTsvBuilders()
{
    // Flattening keeps TSV structure intact against embedded tabs/newlines.
    QCOMPARE(LogView::flattenCell(QStringLiteral("a\tb\nc\rd")), QStringLiteral("a b c d"));

    QVector<QVector<QString>> rows = {
        {QStringLiteral("2026"), QStringLiteral("INFO"), QStringLiteral("hello")},
        {QStringLiteral("2027"), QStringLiteral("WARN"), QStringLiteral("world")},
    };
    QCOMPARE(LogView::columnsToTsv(rows),
             QStringLiteral("2026\tINFO\thello\n2027\tWARN\tworld"));
    QCOMPARE(LogView::columnsToTsv({}), QString());
}

void TestLogView::columnStateRoundTrips()
{
    QTemporaryFile file;
    QVERIFY(writeLog(file,
        "2026-07-21 14:32:05,123 [main] INFO  net.socket - a\n"
        "2026-07-21 14:32:06,000 [worker] WARN  db.pool - b\n"));

    Document doc;
    QVERIFY2(doc.open(file.fileName(),
                      QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"),
                      Encoding::Utf8, QTimeZone::utc()),
             qPrintable(doc.lastError()));

    LogModel model(&doc);
    QCOMPARE(model.columnCount(), 5);

    LogView v1(&doc, &model);
    // Hide the Priority column (logical 2) and move Message (logical 4) to the front.
    v1.header()->setSectionHidden(2, true);
    v1.header()->moveSection(v1.header()->visualIndex(4), 0);
    const QByteArray state = v1.saveColumnState();

    // A fresh view restores the same hidden flags and visual order.
    LogView v2(&doc, &model);
    QVERIFY(!v2.header()->isSectionHidden(2)); // default: visible before restore
    QVERIFY(v2.restoreColumnState(state));
    QVERIFY(v2.header()->isSectionHidden(2));
    QCOMPARE(v2.header()->logicalIndex(0), 4); // Message moved to the front
    QCOMPARE(v2.header()->visualIndex(4), 0);
}

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    TestLogView tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_logview.moc"
