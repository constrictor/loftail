#pragma once

#include "Record.h"
#include "RecordIndex.h"

#include <QVector>
#include <QtGlobal>

#include <algorithm>
#include <utility>

namespace loftail {

// M4 — the filtered record index (invariant #6, ARCHITECTURE.md §7.1/§7.2). When a
// filter is active the visible set is a SUBSET of records, so the custom LogView
// must scroll in line units over the FILTERED subset — the base RecordIndex prefix
// sums include hidden records and would give wrong geometry.
//
// Design: a compact index over the visible records. FilteredIndex keeps
//   * m_visible[viewRow] = source record ordinal (the view->source map), and
//   * m_compact — a real RecordIndex holding COPIES of just the visible Records,
//     with its own two-level block prefix sums (rebuilt once per filter change).
// A Record copy is 32 bytes of byte-coordinates only (invariant #1 preserved — no
// parsed text), and the copy + prefix rebuild is a single linear pass, comfortably
// inside the §11 budget. Because m_compact is an ordinary RecordIndex, every piece
// of tested geometry — LogView's exact line<->record statics AND the AlwaysOn
// EstimatedGeometry — operates over it unchanged: geometry() hands back the compact
// index when filtering and the source index (identity) when not.
//
// Inactive (no active filter) is the common path and stays allocation-free: it is a
// pure identity view over the source index, so indexing appends and unfiltered
// scrolling never touch the compact copy.
//
// M15 adds a third parallel piece, m_context: one byte per visible row saying "this
// record is here only because a nearby one matched" (SPEC.md §6, grep -B/-A). It is a
// separate vector rather than a flag bit stolen from m_visible because visible() is
// public and read as plain ordinals by callers and tests; one byte against the 4 + 32
// already spent per visible row is not worth the aliasing.
class FilteredIndex
{
public:
    // Bind to the source index once; its address is stable (a Document member), so
    // reassigning the source's contents does not invalidate this pointer.
    void setSource(const RecordIndex *source) { m_source = source; }
    const RecordIndex *source() const { return m_source; }

    bool active() const { return m_active; }

    // Materialize the visible subset from `visible` (ascending source ordinals) and
    // build its prefix sums. Marks the index active. `context` is the parallel
    // match/context flag vector (M15); an empty one means "every row is a match",
    // which is what a filter with no context configured produces.
    void setVisible(QVector<qint32> visible, QVector<quint8> context = {})
    {
        m_visible = std::move(visible);
        m_context = std::move(context);
        Q_ASSERT(m_context.isEmpty() || m_context.size() == m_visible.size());
        // Normalize to one flag per visible row so appendVisible()/popLastVisible()
        // never have to reason about a short vector.
        m_context.resize(m_visible.size());
        m_contextCount = 0;
        for (quint8 c : m_context)
            m_contextCount += (c != 0);
        m_compact.records.clear();
        m_compact.records.reserve(m_visible.size());
        if (m_source) {
            for (qint32 s : m_visible)
                m_compact.records.append(m_source->records.at(s));
        }
        m_compact.rebuildBlockSums();
        m_ascending = true;
        for (int i = 1; i < m_visible.size(); ++i) {
            if (m_visible.at(i) <= m_visible.at(i - 1)) {
                m_ascending = false;
                break;
            }
        }
        m_active = true;
    }

    // Drop the subset and return to the identity view over the source index.
    void clear()
    {
        m_active = false;
        m_visible.clear();
        m_visible.squeeze();
        m_context.clear();
        m_context.squeeze();
        m_contextCount = 0;
        m_ascending = true;
        m_compact = RecordIndex();
    }

    // The RecordIndex the view drives geometry from: the compact subset when a
    // filter is active, the source index (identity) otherwise. Both carry valid
    // block prefix sums, so line-unit scrolling is uniform across the two states.
    const RecordIndex &geometry() const { return m_active ? m_compact : *m_source; }

    int recordCount() const
    {
        if (m_active)
            return m_compact.records.size();
        return m_source ? m_source->records.size() : 0;
    }

    // Map a view row to its source record ordinal. Identity when inactive; -1 for an
    // out-of-range row.
    int sourceRow(int viewRow) const
    {
        if (!m_active)
            return (m_source && viewRow >= 0 && viewRow < m_source->records.size()) ? viewRow : -1;
        return (viewRow >= 0 && viewRow < m_visible.size()) ? int(m_visible.at(viewRow)) : -1;
    }

    // The view row showing source record `sourceRow`, or -1 when it is not visible.
    // The inverse of sourceRow(), which a filter change needs because a source ordinal
    // is the ONE coordinate a refilter does not move (SPEC.md §6, ARCHITECTURE.md §7.1.2).
    int viewRowOf(int sourceRow) const
    {
        const int n = recordCount();
        if (!m_active)
            return (sourceRow >= 0 && sourceRow < n) ? sourceRow : -1;
        if (m_ascending) {
            const int r = lowerBound(sourceRow);
            return (r < n && m_visible.at(r) == sourceRow) ? r : -1;
        }
        // Out of order (the digest, see below): a scan is the only correct answer.
        for (int i = 0; i < m_visible.size(); ++i)
            if (m_visible.at(i) == sourceRow)
                return i;
        return -1;
    }

