#pragma once

#include <QHash>
#include <QVector>
#include <QtGlobal>

namespace loftail {

class RecordIndex;

// Estimated line<->record geometry for wrap-ALWAYS-ON (invariant #6,
// ARCHITECTURE.md §7.1.1). It is the machinery the exact path must never touch:
// LogView constructs and consults it ONLY in WrapMode::AlwaysOn. It does no
// decoding and holds no widget or file state — measurements are fed in from the
// view — so it is pure integer/vector math, unit-testable without a QApplication
// or a real log file.
//
// Why estimate at all: with wrap on, a record's rendered height depends on the
// viewport width, so a resize would otherwise invalidate every height — an O(n)
// text-shaping pass on every drag frame. Instead:
//
//   * A fixed-pitch font means wrapped height needs no shaping, only a character
//     count: a physical line of `chars` characters occupies ceil(chars/cols)
//     visual lines. That is the whole measurement model (visualLinesForChars /
//     measuredRecordLines below).
//   * Blocks the user has actually scrolled through are MEASURED exactly and
//     cached, keyed by the current column count.
//   * Blocks not yet visited are ESTIMATED from a running expansion factor —
//     measured visual lines over measured physical lines — applied to each
//     block's (column-independent) physical line count.
//   * The total height, and therefore the scrollbar, starts approximate and
//     REFINES toward truth as blocks are measured (SPEC.md §5 states this
//     plainly). Navigation resolves to real records and is exact; only the thumb
//     geometry is estimated.
//   * A column-count change drops every measurement (the caller debounces the
//     resize so a drag remeasures once, at the end, not per frame).
class EstimatedGeometry
{
public:
    // --- Pure wrap math (static, for direct unit testing) ------------------
    // Visual lines occupied by one physical line of `chars` characters wrapped
    // at `cols` columns: ceil(chars/cols), never fewer than one (an empty line
    // still occupies a row). `cols` <= 0 is treated as "no wrap" (one line).
    static int visualLinesForChars(int chars, int cols);

    // Exact wrapped height, in display lines, of a record whose physical lines
    // hold `lineChars[i]` characters each, wrapped at `cols` and clamped to
    // `cap` (the shared 100-line display cap, RecordIndex::kDisplayLineCap).
    static int measuredRecordLines(const QVector<int> &lineChars, int cols, int cap);

    // --- Lifecycle ---------------------------------------------------------
    // Bind to `idx` (whose block prefix sums must already be built) and seed all
    // blocks as unmeasured, estimated at `cols` columns with expansion 1.0 (i.e.
    // initially every record is its physical-line height — no wrap assumed until
    // real measurements arrive).
    void reset(const RecordIndex *idx, int cols);
    void clear();
    bool isBound() const { return m_idx != nullptr; }
    const RecordIndex *index() const { return m_idx; }

    int columns() const { return m_cols; }
    // Change the viewport column count. When it actually differs, EVERY cached
    // measurement is dropped (heights are width-dependent) and totals fall back
    // to estimates until blocks are remeasured. Returns true when the width
    // changed and the cache was invalidated.
    bool setColumns(int cols);

    // --- Measurement -------------------------------------------------------
    int blockCount() const { return m_blockPhysical.size(); }
    int blockOfRecord(int r) const;
    int recordsInBlock(int block) const;
    bool isBlockMeasured(int block) const;
    int measuredBlockCount() const { return m_measuredBlocks; }

    // Fold in an exact measurement of one whole block: `visualLines[i]` is the
    // wrapped display-line count of the block's i-th record at the CURRENT
    // columns. Marks the block measured, refines the running expansion factor,
    // and rebuilds the block totals. A no-op if the block is already measured at
    // this width.
    void measureBlock(int block, const QVector<quint16> &visualLines);

    // --- Mapping (estimated where unmeasured, exact where measured) --------
    qint64 totalLines() const { return m_total; }
    qint64 firstLineOfRecord(int r) const;
    int recordAtLine(qint64 line) const;
    int recordHeightLines(int r) const;

    // The running visual/physical expansion factor (>= 1.0), 1.0 before any
    // measurement. Exposed for tests and diagnostics.
    double expansionFactor() const;
    // The estimated-or-measured display-line total of one block. Exposed for
    // tests.
    qint64 blockLines(int block) const;

private:
    void rebuild();                       // recompute m_blockLines totals + prefix
    qint64 blockPhysical(int block) const { return m_blockPhysical.at(block); }
    int    blockStartRecord(int block) const;

    const RecordIndex *m_idx = nullptr;
    int    m_cols = 80;

    // Per block, column-INDEPENDENT: sum of display (capped) physical lines.
    QVector<qint64> m_blockPhysical;
    // Per block: exact measured display-line total at m_cols, or -1 when the
    // block is unmeasured (estimated).
    QVector<qint64> m_blockLines;
    // Per MEASURED block only: its per-record wrapped display-line counts, for
    // exact intra-block line<->record mapping. Bounded to visited blocks.
    QHash<int, QVector<quint16>> m_recordLines;
    // Prefix sums of blockLines(): m_prefix[b] = lines before block b; the final
    // element is the grand total (== m_total).
    QVector<qint64> m_prefix;
    qint64 m_total = 0;

    // Running measured totals driving the expansion factor.
    qint64 m_measuredVisual = 0;
    qint64 m_measuredPhysical = 0;
    int    m_measuredBlocks = 0;
};

} // namespace loftail
