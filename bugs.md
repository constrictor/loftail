# Bugs

Defects in shipped behaviour — things that do the wrong thing, not things that
are missing. Gaps and rough edges belong in `improvements.md`.

Found on 2026-08-18 by a review pass over the 26 commits of that day: three
agents driving the real UI (Xvfb + xdotool, screenshots measured with PIL) over
the record table, the panes and the window, and a fourth reading the code. Ranked
by severity, not by area. The first entry — a narrowing conversion that MSVC
refuses, which broke the Windows leg of CI at HEAD — has since been fixed and
removed, and so have entry 1, the estimator reading past a measured block when a
log grew under Line Wrap ▸ Always On, entry 2, every tab but the last one
showing none of its records after several logs were opened at once, entry 3,
wrapped message text clipped off the bottom of every wrapped record, and entry 4,
a wrapped record re-flowing to different line breaks the moment Find was armed.
The numbers left behind are not reused: the entries below keep the ones they were
given, so they can still be referred to by number.

The rest are unfixed. Line numbers are as of commit 35e8cb9.

---

### 5. Selecting a run rewrites the log's seeded highlight rules and saves them

`HighlighterPane::refreshTimeBounds()` (`HighlighterPane.cpp:1278`) does
`m_rules[row].match = m_axes->criteria(); commit();` unconditionally, even though
`AxisEditor::refreshTimeBounds()` early-returns when nothing moved. `criteria()`
always returns *valid* start/end bounds — the editors always hold a datetime —
while `HighlighterSet::defaults()` leaves both invalid. So the comparison fails
and `hasCustomRules()` flips true.

Observed: a fresh window with `app.log` open reads "Highlighters" correctly.
Clicking **All runs**, or any run row, or choosing Timestamp Format ▸ Seconds,
immediately changes it to "Highlighters •" while the rule table shows no change
at all — same three rules, same ticks, same colours, same order. Switching the
display mode back does not clear it. `followLastRunIfMoved` reaches the same
line, so a live log that simply restarts marks itself with no gesture at all.

Worse than a wrong marker: `commit()` → `syncToDocument()` puts the rewritten
rules on the Document, so the session persists them and the log comes back marked
on the next launch. `SPEC.md` §7 says the mark means "a rule added, or one of the
three switched off, recoloured, re-aimed or moved… Put the three back exactly and
the mark goes out". None of that happened, and there is no way back.

Fix: only write back when the editor actually re-rendered — have
`AxisEditor::refreshTimeBounds()` report whether it changed anything, or compare
the new criteria against `m_rules[row].match` and skip when equal. More than a
one-liner because a rule with `timeEnabled` set whose bounds genuinely were
re-rendered must still write back, and `commit()` is also what emits
`highlightersChanged()`.

### 6. The Runs pane's Regex and Case boxes apply a pattern the user has not pressed Apply for

`RunPane::buildUi()` (`RunPane.cpp:95`) connects both checkboxes' `toggled` to
`emitPattern`. Type a run-start pattern — the note under Apply correctly turns
amber, "Edited — press Apply to re-read the runs" — then tick **Regex** without
touching Apply, and the whole log is re-split on the spot: three run rows appear
and the note reverts to its quiet wording.

`SPEC.md` §3a is explicit: "the pattern **takes effect when Apply is pressed** —
splitting the log again re-reads all of it — and the pane says so in a line under
the button". `updateApplyNote()` itself counts the two boxes as part of the
pattern in force, so the pane promises Apply-gating for exactly the control that
bypasses it. The damage is the expensive one Apply exists to prevent: a half-typed
pattern applied to the whole file by a click on an unrelated checkbox.

Fix: drop the two `emitPattern` connections and leave Apply and Return as the
only route, keeping the `updateApplyNote` connections so ticking a box still
turns the note amber. `tst_runpane` has no case for it; one belongs beside
`aTypedPatternMakesTheNoteAskForApply`.

### 7. A log that turns up is read the instant it exists, so its format is judged against an empty file

`LiveController::checkWhileWaiting()` (`LiveController.cpp:260`) resumes a local
waiting document on `logSourceAvailable(path)` — existence. For a real logging
application that is true before the first record is written.

Observed: open a path that does not exist (correct waiting tab), `touch` it, then
append 40 well-formed records. A Preferences dialog appears whose preview says
"No sample lines to preview." and whose Detect button is disabled, while the log
behind it has 40 records that parse perfectly. Cancelling leaves the status bar
reading `format not recognised — File ▸ Preferences…` for the rest of the
session with every column correctly filled in. In a background tab there is no
dialog at all and the tab keeps the same wrong notice silently.

