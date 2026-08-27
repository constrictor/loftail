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

#include "EstimatedGeometry.h"

#include "RecordIndex.h"

#include <algorithm>
#include <cmath>

namespace loftail {

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void EstimatedGeometry::clear()
{
    m_idx = nullptr;
    m_blockPhysical.clear();
    m_blockLines.clear();
    m_blockMeasuredPhysical.clear();
    m_recordLines.clear();
    m_prefix.clear();
    m_total = 0;
    m_measuredVisual = 0;
    m_measuredPhysical = 0;
    m_measuredBlocks = 0;
    m_boundRecords = 0;
    m_tailLines = 0;
}

void EstimatedGeometry::reset(const RecordIndex *idx, int wrapWidth)
{
    clear();
    m_idx = idx;
    m_wrapWidth = qMax(1, wrapWidth);
    if (!m_idx)
        return;

    const int n = int(m_idx->records.size());
    const int blocks = (n + RecordIndex::kBlockSize - 1) / RecordIndex::kBlockSize;
    m_blockPhysical.resize(blocks);
    m_blockLines.fill(-1, blocks); // all unmeasured
    m_blockMeasuredPhysical.fill(0, blocks);
    m_boundRecords = n;
    m_tailLines = n > 0 ? int(RecordIndex::displayLines(m_idx->records.at(n - 1))) : 0;

    // Per-block physical (display, capped) line totals, straight from the index's
    // own prefix sums — one pass, column-independent, computed once.
    for (int b = 0; b < blocks; ++b) {
        const int start = b * RecordIndex::kBlockSize;
        const int end = qMin(start + RecordIndex::kBlockSize, n);
        m_blockPhysical[b] = m_idx->firstLineOfRecord(end) - m_idx->firstLineOfRecord(start);
    }
    rebuild();
}

// ---------------------------------------------------------------------------
// Block identity helpers
// ---------------------------------------------------------------------------

int EstimatedGeometry::blockOfRecord(int r)
{
    return r / RecordIndex::kBlockSize;
}

int EstimatedGeometry::blockStartRecord(int block)
{
    return block * RecordIndex::kBlockSize;
}

int EstimatedGeometry::recordsInBlock(int block) const
{
    if (!m_idx)
        return 0;
    const int n = int(m_idx->records.size());
    const int start = block * RecordIndex::kBlockSize;
    return qMax(0, qMin(start + RecordIndex::kBlockSize, n) - start);
}

const QVector<quint16> *EstimatedGeometry::cachedLines(int block) const
{
    // constFind, never operator[]: QHash's const subscript returns the value BY
    // VALUE, so the obvious spelling copies up to kBlockSize heights on a path
    // recordHeightLines() walks once per painted record.
    const auto it = m_recordLines.constFind(block);
    return it == m_recordLines.cend() ? nullptr : &it.value();
}

int EstimatedGeometry::measuredRecordsInBlock(int block) const
{
    if (block < 0 || block >= m_blockLines.size())
        return 0;
    const QVector<quint16> *lines = cachedLines(block);
    return lines ? int(lines->size()) : 0;
}

bool EstimatedGeometry::isBlockMeasured(int block) const
{
    const int count = recordsInBlock(block);
    return count > 0 && measuredRecordsInBlock(block) == count;
}

double EstimatedGeometry::expansionFactor() const
{
    if (m_measuredPhysical <= 0)
        return 1.0;
    const double f = double(m_measuredVisual) / double(m_measuredPhysical);
    return f < 1.0 ? 1.0 : f; // wrapping never shrinks a record below physical
}

// ---------------------------------------------------------------------------
// Wrap width / invalidation
// ---------------------------------------------------------------------------

bool EstimatedGeometry::setWrapWidth(int wrapWidth)
{
    wrapWidth = qMax(1, wrapWidth);
    if (wrapWidth == m_wrapWidth)
        return false;
    m_wrapWidth = wrapWidth;
    invalidateMeasurements();
    return true;
}

void EstimatedGeometry::invalidateMeasurements()
{
    // Heights are width-dependent: drop every measurement and fall back to
    // estimates. The caller debounces so a drag-resize lands here once.
    m_blockLines.fill(-1); // keep size, drop every measurement
    m_blockMeasuredPhysical.fill(0);
    m_recordLines.clear();
    m_measuredVisual = 0;
    m_measuredPhysical = 0;
    m_measuredBlocks = 0;
    rebuild();
}

// ---------------------------------------------------------------------------
// Measurement
// ---------------------------------------------------------------------------

void EstimatedGeometry::measureBlock(int block, const QVector<quint16> &visualLines, int first)
{
    if (!m_idx || block < 0 || block >= m_blockLines.size())
        return;
    const int count = recordsInBlock(block);
    if (count <= 0)
        return;
    first = std::max(first, 0);
    const int have = measuredRecordsInBlock(block);
    if (first > have)
        return; // a run starting past the measured prefix would leave a hole
    if (first == 0 && have == count)
        return; // already measured, whole, at this width

    const QVector<quint16> *cached = cachedLines(block);
    QVector<quint16> lines = cached ? *cached : QVector<quint16>();
    lines.resize(first);
    lines.reserve(qMin(count, first + int(visualLines.size())));
    for (quint16 v : visualLines) {
        if (lines.size() >= count)
            break;
        lines.append(v);
    }

    setBlockMeasurement(block, lines);
    refreshTotals();
}

// Write one block's measured prefix and the two aggregates derived from it. The
// physical total is read from the LIVE records, which is sound at both call sites:
// measureBlock()'s caller has just measured them, and truncateBlockMeasurement()
// keeps only records the tail growth cannot have touched.
void EstimatedGeometry::setBlockMeasurement(int block, const QVector<quint16> &lines)
{
    if (lines.isEmpty()) {
        m_recordLines.remove(block);
        m_blockLines[block] = -1;
        m_blockMeasuredPhysical[block] = 0;
        return;
    }
    qint64 visual = 0;
    for (quint16 v : lines)
        visual += v;
    qint64 physical = 0;
    const int start = blockStartRecord(block);
    for (int i = 0; i < lines.size(); ++i)
        physical += RecordIndex::displayLines(m_idx->records.at(start + i));

    m_recordLines.insert(block, lines);
    m_blockLines[block] = visual;
    m_blockMeasuredPhysical[block] = physical;
}

void EstimatedGeometry::truncateBlockMeasurement(int block, int keep)
{
    if (block < 0 || block >= m_blockLines.size())
        return;
    const QVector<quint16> *cached = cachedLines(block);
    if (!cached || int(cached->size()) <= qMax(0, keep))
        return;
    QVector<quint16> kept = *cached;
    kept.resize(qMax(0, keep));
    setBlockMeasurement(block, kept);
}

void EstimatedGeometry::refreshTotals()
{
    // O(blocks) — the same order as rebuild(), which follows it — so the three
    // running aggregates are recomputed rather than tracked by deltas. A delta is
    // one more thing to get wrong on a path where blocks appear, grow and lose
    // their measured tail on every ingest tick.
    m_measuredVisual = 0;
    m_measuredPhysical = 0;
    m_measuredBlocks = 0;
    for (int b = 0; b < m_blockLines.size(); ++b) {
        if (m_blockLines.at(b) >= 0) {
            m_measuredVisual += m_blockLines.at(b);
            m_measuredPhysical += m_blockMeasuredPhysical.at(b);
        }
        if (isBlockMeasured(b))
            ++m_measuredBlocks;
    }
    rebuild();
}

bool EstimatedGeometry::syncTail()
{
    if (!m_idx)
        return false;
    const int n = int(m_idx->records.size());
    const int tail = n > 0 ? int(RecordIndex::displayLines(m_idx->records.at(n - 1))) : 0;
    if (n == m_boundRecords && tail == m_tailLines)
        return false;

    // The bound index is append-only and only its TRAILING record changes height in
    // place (invariant #9), so the old trailing record is the first that can have
    // moved — and it is included whether or not the count changed, because a tick
    // may both grow it and append after it.
    const int firstDirty = qMax(0, qMin(m_boundRecords, n) - 1);
    const int firstBlock = firstDirty / RecordIndex::kBlockSize;
    const int blocks = (n + RecordIndex::kBlockSize - 1) / RecordIndex::kBlockSize;
    m_boundRecords = n;
    m_tailLines = tail;

    // Every block past the dirty one is new ground or is gone; the dirty one keeps
    // the records in front of firstDirty, which nothing has touched.
    for (auto it = m_recordLines.begin(); it != m_recordLines.end();)
        it = (it.key() > firstBlock || it.key() >= blocks) ? m_recordLines.erase(it) : ++it;
    for (int b = firstBlock + 1; b < m_blockLines.size(); ++b) {
        m_blockLines[b] = -1;
        m_blockMeasuredPhysical[b] = 0;
    }

    const int had = int(m_blockLines.size());
    m_blockPhysical.resize(blocks);
    m_blockLines.resize(blocks);
    m_blockMeasuredPhysical.resize(blocks);
    for (int b = had; b < blocks; ++b) {
        m_blockLines[b] = -1; // resize() default-constructs to 0, which reads as measured
        m_blockMeasuredPhysical[b] = 0;
    }

    // Physical totals of the blocks the growth reached, straight from the index's
    // own (already extended) prefix sums.
    for (int b = firstBlock; b < blocks; ++b) {
        const int start = b * RecordIndex::kBlockSize;
        const int end = qMin(start + RecordIndex::kBlockSize, n);
        m_blockPhysical[b] = m_idx->firstLineOfRecord(end) - m_idx->firstLineOfRecord(start);
    }

    truncateBlockMeasurement(firstBlock, firstDirty - firstBlock * RecordIndex::kBlockSize);
    refreshTotals();
    return true;
}

// ---------------------------------------------------------------------------
// Totals
// ---------------------------------------------------------------------------

qint64 EstimatedGeometry::blockLines(int block) const
{
    if (block < 0 || block >= m_blockPhysical.size())
        return 0;
    const qint64 measured = qMax<qint64>(0, m_blockLines.at(block));
    if (isBlockMeasured(block))
        return measured; // measured whole, exact
    // Estimated: the physical lines the measured prefix does NOT cover, scaled by
    // the running expansion factor. Never below the physical count (expansion >= 1)
    // so a block cannot estimate shorter than it can possibly render. With nothing
    // measured — the ordinary unvisited block — the prefix is empty and this is the
    // whole block, exactly as it always was.
    const qint64 rest = qMax<qint64>(0, m_blockPhysical.at(block) - m_blockMeasuredPhysical.at(block));
    const double est = double(rest) * expansionFactor();
    return measured + qMax(rest, qint64(std::llround(est)));
}

void EstimatedGeometry::rebuild()
{
    const int blocks = int(m_blockPhysical.size());
    m_prefix.resize(blocks + 1);
    qint64 acc = 0;
    for (int b = 0; b < blocks; ++b) {
        m_prefix[b] = acc;
        acc += blockLines(b);
    }
    m_prefix[blocks] = acc;
    m_total = acc;
}

// ---------------------------------------------------------------------------
// Mapping
// ---------------------------------------------------------------------------

qint64 EstimatedGeometry::firstLineOfRecord(int r) const
{
    if (!m_idx || r <= 0 || m_idx->records.isEmpty())
        return 0;
    const int n = int(m_idx->records.size());
    if (r >= n)
        return m_total;

    const int block = blockOfRecord(r);
    const int start = blockStartRecord(block);
    const qint64 base = m_prefix.at(block);
    const int have = measuredRecordsInBlock(block);

    if (r - start <= have) {
        // Inside the block's measured prefix: exact, by summing the heights.
        const QVector<quint16> *lines = cachedLines(block);
        qint64 line = base;
        for (int i = 0; lines && i < r - start; ++i)
            line += lines->at(i);
        return line;
    }

    // Past the measured prefix: the exact prefix, then map the remainder
    // proportionally by physical position, so the estimate is monotonic and
    // consistent with recordAtLine's inverse.
    const qint64 exact = qMax<qint64>(0, m_blockLines.at(block));
    const qint64 physMeasured = m_blockMeasuredPhysical.at(block);
    const qint64 physRest = m_blockPhysical.at(block) - physMeasured;
    const qint64 estRest = blockLines(block) - exact;
    if (physRest <= 0)
        return base + exact;
    const qint64 physBefore =
        m_idx->firstLineOfRecord(r) - m_idx->firstLineOfRecord(start) - physMeasured;
    return base + exact
           + qint64(std::llround(double(qMax<qint64>(0, physBefore)) * double(estRest)
                                 / double(physRest)));
}

int EstimatedGeometry::recordAtLine(qint64 line) const
{
    if (!m_idx)
        return -1;
    const int n = int(m_idx->records.size());
    if (n == 0)
        return -1;
    if (line <= 0)
        return 0;
    if (line >= m_total)
        return n - 1;

    // Binary search the block prefix: largest b with m_prefix[b] <= line.
    int lo = 0;
    int hi = int(m_blockPhysical.size()) - 1;
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        if (m_prefix.at(mid) <= line)
            lo = mid;
        else
            hi = mid - 1;
    }
    const int block = lo;
    const int start = blockStartRecord(block);
    const int count = recordsInBlock(block);
    const qint64 local = line - m_prefix.at(block);

