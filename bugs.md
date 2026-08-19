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
Entry 5 has gone the same way: selecting a run rewrote the log's seeded highlight
rules and saved them, and the fix took a second, unreported defect with it — a
display-zone change re-pointed every time-bounded rule except the one the pane
happened to be showing. Entry 6 has gone too: the Runs pane's Regex and Case
boxes applied the pattern standing in the field the moment they were ticked, with
no Apply pressed — re-splitting the log, persisting the half-typed pattern into
its settings node and converting a pinned run back to "Last run". And so has
entry 7, a log read the instant it existed, so that its format and its encoding
were settled against an empty file and could never be settled again — which took
two further defects with it: an empty log could not be opened at all without
accepting a format nobody had been shown anything to judge, and a background or
restored tab was told silently that its format was not recognised.
Entry 8 has gone as well: the Priority column opened too narrow to say "Priority"
on the style the user runs, because the caption was measured against the raw
header section rather than against the rect the style puts a label in. Three more
defects went with it — Fit to Contents opened any column whose values were shorter
than its caption truncated for the same reason; the per-role allowance was one
glyph's integer advance multiplied out, so at four zoom levels the Time column
opened narrower than the timestamp it was seeded for; and a zoom that made the
line space smaller re-attached follow on a view the reader had detached and threw
them to the end of the log.
Entry 9 has gone too: moving a column across the message column left every row
measured against the width the message had before the drop — a blank band under
every record one way, the tail of every message clipped away the other. Its fix
took an unreported sibling with it, since a horizontal scroll slides the same
origin and said no more about it than the move did.
Entry 11 has gone as well: with the message column's origin at or past the right edge
of the viewport — which the scan-completion column seed puts it at on an ordinary narrow
window — the wrap width was floored at one pixel, so every record wrapped at one
character per line, measured the 100-line display cap and filled the whole screen by
itself. The wrap width is now one expression with a floor of twenty characters, and the
fix took two unreported defects with it: the exact path measured the selected record at a
50 px floor while the paint laid it out at a 10 px one, so below about 28 px of available
width the record was cut off mid-word with no ellipsis and no tooltip; and the vertical
scroll range was cast into `int` from a `qint64` total that the display cap brings within
range of a large log, where a negative range is one `QAbstractSlider` collapses to zero.

Entry 10 has gone as well: the Find bar's status label shared one stretchable row
with the query box, which was that row's only elastic item, so every pixel the
wording grew by was taken from the box and every control after it — ▲, ▼, Regex,
Case and the close button — slid left by exactly that much. With the pointer
parked on ▼, the wrap note appearing at the last match walked the Case checkbox
under it, and the click meant for "next match" restarted the search
case-sensitively. The label now has a cell whose width comes from the bar and not
from the text, and the report is elided into it with the whole of it on the
tooltip.

Entry 12 has gone too: a marked cell was redrawn in full once per matched run, and a
cell's redraw is its WHOLE paragraph — so a wrapped record up to 100 display lines tall
was drawn again up to 64 times over, which took one repaint of it from 1.4 ms to 17.4 ms
on every scroll, resize, ingest tick and tab switch. The runs of one cell now go into a
`QRegion` and the redraw is issued once through it, which is pixel-identical to what it
replaced where a per-line redraw is not. The fix took an unreported defect with it: the
elided path placed its marks by summing per-character advances, which is the logical
width of a prefix and not the visual position of a shaped run, so a match in Arabic or
Hebrew text was marked over the wrong glyphs — 48 px of a 77 px run in the case measured.

Entry 13 has gone as well: a waiting tab's explanation of itself was written once,
on the transition into waiting, and nothing revised it — so a spooled log, which
since M17 begins waiting on "connecting…" before the far end has answered, kept
saying that in the view and in its tab tooltip for the life of the tab while only
the status bar ever showed the refusal that actually came back. The reason is now
republished whenever it changes, and the fix took a second, unreported symptom
with it: the status line read "connecting…  |  The archive holds no member named
…", because it joins the stale reason to the live status whenever the two differ.

