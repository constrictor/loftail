# loftail — Feature Ideas

**Status:** Brainstorm, 2026-08-05. Unfiltered idea list, not a commitment and not an order.

This is a scratch list. `FUTURE.md` is the actual roadmap — an item moves there once it has been
judged worth building and its accommodation written down. Items here are ranked by
value-per-unit-of-risk, and each notes what already-shipped code would carry it.

---

## Tier 1 — the architecture has already paid for these

1. **Filter with context (`grep -C`).** Show N records either side of each filter match, dimmed, so
   filtering to ERROR keeps "what led to it" instead of destroying it.
   *Carried by:* `FilteredIndex::setVisible()` takes any ascending list of source ordinals and
   rebuilds its own prefix sums, so neighbors are just more ordinals — every piece of tested
   geometry (exact and estimated) works unchanged. New state is one bit per visible row
   ("context, not match") that `LogModel` turns into a dimmed row.

2. **A record context menu — "Filter to this…" / "Highlight this…".** `LogView` has a header context
   menu but no record one. Right-click a record: filter to this subsystem, exclude this thread,
   highlight everything from this thread, start the time range here.
   *Carried by:* every one of those is a `MatchCriteria` the panes already build and persist (M10).
   Converts filtering from typing into pointing, which is how an unfamiliar log actually gets read.

3. **Elapsed-time display mode.** A sixth `TimeDisplay`: seconds since the **previous visible**
   record. Stalls, timeouts and retry storms become visible by scanning one column. Companion:
   mark any gap over a threshold, so the pause is findable without reading.
   *Carried by:* the enum already round-trips through a string and the timestamp header menu
   already offers the modes. "Visible" composes with filters — filter to one subsystem and the
   column shows that subsystem's cadence — and stays well-defined per file, since filters are
   per-file too.

4. **An unread marker while detached.** Scrolling up off the tail drops a separator line where you
   left, labelled "1,240 new records". Removes the "did I miss something?" tax of tailing.
   *Carried by:* pure per-view state (invariant #7); follow is already per-view in `LogView`.

## Tier 2 — distinctive, worth a milestone each

5. **Time-locked side-by-side.** `FUTURE.md` already wants the split inside the document area; the
   interesting part is locking two views by **timestamp** rather than by row — scroll the client
   log, the server log follows to the nearest record in time.
   Threads a needle left deliberately: `SPEC.md` §11 rules out merged time-ordered views forever,
   and correlating two logs is the reason people want merging. Time-locking delivers most of that
   value while each tab stays an independent log — no merge, no ambiguity about which file a row
   came from. Also works on two views of one file (pin the error, scroll the lead-up) and on two
   runs of one file.
   *Carried by:* invariant #10 — every timestamp is already UTC epoch ms, so the lock is a binary
   search into the other view's index.

6. **Run diff.** "How did this run differ from the last one?" — align two runs by normalized message
   shape (timestamps and ids stripped) and mark records added, missing, reordered. Distinctive
   because the run split (`SPEC.md` §3a) is already there and nothing else has it.
   *Cost:* the normalization and the diff, not the plumbing. Cheap v1: ship #5 first and
   "compare with previous run" becomes a menu item opening a time-locked split of two runs, with
   no new machinery at all.

7. **Shareable record links.** A `loftail://` link carrying address + timestamp + run + filter
   preset, so "look at this line" is pasteable into a ticket and the colleague lands on the exact
   record with the same filters.
   *Carried by:* M11's `RemoteLocation` normal form, which the recent list, format cache, session
   and tab-dedupe already agree on — an identity currently spent only on deduping. This is the
   feature remote logs make possible and a local-only viewer cannot have.
   *Note:* registering a private URL scheme is distinct from `SPEC.md` §11's refusal to claim the
   default `.log` handler.

8. **Capture-group columns.** A user regex with named groups over the message becomes columns —
   pull `request_id=(\w+)` out and follow one request across every subsystem.
   *Scoping:* as a display column and filter axis this needs no `Record` change, computing lazily
   on the paint path exactly as the message-text axis does, at the same budget. Sorting or jumping
   by the extracted value is the expensive version — it wants an intern side-table, and `Record` is
   exactly 32 bytes with nothing spare (invariant #1). Ship the lazy half; treat the side-table as
   a separate decision.

## Tier 3 — small wins

9. **Jump to timestamp** (Ctrl+G). `SPEC.md` §4 names it as something parsed timestamps make
   possible, but it does not appear to have shipped.
10. **Find across all open tabs**, with a results list.
11. **A density strip** beside the scrollbar showing where highlighted records and find matches sit
    in the whole file. Navigation aid, not a chart; reuses colors the highlighter already resolves.
12. **Compressed `.gz` logs** — already in `FUTURE.md`, and since M11 it is a second
    `RemoteFetcher` rather than a second mechanism.
13. **Bookmarks** — already scoped in `FUTURE.md`, including the open question of what identifies a
    bookmarked record across a reindex.

## Watch-outs

- **Per-subsystem record counts in the Filters pane** would be genuinely useful and sit right on
  §11's "no charts, statistics, or alerting" line. Worth an explicit ruling rather than drifting
  into it.
- **Anything shaped like "alert me when ERROR appears"** is squarely a non-goal. The tailing
  equivalent that is not is #4's unread marker.

## A suggested order

#2, #1 and #3 as one milestone — all three are about reading a *filtered* log and each is small —
then #5 as the milestone after, since it also unlocks #6 for free.
