# loftail — Product Specification

**Status:** Draft, 2026-07-20.
**Scope:** User-visible behavior only. Internal design lives in `ARCHITECTURE.md`.

This document describes **what loftail does today**; features not yet built live in `FUTURE.md`. A feature moves here when it ships.

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
- **Several logs can be open at once**, each in its own tab (§5a). Opening a file adds a tab rather than replacing what is on screen; dropping several files at once opens all of them. Opening a file that is already open raises its tab instead of opening it twice.
- **From the command line:** `loftail <file>` opens that file directly, at its end and following, like every other open (see live updates below). A `--pattern <p>` switch supplies the log format for a file loftail has not seen before.
- **A compressed or archived log opens the same way**, written `app.log.1.gz` or `bundle.tar.gz/var/log/app.log`. See "Compressed and archived logs" below.
- **A log on another machine opens the same way**, written as an `ssh://user@host/path/to/file.log` address. It is accepted anywhere a path is: the Open dialog, the command line, a drag-and-drop, the recent-files list, and the restored session. See "Remote logs" below.
- **Multiple instances may run simultaneously.** loftail does not enforce a single instance; launching it again opens an independent window with its own files and its own filters.
- The 10 most recently opened files are listed for quick reopening.
- Opening a large file shows progress and remains responsive; the view populates as scanning proceeds rather than blocking until it finishes.
- Scanning can be cancelled, leaving whatever was scanned so far usable.

### Live updates

- **Every file is opened as if it were live.** loftail cannot know whether a file is complete or still being written, so it always watches the open file and appends new records as they arrive. A file that is never appended to simply never produces any — there is no "post-mortem" versus "live" mode for the user to choose, and none to forget to turn on.
- **A file always opens at its last position, following** — scrolled to the newest record, keeping up with new ones, exactly like `tail -f`. This is unconditional; there is no option to open at the top instead.
- **Follow** can then be turned off: scrolling up manually detaches, so the user can inspect history while the file keeps growing; a control returns to the bottom and re-attaches. Follow is simply on at every open.
- Active filters and highlighters apply to incoming records exactly as they do to existing ones.
- If the file is rotated or truncated by the writing application, loftail detects this and reloads rather than showing stale or corrupt data. This happens silently — no notice is shown.
- Because loftail always holds the file open for reading, it must never prevent the writing application from appending to, rotating, or truncating it. Observing a log must not disturb the process producing it.

### Logs that are not there

A log that does not exist yet is opened the same way as one that does, and so is one that goes away while you are reading it. This follows from the same reasoning as live updates: loftail cannot know whether a log is finished, and it equally cannot know whether one is late.

- **A log that has not been written yet opens anyway.** Point loftail at a path before the application has created it — a service about to start, a log rotated out from under you, a machine still booting — and it opens a tab that says it is waiting. The moment the log appears it is read and followed, with no reopening and nothing to press. This is unconditional and there is no switch for it: an address that is not available yet is simply a log that has not turned up.
- **A log deleted while open is waited for too.** The tab empties and says the log is no longer there, keeping its filters, highlighters, log format and run pattern; when the log comes back it fills in again. A rotation is not this — that is detected as a rotation and reloads silently, as it always has.
- **The tab says which it is.** A waiting tab is marked in the tab bar, the view says what it is waiting for, and the status bar says why — "the host is unreachable", "no such file there". Nothing is popped up.
- **A remote log on a host that is down opens and keeps trying.** loftail reconnects in the background using your SSH agent, your usual keys, or a password you have already given it this session. If the host comes back needing a password it cannot ask for unprompted, it says so and **File ▸ Reconnect** asks again.
- **The remembered session keeps a log that is not available.** A file that could not be opened at launch comes back as a waiting tab rather than being dropped, so an unmounted share or a host that was down for an afternoon does not cost you the tab permanently.
- **Only a genuine refusal still fails to open.** An address that names no log, an archive without a chosen log inside it, a host whose key has changed, a rejected password, or a feature this build does not include: these get the same answer however long loftail waits, so they are reported and no tab is opened.
- **A log that arrives is not asked about.** loftail applies the format it remembers for that file, or your default. If neither fits, the log shows as plain text and the status bar points at **Log ▸ Format…** — it will not raise a dialog for a log that turned up while you were reading something else.

