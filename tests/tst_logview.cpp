#include <QtTest>

#include <QApplication>
#include <QHeaderView>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTemporaryFile>

#include "Document.h"
#include "Filter.h"
#include "LogModel.h"
#include "LogView.h"
#include "Priority.h"
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

    // A log with `n` records whose messages are long enough to wrap in AlwaysOn.
    static QByteArray makeLog(int n)
    {
        QByteArray bytes;
        for (int i = 0; i < n; ++i) {
            bytes += "2026-07-21 14:32:05,123 [main] INFO  net.socket - ";
            // A ~200-char message so it wraps to several visual lines at any
            // realistic viewport width, driving the expansion factor above 1.
            bytes += QByteArray("payload ").repeated(25);
            bytes += "\n";
        }
        return bytes;
    }

    static bool openLog(Document &doc, QTemporaryFile &file, int n)
    {
        if (!writeLog(file, makeLog(n)))
            return false;
        return doc.open(file.fileName(),
                        QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"),
                        Encoding::Utf8, QTimeZone::utc());
    }

private slots:
    void wrapOffMappingMatchesBase();
    void selectedRecordWrapPatchesGeometry();
    void flattenAndTsvBuilders();
    void columnStateRoundTrips();
    void everyColumnRendersFixedPitch();
    void alwaysOnMeasuresAndRefines();
    void switchingToExactKeepsEstimationCache();
    void alwaysOnUnreachableFromExactPath();
    void alwaysOnPaintRefinesWhileScrolling();
    void filterRestrictsVisibleSetAndGeometry();
    void followDetachesAndReattaches();
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

// Every column renders in a fixed-pitch font, header included. This is load-bearing
// for the estimated-geometry path, which models a wrapped record's height as
// ceil(chars / cols) instead of shaping text (ARCHITECTURE.md §7.1.1), and it is
// what makes columns line up vertically.
void TestLogView::everyColumnRendersFixedPitch()
{
    QTemporaryFile file;
    QVERIFY(writeLog(file, "2026-07-21 14:32:05,123 [main] INFO  net.socket - a\n"));

    Document doc;
    QVERIFY2(doc.open(file.fileName(),
                      QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"),
                      Encoding::Utf8, QTimeZone::utc()),
             qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);

    // QFontInfo reports the font actually resolved, not the one requested, so this
    // fails if the family falls back to a proportional face.
    QVERIFY(QFontInfo(view.font()).fixedPitch());
    // Cells are painted on the viewport and headings by the header view; both must
    // inherit the same font rather than the proportional application default.
    QVERIFY(QFontInfo(view.viewport()->font()).fixedPitch());
    QVERIFY(QFontInfo(view.header()->font()).fixedPitch());
    QCOMPARE(view.header()->font().family(), view.font().family());

    // A fixed-pitch font is only useful to the geometry model if the advance really
    // is uniform: narrow and wide characters must measure the same.
    const QFontMetrics fm(view.font());
    QCOMPARE(fm.horizontalAdvance(QStringLiteral("iiiiiiiiii")),
             fm.horizontalAdvance(QStringLiteral("WWWWWWWWWW")));
}

// AlwaysOn engages the estimated geometry: measuring the visible block records
// their exact wrapped heights and refines the total toward truth (§7.1.1).
void TestLogView::alwaysOnMeasuresAndRefines()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file, 40), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.resize(700, 400); // a finite viewport so wrapping has a width to work with

    view.setWrapMode(LogView::WrapMode::AlwaysOn);
    const EstimatedGeometry &g = view.estimatedGeometry();
    QVERIFY(g.isBound());
    QVERIFY(g.blockCount() >= 1);
    QVERIFY(!g.isBlockMeasured(0)); // nothing measured until painted/forced

    const qint64 beforeTotal = g.totalLines();

    // Force the block containing record 0 to be measured (what painting does
    // lazily) and confirm the wrapped total exceeds the unwrapped physical total.
    view.measureBlockOfRecord(0);
    QVERIFY(g.isBlockMeasured(0));
    QVERIFY(g.expansionFactor() > 1.0);
    QVERIFY(g.totalLines() > beforeTotal);
    QCOMPARE(g.totalLines(), g.firstLineOfRecord(doc.index().records.size()));
}

