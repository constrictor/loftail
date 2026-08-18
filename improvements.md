# UI usability improvements

Improvements to what already ships — not new features. Things that do the wrong
thing rather than merely lacking something belong in `bugs.md`.

The original 22 items, found on 2026-08-18 by building the app and driving the
real UI (Xvfb + xdotool) against a 4000-record synthetic log, were all
implemented the same day in `56b38e0..96a3e97` — one commit each — and have been
removed from this file. The two below were found by that work and left out of
scope for the items that found them.

Nothing here has been implemented. Line numbers are as of commit 96a3e97.

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
