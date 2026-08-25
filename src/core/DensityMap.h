#pragma once

#include <QVector>

#include <functional>

namespace loftail {

// A coarse map of WHERE things are in a view — the data behind the density strip
// beside the scrollbar (SPEC.md §5, ARCHITECTURE.md §7.1.7).
//
// The strip answers one question: "is there anything I care about further down?" That
// is a question about the WHOLE view, and the view holds millions of records, so it
// cannot be answered on the paint path and it cannot be answered by sampling — a single
// FATAL in ten million records is exactly what the strip exists to point at, and any
// scheme that looks at some of the records misses precisely that one.
//
// So it is answered by a forward scan that is BOUNDED PER SLICE and resumable: the
// owner hands over a row budget whenever it can afford one, and what has been scanned
// so far is what the strip draws. The strip therefore fills in rather than appearing
// complete, which is the honest rendering of a bounded answer — the same ruling Find's
// "47+" tally makes one level up (ARCHITECTURE.md §7.1.3), differing only in that this
// one converges because nothing throws the work away.
//
// The answers are kept in BUCKETS of consecutive view rows, never per record: the strip
// is a couple of hundred pixels tall, so a per-record answer would be a hundred thousand
// times more detail than can be drawn, and holding one per record is exactly the
// per-record parsed state invariant #1 forbids. A bucket holds a fixed number of ROWS
// rather than a fixed fraction of the view, which is what makes an APPEND free: a log
// that grows by a record adds rows to the last bucket and eventually a bucket, and
// nothing already scanned moves. When the count would exceed kMaxBuckets the buckets
// are MERGED pairwise and the row-per-bucket count doubles — coarser, but still
// covering every row that had been scanned, so a tailed log never rescans itself.
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
    // What a bucket holds when nothing in it matched. -1 rather than 0 because 0 is a
    // legitimate answer in both lanes (rule 0 is the first rule, and the find lane
    // stores 0 for "matched").
    static constexpr qint16 kNothing = -1;

    // The finest the map is ever kept. Deliberately well above any strip height — the
    // strip downsamples at paint time, and buckets finer than pixels would be detail
    // nothing can draw — but low enough that a whole-map pass is free: at 2048 the
    // paint's per-bucket O(log n) line lookup comes to tens of microseconds.
    static constexpr int kMaxBuckets = 2048;

    enum class Lane {
        Rules = 0, // the first-match-wins highlight rule index colouring a row
        Find  = 1, // 0 where the armed Find query matches a row
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
    // probe answers what to record for that row — a rule index, 0 for a find match, or
    // kNothing. Returns how many rows were actually scanned, which is 0 when the lane is
    // already complete. Nothing here decides how big a budget is affordable; that is the
    // owner's, and it is what keeps the cost off this class.
    int scan(Lane lane, int budgetRows, const std::function<qint16(int)> &probe);

    // What bucket `bucket` holds in this lane, or kNothing.
    qint16 at(Lane lane, int bucket) const;

    // Whether this lane has anything to draw at all.
    bool anyMark(Lane lane) const;

private:
    struct LaneState {
        QVector<qint16> bucket;
        int             scanned = 0;
    };

    LaneState &state(Lane lane) { return m_lane[int(lane)]; }
    const LaneState &state(Lane lane) const { return m_lane[int(lane)]; }

    // What two answers become when their buckets are merged. A present answer beats an
    // absent one, and the LOWER of two rule indices wins — first-match-wins order is
    // severity order by convention (HighlighterSet::defaults orders FATAL above ERROR
    // above WARN), so merging keeps the louder of the two rather than the later one. In
    // the find lane both present answers are 0, so the same rule reads as "any match".
    static qint16 merge(qint16 a, qint16 b);

    // Bring the bucket geometry into line with m_rows, merging pairwise while there
    // would be more than kMaxBuckets of them. Never splits: a split would have to invent
    // which half of a merged bucket a mark came from.
    void reshape();

    int m_rows = 0;
    int m_rowsPerBucket = 1;
    LaneState m_lane[kLaneCount];
};

} // namespace loftail
