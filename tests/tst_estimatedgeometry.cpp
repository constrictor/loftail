#include <QtTest>

#include "EstimatedGeometry.h"
#include "Record.h"
#include "RecordIndex.h"

using namespace loftail;

// Pure estimated-geometry coverage (ARCHITECTURE.md §7.1.1), with NO QApplication
// and no real file: the character-count height model, the per-block cache keyed by
// column count (hit / invalidate on width change), and the estimate->measured
// refinement that makes the scrollbar total converge to the exact height once
// every block has been visited (the convergence property §10 calls out).
class TestEstimatedGeometry : public QObject
{
    Q_OBJECT

private:
    // A synthetic index spanning several blocks with known physical line counts.
    static RecordIndex makeIndex(int n)
    {
        RecordIndex idx;
        idx.records.reserve(n);
        for (int i = 0; i < n; ++i) {
            Record r{};
            r.lineCount = quint16((i % 3) + 1); // 1..3 physical lines
            idx.records.append(r);
        }
        idx.rebuildBlockSums();
        return idx;
    }

    // Measure a block exactly the way LogView would, but from a synthetic
    // "chars per physical line" model so the test needs no decoder: record r has
    // `displayLines(r)` physical lines, each `charsPerLine` characters wide.
    static void measureBlockSynthetic(EstimatedGeometry &g, const RecordIndex &idx,
                                      int block, int charsPerLine)
    {
        const int start = block * RecordIndex::kBlockSize;
        const int end = qMin(start + RecordIndex::kBlockSize, int(idx.records.size()));
        QVector<quint16> lines;
        lines.reserve(end - start);
        for (int r = start; r < end; ++r) {
            const int phys = RecordIndex::displayLines(idx.records.at(r));
            QVector<int> lineChars(phys, charsPerLine);
            lines.append(quint16(EstimatedGeometry::measuredRecordLines(
                lineChars, g.columns(), RecordIndex::kDisplayLineCap)));
        }
        g.measureBlock(block, lines);
    }

    // The exact total display-line height at `cols`, computed directly from the
    // same synthetic char model — the value estimation must converge to.
    static qint64 exactTotal(const RecordIndex &idx, int cols, int charsPerLine)
    {
        qint64 total = 0;
        for (const Record &rec : idx.records) {
            const int phys = RecordIndex::displayLines(rec);
            QVector<int> lineChars(phys, charsPerLine);
            total += EstimatedGeometry::measuredRecordLines(lineChars, cols,
                                                            RecordIndex::kDisplayLineCap);
        }
        return total;
    }

private slots:
    void charCountHeightModel();
    void measuredRecordCapAndFloor();
    void blockCacheKeyedByWidth();
    void refinementConvergesToExactTotal();
    void mappingRoundTripsWhenFullyMeasured();
    void estimatedMappingIsMonotonicAndInBounds();
};

// ceil(chars/cols), at least one; the whole wrap model reduces to this.
void TestEstimatedGeometry::charCountHeightModel()
{
    QCOMPARE(EstimatedGeometry::visualLinesForChars(0, 80), 1);
    QCOMPARE(EstimatedGeometry::visualLinesForChars(80, 80), 1);
    QCOMPARE(EstimatedGeometry::visualLinesForChars(81, 80), 2);
    QCOMPARE(EstimatedGeometry::visualLinesForChars(160, 80), 2);
    QCOMPARE(EstimatedGeometry::visualLinesForChars(161, 80), 3);
    // Narrower viewport => more visual lines for the same text.
    QCOMPARE(EstimatedGeometry::visualLinesForChars(160, 40), 4);
    // Degenerate widths never divide by zero.
    QCOMPARE(EstimatedGeometry::visualLinesForChars(500, 0), 1);
}

void TestEstimatedGeometry::measuredRecordCapAndFloor()
{
    // Two physical lines, each wrapping to 2 => 4 visual lines.
    QCOMPARE(EstimatedGeometry::measuredRecordLines({120, 120}, 80, 100), 4);
    // Empty record still occupies one line.
    QCOMPARE(EstimatedGeometry::measuredRecordLines({}, 80, 100), 1);
    QCOMPARE(EstimatedGeometry::measuredRecordLines({0}, 80, 100), 1);
    // A pathologically tall record is clamped to the shared display cap.
    QVector<int> many(1000, 200); // 1000 lines * 2 wraps each, capped at 100
    QCOMPARE(EstimatedGeometry::measuredRecordLines(many, 80, 100), 100);
}