### Remote logs

- **A remote log behaves exactly like a local one.** It opens at its end and follows; filters, highlighters, runs, the log format and the remembered session all work identically. Rotation and truncation on the far end are detected and reloaded silently, just as they are locally.
- **Addresses are written `ssh://user@host:port/path`.** The user and port may be omitted (your SSH configuration and port 22 are assumed). `sftp://` is accepted as the same thing, so a file dragged out of a file manager's SSH mount opens. Whatever spelling is used, one log is one tab: reopening it by a different spelling raises the tab that is already showing it.
- **File ▸ Open Remote…** offers the same thing as a form, and remembers hosts. Saved hosts and their logs then appear under **File ▸ Remote Hosts** for one-click reopening, one flat entry per log written `host: /path/to/log`; a host with no remembered log is listed on its own and reopens the form pre-filled. A whole `ssh://…` address pasted into any of the form's fields splits itself across them, so an address from a colleague or a wiki page needs no retyping in pieces — the form says so above its fields, rather than leaving it to be discovered.
- **The form asks for the address, then how to sign in, and hides the rest.** Everything with a good default — how often to check the log for new lines, and whether to fetch only its end — is folded away under **Advanced**, which opens by itself for a saved host that has changed one of them, so a setting moved off its default is never hidden. **Open** stays unavailable until there is both a host and a path to open, rather than accepting the click and doing nothing.
- **One name, one saved host.** A saved host is identified by the name it is listed under — the Name field, or the host itself if you left it blank — so **Save** replaces the host of that name rather than adding a second entry reading the same. It says **Update** instead of **Save** whenever that is what pressing it will do, so the replacement is visible beforehand rather than inferred afterwards; it does not ask, because the name you typed is which entry you meant. Case and surrounding spaces do not make a new entry. Saving another log on a host you have already saved adds it to that host's logs; pointing an existing name at a *different* machine replaces the entry outright, and the previous machine's logs and remembered password do not follow the name over.
- **A host's remembered logs are listed, and can be dropped.** The Path field is a drop-down of every log saved under that host, so what has accumulated there is visible in the dialog rather than only in the Remote Hosts menu; right-clicking it offers **Forget This Path**. **Remove** deletes a whole saved host and asks first, since it may be discarding a remembered password and there is no undo.
- **Signing in.** An SSH agent or one of your usual `~/.ssh` keys is tried first and needs no interaction. If the server insists on a password, loftail asks for it, and asks **once per host** however many of that host's logs are open — including when a session is restored. What you typed can be shown: the box has a reveal control at its right-hand edge (`Ctrl+Shift+H`), so a long password on an unfamiliar keyboard layout can be checked before it is sent rather than after it is rejected.
- **Unknown hosts are confirmed, changed keys are refused.** A host not in your `known_hosts` shows its SHA256 fingerprint and can be accepted once or accepted and remembered; nothing is sent to it until you do. If a host's key has *changed*, loftail will not connect at all, because that is indistinguishable from an intercepted connection.
- **A remembered password goes to your system's keychain where there is one.** On a desktop with KWallet, GNOME Keyring, the Windows Credential Manager or the macOS Keychain, ticking *remember* hands the password to that — and the option names it before you tick, so you know where it is going. loftail writes nothing to a file of its own; your keychain manager is where you go to look at it or remove it.
- **The Open Remote form says where a password would go in every case**, including when you have chosen an agent or a key and the answer is *nowhere*. It is one line under the *remember* option, and it is a warning only when there is something to warn about — which also means the dialog does not change size when you switch between signing in with a key and with a password.
- **Where there is no keychain, it is stored as plain text**, as it always has been. Still off by default, the option still says so plainly and names the file it would be written to, and the file is still readable only by its owner. Nothing is encrypted and loftail does not claim otherwise — an SSH key or agent remains the safer choice. Where there is neither a keychain nor a saved host to keep it in, the option is disabled and says why, rather than being offered and doing nothing.
- **It is never one when you were told the other.** If a keychain is there and refuses — a wallet you declined to unlock, a service that has stopped answering — loftail says so and keeps the password for this session only. It does not quietly write it to a file instead.
- **A build without keychain support** behaves exactly as the plain-text case above, and the option says the same thing about the same file.
- **The log is cached locally while you read it.** The copy lives in the system cache directory, is removed when the log is closed, and is what makes scrolling and searching a remote log as quick as a local one. A very large log can be opened from its end only — *Start from the end of the file only*, under **Advanced** — in which case the status bar says the beginning is not shown.
- **How often a remote log is checked for new lines is adjustable**, under **Advanced**, and defaults to once a second. It is per saved host, so a log on a slow link can be polled less often without affecting the others. There is no equivalent for a local log, where the filesystem says when a file has changed and nothing has to ask.
- **Connection trouble is reported, not popped up.** A failure while opening explains itself in the status bar; one that happens while following shows there too, and loftail keeps trying to reconnect. Following a flaky link never produces a dialog. **A host that is simply unreachable opens anyway** and is waited for — see "Logs that are not there" above.
- **A server that will not do SFTP is still readable.** loftail normally reads a remote log over SFTP; where a server signs you in but offers no working SFTP subsystem, it falls back to reading the log with ordinary shell commands instead of refusing. This covers the server that says no *and* the one that says nothing — an embedded box whose sshd is configured for an `sftp-server` that is not installed accepts the request and then goes quiet, and loftail moves on to shell commands rather than reporting a timeout. The status bar says when it has, because that route runs a command on the server for each read and spots a rotation less precisely.
- **A server with no `stat` is readable too.** Reading a log needs `tail` and `head`; *measuring* it — knowing when it has grown — normally uses `stat`, and small embedded systems often ship without one. loftail uses whichever of `stat`, `ls` or `wc` the server has, so any one of the three is enough. Without `stat` there is no modification time to watch, so a rotation that leaves the log exactly the same size is noticed within about half a minute rather than at once; the status bar says so. A server that has only `wc` is the slow case — the log has to be read through to be measured — so it is checked every fifteen seconds instead of every second, and a log that grows past 64 MB that way is reported rather than kept up.
- **A server that offers none of it says so plainly**, and names what it would need: whether the shell itself is unavailable, or the shell is fine and `tail` and `head` are missing, or the log can be read but not measured and any one of `stat`, `ls` or `wc` would fix it.
- **A build without SSH support** says so: the remote menu entries are visible but disabled and explain why.