    // The view row of the first VISIBLE record at or after `sourceRow`; recordCount()
    // when every visible record is before it. Only meaningful over an ASCENDING subset
    // — which every FILTER subset is (Document::applyFilters emits in one forward pass)
    // and the DIGEST is not: publishDigest() reorders its ordinals by timestamp, so
    // "at or after" has no meaning in its row space and a binary search over it would
    // answer confidently and wrongly.
    int viewRowAtOrAfter(int sourceRow) const
    {
        const int n = recordCount();
        if (!m_active)
            return qBound(0, sourceRow, n);
        Q_ASSERT(m_ascending);
        if (!m_ascending) {
            const int exact = viewRowOf(sourceRow);
            return exact >= 0 ? exact : n;
        }
        return lowerBound(sourceRow);
    }

    // The visible source ordinals (empty when inactive), ascending for every subset
    // but the digest's — see isAscending(). Exposed for tests and diagnostics.
    const QVector<qint32> &visible() const { return m_visible; }

    // Whether the visible ordinals are strictly ascending. False only for a subset
    // published out of order — today exactly one, the digest.
    bool isAscending() const { return m_ascending; }

    // --- M15 filter context (SPEC.md §6) --------------------------------------

    // True when this view row is present only because a nearby record matched. Never
    // true on the identity path — with no filter there is nothing to be context to.
    bool isContext(int viewRow) const
    {
        return m_active && viewRow >= 0 && viewRow < m_context.size() && m_context.at(viewRow);
    }

    // How many visible rows are context rather than matches. Maintained as a counter,
    // not a scan: the status bar reads it on every repaint.
    int contextCount() const { return m_active ? m_contextCount : 0; }

    // The parallel flag vector, one entry per visible row. Tests and diagnostics.
    const QVector<quint8> &contextFlags() const { return m_context; }

    // The source ordinal of the last visible MATCH, or -1. Scans back over the
    // trailing context rows, which the emission rule bounds at `after` of them.
    int lastMatchSource() const
    {
        for (int i = m_visible.size() - 1; i >= 0; --i) {
            if (!m_context.at(i))
                return int(m_visible.at(i));
        }
        return -1;
    }

    // How many TRAILING visible rows have a source ordinal >= minSourceRow. The live
    // path needs the count up front because beginRemoveTail() takes one.
    int trailingCountFrom(int minSourceRow) const
    {
        int n = 0;
        for (int i = m_visible.size() - 1; i >= 0 && m_visible.at(i) >= minSourceRow; --i)
            ++n;
        return n;
    }

    // --- M6 incremental append (live updates) ---------------------------------
    // When a filter is active and records are appended to the source index, the
    // visible subset is extended in place rather than recomputed (invariant #1):
    // newly-visible records are appended and the compact block sums extended from
    // the first new visible row. The trailing (provisional) record may also be
    // popped and re-evaluated when its bytes changed. Only meaningful while active.

    // The source ordinal of the last visible record, or -1 when none is visible.
    int lastVisibleSource() const { return m_visible.isEmpty() ? -1 : int(m_visible.last()); }

    // Drop the trailing visible record (its source row must be the provisional one
    // being reconsidered). Does NOT touch the compact block sums — the caller
    // extends them once after all edits. m_ascending is deliberately left alone:
    // dropping the tail can only preserve ascendingness, and staying conservatively
    // false costs a scan, never a wrong answer.
    void popLastVisible()
    {
        if (!m_visible.isEmpty()) {
            m_contextCount -= (m_context.last() != 0);
            m_context.removeLast();
            m_visible.removeLast();
            m_compact.records.removeLast();
        }
    }

    // Append one now-visible source record (a copy of its 32-byte Record) at the
    // end of the visible subset. Block sums are extended separately via
    // extendCompactSums() after a batch of appends. `context` tags the row as a
    // neighbour of a match rather than a match itself (M15).
    void appendVisible(int sourceRow, const Record &rec, bool context = false)
    {
        m_ascending = m_ascending && (m_visible.isEmpty() || m_visible.last() < sourceRow);
        m_visible.append(sourceRow);
        m_context.append(context ? 1 : 0);
        m_contextCount += context ? 1 : 0;
        m_compact.records.append(rec);
    }

    // Extend the compact subset's block sums in place (invariant #1) after
    // appendVisible()/popLastVisible() edits; `validCount` is the number of leading
    // visible records whose sums are unchanged.
    void extendCompactSums(int validCount) { m_compact.extendBlockSums(validCount); }

private:
    // First index into m_visible whose ordinal is >= sourceRow. Ascending subsets only.
    int lowerBound(int sourceRow) const
    {
        const auto it = std::lower_bound(m_visible.cbegin(), m_visible.cend(), qint32(sourceRow));
        return int(it - m_visible.cbegin());
    }

    const RecordIndex *m_source = nullptr;
    bool               m_active = false;
    bool               m_ascending = true; // m_visible is strictly ascending (not the digest)
    QVector<qint32>    m_visible;      // view row -> source ordinal (active only)
    QVector<quint8>    m_context;      // parallel to m_visible: 1 = context, 0 = match
    int                m_contextCount = 0;
    RecordIndex        m_compact;      // 32-byte record copies of the visible subset
};

} // namespace loftail