// Switching to an exact wrap mode must NOT touch the estimation cache: the exact
// path never reads it, and a round-trip back to AlwaysOn should not re-measure.
void TestLogView::switchingToExactKeepsEstimationCache()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file, 30), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.resize(700, 400);

    view.setWrapMode(LogView::WrapMode::AlwaysOn);
    view.measureBlockOfRecord(0);
    const EstimatedGeometry &g = view.estimatedGeometry();
    QVERIFY(g.isBlockMeasured(0));
    const int cols = g.columns();
    const int measured = g.measuredBlockCount();
    const qint64 total = g.totalLines();

    // Switch to each exact mode; the cache is left exactly as it was.
    view.setWrapMode(LogView::WrapMode::Off);
    QVERIFY(g.isBlockMeasured(0));
    QCOMPARE(g.measuredBlockCount(), measured);
    QCOMPARE(g.columns(), cols);
    QCOMPARE(g.totalLines(), total);

    view.setWrapMode(LogView::WrapMode::SelectedRecordOnly);
    view.setCurrentRecord(5); // exercises the exact selected-record wrap path
    QVERIFY(g.isBlockMeasured(0));
    QCOMPARE(g.measuredBlockCount(), measured);
    QCOMPARE(g.columns(), cols);

    // Returning to AlwaysOn at the same width keeps the measurements (no reset).
    view.setWrapMode(LogView::WrapMode::AlwaysOn);
    QVERIFY(g.isBlockMeasured(0));
    QCOMPARE(g.measuredBlockCount(), measured);
}

// In the exact modes the estimator is never bound at all — proof the estimation
// machinery is unreachable from the exact path (invariant #6).
void TestLogView::alwaysOnUnreachableFromExactPath()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file, 12), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.resize(700, 400);

    // Default mode is Off; geometry queries and navigation exercise the exact
    // path, which must never construct/bind the estimator.
    QVERIFY(!view.estimatedGeometry().isBound());
    view.setWrapMode(LogView::WrapMode::SelectedRecordOnly);
    view.setCurrentRecord(3);
    QVERIFY(!view.estimatedGeometry().isBound());
    // measureBlockOfRecord is a no-op outside AlwaysOn.
    view.measureBlockOfRecord(0);
    QVERIFY(!view.estimatedGeometry().isBound());
}

// The real paint path (not the forced measure hook) drives measurement lazily:
// rendering the viewport measures the visible block, and scrolling into a second
// block measures it too, refining the total further. Uses a >1-block synthetic
// log and QWidget::render to trigger paintEvent synchronously, headless.
void TestLogView::alwaysOnPaintRefinesWhileScrolling()
{
    QTemporaryFile file;
    Document doc;
    // 5000 records spans two index blocks (kBlockSize == 4096).
    QVERIFY2(openLog(doc, file, 5000), qPrintable(doc.lastError()));
    QVERIFY(doc.index().records.size() > RecordIndex::kBlockSize);

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.resize(700, 400);
    view.setWrapMode(LogView::WrapMode::AlwaysOn);
    const EstimatedGeometry &g = view.estimatedGeometry();
    QVERIFY(g.blockCount() >= 2);

    auto paint = [&view]() {
        QPixmap pm(view.viewport()->size());
        view.viewport()->render(&pm); // invokes LogView::paintEvent synchronously
    };

    // First paint at the top measures block 0 and refines the total upward.
    const qint64 estTotal = g.totalLines(); // pre-measurement estimate
    paint();
    QVERIFY(g.isBlockMeasured(0));
    QCOMPARE(g.measuredBlockCount(), 1);
    QVERIFY(g.totalLines() > estTotal);

    // Scroll into the second block and paint: it gets measured too.
    view.setCurrentRecord(4500); // scrolls the target into view
    paint();
    QVERIFY(g.isBlockMeasured(1));
    QVERIFY(g.measuredBlockCount() >= 2);
    // Fully measured blocks make their portion of the mapping exact.
    QCOMPARE(g.totalLines(), g.firstLineOfRecord(doc.index().records.size()));
}