### Compressed and archived logs

Rotated logs arrive compressed, and loftail opens them as they are — no unpacking by hand first.

- **What opens.** A compressed log (`.gz`, `.bz2`, `.xz`, `.zst`) opens directly by its ordinary path: `loftail app.log.1.gz`. An archive holding several logs (`.zip`, `.tar`, `.7z`, and every compressed tar — `.tar.gz`, `.tgz`, `.tar.xz`, and so on) opens too, once you say which log inside it you want.
- **Choosing a log, but only when there is a choice.** Opening an archive that holds several logs lists them — name, size, modified — and lets you pick. Several can be picked at once, each opening in its own tab, exactly as dropping several files does. A compressed log holds only one, and an archive may happen to hold only one; in neither case are you asked.
- **Addresses continue through the container.** A log inside an archive is written by carrying the path on through it: `bundle.tar.gz/var/log/app.log`. That is accepted anywhere a path is — the Open dialog, the command line, drag-and-drop, the recent-files list and the restored session — and it combines with a remote address, so `ssh://user@host/var/log/app.log.1.gz` opens a compressed log on another machine. A single compressed file keeps its plain name and never grows a member.
- **It fills in as it expands.** Records appear while the log is being decompressed, exactly as they do for a file that is growing, so a large archived log is readable long before it has finished expanding. Filters, highlighters, runs and find all apply to records as they arrive.
- **Then it stops, with nothing to switch off.** Once the log has been expanded in full there is nothing more to come, and loftail stops looking. There is still no mode to choose and no control to forget: the follow control behaves exactly as it always does — it simply has nothing left to follow, which is also true of a finished file nobody is writing.
- **The expanded copy lives in the local cache**, is removed when the log is closed, and is what makes scrolling and searching an archived log as quick as a plain one. A log with no room to expand is refused before anything is written, saying how much space it needs and how much there is; running out part-way says so and keeps what was expanded readable.
- **The name decides how a file is read.** `app.log.gz` is read as compressed and `app.log` as plain text, from the name alone — nothing is guessed from the contents. So a file named wrongly fails with a decompression error rather than being quietly rescued, which is the deliberate cost of the name meaning the same thing every time, including for a file that is not there yet.
- **A build without archive support** says so when one is opened, in the same way a build without SSH support does. The Open dialog offers the same file types either way.


