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

Entry 14 has gone: an address with no file-name part was called "" by
`logSourceDisplayName()`, so a refusal read "Cannot open : …" with nothing before
the colon and, in the multi form, a list of lines each beginning with one. The
name is now the deepest non-empty segment of the address, then the scheme word for
a remote address with nothing else, then a placeholder — a segment and never the
raw address, because `tabLabelsFor()` groups on this string and builds a label as
parent directories plus it. That reach turned out to be the larger half of it: the
same "" wore a waiting tab's marker with no name beside it, trailed the title bar
off after the em dash, and put a blank clickable row in the recent-files menu, all
of which the fix covers at once. It took an unreported credential leak with it. The
rule that a URL password is dropped had only ever applied downstream of a
successful `RemoteLocation::parse()`, and an address parse REFUSES —
`ssh://deploy:hunter2@web1`, which has no path — was echoed back in full twice
over: once as the name half, since `QFileInfo::fileName()` on a pathless URL hands
back the whole userinfo, and once inside "Not a valid remote log address: %1".
Both now go through one `RemoteLocation::withoutPassword()`.

Entry 15 has gone too, and the product ruling on it went further than either fix
the entry proposed: `--pattern` still overrides every level of the settings tree,
but it is now judged against the log rather than believed. A pattern that parses
is stored under that log's key (unless it is what the log already inherits, where
the redundancy rule erases the node as it always has); a pattern that does not
raises Preferences, and dismissing that opens no tab and writes nothing. The
mechanism is one deleted parameter: `openWithSettings()`'s `promptIfNoMatch`,
which the command line was the only caller ever to pass false. That single flag
had also been switching off the persistence check, so the entry understated the
defect — a mistyped switch did not merely show plain text for one run, it wrote
the unparseable pattern into `logsettings.json` under the log's own key, after
which every later launch with no switch at all resolved to it and raised the very
dialog the switch had been exempted from; where the switch happened to equal the
defaults it deleted the log's remembered format outright. A second, unreported
defect went with it: an explicitly empty value (`--pattern "$FMT"` with `FMT`
unset) is now stated to be the bare launch rather than left to fall out of
`isEmpty()` meaning two different things on the same line.

Entry 16 has gone as well, in the wider form its amendment described: F3 answered
into a bar that need not be on screen. Every branch of `runFind()` reports into the
Find bar's own label — including the match-and-wrap report, so a reader who had
closed the bar with Escape and pressed F3 got the cursor moved, the marks put back
over the table and the sentence explaining both written where nobody could read it.
`runFind()` now reveals the bar above every branch, through a new
`FindBar::reveal()` that shows and focuses and touches nothing else: revealing
through `activate()` would have cleared the status it was added to show, and a
reveal without the focus would have shipped a bar closable only with the ✕ button,
since Escape is handled in the bar and the bar is a sibling of the table rather
than an ancestor of it. The mirror-image state the entry did not name — Ctrl+F
reopening a bar whose query the table is not marking — is settled by leaving Ctrl+F
searching nothing on purpose: a reopened bar shows the standing query selected for
replacement, with a blank report and no marks, so it claims nothing it is not
showing.

Entry 17 has gone as well, and the note it opened with — that it is the horizontal
sibling of entry 3's line pitch — held all the way down. `viewportCols()` divided
the message column's width by `QFontMetrics::averageCharWidth()`, and every integer
advance Qt offers is a rounded one, below the advance the layout actually uses at
the same 15 of the 27 point sizes. So Always On counted 54 characters into a column
that holds 52, gave a 53-character record one row, and drew the rest nowhere — no
ellipsis and no tooltip, since a wrapped message deliberately offers neither. The
advance is fractional now and the division floored, and the fix had to take the
twenty-character floor with it: `minWrapWidth()` multiplied the same truncated
advance out and `viewportCols()` re-raises the count to it, so one column of
clipping survived a fix to the division alone. What the fix does NOT reach is
recorded as entry 22 below, and was found while testing it.

Entry 18 has gone too: under Always On the message cell was laid out from its own
origin to the right edge of the viewport whatever its section width, and the columns
are painted in visual order with no fill of their own, so every field after the
message was drawn on top of it on the record's first line — the line carrying every
field there is. It needed no gesture to reach, which the entry did not know: an
ordinary `%d [%t] %-5p %m (%c)%n` puts the subsystem after the message, and turning
Always On on was the whole of what the reader did. The message now wraps within its
own section whenever anything is visible after it, and to the viewport's right edge —
byte for byte what shipped — while it is the last visible column. The fix took an
unreported defect with it: hiding the Message column from the header menu left every
record at the wrapped height its now-invisible message wanted, so a log rendered as
one line of fields followed by three blank rows, per record, with the scrollbar still
spanning the lines nothing was in.

Entry 19 has gone as well: the column seed was measured from the font and the data
and never asked how wide the view was, which is right for one column and wrong for
the sum of four. A 40-character subsystem name beside a 40-character thread name
comes to some 850 px of Time, Thread, Priority and Subsystem — more than the
document area of an ordinary window with a pane docked — so the message column's
origin landed at or past the right edge, and it landed there at the exact moment
the scan finished and `onIndexFinished` re-seeded those two columns from the
complete intern tables. A log that had been perfectly readable while it was
indexing lost its messages when it finished loading, and nothing rendered wrongly
while it did: the geometry stayed self-consistent, the widths were then saved and
restored verbatim, and "Reset Widths" re-seeded to exactly the same collapsed
layout. The seed is now bounded by the sum — the columns other than the message
must fit the viewport less forty characters of message, and where they do not, one
cap is lowered over all of them at once, widest first — with a floor at each
column's caption-and-typical-value width, which is precisely what it was seeded at
before the scan finished. So the bound gives back the growth the intern tables
asked for and never a pixel more, and Reset Widths is the way out of a session
restored with the old widths, since those come back marked as the user's own.