`CLAUDE.md` already records this trap for the spooled path — "a resume with
nothing to read settles the format against an empty sample, leaves it unsettled
forever" — and guards it with `notReadyYet()`. The local path has no such guard.

Fix: make the local branch require bytes rather than existence — the local
equivalent of `notReadyYet()`. Mind the M17 note that `resume()` cannot be undone,
and that a log which genuinely stays empty must still open as an ordinary empty
tab rather than waiting for ever.

### 8. The Priority column opens too narrow to say "Priority" — the exact failure SPEC says was fixed

`LogView::seedWidthOf()` (`LogView.cpp:2046`) adds a flat `kColumnPadding` of
10 px to the caption width, but a header section spends `PM_HeaderMargin` on each
side — 12 px on Breeze, the reference desktop's style. So the header renders
`Priori…` on a clean config at 1600×1000. Fit to Contents does not fix it
(73 → 72 px); no zoom level fixes it. The header tooltip correctly says
"Priority", so the column knowingly reports its own truncation.

`SPEC.md` §5 says "a *Priority* column too narrow to fit the word *Priority*
cannot say what it is", and the comment above `seedWidthOf` says "The caption
always fits: a column headed 'Priorit' is the very thing this replaces". It does
not show on Fusion (4 px of margin), which is why it survived — the bug is
style-dependent and shows on the style the user runs.

A smaller relative of the same thing: the per-role allowance is
`seedColumnChars(role) * charWidth()`, and `charWidth()` is a rounded-down single
glyph advance multiplied out, so the gutter partly vanishes into accumulated
rounding. Measured across zoom levels, the Time column's gutter is 14 px at 6 pt,
**1 px at 7 pt**, 19 px at 8 pt, 5 px at 9 pt — at 7 pt the timestamp visibly
touches the thread name (`09:00:00,137scheduler`).

Fix: measure the caption against the rect the style puts it in.
`truncatedHeaderText()` (`:1695`) already does exactly this and carries the
comment explaining why — take its `QStyleOptionHeader` / `SE_HeaderLabel` inset
and add it to the caption term of `seedWidthOf()` and `contentWidthOf()`. For the
rounding, measure a representative string rather than multiplying one glyph.

### 9. Moving a column while Line Wrap is Always On leaves every row measured against the old width

`LogView.cpp:385`, the `QHeaderView::sectionMoved` connection, does only
`viewport()->update(); emit columnLayoutChanged();`. But `viewportCols()` is
derived from `sectionViewportPosition(msgCol)`, so moving any column across the
message column changes the wrap width exactly as a resize does — and
`sectionResized` knows it, calling `recomputeGeometry()`, which restarts the
debounce and reaches `m_estimated.setColumns(viewportCols())`.

Observed: with Always On, drag the Message header to first position. Every record
is drawn with a large blank gap under its text — payload records painted in four
lines occupying about ten lines of row. Resizing the window by 20 px and back
corrects every row instantly. A move in the other direction leaves rows too
short, which is entry 4's clipping made worse.

Fix: have the `sectionMoved` lambda call `recomputeGeometry()` as `sectionResized`
does. It already routes to the debounce in estimating mode and is a no-op
elsewhere.

### 10. The Find bar's buttons slide under the pointer when the status text changes

`FindBar.cpp:29` and `:55` put the query box and the status label in one
stretchable row with the four controls. The label's width therefore moves
everything after it.

Reproduced with the pointer parked on ▼ and never moved, on a query with two
matches: click → `2 of 2`, ▼ still under the pointer; click → `1 of 2, wrapped to
the top`, the row shifts about 130 px left and the **Case checkbox** is now under
the pointer; click → Case is ticked and the search silently restarts
case-sensitive. Measured travel across states is 160 px.

The wrap note's whole purpose is to appear and disappear as the reader steps
through matches, so stepping with the mouse — which is what ▲/▼ are for — reaches
this on any query, and the click after a wrap changes the search options instead
of advancing. The query box shrinking under the caret while typing is the same
cause, milder.

Fix: take the status out of the stretchable flow — a fixed minimum width sized to
the longest wording, or its own fixed cell after the ✕, or move the four controls
to the left of the query box. A size policy on the label alone is not enough: it
must not be able to take width from the query box, which is what shifts
everything between them.

### 11. One record fills the whole viewport when the Message column is narrow and wrap is on

With Line Wrap ▸ Always On and the document area about 640 px wide (a 1100 px
window with the side pane docked), the table draws exactly one record per screen
— one row's colour filling the entire viewport, at both ends of the file.
Widening the window so the Message column fits restores normal rendering
immediately, on both a freshly opened view and one whose wrap mode came from the
settings tree.