// A filter narrows the visible set: the model presents only visible rows and the
// view's line-unit geometry runs over the FILTERED subset (M4, invariant #6). The
// model reset that wraps the recompute is what the UI drives on a filter change.
void TestLogView::filterRestrictsVisibleSetAndGeometry()
{
    QTemporaryFile file;
    QVERIFY(writeLog(file,
        "2026-07-21 10:00:00,000 [main] INFO  net.socket - a\n"   // 0 hidden
        "2026-07-21 10:00:01,000 [main] WARN  net.socket - b\n"   // 1 shown
        "2026-07-21 10:00:02,000 [main] ERROR net.socket - c\n"   // 2 shown
        "2026-07-21 10:00:03,000 [main] DEBUG net.socket - d\n"   // 3 hidden
        "2026-07-21 10:00:04,000 [main] FATAL net.socket - e\n")); // 4 shown

    Document doc;
    QVERIFY2(doc.open(file.fileName(),
                      QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"),
                      Encoding::Utf8, QTimeZone::utc()),
             qPrintable(doc.lastError()));
    QCOMPARE(doc.index().records.size(), 5);

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.resize(700, 400);
    QCOMPARE(model.rowCount(), 5); // unfiltered: identity

    // Apply priority >= WARN via the model-reset path the UI uses.
    doc.filters().priorityEnabled = true;
    doc.filters().minPriority = Priority::Warn;
    model.beginFilterReset();
    doc.applyFilters();
    model.endFilterReset();

    // The model now presents only the 3 visible rows, in order.
    QCOMPARE(model.rowCount(), 3);
    const int prioCol = 2; // Time, Thread, Priority, Subsystem, Message
    QCOMPARE(model.cellText(0, prioCol), QStringLiteral("WARN"));
    QCOMPARE(model.cellText(1, prioCol), QStringLiteral("ERROR"));
    QCOMPARE(model.cellText(2, prioCol), QStringLiteral("FATAL"));

    // The view's geometry runs over the FILTERED index: total lines == the visible
    // rows' display lines, and the last scroll line resolves to the last view row.
    const RecordIndex &geo = doc.filtered().geometry();
    QCOMPARE(geo.records.size(), 3);
    QCOMPARE(LogView::totalScrollLines(geo, -1, 0), geo.totalLines());
    QCOMPARE(LogView::recordAtScrollLine(geo, -1, 0, 0), 0);
    QCOMPARE(LogView::recordAtScrollLine(geo, -1, 0, geo.totalLines() - 1), 2);

    // Navigation stays inside the filtered set.
    view.setCurrentRecord(99);
    QCOMPARE(view.currentRecord(), 2); // clamped to the last visible row

    // Clearing the filter restores the identity view.
    doc.filters() = FilterSet{};
    model.beginFilterReset();
    doc.applyFilters();
    model.endFilterReset();
    QCOMPARE(model.rowCount(), 5);
}

// Follow mode (SPEC.md §3, M6): every open follows the tail; scrolling away detaches;
// scrolling back to the bottom OR the return-to-bottom control re-attaches.
void TestLogView::followDetachesAndReattaches()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file, 300), qPrintable(doc.lastError())); // enough to scroll

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.setWrapMode(LogView::WrapMode::Off);
    view.resize(400, 120); // a small viewport so a vertical scroll range exists

    QSignalSpy spy(&view, &LogView::followingChanged);

    // Normalize to "following at the bottom" (as an open does).
    view.followTail();
    QVERIFY(view.following());
    QScrollBar *sb = view.verticalScrollBar();
    QVERIFY(sb->maximum() > 0);
    QCOMPARE(sb->value(), sb->maximum());

    // Scrolling up detaches follow.
    sb->setValue(sb->maximum() / 2);
    QVERIFY(!view.following());

    // Scrolling back to the bottom re-attaches.
    sb->setValue(sb->maximum());
    QVERIFY(view.following());

    // Detach again, then re-attach via the return-to-bottom control.
    sb->setValue(0);
    QVERIFY(!view.following());
    view.followTail();
    QVERIFY(view.following());
    QCOMPARE(sb->value(), sb->maximum());

    // The state actually toggled (not stuck): several transitions were signalled.
    QVERIFY(spy.count() >= 3);
}

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    TestLogView tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_logview.moc"
