# UI usability improvements

Improvements to what already ships — not new features. Things that do the wrong
thing rather than merely lacking something belong in `bugs.md`.

The original 22 items, found on 2026-08-18 by building the app and driving the
real UI (Xvfb + xdotool) against a 4000-record synthetic log, were all
implemented the same day in `56b38e0..96a3e97` — one commit each — and have been
removed from this file. Items 1 and 2 below were found by that work; 3 to 8 come
from the review pass over it, driving the same UI again.

Nothing here has been implemented. Line numbers are as of commit 35e8cb9.

---

### 1. The Filters pane greys out with no log open and never says why

`FilterPane.cpp:172` is `setEnabled(document != nullptr)`, so with no file open
all five axes sit there greyed and nothing says what would bring them back. The
thread and time axes are additionally hidden outright, because no format is known
to fill them, so the pane is both inert and visibly incomplete with no
explanation offered for either.

This is the gap `2f76ce0` closed one level down, in the Highlighters pane's rule
table, which now says which kind of empty it is — "No file open. Open a log file
to add highlight rules." against "No highlight rules. Press New, or right-click a
record…". The Filters pane wants the same sentence in the same muted palette
colour.

The one structural difference: there it is a table inside the pane that is empty,
so the line is a label over the table's viewport; here the whole pane is inert,
so it belongs at the top of the scroll area, above the axes, and has to survive
the pane being re-enabled without leaving a stale line behind.

Not a defect — greying is a correct signal, just an unexplained one. The Runs
pane had the same gap and it was closed in `96a3e97`.

### 2. macOS cannot open several logs into one window from Finder

`loftail a.log b.log` and File ▸ Open both take several logs now (`347480b`),
and `packaging/linux/loftail.desktop` moved to `Exec=loftail %F` so that a
multi-selection from a Linux file manager becomes one window with N tabs.

macOS does not use argv for this. Selecting several logs in Finder and opening
them sends each one to the already-running application as a `QFileOpenEvent`, and
loftail installs no handler — `grep -rn QFileOpenEvent src/` finds nothing. So on
macOS the gesture opens nothing, or starts a second process, depending on how the
bundle happens to be registered.

The fix is a `QEvent::FileOpen` handler on the application object routing to
`MainWindow::openFiles()` — which is already the single funnel for opening N
addresses — plus a `CFBundleDocumentTypes` entry in the bundle's `Info.plist`, so
Finder offers loftail for log files in the first place.

Treat it as unverified until somebody runs it on a Mac. The `macos-13` job in
`packaging.yml:627` builds and runs the suite, but nothing in CI can drive
Finder, and this is the same class of exposure as the Windows notes in
`CLAUDE.md` — the platform behaviour, not the build.

### 3. The side-pane dock cannot be narrowed below 456 px

Measured `minimumSizeHint().width()`: FilterPane **68**, RunPane **393**,
HighlighterPane **436**. The three are tabbed into one dock area, so the area's
floor is the largest of them, and the splitter refuses to go below 456 px —
dragging it right from 531 px to 1020 px on an 1100 px window leaves the dock at
exactly 456. At an 800×700 window the panes hold 456 px and the log holds 344,
with Priority and Subsystem off screen while the Filters pane sits at its own
comfortable width.

`SPEC.md` §8 promises panes that can be "shown/hidden, resized, moved to either
side"; here resizing is one-directional. FilterPane already solves it properly —
its `QScrollArea` is what gives it a 68 px floor. The other two do not.

RunPane's 393 comes almost entirely from one non-wrapping instruction label
(`RunPane.cpp:35`), a plain `QLabel` with a hard `\n` and no `setWordWrap(true)`,
unlike the `MessageLabel`s beside it. HighlighterPane's 436 comes from the five
equal `QPushButton`s under the rule table (`HighlighterPane.cpp:762`), each
carrying `QPushButton`'s ~80 px style floor.

Fix: `setWordWrap(true)` and drop the embedded newline for the first; for the
second, either let the button row reflow, or use `QToolButton`s as `AxisEditor`
already does for All/None/Invert. Both are wanted — fixing one leaves the other
binding.

### 4. A subsystem too long for its list is cut with no ellipsis, and hovering it shows the wrong thing

`app.core.startup.bootstrap.ConfigurationLoaderFactory` renders as
`app.core.startup.bootstrap.ConfigurationLoade` — hard-clipped at the viewport
edge, so nothing says it is cut. Hovering it pops the list-level tooltip,
"Ctrl+click a subsystem to show only that one", rather than the name. A
horizontal scrollbar appears intermittently, and when it does it takes 14 px out
of a list whose floor is 90 px — one of about three visible rows.

