#pragma once

#include <QVarLengthArray>
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
// ---------------------------------------------------------------------------
// What "either side" is measured over: the IN-BOUND stream, not the ordinals
// ---------------------------------------------------------------------------
// inBound() is the stream being searched and matches() is the search. Context counts
// IN-BOUND records, so `before`/`after` never pull in a record inBound() rejects and
// never spend part of the window on one. Document supplies the run bound AND every
// non-text filter axis as inBound(), and the message-text axis alone as matches()
// (SPEC.md §6): context widens the message search over the records the other filters
// already admit, which is why an inactive text axis makes context inert with no gate
// anywhere — every in-bound row is then a match, so nothing is left to be context to.
//
// Ordering note: a record is tested for MATCH before it is tested for trailing
// context, so a match landing inside a previous match's -A window is tagged a match.
// Leading-context rows are never run through matches() — the suffix invariant below
// guarantees any match in that window was already emitted.
//
// ---------------------------------------------------------------------------
// The suffix invariant (ARCHITECTURE.md §7.2.1) — load-bearing, do not break
// ---------------------------------------------------------------------------
// Write W(m) for the oldest ordinal in the leading window of a match at m — the
// `before`-th in-bound row at or before m. After any emission step, the emitted set
// contains EVERY in-bound source row in
//     [W(lastMatch), lastEmitted].
// Proof sketch: a match at m writes the in-bound rows of [W(m), m-1] above
// lastEmitted, then m; and m >= prevMatch, so W(m) >= W(prevMatch) — the part of
// [W(m), lastEmitted] not written now is inside [W(prevMatch), lastEmitted], which
// the previous step's invariant already covered. A trailing-context emission only
// extends the range rightward and leaves lastMatch alone.
//
// The consequence is the entire reason the live path is cheap: a new match at
// r > lastEmitted needs the in-bound rows of [W(r), r-1], whose part at or below
// lastEmitted is inside [W(lastMatch), lastEmitted] (since r >= lastMatch) and so is
// ALREADY THERE. Only rows above lastEmitted are ever written, which means leading
// context is a TAIL APPEND — the visible list stays ascending and FilteredIndex
// never needs a mid-list insert.
//
// This used to rest on inBound() accepting a CONTIGUOUS range of ordinals, because
// the window was computed as `r - before` in ordinal space. It no longer does: the
// window is found by WALKING BACK from the match over in-bound rows, however many
// ordinals separate them. Two things keep that cheap and stateless:
//
//   * the walk stops at lastEmitted, because the invariant above says everything at
//     or below it that the window wants is already emitted. So the walks of
//     successive matches cover disjoint stretches of ordinals — O(n) over a whole
//     scan, and in the live path bounded by the gap since the last emission;
//   * it therefore needs nothing carried between calls. inBound() is asked about
//     rows below `first`, exactly as the old ordinal clamp did.
struct ContextState
{
    int lastEmitted = -1; // largest source ordinal emitted so far
    int lastMatch   = -1; // ordinal of the most recent MATCH (never a context row)
    // In-bound rows already emitted as trailing context for lastMatch. Counting rows
    // rather than measuring `r - lastMatch` is the same move the backward walk makes
    // at the other end of the window. Reconstructible from the FilteredIndex: they
    // are exactly the visible rows above lastMatch (see LiveController).
    int sinceMatch  = 0;
};

// Walk [first, last] and emit the visible subset with context.
//   InBound(int row) -> bool   the stream being searched: the run byte-offset bound
//                              and the non-text filter axes. Cheap, never decodes.
//   Matches(int row) -> bool   the search: the message-text axis. May decode; called
//                              for in-bound rows only, never for a leading-context row.
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
    QVarLengthArray<int, 32> lead; // the leading window, gathered descending

    for (int r = qMax(first, 0); r <= last; ++r) {
        if (!inBound(r))
            continue;

        if (matches(r)) {
            // Leading context: walk back over IN-BOUND rows — matched or not, since
            // grep -B counts records rather than misses — until the window is full or
            // the walk reaches lastEmitted, below which everything it wants is
            // already there. Stopping there is what makes this a tail append.
            lead.clear();
            for (int q = r - 1; q > st.lastEmitted && lead.size() < before; --q) {
                if (inBound(q))
                    lead.append(q);
            }
            for (int k = lead.size() - 1; k >= 0; --k) // gathered descending, emit ascending
                sink(lead[k], true);
            sink(r, false);
            st.lastMatch = r;
            st.lastEmitted = r;
            st.sinceMatch = 0;
        } else if (st.lastMatch >= 0 && st.sinceMatch < after) {
            // Trailing context. lastMatch is NOT advanced — the -A window is measured
            // from the match, so `after` in-bound records follow it and no more.
            ++st.sinceMatch;
            sink(r, true);
            st.lastEmitted = r;
        }
    }
}

} // namespace loftail