### Runs

A single log file often contains the output of several application runs, one after another. Rather than scroll to find where one run ends and the next begins, the user can name where a run starts and then view one run at a time.

- **A run-start pattern.** In the **Runs** side pane the user supplies a regular expression (or a plain substring) that marks the first line of each run — matched against the **whole log line**, so it can key off a startup banner, a specific subsystem, or any other field. The file is split into runs at every matching line; anything before the first match is kept as a leading "before first run" segment so nothing is unreachable.
- **Viewing one run.** The pane lists the detected runs, each labelled by its start time and first line. Selecting one restricts the entire view — scrolling, filtering, highlighting, find — to just that run's portion of the file; the rest is hidden. An **All runs** choice removes the restriction and shows the whole file again. When a pattern is first applied, the **newest** run is shown, since that is usually the one being worked on.
- **Runs compose with filters and highlighters.** The run restriction narrows *which* records are in view; filters and highlighters then apply within it, exactly as they do to the whole file.
- **Live files keep working.** While tailing the newest run, if the application restarts and writes a new run into the same file, the view stays on the run being watched and the new run simply appears in the list to switch to — the view is never yanked to a different run. Selecting an earlier, finished run detaches follow so its history stays put while the file keeps growing.
- **Remembered per file.** The run-start pattern is remembered for a file the same way its format is, and the session restores which run was being viewed. A file with no run-start pattern behaves exactly as before — one continuous view.

## 4. Log format configuration

Because a log file does not describe its own layout, loftail needs to be told the `ConversionPattern` the writing application used.

- A **Log Format** dialog accepts the pattern string and shows a **live preview**: sample lines from the current file, split into the fields loftail would extract, in the same fixed-width font the record table uses. This makes a wrong pattern immediately obvious rather than failing silently.
- The dialog reports which fields were found. If **priority** or **subsystem** is absent from the pattern, loftail warns that filtering on the missing axis will be unavailable.

- **Every conversion specifier log4cplus's `PatternLayout` defines is accepted**, each becoming its own column:

  | | | | |
  | --- | --- | --- | --- |
  | `%d{…}` / `%D{…}` — timestamp | `%p` — priority | `%c{N}` — subsystem | `%m` — message |
  | `%t` — thread id | `%T` — thread name | `%i` — process id | `%r` — ms since start |
  | `%F` — file | `%b` — file basename | `%L` — line | `%l` — file:line |
  | `%M` — function | `%h` / `%H` — hostname | `%x` — NDC | `%X{key}` — MDC |
  | `%E{VAR}` — environment variable | `%n` — line separator | `%%` — literal percent | |

  Padding and truncation modifiers (`%-5p`, `%.30c`, `%20.30m`) are understood on any of them. A specifier outside this set is rejected with the position of the offending character, rather than being ignored — a pattern written for a different logging library should fail visibly, not produce a table with a column silently missing.
- If a pattern matches poorly, the file still opens: unparsed lines are shown as plain text rather than being hidden or dropped. The user is never left staring at an empty window because of a format mistake.
- **Cancelling the dialog cancels the open.** When the dialog is shown because loftail could not parse a file it was asked to open, dismissing it (Cancel or Esc) abandons that open entirely — no file is opened, and whatever was already on screen stays there. Cancelling means "not like that", so loftail does not fall back to opening the file as a wall of unparsed plain text.
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

Because a log file records no zone information, **how timestamps are read** and **how they are shown** are configured separately. Both are remembered per file, but they are edited in different places.

- **Source time zone** — how to read the timestamps in the file: **Infer from pattern** *(default)*, Local time, UTC, or a fixed offset. Inference uses the date specifier in the configured pattern, since log4cplus distinguishes local-time and UTC forms. Explicit selection exists because the producing application may have been configured in ways the pattern does not reveal, and because logs are routinely read on a different machine than they were written on. *This one lives in the Log Format dialog.*

