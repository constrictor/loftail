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

Entries 21 and 22 have gone together, because they were one defect wearing two
faces: the estimated height model was `ceil(chars / cols)`, and a column count is a
thing two characters of one font need not agree on. A character the fixed-pitch face
does not carry is drawn through a fallback one at up to 1.77x the advance — Han, Kana,
Hangul and emoji — so a CJK log lost a line off 51.7% of its records, and a character
whose ink overhangs its advance breaks a line one place earlier than the arithmetic
predicts, so about one in seven window-and-zoom configurations quietly ate a row off
the occasional ASCII record ending in a `W`. Neither said anything: a wrapped message
is deliberately not elided and offers no tooltip, so the text simply was not there. A
record's height is now measured rather than counted, by a greedy fill over a memoized
per-codepoint advance and ink overhang that reproduces `QTextLine`'s own break test —
0.34 ms per 4096-record block against the 1.4 ms that block already spends decoding,
where laying every record out would have been 24 ms and up to 800 ms on payload
records. The fix took two smaller things with it: the entries' own proposal to detect
such records with `QFontMetricsF::inFont()` cannot work at all, since it resolves
through the fallback chain and answers true for U+4E2D and U+1F600 alike; and a
character of the Common script — `⚠`, `→`, `✓` — has no font of its own and is drawn
by whatever face is carrying the run around it, which is a second way to measure a CJK
line short and is not something either entry knew about.

The numbers left behind are not reused: an entry keeps the number it was given, so it
can still be referred to by one.

Everything the 2026-08-18 pass raised has been fixed. The list below starts again at
23, which is where that pass left the numbering.

A second pass, on 2026-08-29, raised entries 24 to 41 — the same method, four agents
over the working tree and the real application. All eighteen have been fixed, so what
is left below is entry 23 alone: a standing product decision nobody has taken, which
is not a defect and is not waiting on a patch.

Entry 24 has gone: the indexer read its field captures out of `recordStartRe` using
`recordRe`'s group numbers, and `recordStartRe` holds only the groups written before
`%m`, so every field a pattern puts after the message was indexed from an out-of-range
group — which Qt answers with a null string rather than an error. Under `%d{...} [%t]
%-5p %m (%c)%n`, an ordinary log4cplus layout, every record's subsystem was interned as
`""`: a blank column, an empty Filters list, and no integer axis left to filter or
colour on, while the Preferences preview — which drives the full regex — showed the
split exactly as it had been asked for. That is the shape the user reports it in. The
record's first line is now matched a second time against `recordRe` where the pattern
needs it, gated so that a pattern ending `%m%n` pays nothing, and the regex that decides
a record boundary is untouched. What the fix deliberately does not reach is stated in
`SPEC.md` §4 rather than left to be found: a multi-line record's trailing fields sit on
its last line, so those records keep the blank columns they have always had.

Entry 25 has gone too: `bytes()` returned a view into a member buffer, and one
`LogSource` is read by two threads for the whole of a scan — the index worker walking
chunks while the paint path decodes the visible cells — so each call freed the
allocation the other thread was still reading, the indexer parsing freed bytes into
record offsets and a painted cell decoding whatever the indexer's chunk had left behind.
`bytes()` takes the caller's own storage now, and a source that already holds the bytes
in stable memory of its own ignores it and stays zero-copy. The fix took an unreported
sibling with it: the shared read handle was seeked and then read, and three callers
share it, so two readers could each be served the other's offset — with nothing freed
anywhere and nothing for a sanitizer to see. It is a positional read now on both
platforms.

Entry 26 has gone as well: binding the panes to another tab copied the outgoing log's
whole highlight rule list onto the incoming log and saved it there. `setDecimals()`
re-rounds the value a spin box is holding and emits, which the axis editor's handler
reads as a hand edit, and it fired mid-rebind — after the pane had taken the incoming
document and before it had reloaded the rules — so the outgoing log's rules were written
onto the incoming document, persisted, and read straight back, leaving nothing on screen
to notice the loss. The guard now sits in the function that changes the precision rather
than in the handler that hears it, because that emission is a false claim of a user edit
wherever it is heard.

