# Bugs

Defects in shipped behaviour — things that do the wrong thing, not things that
are missing. Gaps and rough edges belong in `improvements.md`.

All three were found on 2026-08-18 while implementing that file's 22 items: each
was turned up by the work running next to it, reported, and deliberately left
alone as out of scope for the item that found it.

Nothing here has been fixed. Line numbers are as of commit 96a3e97.

---

### 1. Copying a large selection freezes the window

`LogView::copySelectionRaw()` (`LogView.cpp:1632`) and `copySelectionAsColumns()`
(`:1659`) both begin at `selectedRecordsSorted()` (`:1215`), which asks
`QItemSelectionModel::selectedRows(0)` — one `QModelIndex` materialised per
selected row — and copies the result into a `QVector<int>` of the same length.
Each then decodes every selected record into a `QStringList` and `join()`s it, so
the whole text exists twice at once. On the GUI thread, unbounded, with no
progress and no way to cancel.

This was always reachable by clicking the first record and Shift+clicking the
last, but Edit ▸ Select All (`581e0b3`) now puts it one keystroke away on a file
of any size. On a four-million-record log it is hundreds of megabytes to
gigabytes of `QString`, plus the same again for the join, and a freeze measured
in minutes — long enough to be indistinguishable from a hang, and a real hang if
the allocation fails.

Invariant #1 exists precisely so that a large log costs 32 bytes per record. The
copy path is the one place that spends the entire file's text at once, and it is
now the easiest thing in the application to trigger by accident.

Fix: bound it. A record count above which the copy asks first is the cheap
version; a chunked build behind a cancellable progress dialog is the honest one.
The doubling is separately avoidable by reserving one `QString` and appending
into it rather than building a list and joining.

### 2. Find, Find Next and Find Previous are live with no log open

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

### 3. The Highlighters tab is marked for every log, so the marker says nothing

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