- **Timestamp display** — how to show them. **Right-clicking the timestamp column's header** offers five mutually exclusive modes. This is the only place the choice is made, and it applies to the *file*, so every view of it agrees.

  | Mode | Shows |
  | --- | --- |
  | **As written in the file** *(default)* | The file's own date format, unconverted — what a text editor shows, which is the least surprising thing when cross-checking against raw log text |
  | **Local time** | The same format, converted to the reader's zone |
  | **UTC** | The same format, converted to UTC |
  | **Seconds** | Seconds since the epoch — `1784644325`, or `1784644325.123` when the file's own format carries milliseconds |
  | **Seconds from run start** | Seconds since the first record of *this record's* run (§3a) — `0.000`, `1.250`, … With no run-start pattern configured the whole file counts as one run, so the mode always works |

  The two seconds modes render a plain number and involve no time zone at all. They exist for reading elapsed time straight out of the log, without arithmetic. Milliseconds appear only when the file's format actually carries them: a log written without sub-second precision shows whole seconds rather than a fabricated `.000` on every row.

Time-range filter bounds (§6) are always entered as wall clock in the display time zone, so what you type matches what you see in the first three modes; the seconds modes do not change how bounds are entered.

**Format autodetection.** Opening a file loftail has not seen before, whose pattern is not already remembered, guesses the `ConversionPattern` from the file itself and **pre-fills the dialog with the guess for confirmation** — never applying it silently, since a wrong pattern quietly mis-splits every record. A **Detect** button re-runs the guess into the pattern field at any time. Detection failure leaves the dialog on the fallback default, so manual entry is always the authority and always available.

## 5. Main view

- Records are displayed as a table, one row per record, with a column per field in the configured format (timestamp, priority, subsystem, message, and any others the pattern defines).

- Columns can be resized, reordered, and individually hidden. Column layout is remembered per view (§5a). Right-clicking any column header offers that list; right-clicking the **timestamp** column also offers its display mode (§4), which applies to the whole file rather than to the view.

- **The table renders in a fixed-width font — every column, and the header.** Logs are column-aligned text written by machines; a proportional font destroys the alignment inside a message and makes timestamps and levels ragged from row to row. The font is the one the desktop designates as fixed-width, at the usual UI text size.

- **Multi-line records are shown in full, in place.** A record whose message spans several lines occupies a correspondingly taller row in the table — the entire text is visible without expanding, selecting, or opening a detail pane. Row heights therefore vary throughout the table.

- **Oversized records are capped.** A record longer than 100 lines (a large stack dump, a serialized payload) displays truncated with an indicator and expands on request, so no single record can fill the viewport. Copying always yields the full text regardless of display truncation.

- **Line wrapping is a user setting with three modes:**
  
  - **Off** — long lines extend horizontally; the view scrolls sideways.
  - **Selected record only** — the focused record wraps so it can be read in full; all others stay unwrapped.
  - **Always on** — every record wraps to the viewport width.
  
  The setting is remembered. Note that in *always on* mode the vertical scrollbar is an approximation that refines as you scroll, since exact total height cannot be known without measuring every record (see `ARCHITECTURE.md` §7.1). Scroll position and navigation stay accurate; only the thumb size and position are estimates.

- Rows can be selected individually or as a range, and copied to the clipboard. Copying yields the original raw text by default, with **Copy as Columns** available as a separate action for pasting into a spreadsheet.

- **Right-clicking a record filters and highlights by what that record contains.** The menu is built from the record under the cursor and offers, for each field it carries: *Show Only Subsystem "net.io"* / *Hide Subsystem "net.io"*, the same pair for its thread, *Show WARN and Above* at the record's own level, and *Start Time Range Here* / *End Time Range Here*. With two or more timestamped records selected it also offers *Filter to Selected Time Range*. A **Highlight** group takes the same values and colors instead of hiding — the rule is added to the end of the list (§7) with a color picked from the palette, so rules already there keep their precedence. Copy and Copy as Columns are on the menu too.
  
  - Every one of these is an ordinary filter or highlight rule: it appears in the Filters or Highlighters pane exactly as if it had been set there by hand, and can be adjusted, undone or saved as a preset from there. Nothing the menu does is expressible only through the menu.
  - **An item the record cannot answer for is not shown** rather than shown greyed. An unparsed plain-text line has no subsystem, thread, level or timestamp (§4), and a format without `%t` or `%d` has none for any record (§6).
  - *Show only* **replaces** that axis's selection; *hide* narrows it and leaves the rest, so repeated hides accumulate. Filtering is per file (§5a), so the effect is visible in every view of that log.
  - Which column was right-clicked decides which items come **first**; it never changes which items are there.