Entry 27 has gone too: one log's address was spelled two ways — raw when its format was
resolved, canonical when its record was stored — so a symlinked log, `latest.log`
pointing at today's file, opened correctly through the pattern naming the link and then
had its settings filed under the target's name, which that pattern does not claim.
Merely opening such a log therefore left a per-log record behind for ever, which is
exactly what the redundancy rule exists to prevent, and a daily rotation burned a fresh
slot a day out of a pool of 500, evicting records somebody had configured. The name as
opened is the one spelling now, which `SPEC.md` §4 carries as a product rule rather than
an implementation note. A record written by a build that canonicalised is **copied**
under the name asked for and never re-keyed: the old spelling is not a dead one but the
target file's own current key, so moving the record would hand a configured file's
settings to a symlink of it, permanently and silently, for nothing more than the link
being opened once.

Entry 28 has gone as well: editing the defaults, or the pattern claiming the open log,
gave that log a permanent private copy of the settings being replaced — so the edit
looked discarded, and every later open of the log resolved to the old format. The
dialog's one per-log row was a snapshot of what the log inherited, and the comparison
deciding whether that row was worth storing ran against a parent the same OK had just
moved under it. The row follows its parent now while it has said nothing of its own; the
fix is in the snapshot and never in the comparison, which must go on storing a log that
genuinely has an override. It took the OK-applies path with it, which could not correct
the damage either — what had just been written was what the tab was already reading, so
the guard deciding whether to re-read the log held.

Entry 29 has gone too: a log opened before it existed came back with no column header at
all and every column at Qt's default 100 px, every value elided. A waiting document has
no compiled format and so no columns, so the constructor's seed measured nothing and the
header was given no band; when the log turned up the column count went 0 to 5 with
nothing watching, there being no index controller built and no model reset to say the
column set had moved. It did not stay confined to the session either — one quit wrote
those widths out, and they come back marked as the user's own, past the reach of every
other seed, with Reset Widths the only way back. This is the M13 headline case, `loftail
/var/log/app.log` before the service has created it, and the unreadable-file case beside
it.

Entry 30 has gone as well, and it had made a milestone's worth of work unreachable:
`isConnected()` tested a pointer that only a teardown clears, so once a connect had
succeeded the answer was permanently true. The fetcher's dropped-link branch could never
be taken, it never let go of the dead session, and a tab whose machine went away
reported that the log was not readable once per session timeout for the rest of its
life — including after the host came back, with File ▸ Reconnect poking the same corpse
and the reboot-grace work reachable only through the branch that was never taken. The
session asks the transport now, latching on an error code that names a socket or
transport failure. The clearing rule is the half worth keeping: the latch is set from
calls that do **not** fail their caller — a partial read is handed back as a positive
byte count and the tab tails on normally — so an absolute latch went stale invisibly and
then fired on the first perfectly benign stat failure afterwards, reporting a dropped
link about a link that never dropped and paying a full connect, host-key check,
authentication and re-fetch from offset 0 for it, once per poll. A call that gets a whole
answer back therefore clears it, which cannot re-open the bug: when the link is genuinely
gone, nothing succeeds.

Entries 31 and 35 have gone together, being the two halves of one primitive's
discipline. The gate that carries a question to the application thread refused a call
made by the running work itself — where there is no second dialog to stack, so the work
was simply skipped and the caller kept its default — and that is what the password
prompt does: it asked the marshalled keychain whether there was a keychain, was refused,
and told the user there was none on a machine where one was running, after which ticking
the box wrote the password into `hosts.json` as plain text. The gate counts askers now
rather than calls, so the thread owning the in-flight call re-enters nested while a
second thread is still refused. The other half is the same branch's exit: a call that ran
inline never re-armed the pump, so a request pushed while the application thread sat in
one whose work spins a nested event loop had its posted drain delivered into that loop,
dropped, and never re-posted — the asking fetcher then slept until something else
happened to make a gate call, with no dialog, no diagnostic line and a tab sitting on
"connecting…".

Entry 32 has gone too: a remote config file's editor tab was never restored. The session
path held a copy of the open function's body without its local/remote split, so it called
the local reader — which refuses a remote address by design — and every launch reported
"must be read over SSH" on the notice strip instead, for as long as the address stayed in
the session, while the quit went on writing it. The restore goes through the same funnel
as the menu item now, and the fix took a smaller sibling with it: the copy also skipped
the writability check, so a build without SSH got that same sentence rather than the one
naming the dependency it lacks.

