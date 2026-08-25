#include "DensityMap.h"

#include <QtGlobal>

#include <algorithm>

namespace loftail {

qint16 DensityMap::merge(qint16 a, qint16 b)
{
    if (a == kNothing)
        return b;
    if (b == kNothing)
        return a;
    return qMin(a, b);
}

int DensityMap::bucketOf(int row) const
{
    if (m_rowsPerBucket <= 0)
        return 0;
    const int b = row / m_rowsPerBucket;
    return qBound(0, b, qMax(0, bucketCount() - 1));
}

int DensityMap::firstRowOf(int bucket) const
{
    return bucket * m_rowsPerBucket;
}

void DensityMap::rebind(int rows)
{
    m_rows = qMax(0, rows);
    m_rowsPerBucket = 1;
    for (LaneState &lane : m_lane) {
        lane.bucket.clear();
        lane.scanned = 0;
    }
    reshape();
}

void DensityMap::setRows(int rows)
{
    const int n = qMax(0, rows);
    if (n == m_rows)
        return;

    if (n < m_rows) {
        // A shrink. Only the tail went, so everything before the last surviving bucket
        // stands — but that bucket itself may hold a mark contributed by a row that is
        // now gone, and nothing records which row put it there. So it is cleared and the
        // scan rewound to its first row: at most rowsPerBucket rows are re-scanned,
        // which is what keeps a filtered live log's every-tick withdrawal of its
        // provisional record from costing a full rescan.
        m_rows = n;
        reshape();
        const int last = bucketCount() - 1;
        if (last >= 0) {
            const int from = firstRowOf(last);
            for (LaneState &lane : m_lane) {
                lane.bucket[last] = kNothing;
                lane.scanned = qMin(lane.scanned, from);
            }
        } else {
            for (LaneState &lane : m_lane)
                lane.scanned = 0;
        }
        return;
    }

    m_rows = n;
    reshape();
}

void DensityMap::reshape()
{
    if (m_rows <= 0) {
        for (LaneState &lane : m_lane) {
            lane.bucket.clear();
            lane.scanned = 0;
        }
        m_rowsPerBucket = 1;
        return;
    }

    // Coarsen until the map fits. Pairwise, so every merge is exact over what has
    // already been scanned — no row loses its answer and `scanned` still means the same
    // thing, which is why a log can grow all day without ever rescanning itself.
    while ((m_rows + m_rowsPerBucket - 1) / m_rowsPerBucket > kMaxBuckets) {
        for (LaneState &lane : m_lane) {
            const int oldCount = int(lane.bucket.size());
            const int newCount = (oldCount + 1) / 2;
            QVector<qint16> merged(newCount, kNothing);
            for (int i = 0; i < oldCount; ++i)
                merged[i / 2] = merge(merged.at(i / 2), lane.bucket.at(i));
            lane.bucket = std::move(merged);
        }
        m_rowsPerBucket *= 2;
    }

    const int want = (m_rows + m_rowsPerBucket - 1) / m_rowsPerBucket;
    for (LaneState &lane : m_lane) {
        if (lane.bucket.size() > want)
            lane.bucket.resize(want);
        else
            while (lane.bucket.size() < want)
                lane.bucket.append(kNothing);
        lane.scanned = qMin(lane.scanned, m_rows);
    }
}

void DensityMap::clear(Lane lane)
{
    LaneState &s = state(lane);
    s.bucket.fill(kNothing); // fill(), not assign(): QList::assign is Qt 6.6, the floor is 6.4
    s.scanned = 0;
}

int DensityMap::scan(Lane lane, int budgetRows, const std::function<qint16(int)> &probe)
{
    LaneState &s = state(lane);
    if (budgetRows <= 0 || s.scanned >= m_rows || s.bucket.isEmpty())
        return 0;

    const int end = qMin(m_rows, s.scanned + budgetRows);
    for (int row = s.scanned; row < end; ++row) {
        const qint16 answer = probe(row);
        if (answer == kNothing)
            continue;
        const int b = bucketOf(row);
        s.bucket[b] = merge(s.bucket.at(b), answer);
    }
    const int done = end - s.scanned;
    s.scanned = end;
    return done;
}

qint16 DensityMap::at(Lane lane, int bucket) const
{
    const LaneState &s = state(lane);
    if (bucket < 0 || bucket >= s.bucket.size())
        return kNothing;
    return s.bucket.at(bucket);
}

bool DensityMap::anyMark(Lane lane) const
{
    return std::ranges::any_of(state(lane).bucket,
                               [](qint16 v) { return v != kNothing; });
}

} // namespace loftail
