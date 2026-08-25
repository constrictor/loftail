# loftail — Feature Ideas

**Status:** Brainstorm, 2026-08-05. Unfiltered idea list, not a commitment and not an order.

This is a scratch list. `FUTURE.md` is the actual roadmap — an item moves there once it has been
judged worth building and its accommodation written down. Items here are ranked by
value-per-unit-of-risk, and each notes what already-shipped code would carry it.

---

## Tier 1 — the architecture has already paid for these

1. ~~**Filter with context (`grep -C`).**~~ **Shipped in M15.** Show N records either side of each
   filter match, dimmed, so filtering to ERROR keeps "what led to it" instead of destroying it.
   *Carried by:* `FilteredIndex::setVisible()` takes any ascending list of source ordinals and
   rebuilds its own prefix sums, so neighbors are just more ordinals — every piece of tested
   geometry (exact and estimated) works unchanged. New state is one bit per visible row
   ("context, not match") that `LogModel` turns into a dimmed row.
   *And that held exactly*, which is worth recording because it is the accommodation note that
   was right about the hard part. What it missed is where the work went: the LIVE path, where
   `-B` looks like it needs to insert behind rows already emitted and — because of a suffix
   invariant — does not. See `FUTURE.md` and `ARCHITECTURE.md` §7.2.1.

2. ~~**Elapsed-time display mode.**~~ **Shipped in M24**, minus the companion. A sixth
   `TimeDisplay`: seconds since the **previous visible** record. Stalls, timeouts and retry
   storms become visible by scanning one column.
   *Carried by:* the enum already round-trips through a string and the timestamp header menu
   already offers the modes. "Visible" composes with filters — filter to one subsystem and the
   column shows that subsystem's cadence — and stays well-defined per file, since filters are
   per-file too.
   *And that held, including the part it did not spell out*: "visible" turned out to mean the
   row above **in the table doing the asking**, which is one lookup through `LogModel::view()`
   and therefore free in the digest strip as well, with no state to keep and nothing to
   invalidate. What the note missed is a decision, not a cost — the two rows with no interval
   to state (the first, and one after an unparsed line) are left blank rather than zero, and the
   filter axis deliberately does NOT follow the column into this mode, a gap being an interval
   where a bound is an instant. See `SPEC.md` §4 and `ARCHITECTURE.md` §5.1.
   **The companion — mark any gap over a threshold — was split off and NOT built.** It needs a
   threshold setting with a home at all three settings levels and a colour that does not collide
   with a highlight rule's, which owns the record's background and wins per action (M19). Judge
   it on its own.