Entries 33 and 39 have gone together, because they are one omission in the three handlers
a live append reaches: a record that grows in place moves no row count, so nothing
revisited it. Under Line Wrap ▸ Selected record only that left the selected record — very
often the trailing one, which is the record a live log keeps rewriting and the one the
window selects itself after a run switch — with the height it was measured at, and the
paint draws only the lines that height gave it, so text arriving after the measurement
was laid out and not drawn, with no ellipsis and no tooltip; nine cached lines against a
true forty-nine in the case measured. In the density scrollbar it left the record wearing
whatever marks its incomplete text had produced, for the life of the tab, and in both
directions, marks being only ever OR-ed into a bucket: a rule matching a word that
arrives in a continuation line never marks the record, and a mark the growth invalidates
is never cleared. The measured height is re-measured by all three handlers now, and the
bar's scan is rewound to the changed row rather than a lane being cleared — which would
cost an ingest tick what a keystroke in Find already costs the find lane. The filtered
path turned out to have been accidentally immune to both, its per-tick withdrawal of the
provisional row reaching the shrink branch.

Entry 34 has gone as well: a stale remote tab lost its strip, its ⊘ and its reason the
moment the transport gave up trying, because "the origin is no longer reported gone" was
being read as "the source is delivering again" — and for a spool the first is exactly the
Waiting state, which a fetcher that has stopped retrying leaves. What was left was a tab
that looked like a healthy live log while showing records that had stopped arriving hours
ago, permanently where the fetcher had latched its refusal, and with a background tab —
the case the mark exists for — carrying nothing at all. The two questions are separate
now, through a new `isDelivering()` on the source; going stale in the first place is
still the vanish branch's decision, so a host that was down before anything arrived still
waits in the ordinary way.

Entry 36 has gone too: the 64 KB sample the encoding detector is given is cut at a byte
boundary, and a multi-byte character straddling that cut was read as "not UTF-8",
flipping the whole file to the system codepage. On Linux that is UTF-8 and only the
reported name was wrong; on Windows it is the ANSI codepage, so a CJK log rendered as
mojibake — on a supported platform and on precisely the population that writes non-ASCII
logs. The UTF-8 validation now sees the sample trimmed back to its last complete
character, and nothing else does: the NUL-parity test above it is a frequency over the
bytes that are there and needs every one of them.

Entry 37 has gone as well: a `%d{...}` made only of codes Qt cannot spell — `%s`, the
skip-only ordinals, or empty braces — fell back to a default written in the **strftime**
vocabulary rather than the display one, so the Time column rendered `%Y-%15-%27
%10:%8:%S` and varied per record, in every display mode rather than only As Written.
There are two default constants now, one per vocabulary, each saying which it speaks and
what swapping them costs. Parsing was correct throughout, the parser being driven by
tokens rather than by the display string.

Entries 38 and 41 have gone together, both being File ▸ Close All taking the tabs down by
a route the per-tab teardown never runs on. It left the notification's context pointer
dangling, so activating a bubble still on screen was a use-after-free; it left the tray
icon standing for logs that were gone, which is the one thing the comment beside that
icon's destruction says must not happen; and it left the status bar naming the log that
had been in front, while the centre correctly said "No file open". The three are one
funnel now, reached from the per-view teardown and from both of Close All's exits —
including the early one taken when only config-editor pages were open.

Entry 40 has gone too: the rotation branch of the SSH fetcher's poll published a healthy
`Live` whether or not the re-fetch had worked, and publishing a state clears the error
with it, so a spool write failure or a link that dropped during a rotation was erased and
reported as an ordinary tail for a poll interval — the append branch immediately below it
having always got this right. The fix took the line above it with it: opening the new
generation's spool file can fail too, and carrying on wrote the new remote file's bytes
onto the end of the previous generation's spool, which the reader is still indexing by
offset.

Reported on 2026-09-04 and fixed the same day: **Ctrl+clicking a subsystem's tick box
deselected every entry including the one clicked**, so the axis showed no records at all.
The chord takes the mouse press so that the view cannot toggle a tick underneath the
exclusive edit — but a check indicator is toggled on the RELEASE, so the release went on
to untick the one row the edit had just ticked. It hid because a release only reaches the
delegate when the index under it is the one the last press the view *saw* recorded: with
the chord's press eaten, the toggle needed a stale pressed index, so the chord worked in a
list nobody had clicked in yet and stopped working after the first ordinary click — which
is also why the existing regression case, which aims at the far side of the label rather
than at the tick, passed throughout.

---

### 23. The Highlighters pane imposes a ~456 px minimum width on the whole pane dock under Breeze

