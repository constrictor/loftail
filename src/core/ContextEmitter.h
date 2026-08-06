#pragma once

#include <QtGlobal>

namespace loftail {

// M15 — filter context (SPEC.md §6, "grep -B/-A"). A filter that hides everything
// but the ERRORs also hides what led to them, so the visible subset can be widened
// to carry N records either side of each match, rendered dimmed by the view.
//
// The whole feature is this one forward pass. It emits SOURCE ORDINALS in ascending
// order, tagged match/context, and knows nothing about records, filters or the
// FilteredIndex — the caller supplies three callables. Two callers drive it and both
// must produce the same answer: Document::applyFilters() over the whole index, and
// LiveController's filtered append branch over the tail. Keeping the rule in one
// template is the same move acceptsInView() made for the run bound.
//
// Ordering note: a record is tested for MATCH before it is tested for trailing
// context, so a match landing inside a previous match's -A window is tagged a match.
// Leading-context rows are never run through matches() — the suffix invariant below
// guarantees any match in that window was already emitted.
//
// ---------------------------------------------------------------------------
// The suffix invariant (ARCHITECTURE.md §7.2) — load-bearing, do not break
// ---------------------------------------------------------------------------
// After any emission step, the emitted set contains EVERY in-bound source row in
//     [max(firstInBound, lastMatch - before), lastEmitted].
// Proof sketch: a match at m writes [max(lastEmitted+1, m-before) .. m-1] then m,
// and m >= prevMatch so m-before >= prevMatch-before, which the previous step's
// invariant already covered; a trailing-context emission only extends the range
// rightward and leaves lastMatch alone.
//
// The consequence is the entire reason the live path is cheap: a new match at
// r > lastEmitted needs [max(.., r-before) .. r-1], whose part at or below
// lastEmitted is inside [lastMatch-before, lastEmitted] (since r >= lastMatch) and
// so is ALREADY THERE. Only rows above lastEmitted are ever written, which means
// leading context is a TAIL APPEND — the visible list stays ascending and
// FilteredIndex never needs a mid-list insert.
//
// This rests on inBound() accepting a CONTIGUOUS range of ordinals. It does today:
// the only bound is the selected run's half-open byte interval, and Record::offset
// is monotone in ordinal. A future non-contiguous view restriction would invalidate
// the invariant and force real insertions.
struct ContextState
{
    int lastEmitted = -1; // largest source ordinal emitted so far
    int lastMatch   = -1; // ordinal of the most recent MATCH (never a context row)
};

// Walk [first, last] and emit the visible subset with context.
//   InBound(int row) -> bool   the run byte-offset bound. Cheap, never decodes.
//   Matches(int row) -> bool   the filter chain. May decode; called for in-bound
//                              rows only, and never for a leading-context row.
//   Sink(int row, bool isContext) -> void
//
// `st` carries across calls so the incremental path resumes exactly where a one-shot
// pass would be. It is deliberately reconstructible from the FilteredIndex rather
// than cached anywhere: nothing here is state a rescan could leave stale.
// NOTE: the sink parameter is NOT called `emit`. That is a Qt keyword macro that
// expands to nothing, which would turn every `emit(q, true)` below into a comma
// expression that compiles cleanly and emits nothing at all.
template <class InBound, class Matches, class Sink>
void emitWithContext(int first, int last, int before, int after, ContextState &st,
                     InBound inBound, Matches matches, Sink sink)
{
    for (int r = qMax(first, 0); r <= last; ++r) {
        if (!inBound(r))
            continue;

        if (matches(r)) {
            // Leading context: only what is not already emitted (the clamp against
            // lastEmitted is what makes this a tail append, see above).
            const int from = qMax(qMax(st.lastEmitted + 1, r - before), 0);
            for (int q = from; q < r; ++q) {
                if (inBound(q))
                    sink(q, true);
            }
            sink(r, false);
            st.lastMatch = r;
            st.lastEmitted = r;
        } else if (st.lastMatch >= 0 && r - st.lastMatch <= after) {
            // Trailing context. lastMatch is NOT advanced — the -A window is measured
            // from the match, so `after` context rows follow it and no more.
            sink(r, true);
            st.lastEmitted = r;
        }
    }
}

} // namespace loftail
