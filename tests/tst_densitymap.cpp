#include <QtTest>

#include "DensityMap.h"

#include <QSet>

using namespace loftail;

// The bucketed map behind the density strip (SPEC.md §5, ARCHITECTURE.md §7.1.7).
// Core-only and applesless: the whole class is integer bookkeeping over a probe the
// caller supplies, which is exactly what makes the one thing that has to be right about
// it — that a log growing under the map never costs a rescan, and that a mark survives
// the coarsening that growth forces — testable without a QApplication.
class TestDensityMap : public QObject
{
    Q_OBJECT

private:
    // A probe that marks one row and nothing else, counting how many rows it was asked
    // about so a "nothing is rescanned" claim can be measured rather than asserted.
    struct Probe {
        int markedRow = -1;
        qint16 answer = 0;
        int calls = 0;
        std::function<qint16(int)> fn()
        {
            return [this](int row) -> qint16 {
                ++calls;
                return row == markedRow ? answer : DensityMap::kNothing;
            };
        }
    };

    static void scanAll(DensityMap &m, DensityMap::Lane lane,
                        const std::function<qint16(int)> &probe)
    {
        while (m.scan(lane, 4096, probe) > 0) { }
    }

private slots:
    void anEmptyViewHasNothingToScan()
    {
        DensityMap m;
        m.rebind(0);
        QCOMPARE(m.bucketCount(), 0);
        QVERIFY(m.complete(DensityMap::Lane::Rules));
        Probe p;
        QCOMPARE(m.scan(DensityMap::Lane::Rules, 100, p.fn()), 0);
        QCOMPARE(p.calls, 0);
    }

    void aScanIsBoundedByTheBudgetAndResumesWhereItStopped()
    {
        DensityMap m;
        m.rebind(1000);
        Probe p;
        p.markedRow = 900;
        QCOMPARE(m.scan(DensityMap::Lane::Rules, 250, p.fn()), 250);
        QCOMPARE(m.scanned(DensityMap::Lane::Rules), 250);
        QVERIFY(!m.complete(DensityMap::Lane::Rules));
        // Nothing has been seen yet, so nothing is claimed.
        QVERIFY(!m.anyMark(DensityMap::Lane::Rules));
        scanAll(m, DensityMap::Lane::Rules, p.fn());
        QVERIFY(m.complete(DensityMap::Lane::Rules));
        // Every row asked about exactly once, budget or no budget: a slice that re-asked
        // about rows it had already seen would never finish on a log being tailed.
        QCOMPARE(p.calls, 1000);
        QCOMPARE(m.at(DensityMap::Lane::Rules, m.bucketOf(900)), qint16(0));
    }

    // The claim that makes the strip affordable on a live log: appending records adds
    // work for the new records and for nothing else, however many times the buckets have
    // to be coarsened on the way.
    void growingTheViewNeverRescansWhatWasAlreadyScanned()
    {
        DensityMap m;
        m.rebind(100);
        Probe p;
        p.markedRow = 50;
        scanAll(m, DensityMap::Lane::Rules, p.fn());
        QCOMPARE(p.calls, 100);
        QVERIFY(m.anyMark(DensityMap::Lane::Rules));

        int rows = 100;
        for (int step = 0; step < 40; ++step) {
            rows += 5000;
            m.setRows(rows);
            scanAll(m, DensityMap::Lane::Rules, p.fn());
        }
        QCOMPARE(p.calls, rows);
        QVERIFY(rows > DensityMap::kMaxBuckets * 4); // the coarsening definitely ran
        QVERIFY(m.rowsPerBucket() > 1);
        QVERIFY(m.bucketCount() <= DensityMap::kMaxBuckets);
        // And the mark placed before any of that coarsening is still in the bucket that
        // now holds row 50 — merging pairwise is what keeps that true.
        QCOMPARE(m.at(DensityMap::Lane::Rules, m.bucketOf(50)), qint16(0));
    }