`HighlighterPane::minimumSizeHint()` measures 436×231 under Fusion and **456×255
under Breeze**, against `FilterPane`'s 68×68 and 83×83. The width comes from the
five-button row under the rule table — `New`, `Remove`, `Clear`, `Up`, `Down` — at
5 × 80 = 400 px on Fusion and 5 × 84 = 420 px on Breeze, plus the spacing between
them and the 2 × 6 px of `AxisEditor::kSideMargin` the row is indented by. Nothing
in the row can give: five `QPushButton`s with five words in them, laid out
side by side, and a button's size hint is a floor.

Because the panes are **tabbed into one dock area** (`MainWindow.cpp`,
`tabifyDockWidget`), that floor is the whole pane dock's floor — it applies while
the Filters or Runs tab is the one on screen just as much as while the
Highlighters tab is. So on a Breeze desktop, which is the reference KDE desktop
and what the user runs, the pane dock cannot be dragged narrower than about
456 px, and it takes those pixels from the log view for the whole session. The
Filters pane, whose five axes live in a `QScrollArea`, imposes no such floor: its
minimum is two orders smaller and it answers a narrow dock with a scrollbar.

Present since M19 put the button row there. Not something to fix blind — the
answers are all product decisions with costs: elide or iconise the five buttons
(which loses the words that say what they do, and `AxisEditor` already reversed
an auto-raise experiment on the argument that a frameless glyph does not read as
a button), wrap the row onto two lines, put the less-used three behind a menu, or
accept the floor and say so. Worth a decision rather than a patch.

It interacts with entry 19's work on the log view's own width budget: the seed
that keeps the message column on screen is measured against the viewport it is
given, and a dock that cannot be narrowed below 456 px is the other half of how
that viewport gets small in the first place.

---

A third pass, on 2026-09-07, raised entries 42 to 47. Different method from the two
before it: no agent drove the UI at all. These came out of building test
infrastructure — a coverage measurement, five libFuzzer targets over the parsers
and a mutation harness — and every one of them was found by a machine rather than
by somebody looking. Four are the fuzzer's, one came out of reading the lines a
coverage report said had never executed, and one out of a test whose first draft
asserted the wrong thing and was right to. Each was recorded with the ruling it
needs, because four of them have two defensible answers and the choice is the
user's; four of them have since been taken and fixed. Entry 47 is the last of
them and arrived after the others: taking entry 44's fix meant asserting its
property whole, and the fuzzer answered with a second address that is not a fixed
point of its own normal form, for a reason that has nothing to do with the first.

Entry 42 has gone: a literal `.` spelled after `%b`, `%a` or `%Z` inside `%d{...}`
was claimed by two halves at once. The name run tolerates a trailing dot because
`strftime` writes the process's locale's abbreviation and a good many locales spell
`Aug.`, and the parser's word reader consumes one for the same reason — so the regex
matched `Aug. 27 10:15:01` by backtracking, the parser read the name as `Aug`, ate
the dot, and handed the `Literal '.'` token the space behind it. Every record of such
a log had no timestamp at all: a blank Time column, no timestamp filtering, no time
bound on any highlight rule and an empty `SincePrevious` throughout, with nothing to
report it and the format dialog's preview showing the split correctly, because the
preview drives `recordRe` and never the parser. The ruling taken was the narrow one,
both halves conditionally: the compiler drops the name run's optional dot and sets a
flag on the token it emits in the same statement, and the parser reads that flag off
the token rather than re-deriving anything from the display string — so the two agree
about *when* the dot is the format's rather than agreeing about an outcome while
disagreeing about the mechanism. Nothing about a locale's own dot moved, which is the
whole reason the other candidate — dropping the optional dot unconditionally — was
not taken; the existing guard for that direction reddens under it, and does so under
nothing else. The fix took `%p` and `%P` with it: they are read back through the same
word reader while their regex never allowed a dot at all, so a format spelling one
after them failed the same way and had never been noticed.