// The cache is keyed by column count: measuring at one width and then changing
// width must invalidate every measurement.
void TestEstimatedGeometry::blockCacheKeyedByWidth()
{
    const RecordIndex idx = makeIndex(RecordIndex::kBlockSize * 3 + 17);
    EstimatedGeometry g;
    g.reset(&idx, 80);
    QVERIFY(g.blockCount() >= 4);

    QVERIFY(!g.isBlockMeasured(0));
    measureBlockSynthetic(g, idx, 0, 200); // long lines => real wrapping
    QVERIFY(g.isBlockMeasured(0));
    QCOMPARE(g.measuredBlockCount(), 1);
    QVERIFY(g.expansionFactor() > 1.0); // observed wrapping raised the estimate

    // Re-measuring the same block at the same width is a hit (no double count).
    measureBlockSynthetic(g, idx, 0, 200);
    QCOMPARE(g.measuredBlockCount(), 1);

    // Same width => setColumns is a no-op and keeps the cache.
    QVERIFY(!g.setColumns(80));
    QVERIFY(g.isBlockMeasured(0));

    // A width change drops every measurement and resets the running average.
    QVERIFY(g.setColumns(40));
    QVERIFY(!g.isBlockMeasured(0));
    QCOMPARE(g.measuredBlockCount(), 0);
    QCOMPARE(g.expansionFactor(), 1.0);
}

// The estimated total starts approximate and refines to the exact height as every
// block is measured (ARCHITECTURE.md §7.1.1, §10).
void TestEstimatedGeometry::refinementConvergesToExactTotal()
{
    const int n = RecordIndex::kBlockSize * 3 + 100;
    const RecordIndex idx = makeIndex(n);
    const int cols = 50;
    const int chars = 175; // each physical line wraps to ceil(175/50)=4 visual lines

    EstimatedGeometry g;
    g.reset(&idx, cols);

    // Before any measurement the estimate assumes no wrapping (expansion 1.0), so
    // the total equals the physical line total — an underestimate here.
    QCOMPARE(g.totalLines(), idx.totalLines());
    const qint64 exact = exactTotal(idx, cols, chars);
    QVERIFY(exact > idx.totalLines());

    // Measure blocks one at a time; the total moves monotonically toward `exact`
    // and lands on it exactly once the final block is measured.
    qint64 prevGap = qAbs(exact - g.totalLines());
    for (int b = 0; b < g.blockCount(); ++b) {
        measureBlockSynthetic(g, idx, b, chars);
        const qint64 gap = qAbs(exact - g.totalLines());
        QVERIFY2(gap <= prevGap, "measuring a block must not worsen the estimate");
        prevGap = gap;
    }
    QCOMPARE(g.measuredBlockCount(), g.blockCount());
    QCOMPARE(g.totalLines(), exact); // fully measured => exact
}

// Once every block is measured, the line<->record mapping is exact and round-trips.
void TestEstimatedGeometry::mappingRoundTripsWhenFullyMeasured()
{
    const int n = RecordIndex::kBlockSize * 2 + 40;
    const RecordIndex idx = makeIndex(n);
    const int cols = 60;
    const int chars = 130; // ceil(130/60) = 3 visual lines per physical line

    EstimatedGeometry g;
    g.reset(&idx, cols);
    for (int b = 0; b < g.blockCount(); ++b)
        measureBlockSynthetic(g, idx, b, chars);

    qint64 acc = 0;
    for (int r = 0; r < n; ++r) {
        QCOMPARE(g.firstLineOfRecord(r), acc);
        QCOMPARE(g.recordAtLine(acc), r);
        const int h = g.recordHeightLines(r);
        QCOMPARE(g.recordAtLine(acc + h - 1), r); // last line still maps to r
        acc += h;
    }
    QCOMPARE(acc, g.totalLines());
}

// For an unmeasured (purely estimated) index the mapping is still well-formed:
// firstLineOfRecord is non-decreasing and every line maps back into range.
void TestEstimatedGeometry::estimatedMappingIsMonotonicAndInBounds()
{
    const int n = RecordIndex::kBlockSize * 2 + 5;
    const RecordIndex idx = makeIndex(n);
    EstimatedGeometry g;
    g.reset(&idx, 80);
    // Measure just the middle block, leaving the rest estimated.
    measureBlockSynthetic(g, idx, 1, 300);

    qint64 prev = -1;
    for (int r = 0; r < n; ++r) {
        const qint64 f = g.firstLineOfRecord(r);
        QVERIFY2(f >= prev, "firstLineOfRecord must be non-decreasing");
        prev = f;
        const int rr = g.recordAtLine(f);
        QVERIFY(rr >= 0 && rr < n);
    }
    // Out-of-range lines clamp to the ends.
    QCOMPARE(g.recordAtLine(-5), 0);
    QCOMPARE(g.recordAtLine(g.totalLines() + 100), n - 1);
}

QTEST_APPLESS_MAIN(TestEstimatedGeometry)
#include "tst_estimatedgeometry.moc"