- **Find / Find Next.** Text search over record content, with next/previous navigation, case-sensitivity and regular-expression options, and wrap-around at the end. Distinct from filtering: find moves the cursor and leaves every record visible; filtering removes non-matching records. Find operates on what is currently visible — if a filter is active, find searches the filtered subset.

- A status area shows total record count, the count after filtering, the current file, and whether it is currently receiving new records. It describes the **active** view; a file being scanned in another tab shows its progress in that tab's own label instead.

## 5a. Tabs and the document area

**The logs occupy the centre of the window and the side panes surround them; the two never mix.** Every open file is a tab in that central area, and no gesture can drag a log out of it or a pane into it. Panes rearrange freely among themselves (§8) — that flexibility is theirs alone, so the place the user reads a log is always in the same place.

- **Every open file is a tab.** Tabs can be reordered by dragging them along the tab bar, and each carries a close button. A tab cannot be torn off, split, or floated into a window of its own.
- **A file can be opened in more than one view.** *New View* opens a second, independently scrolled view onto the log already being read, so one can be pinned to a point in the history while the other keeps tailing. It starts as a copy of the view it was made from and diverges from there. Tabs of the same file are numbered in the order they appear on the tab bar.
  - **Shared** between views of one file: the records themselves, the log format, the timestamp display mode, active filters, active highlighters, and the selected run. Filtering in one view filters both — the panes edit the *file*, not the view.
  - **Private** to each view: scroll position, selection, wrap mode, column layout, follow state, and the Find bar.
- **Closing a tab closes that view.** The file itself closes when its last view does; closing every tab leaves the empty view of §3.
- The whole arrangement — which files are open, how many views each has, and the order of the tabs — is part of the remembered session (§10).

## 6. Filtering

Filtering removes non-matching records from the view. The underlying file is never modified.

- **By subsystem.** The subsystem list is discovered automatically from the file as it is scanned, so the user picks from what is actually present rather than typing from memory. Subsystems can also be entered manually — useful when following a file that has not yet emitted a given subsystem. Every discovered subsystem starts selected, including ones that first appear later in the scan.
  
  - **Except after a *show only*** (§5). Ticking a list is a statement about the file and widens with it; restricting to one named subsystem is not, so a subsystem that turns up afterwards stays unselected — otherwise a filter meant as "only this one" would quietly grow, and on a log still being written it would keep growing. Touching the list by hand returns it to the ordinary behavior, and the restriction travels with a saved preset or session.
- **By priority.** A single **minimum level** is chosen (TRACE…FATAL); records below it are hidden. Selecting WARN, for example, shows WARN, ERROR, and FATAL. The default is TRACE, which shows everything.
- **The subsystem and priority axes are enabled by default**, selecting everything. Unticking a subsystem or raising the minimum level therefore takes effect on the first click. The two axes that need a value typed before they can mean anything — message text and time range — stay disabled until the user turns them on, as does the thread axis.
- **A record that lacks a field is never hidden by a filter on that field.** An unparsed plain-text line has no subsystem, no thread, and no priority; filtering by subsystem must not make it disappear, since §4 promises those lines stay visible.
- **By thread.** Like subsystems, the thread list is discovered from the file as it is scanned. Available only when the log format includes a thread field.
- **By message text.** Substring or regular-expression match against the message, with a case-sensitivity option. Unlike the other axes this cannot offer a pick-list, so it is a text box. A negation option (*hide* matching records) is included, since excluding known noise is as common as isolating a signal.
- **By time range.** A start and/or end bound, entered in the display time zone (§4); records outside the range are hidden. Available only when the log format includes a timestamp field.
- Filters can be **enabled and disabled individually** without being deleted, so a user can toggle a view on and off while keeping it configured.
- **Combination semantics:** within one axis, selected values are OR-ed (any of these subsystems); across axes, AND (matching subsystem **and** matching priority).
- The subsystem list supports select-all / select-none / invert, and a text box to narrow long lists.

### Context

