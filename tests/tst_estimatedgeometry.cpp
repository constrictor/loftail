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

#include "EstimatedGeometry.h"
#include "Record.h"
#include "RecordIndex.h"

using namespace loftail;

// Pure estimated-geometry coverage (ARCHITECTURE.md §7.1.1), with NO QApplication
// and no real file: the per-block cache keyed by WRAP WIDTH (hit / invalidate on a
// width change), and the estimate->measured refinement that makes the scrollbar total
// converge to the exact height once every block has been visited (the convergence
// property §10 calls out).
//
// What a record's height IS is not this class's question and is not asked here: it is
// measured view-side, from the font, by WrapMetrics — so the heights below come from a
// synthetic "one character, one pixel" model whose only job is to be width-dependent.
// The real one is held against a QTextLayout in tst_logview.
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

    // The synthetic height model: one character to the pixel, ceil()ed, at least one
    // line, and clamped to the shared display cap exactly as WrapMetrics::recordLines()
    // clamps the real one.
    static int syntheticLines(int physicalLines, int charsPerLine, int width)
    {
        const int per = (width <= 0 || charsPerLine <= width) ? 1
                                                              : (charsPerLine + width - 1) / width;
        return qBound(1, physicalLines * per, int(RecordIndex::kDisplayLineCap));
    }

    // Measure a block exactly the way LogView would, but from that model so the test
    // needs no decoder and no font: record r has `displayLines(r)` physical lines, each
    // `charsPerLine` characters wide.
    // `first` is the offset into the block the run starts at, exactly as
    // LogView::measureBlock() takes it from measuredRecordsInBlock(): a block whose
    // tail alone was truncated is topped up rather than measured again.
    static void measureBlockSynthetic(EstimatedGeometry &g, const RecordIndex &idx,
                                      int block, int charsPerLine, int first = 0)
    {
        const int start = block * RecordIndex::kBlockSize + first;
        const int end = qMin(block * RecordIndex::kBlockSize + RecordIndex::kBlockSize,
                             int(idx.records.size()));
        QVector<quint16> lines;
        lines.reserve(end - start);
        for (int r = start; r < end; ++r) {
            const int phys = RecordIndex::displayLines(idx.records.at(r));
            lines.append(quint16(syntheticLines(phys, charsPerLine, g.wrapWidth())));
        }
        g.measureBlock(block, lines, first);
    }

    // The exact total display-line height at `width`, computed directly from the
    // same synthetic char model — the value estimation must converge to.
    static qint64 exactTotal(const RecordIndex &idx, int width, int charsPerLine)
    {
        qint64 total = 0;
        for (const Record &rec : idx.records)
            total += syntheticLines(RecordIndex::displayLines(rec), charsPerLine, width);
        return total;
    }

private slots:
    void blockCacheKeyedByWidth();
    void refinementConvergesToExactTotal();
    void mappingRoundTripsWhenFullyMeasured();
    void estimatedMappingIsMonotonicAndInBounds();
    void anAppendReMeasuresOnlyTheRecordsTheGrowthCouldHaveTouched();
};

// The cache is keyed by the WRAP WIDTH: measuring at one width and then changing
// width must invalidate every measurement — and so must a font change, which need not
// move the width at all.
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

    // Same width => setWrapWidth is a no-op and keeps the cache.
    QVERIFY(!g.setWrapWidth(80));
    QVERIFY(g.isBlockMeasured(0));

    // A width change drops every measurement and resets the running average.
    QVERIFY(g.setWrapWidth(40));
    QVERIFY(!g.isBlockMeasured(0));
    QCOMPARE(g.measuredBlockCount(), 0);
    QCOMPARE(g.expansionFactor(), 1.0);

    // And the drop is reachable WITHOUT a width change, which is what a zoom on a view
    // whose every column width was dragged or restored looks like: the origin does not
    // move, so setWrapWidth() answers false, and every cached height is still the old
    // face's (§7.1.5).
    measureBlockSynthetic(g, idx, 0, 200);
    QVERIFY(g.isBlockMeasured(0));
    QVERIFY(!g.setWrapWidth(40));
    g.invalidateMeasurements();
    QVERIFY(!g.isBlockMeasured(0));
    QCOMPARE(g.measuredBlockCount(), 0);
    QCOMPARE(g.expansionFactor(), 1.0);
    QCOMPARE(g.wrapWidth(), 40);
}

