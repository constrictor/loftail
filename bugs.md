# Bugs

Defects in shipped behaviour — things that do the wrong thing, not things that
are missing. Gaps and rough edges belong in `improvements.md`.

A fixed entry is **removed** from this file rather than kept as prose. Numbers are
never reused, so one named here, in `CLAUDE.md` or in a commit message stays
findable in the git history — and the rule a fix established lives in `CLAUDE.md`
beside the code it binds, which is where it is of use, while the guard that holds
it down is named in `tests/GUARDS.md`.

Three passes have been run and closed:

- **2026-08-18**, entries 1–22. Three agents driving the real UI (Xvfb + xdotool,
  screenshots measured with PIL) over the record table, the panes and the window,
  and a fourth reading the code.
- **2026-08-29**, entries 24–41. The same method, four agents over the working
  tree and the real application.
- **2026-09-07**, entries 42–49. No agent drove the UI at all: these came out of
  building test infrastructure — a coverage measurement, five libFuzzer targets
  over the parsers, a mutation harness — so every one was found by a machine
  rather than by somebody looking. The last five arrived in a chain, each the
  previous one's fix asserting a property whole and the fuzzer answering with
  another address that is not a fixed point of its own normal form, for a reason
  having nothing to do with the one before it.

Entry 23 is what is left, and it is not waiting on a patch: it is a standing
product decision nobody has taken.

---

### 23. The Highlighters pane imposes a ~450 px minimum width on the whole pane dock under Breeze

`HighlighterPane::minimumSizeHint()` measures **417×231 under Fusion and 450×255
under Breeze** (measured at HEAD on 2026-09-07), against `FilterPane`'s 68×68 and
83×83. The width comes from the button row under the rule table — `New`, `Remove`,
`Clear`, `↑`, `↓`, `Copy From…` — where Fusion floors a *text* button at 80 px
whatever it says, plus the spacing between them and the 2 × 6 px of
`AxisEditor::kSideMargin` the row is indented by. Turning `Up` and `Down` into
glyphs has already been done and bought ~19 px under Fusion but only ~6 under
Breeze, so the floor stands: a `QPushButton` with a word in it has a size hint,
and a size hint is a floor.

Because the panes are **tabbed into one dock area** (`MainWindow.cpp`,
`tabifyDockWidget`), that floor is the whole pane dock's floor — it applies while
the Filters or Runs tab is the one on screen just as much as while the
Highlighters tab is. So on a Breeze desktop, which is the reference KDE desktop
and what the user runs, the pane dock cannot be dragged narrower than about
450 px, and it takes those pixels from the log view for the whole session. The
Filters pane, whose five axes live in a `QScrollArea`, imposes no such floor: its
minimum is two orders smaller and it answers a narrow dock with a scrollbar.

Present since M19 put the button row there. Not something to fix blind — the
answers are all product decisions with costs: iconise the buttons that are still
words (which loses the words that say what they do, and `AxisEditor` already
reversed an auto-raise experiment on the argument that a frameless glyph does not
read as a button), wrap the row onto two lines, put the less-used three behind a
menu, or accept the floor and say so. Worth a decision rather than a patch.

It interacts with entry 19's work on the log view's own width budget: the seed
that keeps the message column on screen is measured against the viewport it is
given, and a dock that cannot be narrowed below 450 px is the other half of how
that viewport gets small in the first place.

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