These are dotted logger paths whose distinguishing part is the *end*;
`AxisEditor.cpp:87` says so itself when arguing for tool buttons ("the ones that
matter differ at the END"). The Runs pane settled the same question the other way
and deliberately (`RunPane.cpp:79`: elide right, no horizontal scrollbar, the
full label as the item's tooltip), and `CLAUDE.md` records that reasoning. The
value lists are the one thing `SPEC.md` §6 says should grow, so spending their
height on a scrollbar is the worst place to spend it.

Fix: `setTextElideMode(Qt::ElideRight)` and
`setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff)` on both value lists, and
a per-item `setToolTip(name)` in `populateList` (`AxisEditor.cpp:1554`). An item
tooltip supersedes the list's, so the Ctrl+click hint needs somewhere else to
live — the axis group box's title, or appended to each item's tooltip.

### 5. The pane-activity marker leaks into the View ▸ Panes menu

`MainWindow.cpp:322` and `:346` write the marker by setting the dock's window
title — `setWindowTitle(active ? tr("Filters •") : …)`. `QDockWidget` also uses
that string for `toggleViewAction()`, so View ▸ Panes lists `Filters`,
`Highlighters •`, `Runs`: a bullet on a checkbox menu item whose only job is to
show or hide the pane.

`SPEC.md` §6 and §7 put the marker on the *tab*, with a reason specific to tabs —
it answers "are these in force" for a pane you cannot see. In a list of pane
names it answers nothing, since the pane is one click away, and it reads as a
stray character or part of the name. The menu item is the one place the string is
used as an identifier rather than as a status line.

Fix: keep the dock title clean and mark the tab bar directly, or set
`toggleViewAction()->setText()` once at `addPaneDock()` time and re-set it after
each title write. The dock's *object* name must stay untouched either way —
`restoreState()` keys off it, and translating or changing one breaks saved
layouts on upgrade.

### 6. The refusal strip prints each address twice

`loftail d1 … d10` against ten directories gives eight lines of the form
`d1: Cannot open file: /long/path/to/logs/d1` — the short name, then the same
address again in full — each wrapping to two lines, plus "… and 2 more". On a
950 px window the strip took about 320 px and pushed the tab bar and the log a
third of the way down the window. The single-refusal form has it too.

The strip's own format is `<name>: <reason>` (`MainWindow.cpp:1288`), so the
reason should be the reason alone; `Document.cpp:151` supplies
`Cannot open file: %1` with the path in it. The eight-entry cap is honoured, but
each surviving line is twice as long as it needs to be.

Fix: make the local open failure's message the reason only — the OS error, or
"the file could not be opened" — since every caller on this path already prefixes
the address. Check `Document.cpp:359` and `:408` (`Cannot reopen file: %1`) for
the same duplication on the reload path.

### 7. Reopening a log that is already open does not move it up Open Recent

`MainWindow::openWithSettings()` (`MainWindow.cpp:1457`) raises the existing tab
and returns before `rememberRecentFile(path)`. So picking an already-open log
from Open Recent — or from the Open dialog — correctly brings its tab forward,
but the entry keeps its old place in the list.

`SPEC.md` §3 calls it "the 10 most recently opened logs". The gesture is an open,
so the ordering silently stops reflecting what the reader last reached for, and
the entry can eventually fall off the end of a list it should be at the top of.

Arguable — the log never left the screen, which is why it was left alone when
Open Recent was reworked in `8f1c46d`. Fix: call `rememberRecentFile(path)` on
the raise path too.

### 8. The bounded copy's chunked branch is unreachable from any test

`kCopyChunk` (`LogView.cpp:47`) is a file-local `constexpr int = 2000` with no
seam, while the threshold beside it is settable (`setCopyProgressThreshold`). The
four copy tests select at most ten records, so `++done % kCopyChunk != 0` is
never false and the entire in-loop body — the progress update, the
`processEvents()`, and the guard that ends the walk — never executes. Only the
post-loop `setValue(); processEvents();` runs, which is why
`cancellingACopyLeavesTheClipboardAsItWas` can test only a cancel arriving after
the last record was read, as its own comment concedes.

That branch is the entire substance of `959a4bc`: a model reset abandoning the
copy and reporting it, a tail removal deliberately *not* abandoning it, the
`QPointer`s behind both. None of it is exercised, and the reset branch is
reachable in ordinary use — a rotation, or `followLastRunIfMoved()` re-applying
the view on a log that restarts, both land inside `processEvents()` during a long
copy. Grepping `tests/` for `modelAboutToBeReset` or the abandonment message
finds nothing. `aCopyBigEnoughToWaitForOffersToStopAndCopiesTheSameText`'s
`cancelWasOffered = !dlg->wasCanceled()` is also a check that cannot fail —
nothing has cancelled it at that point.

Fix: give `kCopyChunk` the seam the threshold already has, then drive three cases
at a chunk of 2 over ten records — a cancel raised between chunks, a
`beginResetModel()`/`endResetModel()` from a `singleShot(0)` asserting the
clipboard is untouched, and an append that must *not* abandon. Making it settable
is the whole of the extra work; `tst_logview`'s existing `singleShot`-into-the-
dialog idiom covers the rest.
