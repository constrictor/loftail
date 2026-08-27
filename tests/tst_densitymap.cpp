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
        int answerClass = 0;
        int calls = 0;
        std::function<DensityMap::Marks(int)> fn()
        {
            return [this](int row) -> DensityMap::Marks {
                ++calls;
                return row == markedRow ? DensityMap::classBit(answerClass) : DensityMap::kNone;
            };
        }
    };

    static void scanAll(DensityMap &m, DensityMap::Lane lane,
                        const std::function<DensityMap::Marks(int)> &probe)
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
        QCOMPARE(m.at(DensityMap::Lane::Rules, m.bucketOf(900)), DensityMap::classBit(0));
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
        QCOMPARE(m.at(DensityMap::Lane::Rules, m.bucketOf(50)), DensityMap::classBit(0));
    }

    // Merging a bucket keeps BOTH rules, which is what stops a lone record of one
    // colour disappearing beside a common record of another. A bucket of a big log
    // covers thousands of rows, and while it held a single winning rule index the
    // quieter colour was simply not on the bar anywhere.
    void mergingTwoBucketsKeepsBothRules()
    {
        DensityMap m;
        m.rebind(4 * DensityMap::kMaxBuckets);
        // Row 0 is rule 3, row 1 is rule 1 — adjacent, so they share a bucket at once.
        scanAll(m, DensityMap::Lane::Rules, [](int row) -> DensityMap::Marks {
            if (row == 0)
                return DensityMap::classBit(3);
            if (row == 1)
                return DensityMap::classBit(1);
            return DensityMap::kNone;
        });
        const DensityMap::Marks both = m.at(DensityMap::Lane::Rules, m.bucketOf(0));
        QVERIFY(both & DensityMap::classBit(1));
        QVERIFY(both & DensityMap::classBit(3));
        // And a renderer with room for one colour is told which of them is the loudest.
        QCOMPARE(DensityMap::lowestClass(both), 1);
        QCOMPARE(DensityMap::lowestClass(DensityMap::kNone), -1);
    }

    // A rule index past what a bucket can tell apart folds into the last class rather
    // than being dropped: a mark in the wrong colour still says a record is there, and
    // no mark at all says nothing.
    void aRuleIndexPastTheLastClassStillMarks()
    {
        QCOMPARE(DensityMap::classBit(DensityMap::kClassCount),
                 DensityMap::classBit(DensityMap::kClassCount - 1));
        QCOMPARE(DensityMap::classBit(-1), DensityMap::classBit(0));
    }

    void theTwoLanesAreInvalidatedIndependently()
    {
        DensityMap m;
        m.rebind(200);
        scanAll(m, DensityMap::Lane::Rules, [](int row) -> DensityMap::Marks {
            return row == 10 ? DensityMap::classBit(0) : DensityMap::kNone;
        });
        scanAll(m, DensityMap::Lane::Find, [](int row) -> DensityMap::Marks {
            return row == 20 ? DensityMap::classBit(0) : DensityMap::kNone;
        });
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
        scanAll(m, DensityMap::Lane::Rules, [](int row) -> DensityMap::Marks {
            return row == 5 ? DensityMap::classBit(0) : DensityMap::kNone;
        });
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
