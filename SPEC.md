# loftail — Product Specification

**Status:** Draft, 2026-07-20. Supersedes `idea.md`.
**Scope:** User-visible behavior only. Internal design lives in `ARCHITECTURE.md`.

Items marked **[?]** are proposals that need your confirmation — they were decided by inference, not instruction. Items marked **P2** are planned but out of scope for the first release.

---

## 1. Purpose

loftail is a desktop application for reading logs produced by log4cplus. It serves two situations:

- **Post-mortem** — open a log file after the fact and navigate a large volume of records to find what went wrong.
- **Live** — follow a log file as it is being written, like `tail -f`, without losing filters or position.

It runs on Windows, macOS, and Linux.

## 2. Core concepts

| Term | Meaning to the user |
|---|---|
| **Record** | One logged event. Usually one line, but a record may span multiple lines when the logged message itself contains line breaks. loftail treats such a record as a single row. |
| **Subsystem** | The logger name the application passed to `getInstance()` — e.g. `net.socket`, `db.pool`. The primary axis for filtering. |
| **Priority** | The severity level: TRACE, DEBUG, INFO, WARN, ERROR, FATAL. |
| **Log format** | A log4cplus `ConversionPattern` describing how records are laid out on disk. loftail must know it to split records into fields. |

## 3. Opening logs

- Open a log file via menu, toolbar, keyboard shortcut, or drag-and-drop onto the window.
- The most recently opened files are listed for quick reopening. **[?]** Proposed: 10 entries.
- Opening a large file shows progress and remains responsive; the view populates as scanning proceeds rather than blocking until it finishes.
- Scanning can be cancelled, leaving whatever was scanned so far usable. **[?]**

### Live tailing

- Live tailing is a toggle on the currently open file, not a separate mode of opening it.
- When enabled, new records appear as they are written.
- **Follow** is a separate toggle: when on, the view scrolls to keep the newest record visible. Scrolling up manually turns follow off; a control returns to the bottom and turns it back on. This lets the user inspect history while the file keeps growing.
- Active filters apply to incoming records exactly as they do to existing ones.
- If the file is rotated or truncated by the writing application, loftail detects this and reloads rather than showing stale or corrupt data. The user is informed that rotation occurred. **[?]**

## 4. Log format configuration

Because a log file does not describe its own layout, loftail needs to be told the `ConversionPattern` the writing application used.

- A **Log Format** dialog accepts the pattern string and shows a **live preview**: sample lines from the current file, split into the fields loftail would extract. This makes a wrong pattern immediately obvious rather than failing silently.
- The dialog reports which fields were found. If **priority** or **subsystem** is absent from the pattern, loftail warns that filtering on the missing axis will be unavailable.
- If a pattern matches poorly, the file still opens: unparsed lines are shown as plain text rather than being hidden or dropped. The user is never left staring at an empty window because of a format mistake.
- The chosen format is remembered per file, so a file already configured opens correctly without asking again. **[?]** Proposed: also remembered per directory, so sibling log files from the same application inherit it.

### Format autodetection — **P2**

A later release will guess the format when a file is opened and pre-fill the Log Format dialog with its best guess, shown for confirmation rather than applied silently. Manual entry remains available and authoritative. The dialog is the same one used in P1.

## 5. Main view

- Records are displayed as a table, one row per record, with a column per field in the configured format (timestamp, priority, subsystem, message, and any others the pattern defines).
- Columns can be resized, reordered, and individually hidden. Column layout is remembered. **[?]**
- **Multi-line records are shown in full, in place.** A record whose message spans several lines occupies a correspondingly taller row in the table — the entire text is visible without expanding, selecting, or opening a detail pane. Row heights therefore vary throughout the table.
- **Oversized records are capped.** A record longer than 100 lines (a large stack dump, a serialized payload) displays truncated with an indicator and expands on request, so no single record can fill the viewport. Copying always yields the full text regardless of display truncation.
- Rows can be selected individually or as a range, and copied to the clipboard as text.
- Text search within the loaded log, with next/previous navigation. **[?]** Distinct from filtering: search moves the cursor, filtering removes rows.
- A status area shows total record count, the count after filtering, and the current file and its live/static state.

## 6. Filtering

Filtering removes non-matching records from the view. The underlying file is never modified.

- **By subsystem.** The subsystem list is discovered automatically from the file as it is scanned, so the user picks from what is actually present rather than typing from memory. Subsystems can also be entered manually — useful when tailing a file that has not yet emitted a given subsystem.
- **By priority.** Levels are selected as a set of checkboxes (TRACE…FATAL), all enabled by default. **[?]** Proposed over a minimum-threshold model, since it also allows isolating a single level.
- Filters can be **enabled and disabled individually** without being deleted, so a user can toggle a view on and off while keeping it configured.
- **[?]** Proposed combination semantics: within one axis, selected values are OR-ed (any of these subsystems); across axes, AND (matching subsystem **and** matching priority).
- The subsystem list supports select-all / select-none / invert, and a text box to narrow long lists. **[?]**

## 7. Highlighting

Highlighting colors matching records without removing anything, for spotting events in context.

- A highlight rule matches on subsystem and/or priority and applies a color.
- Rules are an **ordered list**; when several match a record, the first match wins. Order is user-adjustable. **[?]**
- Rules can be enabled and disabled individually, like filters.
- **[?]** Proposed: rules set background color, with foreground color optional, so highlighting stays legible against the application theme.

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
- **[?]** Proposed: presets can be exported to and imported from a file, for sharing with colleagues.

## 10. Session persistence

On relaunch, loftail restores:

- The last opened file, and whether live tailing was active
- The log format in use
- Active filters and highlighters, including which were enabled
- Saved presets
- Window geometry, pane layout, and column layout

**[?]** Proposed scoping, which matters for the eventual multi-file support: the log format, active filters, active highlighters, and column layout are remembered **per file**, so returning to a given log restores how you were reading *that* log. Presets and window/pane layout are global, shared across all files. This way, when several files can be open at once, each keeps its own working state without further redesign.

**[?]** Proposed: if the last opened file is missing or unreadable, loftail opens with an empty view and reports it, rather than showing an error dialog on every launch.

## 11. Non-goals

To keep the first release focused, loftail does not:

- Edit, write, or delete log files — it is strictly a reader
- Read log formats from other logging frameworks (the format is configurable, so some will happen to work; none are supported)
- Open several log files simultaneously — **planned for a later release.** The first release shows one file at a time; the architecture is built to accommodate more (see `ARCHITECTURE.md` §12) so that adding it later is additive rather than a rewrite
- Aggregate several files into a single merged, time-ordered view — this is a distinct feature from opening several files, and is not planned
- Provide charts, statistics, or alerting
- Retrieve logs over the network

---

## Open questions

1. **Priority filtering model** (§6) — set of checkboxes as proposed, or minimum-severity threshold?
2. **Bookmarks.** Not in the original sketch, but standard in log viewers: mark records of interest and jump between them. In or out?
3. **Long-line wrapping** (§5) — see `ARCHITECTURE.md` §7.1; this one has teeth.
4. **Message-text filtering** (§6) — filtering is currently subsystem and priority only.
5. **Time-range filtering** (§6) — depends on whether timestamps are parsed or opaque.
6. **Character encoding** (§4) — log4cplus UNICODE builds emit UTF-16.
7. **Compressed logs** (§3) — rotated logs are often `.gz`.
8. **Command-line invocation and file association** (§3).

*Resolved 2026-07-20: multi-line records render at full height (§5); oversized records cap at 100 lines (§5); multiple open files deferred but architecturally accommodated (§11).*
