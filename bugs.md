# Bugs

Defects in shipped behaviour — things that do the wrong thing, not things that
are missing. Gaps and rough edges belong in `improvements.md`.

The one left was found on 2026-08-18 while implementing that file's 22 items: it
was turned up by the work running next to it, reported, and deliberately left
alone as out of scope for the item that found it.

It has not been fixed. Line numbers are as of commit 96a3e97.

---

### 1. The Highlighters tab is marked for every log, so the marker says nothing

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
