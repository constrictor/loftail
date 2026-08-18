# Bugs

Defects in shipped behaviour — things that do the wrong thing, not things that
are missing. Gaps and rough edges belong in `improvements.md`.

Both were found on 2026-08-18 while implementing that file's 22 items: each was
turned up by the work running next to it, reported, and deliberately left alone
as out of scope for the item that found it.

Nothing here has been fixed. Line numbers are as of commit 96a3e97.

---

### 1. Find, Find Next and Find Previous are live with no log open

`MainWindow.cpp:525`, `:531` and `:536`. They are the only Edit-menu actions
missing from `updateActionStates()` — Copy, Copy as Columns and Select All, the
three entries immediately above them, are all tracked. So with no file open the
menu offers all three, Edit ▸ Find opens nothing (its handler tests
`m_activeView` and returns), and F3 does nothing at all.

This is exactly the defect `3bacaca` fixed for View ▸ Clear Filters; these three
are what is left of it. It is *not* the same one-line fix: all three are local
`QAction *` variables with no member pointers, and `findAction` has no object
name — the other two acquired one when Find learned to report its result. It
needs three members, one object name, three lines in `updateActionStates()`, and
a decision about whether Find Next and Find Previous should additionally require
a non-empty query.

### 2. The Highlighters tab is marked for every log, so the marker says nothing

`HighlighterPane::hasRules()` (`HighlighterPane.h:94`) is `!m_rules.isEmpty()`,
and `updateActivity()` (`HighlighterPane.cpp:1137`) emits `activityChanged` from
it — which is what puts the dot on the Highlighters dock tab. Since `c3781fd` a
freshly opened log arrives with three default rules, so that is true for every
log from the moment it opens.

The dot means "this pane is in force", and its entire value is in being
*sometimes absent*: the Filters tab's equivalent stays off until the user filters
something, which is what makes it worth glancing at. On the Highlighters tab it
is now always on, so it carries no information, and it has consumed the one
signal that used to say "somebody set rules on this log".

Truthful and useless, which is the worst combination for a status marker.

Fix: mark only when the rules differ from `HighlighterSet::defaults()`. That
needs an equality operator on `HighlightRule`, which does not exist today. Note
what the comparison has to cover: a seeded rule that has been reordered, unticked
or recoloured is a difference, and a seeded rule the user has never touched is
not — so it is the whole rule list compared in order, not a count.