Searching the messages for one string also hides everything that led up to it, which is usually the thing worth reading. **Context** brings the neighbours back: two spinners, *Before* and *After*, ask for that many records either side of every match — `grep -B` and `-A`.

- **Context belongs to the message-text search, and it sits in that section of the pane.** It is `grep`'s option, and `grep` is what the message axis is: "show me two records either side of every *mention of this*". The other axes select a *class* of record rather than an event to read around, so "two records either side of every WARN" is not a question context answers — with no message search set up, the spinners do nothing and the pane greys them out to say so.
- **A neighbour still has to pass every other filter.** Context relaxes the message search alone: filtering to WARN-and-above and searching for `timeout` with *Before* at 2 shows the two preceding **WARN-or-worse** records, stepping over the INFOs between them. The filters say what stream is being read; context says how much of that stream to keep around each hit.
- Context records are **shown dimmed**, so a match is still distinguishable at a glance from what was pulled in around it. A dimmed record that also carries a highlight color (§7) keeps that color, softened.
- They are ordinary rows in every other respect: selectable, copyable (§5), reachable by Find (§5), and the record menu on one acts on *that* record.
- Overlapping windows do not repeat a record, and a record that is itself a match is shown as a match even when it also falls inside a neighbour's window.
- Context never reaches **outside the selected run** (§3a): the lead-up to the first error of a run is what that run logged, not the tail of the run before it.
- Both values are **per file**, like the filters themselves, and travel with a saved preset and a restored session (§9, §10).
- With context on, the record count in the status bar says how much of what is shown is context rather than match.

## 7. Highlighting

Highlighting colors matching records without removing anything, for spotting events in context.

- **A highlight rule matches on the same criteria a filter does** (§6): subsystem, thread, priority, time range, and message text. Anything worth hiding is worth being able to color instead — so a rule can pick out every record whose message matches a regex, or every record from one thread inside a time window, without removing anything from the view. Combination is the same as for filters: OR within an axis, AND across them.
- Priority is matched as a **minimum level** (`≥`), the same way the priority filter works, so a rule for "ERROR and above" also colors FATAL; order the list high-severity-first for a per-level look.
- **A record that lacks a field is never matched by a rule on that field** — the deliberate mirror image of §6's promise for filters. An unparsed plain-text line has no subsystem, thread, priority or timestamp: filtering must not *hide* it, and highlighting must not *color* it, or a subsystem rule would paint every such line. The message-text axis has no such case, since an unparsed line's whole text is its message.
- Each axis is **opt-in**, and a rule with no axis set colors nothing. (This is the one place the two panes differ by design: the Filters pane ships its subsystem and priority axes enabled, because there they select everything and so hide nothing, while an all-inclusive highlight rule would color everything.)
- Rules are an **ordered list**; when several match a record, the first match wins. Order is user-adjustable.
- Rules can be enabled and disabled individually, like filters.
- **A rule sets a background color and a text (foreground) color, chosen independently.** Either can be left at its default, which is the record's normal un-highlighted appearance — so a rule may recolor only the background, only the text, or both. A highlight can therefore be as quiet as tinting the text or as loud as a full-row fill.
- **Colors come from a curated palette**, not a free color picker. Each palette entry is defined once for light themes and once for dark, so a highlight stays legible in both and switching themes needs no rework. A rule stores, for each of background and text, either a palette entry or *default* — never a raw color value.
- **The palette is a grid: eight hues in each of three tone bands, plus a neutral closing each band** — 27 entries. The band is how loud the highlight is, and it is the user's choice, not the theme's:
  - **Deep** — dark and saturated. A strong fill under light text, or text on a light row. Closed by **Ink**, a near-black.
  - **Vivid** — maximum chroma. The screaming one; this is what highlighting is for. Closed by **Gray**.
  - **Soft** — pale. A quiet tint under dark text, or text on a dark row. Closed by **Paper**, a near-white.

  A slot keeps its tone in both themes and shifts only enough to sit correctly against the surrounding background, so a rule looks like itself whichever theme it is read in. Every colour has a partner that reads on it — picking a background and then Ink or Paper for the text always gives legible text, on either theme. The hues are the usual severity associations (reds, ambers, greens) plus enough distinguishing ones to tell a handful of rules apart at a glance.
