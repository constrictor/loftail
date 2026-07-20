# loftail — Product Specification

**Status:** Draft, 2026-07-20.
**Scope:** User-visible behavior only. Internal design lives in `ARCHITECTURE.md`.

Items marked **[?]** are proposals that need your confirmation — they were decided by inference, not instruction. This document describes the **first release only**; features planned for later releases live in `FUTURE.md`.

---

## 1. Purpose

loftail is a desktop application for reading logs produced by log4cplus. It covers a spectrum of use with no mode switch:

- Open a finished log and navigate a large volume of records to find what went wrong.
- Follow a log as it is being written, like `tail -f`, without losing filters or position.

These are not separate modes. Every file is opened watched for new content (§3); a finished log is just one that never grows. This removes a decision the user would otherwise have to make correctly — and often could not, since a file gives no sign of whether more is coming.

It runs on Windows, macOS, and Linux.

## 2. Core concepts

| Term           | Meaning to the user                                                                                                                                                         |
| -------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Record**     | One logged event. Usually one line, but a record may span multiple lines when the logged message itself contains line breaks. loftail treats such a record as a single row. |
| **Subsystem**  | The logger name the application passed to `getInstance()` — e.g. `net.socket`, `db.pool`. The primary axis for filtering.                                                   |
| **Priority**   | The severity level: TRACE, DEBUG, INFO, WARN, ERROR, FATAL.                                                                                                                 |
| **Log format** | A log4cplus `ConversionPattern` describing how records are laid out on disk. loftail must know it to split records into fields.                                             |

## 3. Opening logs

- Open a log file via menu, toolbar, keyboard shortcut, or drag-and-drop onto the window.
- **From the command line:** `loftail <file>` opens that file directly. **[?]** Proposed additional switches: `--follow` to open at the end with follow on (updates are always watched regardless — see §3 live updates — so this only sets the initial scroll position and follow state), and `--pattern <p>` to supply the log format for a file loftail has not seen before.
- **Multiple instances may run simultaneously.** loftail does not enforce a single instance; launching it again opens an independent window with its own file and its own filters.
- The 10 most recently opened files are listed for quick reopening.
- Opening a large file shows progress and remains responsive; the view populates as scanning proceeds rather than blocking until it finishes.
- Scanning can be cancelled, leaving whatever was scanned so far usable.

### Live updates

- **Every file is opened as if it were live.** loftail cannot know whether a file is complete or still being written, so it always watches the open file and appends new records as they arrive. A file that is never appended to simply never produces any — there is no "post-mortem" versus "live" mode for the user to choose, and none to forget to turn on.
- **Follow** is the one toggle in this area: when on, the view scrolls to keep the newest record visible. Scrolling up manually turns follow off; a control returns to the bottom and turns it back on. This lets the user inspect history while the file keeps growing. Follow defaults on for a file opened at its end. **[?]**
- Active filters and highlighters apply to incoming records exactly as they do to existing ones.
- If the file is rotated or truncated by the writing application, loftail detects this and reloads rather than showing stale or corrupt data. This happens silently — no notice is shown.
- Because loftail always holds the file open for reading, it must never prevent the writing application from appending to, rotating, or truncating it. Observing a log must not disturb the process producing it.

## 4. Log format configuration

Because a log file does not describe its own layout, loftail needs to be told the `ConversionPattern` the writing application used.

- A **Log Format** dialog accepts the pattern string and shows a **live preview**: sample lines from the current file, split into the fields loftail would extract. This makes a wrong pattern immediately obvious rather than failing silently.
- The dialog reports which fields were found. If **priority** or **subsystem** is absent from the pattern, loftail warns that filtering on the missing axis will be unavailable.
- If a pattern matches poorly, the file still opens: unparsed lines are shown as plain text rather than being hidden or dropped. The user is never left staring at an empty window because of a format mistake.
- The chosen format is remembered per file, so a file already configured opens correctly without asking again. It is remembered per file only — a newly opened file is never assumed to share another's format.

### Character encoding

Encoding is an explicit setting in the Log Format dialog, offered as a list:

| Choice                      | Behavior                                                    |
| --------------------------- | ----------------------------------------------------------- |
| **Auto-detect** *(default)* | Byte-order mark where present, content inspection otherwise |
| UTF-8                       | Forced, BOM tolerated and skipped                           |
| UTF-16 LE / UTF-16 BE       | Forced                                                      |
| System 8-bit                | The platform's local codepage                               |