A message column at or near zero width appears to give the estimated line count
no bound. `SPEC.md` §5 says every record wraps to the viewport width; it does not
contemplate a record taller than the viewport for want of one.

Fix: clamp the wrap width used for the estimated line count to a sensible
minimum, and check the same clamp on the exact path's `measureWrappedLines()`.

### 12. A marked wrapped cell redraws the whole record layout once per match

`drawWrappedCell()`'s `drawLayout` (`LogView.cpp:227`) is
`layout.draw(&p, rect.topLeft())` — every line of the paragraph — and `paintMark`
(`:246`) calls it once per (display line × intersecting span) pair with only the
clip changed. `spans()` is capped at 64 marks and a record at 100 display lines,
so one matching message cell can cost 64 full 100-line layout draws per repaint.

Benchmarked: a 94-line paragraph draws once in 2.2 ms; 64 clipped redraws take
**50.4 ms**, for one cell of one record. A 20-line record is still ~11 ms, and
every matching record on screen pays it, on every repaint — scroll, resize,
ingest tick, tab switch. A single-letter query under wrap-always-on saturates the
cap on most records, which is an ordinary thing to type.

`60b4aa5`'s own contract is that marking costs "one `spans()` call per
already-decoded string", and the 64 cap exists because "this is the paint path".
The per-span cost was capped; the per-span work was not — the cap is what makes
the 64× multiplier reachable rather than unbounded, not what makes it cheap.

Fix: `paintMark` already knows which `QTextLine` the patch belongs to, so pass a
callable that redraws only that line (`QTextLine::draw`) instead of the whole
layout. The glyphs still come from the same layout at the same position, so the
"redrawn by the SAME call that drew them" rule survives, and O(lines × spans)
becomes O(spans). `drawElidedCell` has the same shape one order down and deserves
the same treatment, though its string is bounded by the column width.

### 13. A refused archive member says "connecting…" in the view for ever

`MainWindow.cpp:2201` writes the view's placeholder on a waiting *transition*
only, while `LiveController::checkWhileWaiting()` (`LiveController.cpp:264`)
refreshes the status bar every tick and never touches it.

Observed: `loftail bundle.tar.gz/var/log/nosuch.log`. The tab opens waiting, and
45 seconds later the view still reads **connecting…** in the middle of the empty
table while the status bar reads "The archive holds no member named
var/log/nosuch.log." Nothing is connecting, and in this case there is no network
at all.

`SPEC.md` §3 says "the view says what it is waiting for, and the status bar says
why". The same staleness applies to any spooled log whose reason changes while it
waits.

Fix: have `checkWhileWaiting()` re-emit the waiting reason when it differs from
the last published one, and update `Document::m_waitReason` so a new view and the
tab tooltip agree. Guard against re-emitting every tick by comparing against the
last text, as `publishSourceStatus` already does.

### 14. A refusal whose address has no file-name part is reported as "Cannot open : …"

`loftail ssh://` puts `Cannot open : Not a valid remote log address: ssh://` in
the strip above the tabs — nothing before the colon.
`RemoteLocation::plainDisplayName()` (`RemoteLocation.cpp:101`) is
`QFileInfo(path).fileName()`, which is empty for `ssh://` and for any address
ending in a slash (`/var/log/` gives `""` too).

`SPEC.md` §3 says the strip "names the address and the reason". In the
multi-refusal form (`"%1: %2"`) the line begins with a bare colon and there is no
way to tell which of several addresses it was.

Fix: fall back to the raw address when the computed display name is empty. The
same function names tab labels and recent-file entries, so one fallback covers
all three.

### 15. `--pattern` overrides a remembered format, contradicting both `--help` and SPEC

`MainWindow::openFile()` (`MainWindow.cpp:1382`) applies the switch
unconditionally: `if (!pattern.isEmpty()) settings.pattern = pattern;`. With a
saved `*.log` pattern node holding a working conversion pattern,
`loftail --pattern "…" rotate.log` opens the log as unparsed plain text.

`--help` (`CommandLine.cpp:9`) says the switch is "Used only for a file loftail
has not seen before; a file with a remembered format ignores it", and `SPEC.md`
§3 says the same. M20 made every level resolve on every open and the override was
left unconditional, so the shipped text describes behaviour the shipped code does
not have.

Which side moves is a product call. Fix: either gate the override on the resolved
node being the built-in default, or — more likely what is wanted — keep the
override and rewrite both sentences to say the pattern applies to every file on
that command line. Note the same `pattern.isEmpty()` also decides
`promptIfNoMatch`, so the "no blocking dialog for a scripted open" property has
to survive either way.

