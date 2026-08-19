#pragma once

#include <QHash>
#include <QVector>
#include <QtGlobal>

namespace loftail {

class RecordIndex;

// Estimated line<->record geometry for wrap-ALWAYS-ON (invariant #6,
// ARCHITECTURE.md §7.1.1). It is the machinery the exact path must never touch:
// LogView constructs and consults it ONLY in WrapMode::AlwaysOn. It does no
// decoding, no measuring and holds no widget or file state — every height is fed
// in from the view — so it is pure integer/vector math, unit-testable without a
// QApplication or a real log file.
//
// Why estimate at all: with wrap on, a record's rendered height depends on the
// viewport width, so a resize would otherwise invalidate every height — an O(n)
// text-shaping pass on every drag frame. Instead:
//
//   * A record's wrapped height is a per-record number the VIEW works out, from
//     the font, by a greedy fill over per-codepoint advances (ui/WrapMetrics.h).
//     Nothing here knows how it was arrived at; this class only caches, sums and
//     estimates the quint16 heights it is handed.
//   * Blocks the user has actually scrolled through are MEASURED exactly and
//     cached, keyed by the current WRAP WIDTH IN PIXELS. It used to be a column
//     count, which was the same key in a coarser unit — and a unit the model has
//     stopped believing in, since two characters of one font need not be the same
//     width (bugs.md 21, 22).
//   * Blocks not yet visited are ESTIMATED from a running expansion factor —
//     measured visual lines over measured physical lines — applied to each
//     block's (column-independent) physical line count.
//   * The total height, and therefore the scrollbar, starts approximate and
//     REFINES toward truth as blocks are measured (SPEC.md §5 states this
//     plainly). Navigation resolves to real records and is exact; only the thumb
//     geometry is estimated.
//   * A wrap-width change drops every measurement (the caller debounces the
//     resize so a drag remeasures once, at the end, not per frame). So does a FONT
//     change, which the view asks for explicitly through invalidateMeasurements():
//     the width need not move with the font, and the heights always do.
//   * A block may be measured only in PART. The bound index is append-only and
//     live (invariant #5), so a measured block's record count grows under it and
//     its trailing record grows continuation lines in place; syncTail() gives back
//     the records the growth touched and keeps the rest, and the unmeasured
//     remainder of such a block is estimated exactly as an unvisited block is.
class EstimatedGeometry
{
public:
    // --- Lifecycle ---------------------------------------------------------
    // Bind to `idx` (whose block prefix sums must already be built) and seed all
    // blocks as unmeasured, estimated at `wrapWidth` pixels with expansion 1.0
    // (i.e. initially every record is its physical-line height — no wrap assumed
    // until real measurements arrive).
    void reset(const RecordIndex *idx, int wrapWidth);
    void clear();
    bool isBound() const { return m_idx != nullptr; }
    const RecordIndex *index() const { return m_idx; }

    int wrapWidth() const { return m_wrapWidth; }
    // Change the pixel width a record is measured at. When it actually differs,
    // EVERY cached measurement is dropped (heights are width-dependent) and totals
    // fall back to estimates until blocks are remeasured. Returns true when the
    // width changed and the cache was invalidated.
    bool setWrapWidth(int wrapWidth);
    // Drop every measurement at the CURRENT width. The one caller is the view's
    // font change: heights are the font's as much as the width's, and a zoom whose
    // column origins did not move leaves setWrapWidth() a no-op — so without this
    // the model goes on answering with heights measured at the old face's advances
    // for the rest of the session (§7.1.5).
    void invalidateMeasurements();

    // --- Measurement -------------------------------------------------------
    int blockCount() const { return m_blockPhysical.size(); }
    int blockOfRecord(int r) const;
    int recordsInBlock(int block) const;
    // How many of `block`'s records have an exact cached height. Equal to
    // recordsInBlock() when the block is fully measured, and SHORT of it when the
    // tail has grown under it: syncTail() truncates the trailing block's cache to
    // the records the growth cannot have touched rather than dropping it, so a
    // live log re-measures the record that changed and not the other 4095.
    int measuredRecordsInBlock(int block) const;
    bool isBlockMeasured(int block) const;
    int measuredBlockCount() const { return m_measuredBlocks; }

    // Fold in exact measurements for the records of `block` from its `first`-th
    // onward: `visualLines[i]` is the wrapped display-line count of record
    // blockStartRecord(block) + first + i at the CURRENT wrap width. `first` must not
    // exceed measuredRecordsInBlock(block), or the run would leave a hole; the run
    // itself is truncated to the block. The default `first` of 0 with the whole
    // block's heights is the ordinary "this block has now been visited" call and
    // is a no-op when the block is already fully measured at this width.
    void measureBlock(int block, const QVector<quint16> &visualLines, int first = 0);

    // --- Growth (the bound index is append-only) ---------------------------
    // Fold in growth at the tail of the bound index: records appended, and/or the
    // trailing record grown continuation lines in place. Returns true when
    // anything moved. This is what a plain rebind cannot do — the block count only
    // moves once every kBlockSize records, so without it a measured block goes on
    // answering for a record count it no longer has (ARCHITECTURE.md §7.1.1).
    //
    // Only the records at and after the OLD trailing one can have changed
    // (invariant #9: the indexer is a single forward pass), so the cost is
    // O(kBlockSize + appended) — the same bound RecordIndex::extendBlockSums()
    // carries, and for the same reason.
    bool syncTail();

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
    void refreshTotals();                 // recount the measured aggregates + rebuild
    void setBlockMeasurement(int block, const QVector<quint16> &lines);
    void truncateBlockMeasurement(int block, int keep);
    const QVector<quint16> *cachedLines(int block) const;
    qint64 blockPhysical(int block) const { return m_blockPhysical.at(block); }
    int    blockStartRecord(int block) const;

    const RecordIndex *m_idx = nullptr;
    int    m_wrapWidth = 1;

    // Per block, width-INDEPENDENT: sum of display (capped) physical lines.
    QVector<qint64> m_blockPhysical;
    // Per block: the display-line total of its MEASURED PREFIX at m_wrapWidth, or -1
    // when nothing in the block is measured. Equal to the block's exact total when
    // the prefix covers the whole block.
    QVector<qint64> m_blockLines;
    // Per block: the (width-independent) physical line total of that same
    // measured prefix, so the unmeasured remainder can still be estimated.
    QVector<qint64> m_blockMeasuredPhysical;
    // Per MEASURED block only: the per-record wrapped display-line counts of its
    // measured prefix, for exact intra-block line<->record mapping. Bounded to
    // visited blocks; its SIZE is measuredRecordsInBlock().
    QHash<int, QVector<quint16>> m_recordLines;
    // Prefix sums of blockLines(): m_prefix[b] = lines before block b; the final
    // element is the grand total (== m_total).
    QVector<qint64> m_prefix;
    qint64 m_total = 0;

    // Running measured totals driving the expansion factor.
    qint64 m_measuredVisual = 0;
    qint64 m_measuredPhysical = 0;
    int    m_measuredBlocks = 0;

    // What the bound index looked like when it was last reset or synced: the
    // record count, and the display-line count of its trailing record. syncTail()
    // compares against both, because a live log grows in two ways and only one of
    // them moves the count.
    int    m_boundRecords = 0;
    int    m_tailLines = 0;
};

} // namespace loftail