Entry 43 has gone with it: `Decoder::decode()` and `Decoder::encode()` are the two
directions of one class, which is the whole of what lets `ConfigFileIO` replay a config
file's own encoding, byte-order mark and line endings back at it — and they were not
inverse at one character. Qt's decoders take a mark at the start of the range they are
given for the file's own and drop it, so a text beginning with U+FEFF came back one
character shorter, while the same character anywhere else survived; a config opened, not
edited and saved lost it, silently, on the file that decides what an application logs and
on the one path where the write is in place with no previous inode behind it. Both
rulings were taken. `decode()` now sets `ConvertInitialBom` in every branch — the file's
own mark is `bomLength()`'s business and every caller has already stepped past it, so a
mark still standing there is a character of the text — which took `QString::fromUtf8()`
and `fromLocal8Bit()` out of that function with it, the two spellings that were doing the
dropping and could not be told not to; detection is untouched, `sniff()` running a
converter of its own whose flags are entry 36's. And `configBytesReadBackAs()` beside it
reads what is about to be written back through the very detection the editor's read runs
and refuses a save that would not survive it, reporting it on the page's own notice with
nothing written and the buffer still holding every keystroke — a guard for the next
asymmetry rather than for this one, which is why it is pinned on an unpaired surrogate
that no encoding can spell. `ConfigView::bytesToSave()` is the one way in, so the check
cannot be walked around by a fourth save site. The fuzz target that found it lost its
carve-out in the same change, and checking what that carve-out was actually hiding turned
up one more thing worth recording: the minimised input this entry named,
`corpus/decoder/utf8_leading_bom_in_line`, decodes to an EMPTY line under the old code and
so passed the round-trip property vacuously — the shape that reproduces it carries the
mark twice, the old decoder dropping the first and keeping the second, which is the only
way a decoded line could begin with one at all. `corpus/decoder/utf8_two_marks_in_line`
is that input, and it is the one that reddens when the fix is reverted; the minimised one
is kept as the shape of the report rather than as the guard.

Entry 44 has gone too, and its ruling was the user's: a Unicode noncharacter in a
remote path or account made `normalize()` non-idempotent. `toString()`
percent-encodes one and `QUrl` declines to give it back, answering U+FFFD per
byte — so `ssh://h/x<U+FFFF>y` normalized to `ssh://h:22/x%EF%BF%BFy` and
re-parsed to a path holding three replacement characters. Every entry point
normalizes and `Document::prepare()` normalizes again, so such a log had two
spellings: a second slot out of the pool of 500, and settings written under one
name and read back under the other. `RemoteLocation::parse()` refuses such an
address now, in all three of the user, host and path — a refusal decided with no
I/O, so it fails the open and is reported through the message every unparseable
address already gets, over `withoutPassword()`. The rule is the sixty-six code
points and not "whatever the round trip does not preserve", which was the
alternative: re-parsing the normal form inside `parse()` costs 860 ns on top of a
355 ns parse where the scan costs 23, it would make the addresses loftail accepts
a function of the Qt build, and it would make the fuzz target's fixed-point
property tautological — and that property, now asserted for every address rather
than with the two noncharacter inputs excused, is the thing that would find a
second class of character if one ever appeared.

Entry 45 has gone with it, and it is the same shape one field over: a port outside
1..65535 is a malformed address now, refused at `RemoteLocation::parse()` where
every other part of an address is judged. `QUrl::port(default)` substitutes the
default only where the address spells NO port, so an explicit `:0` was a port the
address spelled and was taken at face value — `target()` answered `user@host:0`,
`SshSessionCache` keyed on that, `connectTo()` handed it to a socket, and what the
user was shown was a transport error against a machine that was up. The refusal is
decided with no I/O, so per M17 it fails the open and names the address over
`withoutPassword()`, which is the reply every unparseable address already gets. The
lower bound is loftail's alone; the upper is Qt's as well — `ssh://h:65536/p` and
`ssh://h:-1/p` are already invalid URLs — and it is written out all the same, so
that the set of addresses loftail accepts is not a function of the Qt build, which
is entry 44's own argument. Nothing else carries a port past that check: the Open
Remote dialog's spin box is 1..65535, and a hand-edited `hosts.json` reaches an
open only through `locationFor().toString()` and back through `parse()`, so it is
refused there rather than repaired in the store — repairing it would silently open
a different host from the one the file names. The fuzz target's port property is
the whole range now, where it was `>= 0` with the gap written out in a comment.

---

### 46. `KeychainSecretStore::available()`'s probe latch is read from any thread without synchronisation

`available()` opens with `if (m_probed) return m_available;`, and that line sits
**above** the check that refuses to run off the application thread. So a store that
has already probed answers from any thread, and the answer is an unsynchronised
read of a plain `bool` pair written by another one.

The `.cpp` says the path is unreachable, and it is right about the route it means:
everything above it goes through `secretStore()`, which marshals through
`GuiCallGate`, so no second thread reaches this object in the shipped
application. That is why this is recorded rather than fixed — it is a latent race
in code that, until 2026-09-07, had **never been executed at all** (0.0% of 105
lines, the only file in the tree at a flat zero), and the guard that makes it
unreachable is a call-site discipline rather than anything the class enforces.