// The estimated total starts approximate and refines to the exact height as every
// block is measured (ARCHITECTURE.md §7.1.1, §10).
void TestEstimatedGeometry::refinementConvergesToExactTotal()
{
    const int n = RecordIndex::kBlockSize * 3 + 100;
    const RecordIndex idx = makeIndex(n);
    const int width = 50;
    const int chars = 175; // each physical line wraps to ceil(175/50)=4 visual lines

    EstimatedGeometry g;
    g.reset(&idx, width);

    // Before any measurement the estimate assumes no wrapping (expansion 1.0), so
    // the total equals the physical line total — an underestimate here.
    QCOMPARE(g.totalLines(), idx.totalLines());
    const qint64 exact = exactTotal(idx, width, chars);
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
    const int width = 60;
    const int chars = 130; // ceil(130/60) = 3 visual lines per physical line

    EstimatedGeometry g;
    g.reset(&idx, width);
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

// --- an append costs the records that moved, not the block (§7.1.1) ----------
//
// The bound index is append-only and live, so a measured block's record count grows
// under it and its trailing record grows continuation lines in place. syncTail()
// TRUNCATES that block's cache back to the old trailing record rather than dropping it,
// and CLAUDE.md says what dropping costs: LogView::measureBlock() decodes and measures
// every record of a block that is not fully measured, so a tailed log would re-decode
// 4096 messages on every ingest tick — invisible in every value the view holds, and
// catastrophic.
//
// Counted, because that is the only exact statement of it: what the view will re-measure
// is recordsInBlock() - measuredRecordsInBlock(), and the claim is that an append leaves
// that at the two records the growth could have touched, whatever the block holds.
void TestEstimatedGeometry::anAppendReMeasuresOnlyTheRecordsTheGrowthCouldHaveTouched()
{
    // TWO FULL BLOCKS, which is what makes the claim visible: the growth lands in a
    // block of 4096 measured records, and the question is how many of them survive it.
    RecordIndex idx = makeIndex(2 * RecordIndex::kBlockSize);
    EstimatedGeometry g;
    g.reset(&idx, 80);
    measureBlockSynthetic(g, idx, 0, 200);
    measureBlockSynthetic(g, idx, 1, 200);
    QVERIFY(g.isBlockMeasured(0));
    QVERIFY(g.isBlockMeasured(1));

    // What LogView::measureBlock() would decode and measure for one block: it starts at
    // measuredRecordsInBlock() and runs to the end of the block.
    const auto owed = [&g](int block) {
        return g.recordsInBlock(block) - g.measuredRecordsInBlock(block);
    };
    QCOMPARE(owed(0) + owed(1), 0);

    // The trailing record grows continuation lines in place — no record count moves at
    // all, which is why syncTail() compares the tail's height as well as the count.
    Record grown = idx.records.last();
    grown.lineCount = quint16(grown.lineCount + 4);
    idx.records.last() = grown;
    idx.extendBlockSums(int(idx.records.size()) - 1);
    QVERIFY(g.syncTail());

    QCOMPARE(owed(1), 1); // the record that grew, and none of the other 4095
    QCOMPARE(owed(0), 0); // the block in front of it was not touched
    QVERIFY2(owed(1) < RecordIndex::kBlockSize / 100,
             "the tick cost a whole block's measurement: the trailing block was dropped "
             "rather than truncated back to the record the growth could have touched");

    // ... and then a record is appended, which is the same claim with the new block's
    // one record added to it.
    measureBlockSynthetic(g, idx, 1, 200, /*first=*/RecordIndex::kBlockSize - 1);
    QCOMPARE(owed(1), 0);
    const int before = int(idx.records.size());
    Record fresh{};
    fresh.lineCount = 2;
    idx.records.append(fresh);
    idx.extendBlockSums(before);
    QVERIFY(g.syncTail());

    QCOMPARE(owed(0), 0);
    QCOMPARE(owed(1), 1); // the old trailing record, reconsidered
    QCOMPARE(owed(2), 1); // and the one that arrived
    QVERIFY2(owed(0) + owed(1) + owed(2) < RecordIndex::kBlockSize / 100,
             "an append cost a whole block's measurement");

    // Nothing about the ANSWERS moved: what is still cached is still exact, and the
    // block in front of the tail is still a measured block rather than an estimated one.
    QVERIFY(g.isBlockMeasured(0));
    QCOMPARE(g.recordHeightLines(0), syntheticLines(RecordIndex::displayLines(idx.records.at(0)),
                                                    200, g.wrapWidth()));
}

QTEST_APPLESS_MAIN(TestEstimatedGeometry)
#include "tst_estimatedgeometry.moc"