- Auto-detect is the default because it is right nearly always — this matters because log4cplus built for `wchar_t` writes UTF-16 on Windows, and users should not have to know that.
- When auto-detect is active the dialog shows **which** encoding it settled on, so a wrong guess is visible rather than silent.
- The choice is explicit and forceable because no heuristic is reliable on short files, on files whose first records are pure ASCII, or on legacy 8-bit logs — auto-detect is a convenience, not a guarantee.
- The setting is remembered per file along with the rest of the format.

### Timestamps and time zones

Timestamps are parsed into real points in time, not treated as opaque text — this is what makes time-range filtering (§6) and jump-to-time possible.

Because a log file records no zone information, two settings control interpretation, both in the Log Format dialog and both remembered per file:

- **Source time zone** — how to read the timestamps in the file: **Infer from pattern** *(default)*, Local time, UTC, or a fixed offset. Inference uses the date specifier in the configured pattern, since log4cplus distinguishes local-time and UTC forms. Explicit selection exists because the producing application may have been configured in ways the pattern does not reveal, and because logs are routinely read on a different machine than they were written on.
- **Display time zone** — how to show them: **As written in the file** *(default)*, Local time, or UTC. The default performs no conversion, so what loftail shows matches what a text editor shows — the least surprising behavior when cross-checking against raw log text.

Time-range filter bounds are entered in the display time zone, so what you type matches what you see.

Format autodetection — guessing the pattern on open — is planned for a later release; see `FUTURE.md`. In the first release the pattern is always entered manually.

## 5. Main view

- Records are displayed as a table, one row per record, with a column per field in the configured format (timestamp, priority, subsystem, message, and any others the pattern defines).

- Columns can be resized, reordered, and individually hidden. Column layout is remembered. **[?]**

- **Multi-line records are shown in full, in place.** A record whose message spans several lines occupies a correspondingly taller row in the table — the entire text is visible without expanding, selecting, or opening a detail pane. Row heights therefore vary throughout the table.

- **Oversized records are capped.** A record longer than 100 lines (a large stack dump, a serialized payload) displays truncated with an indicator and expands on request, so no single record can fill the viewport. Copying always yields the full text regardless of display truncation.

- **Line wrapping is a user setting with three modes:**
  
  - **Off** — long lines extend horizontally; the view scrolls sideways.
  - **Selected record only** — the focused record wraps so it can be read in full; all others stay unwrapped.
  - **Always on** — every record wraps to the viewport width.
  
  The setting is remembered. Note that in *always on* mode the vertical scrollbar is an approximation that refines as you scroll, since exact total height cannot be known without measuring every record (see `ARCHITECTURE.md` §7.1). Scroll position and navigation stay accurate; only the thumb size and position are estimates.

- Rows can be selected individually or as a range, and copied to the clipboard. Copying yields the original raw text by default, with **Copy as Columns** available as a separate action for pasting into a spreadsheet.

- **Find / Find Next.** Text search over record content, with next/previous navigation, case-sensitivity and regular-expression options, and wrap-around at the end. Distinct from filtering: find moves the cursor and leaves every record visible; filtering removes non-matching records. Find operates on what is currently visible — if a filter is active, find searches the filtered subset.

- A status area shows total record count, the count after filtering, the current file, and whether it is currently receiving new records.

## 6. Filtering

Filtering removes non-matching records from the view. The underlying file is never modified.

- **By subsystem.** The subsystem list is discovered automatically from the file as it is scanned, so the user picks from what is actually present rather than typing from memory. Subsystems can also be entered manually — useful when following a file that has not yet emitted a given subsystem.
- **By priority.** A single **minimum level** is chosen (TRACE…FATAL); records below it are hidden. Selecting WARN, for example, shows WARN, ERROR, and FATAL. The default is TRACE, which shows everything.
- **By thread.** Like subsystems, the thread list is discovered from the file as it is scanned. Available only when the log format includes a thread field.
- **By message text.** Substring or regular-expression match against the message, with a case-sensitivity option. Unlike the other axes this cannot offer a pick-list, so it is a text box. A negation option (*hide* matching records) is included, since excluding known noise is as common as isolating a signal.
- **By time range.** A start and/or end bound, entered in the display time zone (§4); records outside the range are hidden. Available only when the log format includes a timestamp field.
- Filters can be **enabled and disabled individually** without being deleted, so a user can toggle a view on and off while keeping it configured.
- **[?]** Proposed combination semantics: within one axis, selected values are OR-ed (any of these subsystems); across axes, AND (matching subsystem **and** matching priority).
- The subsystem list supports select-all / select-none / invert, and a text box to narrow long lists.

## 7. Highlighting

Highlighting colors matching records without removing anything, for spotting events in context.