    // Merging keeps the LOWER rule index, because first-match-wins order is severity
    // order by convention: a bucket holding a FATAL and a WARN must read as the FATAL.
    void mergingTwoBucketsKeepsTheLouderRule()
    {
        DensityMap m;
        m.rebind(4 * DensityMap::kMaxBuckets);
        // Row 0 is rule 3, row 1 is rule 1 — adjacent, so they share a bucket at once.
        scanAll(m, DensityMap::Lane::Rules, [](int row) -> qint16 {
            if (row == 0)
                return 3;
            if (row == 1)
                return 1;
            return DensityMap::kNothing;
        });
        QCOMPARE(m.at(DensityMap::Lane::Rules, m.bucketOf(0)), qint16(1));
    }

    void theTwoLanesAreInvalidatedIndependently()
    {
        DensityMap m;
        m.rebind(200);
        scanAll(m, DensityMap::Lane::Rules,
                [](int row) -> qint16 { return row == 10 ? 0 : DensityMap::kNothing; });
        scanAll(m, DensityMap::Lane::Find,
                [](int row) -> qint16 { return row == 20 ? 0 : DensityMap::kNothing; });
        QVERIFY(m.anyMark(DensityMap::Lane::Rules));
        QVERIFY(m.anyMark(DensityMap::Lane::Find));

        // Typing in the Find bar must not throw away the rule scan — the whole reason
        // there are two lanes over one bucket geometry.
        m.clear(DensityMap::Lane::Find);
        QVERIFY(m.anyMark(DensityMap::Lane::Rules));
        QVERIFY(!m.anyMark(DensityMap::Lane::Find));
        QVERIFY(m.complete(DensityMap::Lane::Rules));
        QCOMPARE(m.scanned(DensityMap::Lane::Find), 0);
    }

    // The ingest tick of a filtered live log withdraws its provisional row and re-adds
    // it. That must cost a bucket, not a file.
    void aShrinkRewindsOnlyTheLastBucket()
    {
        DensityMap m;
        m.rebind(10000);
        Probe p;
        p.markedRow = 9999;
        scanAll(m, DensityMap::Lane::Rules, p.fn());
        QCOMPARE(p.calls, 10000);
        const int wasScanned = m.scanned(DensityMap::Lane::Rules);
        QCOMPARE(wasScanned, 10000);

        m.setRows(9999);
        // The last bucket is cleared because the row that marked it is gone, and the
        // scan rewinds to that bucket's first row — never further.
        QVERIFY(m.scanned(DensityMap::Lane::Rules) >= 9999 - m.rowsPerBucket());
        QVERIFY(!m.anyMark(DensityMap::Lane::Rules));
        p.markedRow = -1;
        scanAll(m, DensityMap::Lane::Rules, p.fn());
        QVERIFY(p.calls - 10000 <= m.rowsPerBucket());
    }

    // A rebind is the one thing that throws everything away, because a view row now
    // names a different record.
    void rebindingDropsEverything()
    {
        DensityMap m;
        m.rebind(500);
        scanAll(m, DensityMap::Lane::Rules,
                [](int row) -> qint16 { return row == 5 ? 0 : DensityMap::kNothing; });
        QVERIFY(m.anyMark(DensityMap::Lane::Rules));
        m.rebind(500);
        QVERIFY(!m.anyMark(DensityMap::Lane::Rules));
        QCOMPARE(m.scanned(DensityMap::Lane::Rules), 0);
        QCOMPARE(m.rowsPerBucket(), 1);
    }

    // Every row belongs to exactly one bucket and every bucket to a real row range —
    // the arithmetic the paint path places its bands with.
    void everyRowLandsInABucketThatCoversIt()
    {
        DensityMap m;
        m.rebind(3 * DensityMap::kMaxBuckets + 7);
        for (const int row : {0, 1, 100, m.rows() / 2, m.rows() - 1}) {
            const int b = m.bucketOf(row);
            QVERIFY(b >= 0 && b < m.bucketCount());
            QVERIFY(m.firstRowOf(b) <= row);
            QVERIFY(row < m.firstRowOf(b) + m.rowsPerBucket());
        }
    }
};

QTEST_APPLESS_MAIN(TestDensityMap)
#include "tst_densitymap.moc"