    const int have = qMin(measuredRecordsInBlock(block), count);
    const qint64 exact = qMax<qint64>(0, m_blockLines.at(block));
    if (const QVector<quint16> *lines = cachedLines(block); lines && local < exact) {
        qint64 acc = 0;
        for (int i = 0; i < have; ++i) {
            acc += lines->at(i);
            if (local < acc)
                return start + i;
        }
    }
    if (have >= count)
        return start + count - 1;

    // Past the measured prefix: invert the proportional map over what is left.
    // Find the physical offset the line corresponds to, then the record whose
    // physical span contains it.
    const qint64 physRest = m_blockPhysical.at(block) - m_blockMeasuredPhysical.at(block);
    const qint64 estRest = blockLines(block) - exact;
    if (physRest <= 0 || estRest <= 0)
        return start + have;
    const qint64 localRest = qMax<qint64>(0, local - exact);
    const auto targetPhys = qint64(double(localRest) * double(physRest) / double(estRest));
    // Walk the block's physical heights directly (O(1) per record) rather than
    // differencing firstLineOfRecord, which would make this O(blockSize^2).
    qint64 acc = 0;
    for (int i = have; i < count; ++i) {
        acc += RecordIndex::displayLines(m_idx->records.at(start + i));
        if (targetPhys < acc)
            return start + i;
    }
    return start + count - 1;
}

int EstimatedGeometry::recordHeightLines(int r) const
{
    if (!m_idx || r < 0 || r >= m_idx->records.size())
        return 1;
    const int block = blockOfRecord(r);
    const int i = r - blockStartRecord(block);
    // Belt and braces: the cached prefix is asked for its SIZE, never assumed to
    // span the block. A measured block whose records grew under it is the whole of
    // bugs.md 1 — an at() here read past the end of the vector, aborting a debug
    // build and returning allocation slack in a release one.
    if (const QVector<quint16> *lines = cachedLines(block); lines && i >= 0 && i < lines->size())
        return lines->at(i);
    const int phys = RecordIndex::displayLines(m_idx->records.at(r));
    const int est = int(std::llround(double(phys) * expansionFactor()));
    return qBound(1, qMax(phys, est), int(RecordIndex::kDisplayLineCap));
}

} // namespace loftail