- A highlight rule matches on subsystem and/or priority and restyles the matching records.
- Rules are an **ordered list**; when several match a record, the first match wins. Order is user-adjustable.
- Rules can be enabled and disabled individually, like filters.
- **A rule sets a background color and a text (foreground) color, chosen independently.** Either can be left at its default, which is the record's normal un-highlighted appearance — so a rule may recolor only the background, only the text, or both. A highlight can therefore be as quiet as tinting the text or as loud as a full-row fill.
- **Colors come from a curated palette**, not a free color picker. Each palette entry is defined once for light themes and once for dark, so a highlight stays legible in both and switching themes needs no rework. A rule stores, for each of background and text, either a palette entry or *default* — never a raw color value. The palette has 12 entries, covering the usual severity associations (reds, ambers, greens) plus neutral distinguishing hues.

## 8. Side panes

Filters, highlighters, and presets are each presented in a side pane, so they are visible and toggleable without opening dialogs.

- Panes can be shown/hidden, resized, moved to either side, floated as separate windows, or tabbed together.
- Pane layout is part of the remembered session (§10).
- Enabling and disabling an individual filter or highlighter is a single click within its pane — no dialog.

## 9. Presets

- **Filter presets** store a complete set of filters; **highlighter presets** store a complete set of highlight rules. The two are independent and separately recallable.
- Presets are created from the current state, and can be renamed and deleted.
- Applying a preset replaces the current set on that axis. **[?]** Proposed over merging, which makes the result hard to predict.
- Presets are listed in a side pane and applied in one click.
- Presets persist across sessions and are independent of any particular log file.
- Presets can be exported to and imported from a JSON file, for sharing with colleagues.

## 10. Session persistence

On relaunch, loftail restores:

- The last opened file, and its follow state (updates are always watched, so only follow is a remembered choice)
- The log format in use
- Active filters and highlighters, including which were enabled
- Saved presets
- Window geometry, pane layout, and column layout

**Scoping**, which matters for the eventual multi-file support: the log format, active filters, active highlighters, and column layout are remembered **per file**, so returning to a given log restores how you were reading *that* log. Presets and window/pane layout are global, shared across all files. This way, when several files can be open at once, each keeps its own working state without further redesign.

**Multiple instances.** Because instances run independently (§3), two of them can save session state at the same time. Per-file state is keyed by file, so instances viewing different logs never conflict. For genuinely global state — window layout, and which file to restore on next launch — **the last instance to close wins**.

If the last opened file is missing or unreadable, loftail opens with an empty view and reports it, rather than showing an error dialog on every launch.

## 11. Non-goals

These are things loftail will **not** do — as distinct from features deferred to a later release, which are in `FUTURE.md`. loftail does not:

- Edit, write, or delete log files — it is strictly a reader
- Read log formats from other logging frameworks (the format is configurable, so some will happen to work; none are supported)
- Aggregate several files into a single merged, time-ordered view — distinct from simply opening several files (which *is* planned; see `FUTURE.md`), and not planned at all
- Provide charts, statistics, or alerting
- Install itself as the system handler for `.log` files — file association is an installer concern, not an application one

---

## Open questions

The three headline questions are now settled: priority filtering uses a **minimum level** (§6); **bookmarks** are deferred to a later release (`FUTURE.md`); the display time zone defaults to **as written in the file** (§4).

Smaller inline proposals still await confirmation, marked **[?]** where they appear — none blocks anything: the CLI switch names (§3), follow-defaults-on when a file is opened at its end (§3), column reorder/hide and layout persistence (§5), filter combination semantics — OR within an axis, AND across axes (§6), and apply-preset-replaces-rather-than-merges (§9). Each is noted in `PLAN.md` against the milestone where it must be settled.

*Resolved 2026-07-20: full-height multi-line records with a 100-line cap (§5); wrapping is a three-mode setting (§5); encoding is an explicit setting defaulting to auto-detect (§4); timestamps are parsed, with configurable source and display time zones defaulting to as-written (§4); priority filtering is by minimum level, message-text/thread/time-range filtering are in scope (§6); Find/Find Next searches the filtered subset (§5); copy defaults to raw text with Copy as Columns as a separate action (§5); highlight rules set an independent background and text color, each from a 12-entry dual-theme palette or left at default (§7); presets export/import as JSON (§9); per-file vs global session scoping settled, last-instance-to-close wins for global state, missing last file opens empty (§10); command-line invocation and multiple simultaneous instances are supported (§3). Deferred to `FUTURE.md`: format autodetection, multiple open files, compressed and SSH sources, bookmarks.*