The numbers left behind are not reused: the entries below keep the ones they were
given, so they can still be referred to by number.

The rest are unfixed. Line numbers are as of commit 35e8cb9.

---

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

**Amended 2026-08-19: this entry is narrower than the defect it describes.** It is
written about the empty-query branch only. Measured: with a **non-empty** query,
`Esc` closes the bar (clearing the marks via `DocumentView.cpp:57`), and a
subsequent **F3 still searches** — it re-arms the find marks in the table, moves
the cursor, and writes `2 of 2` / `…, wrapped to the top` into the **hidden**
label. So marks reappear over the table with no bar and no visible query, and the
wrap report is silent again on a real query, not just on an empty one.
Reproduction: open a log, Ctrl+F, type a query with two matches, Esc, then F3
twice — the cursor wraps to the first match with nothing on screen saying so.
Same fix site (`runFind()` calling `activateFind()`), wider trigger.

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

### 18. Under Always On the message column is drawn over any column moved after it

`LogView.cpp:1157` wraps the message cell within `availW = qMax(10, vw - x)` —
its own origin to the **right edge of the viewport**, whatever its section width
— and paints the columns in visual order, so the message is drawn first and the
fields after it are drawn on top of it with no fill of their own. (Line numbers
in this entry are as of the commit that fixed entry 9.) The selected record's
cell under *Selected record only* is the same expression at `:1209`, so the mode
does not save it either. That is exactly
right while the message column is last, which is where the format puts it and
where `SPEC.md` §5's "every record wraps to the viewport width" is written from.
It is not right once the column has been dragged: the header is movable
(`LogView.cpp:380`), and with the message column moved off the end every
following field's cell sits inside the message's wrap area and the two texts are
drawn one over the other on the record's first line — two strings of glyphs in
the same colour on the same pixels, unreadable, for as long as the columns stay
in that order.

Measured while fixing entry 9: with the message column dragged to visual 0 in a
880×400 view, its wrap width is the whole 864 px of the viewport and the Time,
Thread, Priority and Subsystem cells occupy 0–440 px of it. The lines below the
first are message text alone, so the damage is confined to the record's first
line — which is the one carrying every field the reader moved the column to see.