### 16. F3 with the Find bar closed answers into a bar nobody can see

`MainWindow::runFind()` (`MainWindow.cpp:2791`) reports `no search text` through
`findBar->setStatus()` (`:2807`) when a deliberate navigation is made on an empty
query. `7f32c65` added that branch precisely so that F3 stops being silent — but
the bar is a `DocumentView` child that starts hidden and is only put on screen by
`DocumentView::activateFind()` (`DocumentView.cpp:105`), which nothing on this
path calls.

So a reader who has opened a log and pressed F3 without ever opening the Find bar
gets exactly the silence the branch was written to remove. What it does cover is
the case where the bar is already open — which needed it least, since the empty
box is right there.

It was reported when the branch was written and left alone deliberately, because
revealing the bar changes what the action *does* and that was outside the scope
of an enablement fix. It is still the right change: F3 asking a question is a
reasonable moment to put the box on screen.

Fix: have the empty-query navigation branch call `activateFind()`, which already
does the show-and-focus. Decide whether focus moves with it (it probably should),
and keep `FindBar`'s Escape-closes handling working. The branch is reached only
on an empty query, so a *not found* on a real query must go on leaving focus
where it is.

### 17. `viewportCols()` trusts `averageCharWidth()`, which is truncated, so Always On clips the tail of long records

`LogView::viewportCols()` divides the message column's width by
`fontMetrics().averageCharWidth()`. That is an **integer**, and at the
reference fixed-pitch face at the shipped 9 pt it answers **7** where the real
advance is **7.21875** — so the column count comes out too high, the estimated
geometry gives the record too few rows, and the wrapped cell is clipped to the
rows it was given. Nothing says so: a wrapped message is deliberately not elided
and offers no tooltip (`SPEC.md` §5).

This is the horizontal sibling of the fix in `a172374`, which replaced
`QFontMetrics::height()` with `qCeil(QFontMetricsF::height())` for exactly the
same reason one axis over, and it survived that pass untouched.

Measured with Always On, a 880 px viewport (message column 395 px) and one record
whose message is N `a`s:

```
N=54  : estimator cols 56, record 1 row , the text needs 1 — ok
N=55  : estimator cols 56, record 1 row , the text needs 2 — CLIPPED
N=56  : estimator cols 56, record 1 row , the text needs 2 — CLIPPED
N=110 : estimator cols 56, record 2 rows, the text needs 3 — CLIPPED
```

`averageCharWidth()` is below the true advance at **15 of the 27 point sizes the
zoom offers** — 7, 9, 11, 13, 14, 15, 18, 19, 20, 22, 24, 26, 28, 29, 30 —
including the default. The share of message-line lengths that lose at least one
line at 9 pt and 395 px: 2% under 100 characters, 14% for 100–300, 29% for
300–600, 60% above 600.

Not fixed by the entry-4 work, which made the two wrapped-cell paths agree with
each other and with `measureWrappedLines()`; the count they are all clipped to
still comes from here, and `layoutWrappedText()` stops at the same
`rect.height() / lineHeight()` rows `drawText` used to clip at.

Fix: take the advance from `QFontMetricsF` — or `horizontalAdvance()` of a sample
character — and floor the division, rather than trusting the integer
`averageCharWidth()`. `updateScrollBars()` also uses it, for the horizontal
scrollbar's single step, where being a fraction of a character out costs nothing.

---

## Seen but not confirmed

Not findings. Recorded so the next pass knows where to look rather than
rediscovering them.

- **The application exited twice, silently.** No output on stdout or stderr, no
  message, both times shortly after a screenshot-and-drag sequence in the panes.
  Not reproducible deliberately afterwards — the same drag ran clean repeatedly.
  Distinct from entry 2, which aborts loudly with a stack trace.
- **The empty rule table's placeholder may not be muted.** It measured at full
  text brightness (peak 252,252,252, brighter than the New button's label at 240)
  while the pane's `placeholderText` role measured `#42464a` after show. That
  looks like `applyPlaceholderColour()` reading the palette during `buildUi()`,
  before the widget's final palette resolves — but it could not be separated
  cleanly from subpixel antialiasing in the capture.
- **The Notify degrade path is still unexercised.** The review box has a live
  session bus, so notifications reported as supported and the bell buttons
  behaved normally. Whether a disabled Notify button is drawn distinguishably
  from an off one remains unknown, on what `CLAUDE.md` says is the common path on
  a stock GNOME/Wayland session.
