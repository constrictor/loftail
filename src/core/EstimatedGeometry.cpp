#include "EstimatedGeometry.h"

#include "RecordIndex.h"

#include <cmath>

namespace loftail {

// ---------------------------------------------------------------------------
// Pure wrap math
// ---------------------------------------------------------------------------

int EstimatedGeometry::visualLinesForChars(int chars, int cols)
{
    if (cols <= 0)
        return 1;
    if (chars <= cols)
        return 1;
    return (chars + cols - 1) / cols; // ceil(chars / cols)
}

int EstimatedGeometry::measuredRecordLines(const QVector<int> &lineChars, int cols, int cap)
{
    int lines = 0;
    for (int chars : lineChars) {
        lines += visualLinesForChars(chars, cols);
        if (lines >= cap)
            return cap;
    }
    return qMax(1, qMin(lines, cap));
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void EstimatedGeometry::clear()
{
    m_idx = nullptr;
    m_blockPhysical.clear();
    m_blockLines.clear();
    m_recordLines.clear();
    m_prefix.clear();
    m_total = 0;
    m_measuredVisual = 0;
    m_measuredPhysical = 0;
    m_measuredBlocks = 0;
}

void EstimatedGeometry::reset(const RecordIndex *idx, int cols)
{
    clear();
    m_idx = idx;
    m_cols = qMax(1, cols);
    if (!m_idx)
        return;

    const int n = m_idx->records.size();
    const int blocks = (n + RecordIndex::kBlockSize - 1) / RecordIndex::kBlockSize;
    m_blockPhysical.resize(blocks);
    m_blockLines.fill(-1, blocks); // all unmeasured

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

int EstimatedGeometry::blockOfRecord(int r) const
{
    return r / RecordIndex::kBlockSize;
}

int EstimatedGeometry::blockStartRecord(int block) const
{
    return block * RecordIndex::kBlockSize;
}

int EstimatedGeometry::recordsInBlock(int block) const
{
    if (!m_idx)
        return 0;
    const int n = m_idx->records.size();
    const int start = block * RecordIndex::kBlockSize;
    return qMax(0, qMin(start + RecordIndex::kBlockSize, n) - start);
}

bool EstimatedGeometry::isBlockMeasured(int block) const
{
    return block >= 0 && block < m_blockLines.size() && m_blockLines.at(block) >= 0;
}

double EstimatedGeometry::expansionFactor() const
{
    if (m_measuredPhysical <= 0)
        return 1.0;
    const double f = double(m_measuredVisual) / double(m_measuredPhysical);
    return f < 1.0 ? 1.0 : f; // wrapping never shrinks a record below physical
}

// ---------------------------------------------------------------------------
// Columns / invalidation
// ---------------------------------------------------------------------------

bool EstimatedGeometry::setColumns(int cols)
{
    cols = qMax(1, cols);
    if (cols == m_cols)
        return false;
    m_cols = cols;
    // Heights are width-dependent: drop every measurement and fall back to
    // estimates. The caller debounces so a drag-resize lands here once.
    m_blockLines.fill(-1); // keep size, drop every measurement
    m_recordLines.clear();
    m_measuredVisual = 0;
    m_measuredPhysical = 0;
    m_measuredBlocks = 0;
    rebuild();
    return true;
}

// ---------------------------------------------------------------------------
// Measurement
// ---------------------------------------------------------------------------

void EstimatedGeometry::measureBlock(int block, const QVector<quint16> &visualLines)
{
    if (!m_idx || block < 0 || block >= m_blockLines.size())
        return;
    if (isBlockMeasured(block))
        return;

    qint64 visual = 0;
    for (quint16 v : visualLines)
        visual += v;

    m_blockLines[block] = visual;
    m_recordLines.insert(block, visualLines);
    m_measuredVisual += visual;
    m_measuredPhysical += m_blockPhysical.at(block);
    ++m_measuredBlocks;

    rebuild();
}

// ---------------------------------------------------------------------------
// Totals
// ---------------------------------------------------------------------------

qint64 EstimatedGeometry::blockLines(int block) const
{
    if (block < 0 || block >= m_blockPhysical.size())
        return 0;
    if (m_blockLines.at(block) >= 0)
        return m_blockLines.at(block); // measured, exact
    // Estimated: physical lines scaled by the running expansion factor. Never
    // below the physical count (expansion >= 1) so a block cannot estimate
    // shorter than it can possibly render.
    const double est = double(m_blockPhysical.at(block)) * expansionFactor();
    return qMax(m_blockPhysical.at(block), qint64(std::llround(est)));
}

void EstimatedGeometry::rebuild()
{
    const int blocks = m_blockPhysical.size();
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
    const int n = m_idx->records.size();
    if (r >= n)
        return m_total;

    const int block = blockOfRecord(r);
    const int start = blockStartRecord(block);
    const qint64 base = m_prefix.at(block);

    if (isBlockMeasured(block)) {
        const QVector<quint16> &lines = m_recordLines[block];
        qint64 line = base;
        for (int i = 0; i < r - start; ++i)
            line += lines.at(i);
        return line;
    }

    // Estimated block: map proportionally by physical position, so the estimate
    // is monotonic and consistent with recordAtLine's inverse.
    const qint64 physBefore = m_idx->firstLineOfRecord(r) - m_idx->firstLineOfRecord(start);
    const qint64 physBlock = m_blockPhysical.at(block);
    const qint64 estBlock = blockLines(block);
    if (physBlock <= 0)
        return base;
    return base + qint64(std::llround(double(physBefore) * double(estBlock) / double(physBlock)));
}

int EstimatedGeometry::recordAtLine(qint64 line) const
{
    if (!m_idx)
        return -1;
    const int n = m_idx->records.size();
    if (n == 0)
        return -1;
    if (line <= 0)
        return 0;
    if (line >= m_total)
        return n - 1;

    // Binary search the block prefix: largest b with m_prefix[b] <= line.
    int lo = 0, hi = m_blockPhysical.size() - 1;
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

    if (isBlockMeasured(block)) {
        const QVector<quint16> &lines = m_recordLines[block];
        qint64 acc = 0;
        for (int i = 0; i < count; ++i) {
            acc += lines.at(i);
            if (local < acc)
                return start + i;
        }
        return start + count - 1;
    }

    // Estimated block: invert the proportional map. Find the physical offset the
    // line corresponds to, then the record whose physical span contains it.
    const qint64 physBlock = m_blockPhysical.at(block);
    const qint64 estBlock = blockLines(block);
    if (physBlock <= 0 || estBlock <= 0)
        return start;
    const qint64 targetPhys = qint64(double(local) * double(physBlock) / double(estBlock));
    // Walk the block's physical heights directly (O(1) per record) rather than
    // differencing firstLineOfRecord, which would make this O(blockSize^2).
    qint64 acc = 0;
    for (int i = 0; i < count; ++i) {
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
    if (isBlockMeasured(block)) {
        const QVector<quint16> &lines = m_recordLines[block];
        return lines.at(r - blockStartRecord(block));
    }
    const int phys = RecordIndex::displayLines(m_idx->records.at(r));
    const int est = int(std::llround(double(phys) * expansionFactor()));
    return qBound(1, qMax(phys, est), int(RecordIndex::kDisplayLineCap));
}

} // namespace loftail