Entry 20 has gone too: an archived log opened while its container was not there yet
waited for ever and never came back when the container appeared, because
`ArchiveFetcher::start()` failed the open outright — which destroyed the spool, and
with it the fetcher that would have been retrying, leaving the one state the waiting
model says cannot exist: a spooled document waiting with nothing looking for its log.
It hit every archive address whose local container was absent, a bare `.gz` with no
member spelled included, and a session restored with such a log came back to a tab
that could never recover. `start()` now publishes a wait, spawns its worker and lets
the retry loop that was already there — and unreachable — go looking; a container that
is there and will not open stays a refusal that keeps its tab and says why. The fix
took two unreported defects with it: a multi-member container spelled with no member
picked was judged well-formed while it was missing, so what should have been an
outright refusal became a wait the container's arrival could not end; and a resume
that declined while the presence check said the log was back wrote nothing at all to
the diagnostics log, so a document retrying eighty times a minute for ever was
indistinguishable from one that had just missed a tick. A third, adjacent, went at the
same time: a log that exists but cannot be read said it "had not appeared yet",
sending the reader to look for a file they can see in their file manager.

The numbers left behind are not reused: the entries below keep the ones they were
given, so they can still be referred to by number.

The rest are unfixed. Line numbers are as of commit 35e8cb9.

---

### 21. A record of CJK or emoji is clipped by the same height model, and the entry-17 fix does not reach it

Found while confirming entry 17, and it is the same user-visible failure with a
different cause. `EstimatedGeometry::measuredRecordLines()` is `ceil(chars / cols)`
(`src/core/EstimatedGeometry.cpp:22`), and `cols` is one number for the whole view —
which assumes every glyph is drawn at the PRIMARY face's advance. A character the
fixed-pitch face does not carry resolves through a fallback face with a wider one:
at the reference face at 9 pt a Latin character advances 7.21875 px and U+4E2D
advances **12**. So a record of such text needs more lines than it is counted for,
and `drawWrappedCell()` clips it to the rows it was given (`LogView.cpp:307`,
`maxLines = rect.height() / lineHeight()`) — no ellipsis and no tooltip, because a
wrapped message deliberately offers neither (`SPEC.md` §5). Same class as entry 17:
text that exists nowhere on screen and nothing on screen saying so.

Measured at 9 pt in a 379 px wrap width (an 880 px viewport, Line Wrap ▸ Always On),
one record whose message is N × U+4E2D:

```
N=20 : 240 px, layout 1 line , model 1 row  — ok
N=30 : 360 px, layout 1 line , model 1 row  — ok
N=40 : 480 px, layout 2 lines, model 1 row  — CLIPPED, the second line is not drawn
N=60 : 720 px, layout 2 lines, model 2 rows — ok again, both sides round to 2
```

The model's row count is `ceil(40/52)` with entry 17 fixed and `ceil(40/54)` without
it, so the entry-17 work neither causes this nor helps it; the two are independent.
`ARCHITECTURE.md` §7.1.1 states the fixed-pitch assumption for the primary face and
§7.1.4 already notes fallback CJK/emoji overhang as the one case the mark region
behaves differently, but nothing anywhere covers the height model losing text on such
a record.

There is no arithmetic answer, because the advance is per character and the model
counts characters. The likely fix is to MEASURE such records rather than count them —
`LogView::measureWrappedLines()` already lays a cell out exactly, and it is what the
exact path uses — falling back to it when a record's text is not wholly in the
primary face (`QFontMetricsF::inFont()` per character, or a cheaper "any character
above U+02FF" screen at index time). That is a design decision, not a one-liner: it
puts a text-shaping pass back on a path §7.1.1 exists to keep free of one, so what
has to be settled is how such records are detected cheaply enough and whether the
answer is cached per record or per block.

---

### 22. The column count still overcounts by one on a line ending in an overhanging glyph

Also found while fixing entry 17, and it is what remains after it. `QTextLine`'s
break test allows for the last glyph's RIGHT BEARING, so where a glyph's ink
overhangs its advance the line breaks one character earlier than any
width-over-advance arithmetic predicts — and `viewportCols()` is exactly that
arithmetic, `qFloor(messageWrapWidth() / advance)` (`src/ui/LogView.cpp:653,691`,
as of the commit that fixed entry 17).
The record is then given one row too few and clipped to it, silently, in the same
way entry 17 was.

Swept at the reference fixed-pitch face over widths 60–1400 px at all 27 point sizes,
counting the widths at which `qFloor(w / advance)` exceeds what `QTextLine` places:

```
character:   '0'   'i'   '.'   'n'   'x'   'W'
overcounts:    0     0     0     0   248    1590      (of 36,207 size x width pairs)
```

`'x'` misses only at 7 pt; `'W'` misses at 15 of the 27 sizes — 7, 9, 11, 13, 14, 15,
18, 19, 20, 22, 24, 26, 28, 29, 30, which is the same set of sizes the truncated
advance failed at, since it is the same fractional remainder that leaves room for the
bearing to matter. At 9 pt with a 390 px column: the arithmetic says 54, a line of
`'0'` holds 54, a line of `'W'` holds 53.

The exposure is much narrower than entry 17's — it is one column, not up to 22, and
only for a line that happens to end on such a glyph — but the failure is the same and
so is its silence. It has the same shape as entry 21: the height model knows an
advance and the layout knows glyphs, and where those differ the model is the one that
is wrong. The same answer probably serves both, and `tst_logview::laidOutFit()`
measures in DIGITS specifically to keep the entry-17 cases clear of this one.

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