Not entry 9, which is now fixed: the rows are the right height and every
character is drawn. What is wrong is where. Two shapes of fix are open and the
choice is a product decision rather than a mechanical one: wrap the message
within its own section when it is not the last visual column (which makes the
wrap width a per-layout question rather than "to the right edge", and would want
`SPEC.md` §5 rewording), or wrap it to the right edge only while it is last and
otherwise treat a move as the reader asking for the narrow column. Note that the
height model does not care either way — `viewportCols()` and the paint already
derive the same width from the same origin, so both stay in step as long as they
keep deriving it from one expression. (That expression now exists and is
`LogView::messageWrapWidth()`, entry 11's fix.)

---

### 19. The column seed never consults the viewport width, so a view can collapse when indexing finishes

`LogView::seedColumnWidths()` sizes each column from the font and the data —
`seedWidthOf()` takes the wider of the header caption and a per-role allowance in
characters, and for Subsystem and Thread that allowance is the widest interned
name up to `kSeedNameMaxChars` (40). Nothing in it asks how wide the view is. At
the shipped 9 pt that is up to about 850 px of Time, Thread, Priority and
Subsystem before the Message column starts — more than the whole document area of
a 1100 px window with a pane docked.

The timing is what makes it a defect rather than a preference.
`MainWindow::onIndexFinished` (`MainWindow.cpp:2100`) re-seeds Subsystem and
Thread once the intern tables are complete, which is the only moment those two
columns are measured exactly. So a view that rendered perfectly well while it was
indexing — narrow columns, the message column with most of the width — jumps to
the seeded widths when the scan completes and pushes the message column's origin
to or past the right edge of the viewport, on any window narrower than that sum.
The reader sees the table re-lay itself out and the messages go away at the moment
the log finishes loading.

Entry 11's fix does not touch this. It clamps the wrap *width* at a floor of
twenty characters, so the records keep sane heights and a screen still holds
several of them; the underlying condition — the columns before Message do not fit,
so the message is drawn from an origin at or past the right edge and is reached
only by scrolling sideways — is exactly as it was, and so is the collapse-on-scan-
completion timing.

Two shapes of fix, and the choice is a product decision. Bound each seeded column
to a share of the viewport, so no column may be seeded past what is on screen —
cheap, and it leaves a dragged width alone since `m_userSizedColumns` already
guards those. Or bound the seed by what is left: seed in role order and stop
widening once the message column would be pushed below a usable width. Either way
the seed must go on being the two-callers-only pass it is (`CLAUDE.md`: constructor
and `onIndexFinished`, never the ingest path), and neither may overwrite a width
the user set.

It interacts with entry 18. If the answer there is that the message cell wraps
within its own section rather than to the viewport's right edge, then a message
column pushed off the edge is a column with no visible cell at all rather than one
whose text is off to the right — so the two want deciding together.

---

### 20. An archived log opened while its container is missing waits for ever, and never comes back when the container appears

Line numbers in this entry are as of the commit that fixed the waiting-reason
staleness described in the preamble; `Document.cpp` moved by about eleven lines
between confirming this and recording it.

`Document::resume()` opens with `OpenPolicy::Reuse` (`Document.cpp:274-300`), and
`openSpooled()` refuses to *create* a spool under `Reuse`
(`src/core/LogSourceFactory.cpp:41-48`) — it will only re-attach to one the registry
already holds. That is right for the case it was written for, a remote document whose
spool is already live and is what did the waiting. It is wrong for an archived log
whose container is not there yet, because that document never got a spool in the first
place.

When a local container does not exist at open time, `ArchiveFetcher::start()` fails
outright (`ArchiveFetcher.cpp:197-205`), so `openLogSource()` returns null and
`prepare()` falls into the source-less waiting branch (`Document.cpp:143-147`) —
reached rather than refused because `logSourceAvailable()` for an archive address asks
only about the container (`RemoteLocation.cpp:189-197`). From that point the spool
registry is permanently empty for that key, so every `resume()` on every tick fails,
for ever, and nothing in the loop can populate it.

Reproduced at core level. Open `bundle.tar.gz/var/log/app.log` before the file exists,
create the container **with** the member in it, then poll:

```
prepare -> true waiting: true reason: "app.log (bundle.tar.gz) has not appeared yet…" source: 0x0
--- creating the container now ---
  resumeRequested -> resume() returned false        (x6 ticks, records: 0)
  ...
  resumeRequested -> resume() returned true         <-- only after a SECOND Document
fresh document on the same address -> true waiting: false records: 2
```

The last two lines are the proof: the waiting document recovered only once an
unrelated `Document` opened the same address `Interactive`ly and thereby populated the
registry. Also reproduced through the real `MainWindow` — the tab still reads "has not
appeared yet" 9 s after the container was created, with 16 poll ticks in between.

This breaks `SPEC.md:92` ("The moment the log appears it is read and followed") and
`SPEC.md:98` explicitly: a session restored with an archived log on a share that is not
mounted yet gets a tab that can never recover, which is the exact case that promise
names.

It is not remote-specific, and the reason is worth stating: `logSourceAvailable()` is
optimistic for `ssh://`, so a remote address fails the open outright instead of
reaching this state. Local containers are what hit it.

Note that this is a different mechanism from the staleness recorded in the preamble
above (the former entry 13), which was about the *sentence* a waiting tab shows. This
one is about a wait that cannot end.

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