It surfaced from the other side: the first draft of
`tst_keychainlive::everyOperationRefusesToRunOffTheApplicationThread` asserted
that `available()` refuses off-thread, and it does not. The case now drives a
second, unprobed store, which is the first execution of the thread guard that
`ARCHITECTURE.md` §6.3.2 describes. The ruling: either move the thread check above
the latch, which costs nothing and makes the class enforce its own contract, or
make the pair atomic, or write down that the discipline is the caller's and leave
it — but the current state, where the comment claims a guarantee one line below
where it stops holding, is the one that should not stand.

---

### 47. `QDir::cleanPath()` is not idempotent, so an archive container has two spellings

Found by the address fuzz target within five minutes of entry 44's property being
asserted, and it is entry 44's defect by a different route: `normalizeLogPath()`
is not a fixed point for an archive address whose container path holds a leading
`/.` in front of a `//`. `ArchiveLocation::toString()` cleans its container, and
`QDir::cleanPath("/.//a.zip")` answers `//a.zip` — Qt keeping a leading double
slash, which is a POSIX double-slash root — which cleans again to `/a.zip`. So
`normalize(normalize(s))` is not `normalize(s)`, and the cost is exactly entry
44's: every entry point normalizes and `Document::prepare()` normalizes again, so
such a log has two settings keys, a second slot out of the pool of 500, and
settings written under one name and read back under the other.

It is narrower than entry 44 in reach — it needs a `/.` immediately before a `//`,
which no shell completion produces — and wider in that it is not about a character
at all: any path shaped that way, remote or local, that also names an archive.

The ruling is a genuine question and is why this is recorded rather than fixed.
Cleaning twice inside `ArchiveLocation::toString()` is one answer and is a
one-liner; refusing the address is the other and is what entry 44 took, but a
double slash names a real file where a noncharacter does not, so a refusal here
would decline to open a log that exists. `tests/fuzz/corpus/address/a033_dot_slash_before_a_container`
is the input, and `fuzz_address`'s idempotence property is narrowed around it with
that name in the comment.

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

Six more, all noticed while fixing the 2026-08-29 entries and none of them driven
against a running application. They are adjacent to what was fixed rather than part
of it, which is why they were left rather than folded in.

- **`Document::messageText()` searches text the Message column does not show.**
  It returns the whole tail past `recordStartRe`'s match, so under a pattern that
  writes a field after `%m` — `%m (%c)%n` — the text axis and Find search
  `hello (a.b.c)` while the column shows `hello`, contradicting that function's own
  comment. Named in entry 24 and deliberately not taken with it: entry 24's fix is
  about which regex supplies a *field*, and this is about where the message ends.
- **`Document::reparseTimestamps()` skips a multi-line record under the same kind of
  pattern.** It matches `recordRe` against a record's first line, and `recordRe` is
  anchored, so a record whose message runs to several lines does not match it and
  keeps its old-zone timestamp when the source zone is changed. Same root as entry 24
  and outside its fix for the same reason.
- **A format that compiles to zero fields leaves the tab with no header until it is
  resized.** Entry 29's repair is on the resume path. `onIndexFinished` re-seeds the
  widths but never re-lays the chrome, so a log opened under an empty or uncompilable
  pattern — which is supported, and renders as plain text — still has no header band
  when the pattern is corrected in Preferences, the column count going 0 to n by a
  route entry 29's funnel is not on.
- **One poll of silence survives entry 30 on the exec transport.** `runCommand()`
  latches the dead-session flag when its read is cut short even though it returns
  true, because whether partial output is an *answer* is decidable only in the caller
  that parses it. A blip therefore costs the rest of that poll, and only becomes
  visible if the very next poll's stat also fails.
- **A failed `beginGeneration()` in the rotation branch leaves the session's handle on
  the new file and the published generation on the old.** Entry 40's fix stops the
  fetch and keeps the error, which is the visible half; a later poll that does not
  re-detect the rotation could still append new-file bytes at old offsets. The
  exposure is unchanged from before the fix rather than introduced by it.
- **A filtered live log's provisional record loses its selection highlight every
  tick.** The per-tick pop and re-add is a removal as far as `QItemSelectionModel` is
  concerned, so the row is dropped from the selection while remaining the current
  record. Noticed while writing entry 33's filtered regression case, which works
  around it; it predates that work.
