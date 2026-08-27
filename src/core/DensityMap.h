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

#pragma once

#include <QVector>

#include <functional>

namespace loftail {

// A coarse map of WHERE things are in a view — the data behind the density marks in the
// scrollbar (SPEC.md §5, ARCHITECTURE.md §7.1.7).
//
// The marks answer one question: "is there anything I care about further down?" That
// is a question about the WHOLE view, and the view holds millions of records, so it
// cannot be answered on the paint path and it cannot be answered by sampling — a single
// FATAL in ten million records is exactly what the marks exist to point at, and any
// scheme that looks at some of the records misses precisely that one.
//
// So it is answered by a forward scan that is BOUNDED PER SLICE and resumable: the
// owner hands over a row budget whenever it can afford one, and what has been scanned
// so far is what the bar draws. The marks therefore fill in rather than appearing
// complete, which is the honest rendering of a bounded answer — the same ruling Find's
// "47+" tally makes one level up (ARCHITECTURE.md §7.1.3), differing only in that this
// one converges because nothing throws the work away.
//
// The answers are kept in BUCKETS of consecutive view rows, never per record: the bar
// is a couple of hundred pixels tall, so a per-record answer would be a hundred thousand
// times more detail than can be drawn, and holding one per record is exactly the
// per-record parsed state invariant #1 forbids. A bucket holds a fixed number of ROWS
// rather than a fixed fraction of the view, which is what makes an APPEND free: a log
// that grows by a record adds rows to the last bucket and eventually a bucket, and
// nothing already scanned moves. When the count would exceed kMaxBuckets the buckets
// are MERGED pairwise and the row-per-bucket count doubles — coarser, but still
// covering every row that had been scanned, so a tailed log never rescans itself.
//
// A bucket holds a BIT PER MARK CLASS and not one winning answer, and that is what
// makes a lone record of one colour survive beside a thousand of another. It used to
// hold a single rule index, merged by keeping the lower one on the convention that
// first-match-wins order is severity order — so a bucket carrying one WARN and one
// FATAL reported only the FATAL, and on a big log, where a bucket is thousands of rows,
// whole colours simply vanished from the bar. A bitmask merges by OR, which loses
// nothing and needs no convention at all; deciding what to DRAW where two classes share
// a pixel is the paint path's business (it gives each class a column of its own), and
// this class no longer has an opinion about it.
//
// Two LANES share one bucket geometry, because they are invalidated by different things
// and would otherwise throw each other's work away: highlight rules change when the user
// edits them (rarely), and the Find query changes on every keystroke.
//
// No QObject and no timer: the slice budget comes from the owner, so this is pure,
// testable without a QApplication, and cannot start work of its own accord.
class DensityMap
{
public:
    // What a bucket holds: a set of mark classes, one bit each. In the rule lane a class
    // is a highlight rule's index; in the find lane there is only class 0, "matched".
    using Marks = quint32;

    // What a bucket holds when nothing in it matched.
    static constexpr Marks kNone = 0;

    // How many classes a bucket can tell apart. A rule index at or past this folds into
    // the last class and is drawn in that rule's colour — the alternative is a wider
    // bucket for a case (thirty-one distinct highlight rules all firing in one log) that
    // costs a colour rather than a mark.
    static constexpr int kClassCount = 31;

    // The bit standing for one class. Clamped rather than asserted, because the caller's
    // class is a rule index and a rule list has no bound.
    static constexpr Marks classBit(int cls)
    {
        return Marks(1) << Marks(cls < 0 ? 0 : (cls >= kClassCount ? kClassCount - 1 : cls));
    }

    // The lowest class in `marks`, or -1 when there are none. The lowest is the loudest
    // where the classes are rule indices, first-match-wins order being severity order by
    // convention (ARCHITECTURE.md §7.5.4) — so this is what a renderer with room for one
    // colour asks for.
    static int lowestClass(Marks marks);

    // The finest the map is ever kept. Deliberately well above any bar height — the
    // paint downsamples, and buckets finer than pixels would be detail nothing can draw —
    // but low enough that a whole-map pass is free: at 2048 the paint's per-bucket
    // O(log n) line lookup comes to tens of microseconds.
    static constexpr int kMaxBuckets = 2048;

    enum class Lane {
        Rules = 0, // which highlight rules colour the rows in a bucket
        Find  = 1, // class 0 where the armed Find query matches a row
    };
    static constexpr int kLaneCount = 2;

    // A new row space: a filter re-apply, a rotation rescan, a run switch. Nothing is
    // kept, because a view row now names a different record.
    void rebind(int rows);

    // The SAME row space, resized — records appended, or the provisional trailing record
    // withdrawn for re-evaluation (which happens on every ingest tick of a filtered live
    // log, and so may not cost a rescan). Growth keeps everything; a shrink drops only
    // the buckets past the new end and rewinds the scan to the start of the last
    // surviving bucket, since that bucket may be carrying a mark from a row that is gone.
    void setRows(int rows);

    int rows() const { return m_rows; }
    int rowsPerBucket() const { return m_rowsPerBucket; }
    int bucketCount() const { return int(m_lane[0].bucket.size()); }
    int bucketOf(int row) const;
    int firstRowOf(int bucket) const;

    // Forget one lane's answers and start it again from row 0. The other lane keeps
    // everything it has.
    void clear(Lane lane);

    // How far this lane has been scanned, and whether that is all of it.
    int scanned(Lane lane) const { return state(lane).scanned; }
    bool complete(Lane lane) const { return state(lane).scanned >= m_rows; }

    // Scan at most `budgetRows` more rows of one lane, calling `probe` for each. The
    // probe answers what to record for that row — classBit() of a rule index, of 0 for a
    // find match, or kNone. Returns how many rows were actually scanned, which is 0 when
    // the lane is already complete. Nothing here decides how big a budget is affordable;
    // that is the owner's, and it is what keeps the cost off this class.
    int scan(Lane lane, int budgetRows, const std::function<Marks(int)> &probe);

    // What bucket `bucket` holds in this lane, or kNone.
    Marks at(Lane lane, int bucket) const;

    // Every class this lane has found anywhere, which is what the paint path allocates
    // its columns from: a colour with nothing in the log gets no width.
    Marks unionMask(Lane lane) const;

    // Whether this lane has anything to draw at all.
    bool anyMark(Lane lane) const { return unionMask(lane) != kNone; }

private:
    struct LaneState {
        QVector<Marks> bucket;
        int            scanned = 0;
    };

    LaneState &state(Lane lane) { return m_lane[int(lane)]; }
    const LaneState &state(Lane lane) const { return m_lane[int(lane)]; }

    // Bring the bucket geometry into line with m_rows, merging pairwise while there
    // would be more than kMaxBuckets of them. Merging is a bitwise OR, so it is exact:
    // no class is lost, whatever a bucket comes to cover. Never splits — a split would
    // have to invent which half of a merged bucket a mark came from.
    void reshape();

    int m_rows = 0;
    int m_rowsPerBucket = 1;
    LaneState m_lane[kLaneCount];
};

} // namespace loftail