- **A highlight made from the record menu picks both colors**, cycling the hues so a second one-click rule is distinguishable from the first, and pairing each background with the text colour that reads on it.
- Rules saved before the palette grew still load and still match the same records; they keep their entry and so take its new colour.

## 8. Side panes

Filters, highlighters, presets, and runs (§3) are each presented in a side pane, so they are visible and toggleable without opening dialogs.

- Panes can be shown/hidden, resized, moved to either side, or tabbed together. They arrange themselves *around* the log area and never inside it (§5a): a pane cannot be tabbed next to a log, and dragging one can never displace the file being read. A closed pane is brought back from the View menu.
- **Dragging a pane moves that pane**, not the group it is tabbed with, and a pane can be dropped on the left or right side only — not as a strip above or below the log.
- **Panes can also be floated as separate windows, where the platform supports it.** Under Wayland they stay docked: the compositor does not let an application follow the pointer outside its window or place a window under the cursor, so a torn-off pane could not be dragged or positioned — it would strand mid-drag rather than float. The same machine under XWayland does allow it.
- **There is one of each pane, and it follows the active view.** With several logs open, the panes always show and edit whichever one is being read; moving to another log rebinds them to that log's filters and highlighters, and moving between two views of the *same* log changes nothing, because those views share them.
- Pane layout is part of the remembered session (§10).
- Enabling and disabling an individual filter or highlighter is a single click within its pane — no dialog.

## 9. Presets

- **Filter presets** store a complete set of filters; **highlighter presets** store a complete set of highlight rules. The two are independent and separately recallable.
- Presets are created from the current state, and can be renamed and deleted.
- Applying a preset replaces the current set on that axis, rather than merging — which would make the result hard to predict.
- Presets are listed in a side pane and applied in one click.
- Presets persist across sessions and are independent of any particular log file.
- Presets can be exported to and imported from a JSON file, for sharing with colleagues.

## 10. Session persistence

On relaunch, loftail restores:

- Every file that was open — each reopened at its end and following, like any open (§3), so follow state is never a remembered choice
- The log format in use for each, and its timestamp display mode (§4)
- The run-start pattern and which run was being viewed, per file (§3)
- Active filters and highlighters per file, including which were enabled
- Every view: how many views each file had, each one's column layout and wrap mode, and which view was active
- Saved presets
- Window geometry, the order of the tabs, and the arrangement of the side panes (§5a, §8)

**Scoping:** the log format, the timestamp display mode, the run-start pattern, active filters, active highlighters, and the run selection are remembered **per file**, so returning to a given log restores how you were reading *that* log. Column layout and wrap mode are remembered **per view**, so a second view showing wide messages and one showing just timestamps each keep their shape. Presets and the window/pane layout are global.

**Multiple instances.** Because instances run independently (§3), two of them can save session state at the same time. Per-file state is keyed by file, so instances viewing different logs never conflict. For genuinely global state — window layout, and which files to restore on next launch — **the last instance to close wins**.

If a file from the last session is not there, its tab comes back **waiting for it** (§3) — so a log on an unmounted share, or a host that is down for an afternoon, keeps its place and is picked up when it returns rather than being quietly forgotten. Only a file that is actively refused — an address that names no log, a host whose key has changed, an archive without a chosen log inside it — is left out, and those are listed rather than raising an error dialog on every launch. If none can be opened, loftail starts with an empty view.

## 11. Non-goals

These are things loftail will **not** do — as distinct from features not yet built, which are in `FUTURE.md`. loftail does not:

- Edit, write, or delete log files — it is strictly a reader
- Read log formats from other logging frameworks (the format is configurable, so some will happen to work; none are supported)
- Aggregate several files into a single merged, time-ordered view — distinct from simply opening several files, which loftail does (§5a). Merging is not planned at all: each tab stays an independent log
- Provide charts, statistics, or alerting
- Install itself as the **default** system handler for `.log` files — loftail advertises that it *can* open them (so it appears in the OS "Open With" list), but claiming the default handler is a user/installer concern, not something the application does
- Speak any language but English, **for now**. This is the one entry here that is a *not yet* rather than a *never*: the interface is written so a translation can be dropped in without touching the code, and until one is, loftail keeps Qt's own buttons and messages in English too rather than showing a dialog half in your language and half in its own