3. **An unread marker while detached.** Scrolling up off the tail drops a separator line where you
   left, labelled "1,240 new records". Removes the "did I miss something?" tax of tailing.
   *Carried by:* pure per-view state (invariant #7); follow is already per-view in `LogView`.

## Tier 2 — distinctive, worth a milestone each

4. **Time-locked side-by-side.** `FUTURE.md` already wants the split inside the document area; the
   interesting part is locking two views by **timestamp** rather than by row — scroll the client
   log, the server log follows to the nearest record in time.
   Threads a needle left deliberately: `SPEC.md` §11 rules out merged time-ordered views forever,
   and correlating two logs is the reason people want merging. Time-locking delivers most of that
   value while each tab stays an independent log — no merge, no ambiguity about which file a row
   came from. Also works on two views of one file (pin the error, scroll the lead-up) and on two
   runs of one file.
   *Carried by:* invariant #10 — every timestamp is already UTC epoch ms, so the lock is a binary
   search into the other view's index.

5. **Run diff.** "How did this run differ from the last one?" — align two runs by normalized message
   shape (timestamps and ids stripped) and mark records added, missing, reordered. Distinctive
   because the run split (`SPEC.md` §3a) is already there and nothing else has it.
   *Cost:* the normalization and the diff, not the plumbing. Cheap v1: ship #4 first and
   "compare with previous run" becomes a menu item opening a time-locked split of two runs, with
   no new machinery at all.

6. **Shareable record links.** A `loftail://` link carrying address + timestamp + run + filter
   preset, so "look at this line" is pasteable into a ticket and the colleague lands on the exact
   record with the same filters.
   *Carried by:* M11's `RemoteLocation` normal form, which the recent list, format cache, session
   and tab-dedupe already agree on — an identity currently spent only on deduping. This is the
   feature remote logs make possible and a local-only viewer cannot have.
   *Note:* registering a private URL scheme is distinct from `SPEC.md` §11's refusal to claim the
   default `.log` handler.

7. **Capture-group columns.** A user regex with named groups over the message becomes columns —
   pull `request_id=(\w+)` out and follow one request across every subsystem.
   *Scoping:* as a display column and filter axis this needs no `Record` change, computing lazily
   on the paint path exactly as the message-text axis does, at the same budget. Sorting or jumping
   by the extracted value is the expensive version — it wants an intern side-table, and `Record` is
   exactly 32 bytes with nothing spare (invariant #1). Ship the lazy half; treat the side-table as
   a separate decision.

## Tier 3 — small wins

8. **Jump to timestamp** (Ctrl+G). `SPEC.md` §4 names it as something parsed timestamps make
   possible, but it does not appear to have shipped.
9. **Find across all open tabs**, with a results list.
10. **A density strip** beside the scrollbar showing where highlighted records and find matches sit
    in the whole file. Navigation aid, not a chart; reuses colors the highlighter already resolves.
11. **Bookmarks** — already scoped in `FUTURE.md`, including the open question of what identifies a
    bookmarked record across a reindex.

## ~~Added 2026-08-10 — a highlight rule that does more than colour~~ — SHIPPED in M19

Today a rule has exactly one effect: it recolours the records it matches. The idea is that the
effect becomes a *choice* per rule — colour is one action among several, all sharing the five match
axes and the ordered first-match-wins list already built (`SPEC.md` §7). Numbered after Tier 3 so
the tiers above keep their numbers; the three are not one milestone and are not equally safe.

**All three shipped in M19, on 2026-08-10, and the ruling on 13/14 went the other way from what is
predicted below** — the user's call, recorded rather than argued: ship them, and leave `SPEC.md`
§11's alerting non-goal untouched even though the notification contradicts it as written. The
contradiction lives in the commit message and in `PLAN.md` M19, not in the spec.

**Two predictions below were wrong, and both are worth keeping visible.** #12 said the decision to
make first is whether `LogModel` gains an index it does not own or the digest gets a model of its
own; that held, and the answer was the first — seven call sites, four in `LogModel` and three in
`LogView`, routed through one accessor. But #12 also called the digest **per-view state
(invariant #7), and that is wrong on the facts**: the ordinals come from per-*file* rules over the
per-*file* index inside the per-*file* run bound, so two views of one log can only compute the same
list. What is per-view is the widget. And #12 said the digest "maintains itself on the live path
from the match the highlighter already runs per appended record" — **there was no such match**.
Highlighting was entirely lazy and pull-based, so the live pass had to be written, gated on one
`anyEnabled()` walk so a plain colouring setup still pays nothing. See `ARCHITECTURE.md` §7.5.

12. **A highlight digest pane under the log.** A strip below the record table showing the **last
    matching record per rule** — one row per enabled rule, rendered exactly as it is in the log
    (same columns, same rule colours), and **sized to fit its rows** rather than scrolled, so with
    three rules it is three lines tall and with none it is not there at all. Answers "what is the
    newest of each thing I care about" while the view is somewhere else entirely, which is the
    question a tailed log with rules is really being asked. Also the honest version of "alert me":
    it *shows* rather than interrupts.
    *Per rule, opt-in* — a rule is in the digest only if it is enabled for it, since a rule that
    colours every INFO is not a rule whose latest match is worth a permanent line.
    *Carried by:* the digest is an ascending list of source ordinals of length ≤ rule count, which
    is precisely what `FilteredIndex::setVisible()` takes — the same lever context reuse turned on
    in M15, and it maintains itself on the live path from the match the highlighter already runs
    per appended record.
    *Not carried, and this is the cost:* it is a **second view of the same document showing a
    different subset**, and `LogModel` binds to a `const Document *` and reads *the* document's
    `FilteredIndex`. Either the model gains an index to read that is not the document's, or the
    digest gets a small model of its own that reuses the cell formatting. Decide that before
    anything else here.
    *Scope check:* it is **not** a dock. `SPEC.md` §8 promises panes attach left or right and never
    as a strip above or below the log, and §5a keeps the document area free of them — so this
    belongs inside the view, under the table and above the Find bar, which also makes it **per-view
    state** (invariant #7): two views of one log may want different digests, and both read the same
    per-file rules.

13. **Flash the window on a match.** A rule marks its tab, or bounces the taskbar entry, when a new
    matching record arrives while the log is not being looked at.

14. **A desktop notification on a match.** The same trigger, out to the OS notification service.

**13 and 14 need a product ruling before either is designed, and the ruling may well be "no".**
`SPEC.md` §11 lists alerting as a non-goal and the Watch-outs below already call "alert me when
ERROR appears" squarely out of scope. That was written about *filters*; these arrive attached to a
rule the user configured by hand, which is a narrower thing, and #12 does not cross the line at all
— it is a view. But 13 and 14 land on the far side of it as stated: a background process that
interrupts is a different product from a viewer, and it brings the whole tail of questions with it
(rate limiting, what happens when a rule matches ten thousand records, whether the notification
outlives the app). If they are wanted, the cheapest honest version is the **tab marker only** —
in-window, no OS surface, no daemon — and it should be judged on its own rather than smuggled in
behind #12.

## Watch-outs

- **Per-subsystem record counts in the Filters pane** would be genuinely useful and sit right on
  §11's "no charts, statistics, or alerting" line. Worth an explicit ruling rather than drifting
  into it.
- **Anything shaped like "alert me when ERROR appears"** is squarely a non-goal. The tailing
  equivalent that is not is #3's unread marker — and, arguably, #12's digest pane, which shows the
  newest match per rule without interrupting anything. #13 and #14 are the ones that need the
  ruling; see the note under them.

## A suggested order

#1 and #2 as one milestone — both are about reading a *filtered* log and each is small. The record
menu that was to lead them has **shipped** (`SPEC.md` §5, `ARCHITECTURE.md` §7.4), which is also
what made the pair worth grouping: pointing at a record is how the filter gets set in the first
place. Then #4 as the milestone after, since it also unlocks #5 for free.

**Update, 2026-08-06:** #1 shipped on its own as M15, not paired with #2. Splitting them was right —
#1 turned out to have real depth in the live-append path, and #2 has none of that. #2 is still the
obvious next small one.

**Update, 2026-08-25:** #2 shipped as M24, and the 2026-08-06 note was right on both counts — it
had none of #1's depth, and the whole of it is one branch in `LogModel::cellText` plus an enum
value. Of the rest, #8 (jump to timestamp) is the next small one and #3 (the unread marker) the
next one worth a milestone; #4 remains the one that unlocks #5 for free.
