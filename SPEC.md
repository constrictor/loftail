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

### Which build this is

**Help ▸ About** names the release and the build behind it:

```
loftail 0.1.0
Build: 100.g443daf4
```

The **release** is the version the download was published under — the same number the `.deb`, the AppImage and the Windows zip carry in their file names. The **build** is the one below it: which continuous-integration run produced these exact bytes, and the commit it was made from. Two downloads of one release can differ, and this is what tells them apart, so it is what a bug report should quote.

A copy built from source on your own machine says `Build: local build`, because there is no run behind it to name.

The same string is what `loftail --version` prints, in one line — `loftail 0.1.0+100.g443daf4`. The dialog exists because an installed application launched from a desktop menu has no command line to ask on.

## 2. Core concepts

| Term           | Meaning to the user                                                                                                                                                         |
| -------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Record**     | One logged event. Usually one line, but a record may span multiple lines when the logged message itself contains line breaks. loftail treats such a record as a single row. |
| **Subsystem**  | The logger name the application passed to `getInstance()` — e.g. `net.socket`, `db.pool`. The primary axis for filtering.                                                   |
| **Priority**   | The severity level: TRACE, DEBUG, INFO, WARN, ERROR, FATAL.                                                                                                                 |
| **Log format** | A log4cplus `ConversionPattern` describing how records are laid out on disk. loftail must know it to split records into fields.                                             |

## 3. Opening logs

- Open a log file via menu, toolbar, keyboard shortcut, or drag-and-drop onto the window.
- **Several logs can be open at once**, each in its own tab (§5a). Opening a file adds a tab rather than replacing what is on screen. Opening a file that is already open raises its tab instead of opening it twice.
- **Several logs can be asked for at once**, and every way in accepts them: the Open dialog is multi-select, a drop of several files opens all of them, and the command line takes as many as are named. They open in the order given, each in its own tab, and the last one is the tab left in front. If one of them cannot be opened the others still open, and the ones that could not are named together in a single message rather than each replacing the last.
- **From the command line:** `loftail <file>...` opens those files directly, at their end and following, like every other open (see live updates below). Files named there are *added* to whatever the remembered session brings back, not a replacement for it. A `--pattern <p>` switch supplies the log format for a file loftail has not seen before, and covers every file named.
- **`--version` identifies the build, not just the release.** A build from the project's own CI reports which one it is — `loftail 0.1.0+1234.g1a2b3c4`, the release, then the run number and the commit it was made from. A build made from source reports the release alone, `loftail 0.1.0`, which is how it says it is not one of those. The downloadable packages are named for the release either way, so a package's filename still says which release it installs and nothing more.
- **A compressed or archived log opens the same way**, written `app.log.1.gz` or `bundle.tar.gz/var/log/app.log`. See "Compressed and archived logs" below.
- **A log on another machine opens the same way**, written as an `ssh://user@host/path/to/file.log` address. It is accepted anywhere a path is: the Open dialog, the command line, a drag-and-drop, the recent-files list, and the restored session. See "Remote logs" below.
- **Multiple instances may run simultaneously.** loftail does not enforce a single instance; launching it again opens an independent window with its own files and its own filters.
- The 10 most recently opened files are listed for quick reopening.
- Opening a large file shows progress and remains responsive; the view populates as scanning proceeds rather than blocking until it finishes.
- Scanning can be cancelled, leaving whatever was scanned so far usable. The stop button sits **beside the progress bar**, appearing and disappearing with it: the progress bar is the only thing that says a scan is running, and on an ordinary log the whole chance to act is over in well under a second — long enough to press what is already under the eye, not long enough to go looking for it in a menu.

### Live updates

- **Every file is opened as if it were live.** loftail cannot know whether a file is complete or still being written, so it always watches the open file and appends new records as they arrive. A file that is never appended to simply never produces any — there is no "post-mortem" versus "live" mode for the user to choose, and none to forget to turn on.
- **A file always opens at its last position, following** — scrolled to the newest record, keeping up with new ones, exactly like `tail -f`. This is unconditional; there is no option to open at the top instead.
- **Follow** can then be turned off: scrolling up manually detaches, so the user can inspect history while the file keeps growing; a control returns to the bottom and re-attaches. Follow is simply on at every open.
- Active filters and highlighters apply to incoming records exactly as they do to existing ones.
- If the file is rotated or truncated by the writing application, loftail detects this and reloads rather than showing stale or corrupt data. This happens silently — no notice is shown.
- **Rewriting a log in place counts as replacing it.** Copying another file over the open one, saving over it from an editor, or restarting a service onto the same path all reload, whether the new content is shorter, longer or exactly the same length as what it replaced. The rule the user can rely on is simply that what is on screen is what is in the file: anything that is not a plain append to the end reloads.
- Because loftail always holds the file open for reading, it must never prevent the writing application from appending to, rotating, or truncating it. Observing a log must not disturb the process producing it.

### Reloading by hand

Everything above is automatic and has no control — there is no tail mode to turn on, no rotation notice to acknowledge, and nothing to press when a log comes back. **View ▸ Reload (F5)** is the one exception, and it is deliberately not a setting: it is the gesture for the case where what is on screen has stopped agreeing with the file and you should not have to work out why.

- **A format you change is applied to the log you changed it on.** Editing a log's conversion pattern or its character encoding in Preferences and ticking *Apply to current file* re-reads that log through the new format, in its own tab: the tab, your filters and your highlight rules stay, and the side panes update to offer whatever the new format can fill — a pattern with no thread field has no Thread filter. If the log is one that has not turned up yet, the new format is what it will be read with when it does.
- **F5 re-reads the active log from the beginning**, with the format it already has. The tab stays, and so do its views, your filters, your highlight rules and the run pattern; the log is followed again afterwards exactly as before.
- **It does not freeze the window.** A large log is re-read on a worker thread with the same progress bar an ordinary open shows, and the scan can be cancelled the same way.
- **On a remote log it re-reads what has been fetched, not the far end.** Reaching the server again is **File ▸ Reconnect**, which is a different question and has its own answer.
- **On a log that has not turned up yet it means "try now".** There is nothing to re-read, so F5 asks the connection to retry immediately instead of waiting out its backoff.

### Diagnostics

loftail keeps a small log about itself, because the two things hardest to report from memory are also the two you cannot watch: a connection that failed while you were doing something else, and a log that never turned up.

- **It records attempts and outcomes, not activity.** A connection attempt and what came of it, which authentication method a server accepted, which transport and which size command it settled on, a host key that did not match, a log that went away and when it came back, a reload. A poll that found nothing new is not recorded.
- **It never records a password, a passphrase, a key, or anything from the logs you are reading.** That it *asked* for a password and whether the server accepted it is the diagnostic question; the password is not part of it. The file is meant to be attachable to a bug report as it stands.
- **Something that repeats is collapsed and counted.** A host that has been down for an hour gets one line a minute saying so, and that line says how many attempts it stands for — so the record of how hard loftail tried stays honest without the file filling up.
- **It is on, it is capped, and it does not need managing.** One megabyte, rolling over once. **Help ▸ Show Diagnostics Log** opens the folder it is in.

### Logs that are not there

A log that does not exist yet is opened the same way as one that does, and so is one that goes away while you are reading it. This follows from the same reasoning as live updates: loftail cannot know whether a log is finished, and it equally cannot know whether one is late.

- **A log that has not been written yet opens anyway.** Point loftail at a path before the application has created it — a service about to start, a log rotated out from under you, a machine still booting — and it opens a tab that says it is waiting. The moment the log appears it is read and followed, with no reopening and nothing to press. This is unconditional and there is no switch for it: an address that is not available yet is simply a log that has not turned up.
- **A log deleted while open is waited for too.** The tab empties and says the log is no longer there, keeping its filters, highlighters, log format and run pattern; when the log comes back it fills in again. A rotation is not this — that is detected as a rotation and reloads silently, as it always has.
- **The tab says which it is.** A waiting tab is marked in the tab bar, the view says what it is waiting for, and the status bar says why — "the host is unreachable", "no such file there". Nothing is popped up.
- **A remote log on a host that is down opens and keeps trying.** loftail reconnects in the background using your SSH agent, your usual keys, or a password you have already given it this session. If the host comes back needing a password it cannot ask for unprompted, it says so and **File ▸ Reconnect** asks again.
- **The remembered session keeps a log that is not available.** A file that could not be opened at launch comes back as a waiting tab rather than being dropped, so an unmounted share or a host that was down for an afternoon does not cost you the tab permanently.
- **A tab appears at once, before loftail has reached the log at all.** Opening a remote or archived log never makes you wait for it: the tab is there immediately, saying it is connecting or expanding, and the window stays usable throughout. Starting loftail with such logs in the remembered session shows the window straight away rather than after every host has answered, and closing a tab on a host that is not answering is instant.
- **A refusal keeps its tab and says why.** A rejected password, a host whose key has changed, a login you cancelled, an archive that would not open: the tab is already on screen by the time the far end answers, so it stays and reports what the server said, rather than appearing and vanishing again. **File ▸ Reconnect** tries once more, asking for a password if one is needed.
- **Only what can be settled without asking anyone still fails to open.** An address that names no log, an archive without a chosen log inside it, or a feature this build does not include: nothing about these could change, and nothing could be shown in a tab, so they are reported and no tab is opened.
- **A log that arrives is not asked about.** loftail applies the format it remembers for that file, or your default. If neither fits, the log shows as plain text and the status bar points at **Log ▸ Format…** — it will not raise a dialog for a log that turned up while you were reading something else.

### Remote logs

- **A remote log behaves exactly like a local one.** It opens at its end and follows; filters, highlighters, runs, the log format and the remembered session all work identically. Rotation, truncation and rewriting in place on the far end are detected and reloaded silently, just as they are locally — with one difference of degree: settling whether a remote log grew or was rewritten costs a read over the network, so it is checked periodically rather than on every poll, and a rewrite that grows the file may take up to half a minute to be noticed. Nothing on this end can tell the two apart any sooner without asking the server about a log that is almost certainly just being appended to.
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
- **Connection trouble is reported, not popped up.** A failure while opening explains itself in the tab and the status bar; one that happens while following shows there too, and loftail keeps trying to reconnect. Following a flaky link never produces a dialog. **A host that is simply unreachable opens anyway** and is waited for — see "Logs that are not there" above.
- **A server that will not do SFTP is still readable.** loftail normally reads a remote log over SFTP; where a server signs you in but offers no working SFTP subsystem, it falls back to reading the log with ordinary shell commands instead of refusing. This covers the server that says no *and* the one that says nothing — an embedded box whose sshd is configured for an `sftp-server` that is not installed accepts the request and then goes quiet, and loftail moves on to shell commands rather than reporting a timeout. It happens quietly: the route costs a command on the server per read and spots a rotation less precisely, but a tail that is keeping up is a tail that is keeping up, and a permanent line in the status bar saying otherwise is one the reader stops seeing. What can actually stop the log arriving still reports itself, as any other trouble would.
- **A server with no `stat` is readable too.** Reading a log needs `tail` and `head`; *measuring* it — knowing when it has grown — normally uses `stat`, and small embedded systems often ship without one. loftail uses whichever of `stat`, `ls` or `wc` the server has, so any one of the three is enough. Without `stat` there is no modification time to watch, so a rotation that leaves the log exactly the same size is noticed within about half a minute rather than at once. A server that has only `wc` is the slow case — the log has to be read through to be measured — so it is checked every fifteen seconds instead of every second, and a log that grows past 64 MB that way is reported rather than kept up.
- **A server that offers none of it says so plainly**, and names what it would need: whether the shell itself is unavailable, or the shell is fine and `tail` and `head` are missing, or the log can be read but not measured and any one of `stat`, `ls` or `wc` would fix it.
- **A build without SSH support** says so: the remote menu entries are visible but disabled and explain why.

### Compressed and archived logs

Rotated logs arrive compressed, and loftail opens them as they are — no unpacking by hand first.

- **What opens.** A compressed log (`.gz`, `.bz2`, `.xz`, `.zst`) opens directly by its ordinary path: `loftail app.log.1.gz`. An archive holding several logs (`.zip`, `.tar`, `.7z`, and every compressed tar — `.tar.gz`, `.tgz`, `.tar.xz`, and so on) opens too, once you say which log inside it you want.
- **Choosing a log, but only when there is a choice.** Opening an archive that holds several logs lists them — name, size, modified — and lets you pick. Several can be picked at once, each opening in its own tab, exactly as dropping several files does. A compressed log holds only one, and an archive may happen to hold only one; in neither case are you asked.
- **Looking inside a large archive can be interrupted.** A compressed tar has no index, so listing what is in one costs as much as unpacking it. loftail shows what it is doing and offers Cancel, the rest of the window keeps working, and logs already open go on updating. An archive on another machine is fetched in full before it is listed, so what you are shown is all of it and not just the part that had arrived.
- **Addresses continue through the container.** A log inside an archive is written by carrying the path on through it: `bundle.tar.gz/var/log/app.log`. That is accepted anywhere a path is — the Open dialog, the command line, drag-and-drop, the recent-files list and the restored session — and it combines with a remote address, so `ssh://user@host/var/log/app.log.1.gz` opens a compressed log on another machine. A single compressed file keeps its plain name and never grows a member.
- **It fills in as it expands.** Records appear while the log is being decompressed, exactly as they do for a file that is growing, so a large archived log is readable long before it has finished expanding. Filters, highlighters, runs and find all apply to records as they arrive.
- **Then it stops, with nothing to switch off.** Once the log has been expanded in full there is nothing more to come, and loftail stops looking. There is still no mode to choose and no control to forget: the follow control behaves exactly as it always does — it simply has nothing left to follow, which is also true of a finished file nobody is writing.
- **The expanded copy lives in the local cache**, is removed when the log is closed, and is what makes scrolling and searching an archived log as quick as a plain one. A log with no room to expand is refused before anything is written, saying how much space it needs and how much there is; running out part-way says so and keeps what was expanded readable.
- **The name decides how a file is read.** `app.log.gz` is read as compressed and `app.log` as plain text, from the name alone — nothing is guessed from the contents. So a file named wrongly fails with a decompression error rather than being quietly rescued, which is the deliberate cost of the name meaning the same thing every time, including for a file that is not there yet.
- **A build without archive support** says so when one is opened, in the same way a build without SSH support does. The Open dialog offers the same file types either way.


### Runs

A single log file often contains the output of several application runs, one after another. Rather than scroll to find where one run ends and the next begins, the user can name where a run starts and then view one run at a time.

- **A run-start pattern.** In the **Runs** side pane the user supplies a regular expression (or a plain substring) that marks the first line of each run — matched against the **whole log line**, so it can key off a startup banner, a specific subsystem, or any other field. The file is split into runs at every matching line; anything before the first match is kept as a leading "before first run" segment so nothing is unreachable.
- **Viewing one run.** The pane lists the detected runs, each labelled by its start time and first line. Selecting one restricts the entire view — scrolling, filtering, highlighting, find — to just that run's portion of the file; the rest is hidden. An **All runs** choice removes the restriction and shows the whole file again.
- **Last run.** The first entry in the list, and the one every log opens on, is **Last run** — not a run but a standing instruction to show whichever run is last. It is what the user usually means: the run the application is in *now*. A log with no runs detected — no pattern, or a pattern nothing has matched yet — is the whole file under it, exactly as a log with no run-start pattern has always been, so it is never a way to see nothing; an empty file shows nothing because it holds nothing.
- **Runs compose with filters and highlighters.** The run restriction narrows *which* records are in view; filters and highlighters then apply within it, exactly as they do to the whole file.
- **Live files keep working, and which run is watched is the user's choice.** Under **Last run**, an application that restarts and writes a new run into the same file takes the view with it — the new run is what is being watched now, with no click. Picking a specific run instead **pins** it: the new run appears in the list to switch to and the view stays where it was put, whether or not the run pinned was the last one at the time. Selecting an earlier, finished run detaches follow so its history stays put while the file keeps growing.
- **Remembered per file.** The run-start pattern is remembered for a file the same way its format is, and the session restores which run was being viewed — including **Last run** as itself, so a log left following its newest run comes back following whichever run is newest *then*, not pinned to the one that was newest when the session was saved. A file with no run-start pattern behaves exactly as before — one continuous view.

## 4. Log format configuration

Because a log file does not describe its own layout, loftail needs to be told the `ConversionPattern` the writing application used. That is set in **File ▸ Preferences**, at whichever of three levels it belongs to — see "Settings for a log, a kind of log, and every log" below.

- The format editor accepts the pattern string and shows a **live preview**: sample lines from the current file, split into the fields loftail would extract, in the same fixed-width font the record table uses. This makes a wrong pattern immediately obvious rather than failing silently.
- It reports which fields were found. If **priority** or **subsystem** is absent from the pattern, loftail warns that filtering on the missing axis will be unavailable.

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
- **Cancelling cancels the open.** When Preferences opens by itself because loftail could not parse a file it was asked to open, dismissing it (Cancel or Esc) abandons that open entirely — no file is opened, whatever was already on screen stays there, and the entry it had just created for that log is discarded along with every other edit. Cancelling means "not like that", so loftail does not fall back to opening the file as a wall of unparsed plain text.

### Settings for a log, a kind of log, and every log

Most people have a handful of house layouts and a great many log files. So the settings that say how a log is read are arranged in **three levels**, and the most specific one that names a log decides:

| Level | Applies to |
| --- | --- |
| **Default settings** | any log the two levels below do not name |
| **A file pattern** | every log whose name matches — `*.log`, `audit-*`, a regular expression |
| **One log** | that log alone, local or remote |

Each level holds the same complete set: the conversion pattern, the character encoding, the source time zone, how timestamps are displayed, the run-start pattern with its two flags, and the line wrapping a newly opened view starts in. **The deepest level that names a log supplies all of them** — the levels are not merged field by field, so what a log opens with is always exactly what one entry says.

**File ▸ Preferences** (`Ctrl+P`, or the platform's own convention where it has one — `⌘,` on macOS) shows the whole arrangement as a tree, with the selected entry's settings beside it. What the selected entry *is* comes first — the pattern it matches on, or the log's address — with a rule under it separating that from what it gives its logs. Below the rule the settings read as three named blocks: **File format** (the conversion pattern, the encoding, the source time zone) with a live preview of the sample under it, **Run splitting**, and **Display**.

- **File patterns are ordered, and the first match wins.** A log matching two patterns takes the higher one; ↑ and ↓ change which. A pattern is a **wildcard** (`*` and `?`, matching the whole name) or a **regular expression** (matching anywhere in it), optionally case sensitive.
- **A pattern added starts empty, and an empty pattern matches nothing.** It claims logs only once it says which — a new row cannot take over every log on the machine while you are still deciding what it is for. Its *settings* are seeded from wherever it was added: from the selected log, so "make a pattern out of how this one is read" is one button.
- By default a pattern is matched against the log's **own file name** — the member's name inside an archive, the file name at the end of an `ssh://` address. **Match the whole path** widens it to the entire address, host included, so "every log on prod-web" is expressible.
- **A log gets an entry of its own only when you change something.** Opening a log leaves nothing behind; entries appear when a setting is made to differ from what the log would otherwise inherit, and disappear again when it is brought back into line — **including when the line moves**. Teach a pattern what a log's own entry already said, and that entry goes: the log follows the pattern from then on rather than holding a copy of it that stops tracking the moment the pattern is edited again. The same happens to entries that agreed with their pattern all along, the first time Preferences is opened on them.
- **Promote** moves the selected entry's settings up exactly one level, and says which level that is. **Promote to Parent Pattern** gives one log's settings to the pattern that matched it, so every log that pattern matches gets them; the log's own entry then has nothing left to say and is removed. **Promote to Default Settings** does the same for a pattern, making its settings what every log with no pattern of its own opens with — but the pattern *stays*, because it is a matcher as well as a set of settings and removing it would re-home its logs onto whatever pattern matched next.
- **Apply to current file** puts the selected entry's settings on the log that is open, without waiting for the next time it is opened. Which log that is is named in its tooltip.
- A log matched by no pattern is listed under **Logs with no matching pattern**. That heading is not itself a setting and has nothing to edit; it is simply where such logs are shown. Such a log cannot be promoted either — its only parent is the defaults, and handing one log's settings to every log is a level skipped rather than a level up. Give it a pattern first.
- **Forget Individual Files** deletes every per-log entry at once, so each log falls back to its pattern or to the defaults. Logs already open are unaffected.
- Nothing is written until **OK**. Cancel discards every edit, including a pattern added, an entry deleted and a bulk forget.

**Settings are never applied silently to a log they cannot parse.** Whichever level answered, loftail checks the result against the file it just opened; if not one record parses, Preferences appears with an entry for that log, pre-filled with the autodetected pattern. A wrong entry costs a dialog, never a mis-split table.

Out of the box the defaults are log4cplus's own conventional layout. Clearing the pattern entirely is a valid answer, and means "ask me about every log".

### Character encoding

Encoding is an explicit setting, offered as a list:

| Choice                      | Behavior                                                    |
| --------------------------- | ----------------------------------------------------------- |
| **Auto-detect** *(default)* | Byte-order mark where present, content inspection otherwise |
| UTF-8                       | Forced, BOM tolerated and skipped                           |
| UTF-16 LE / UTF-16 BE       | Forced                                                      |
| System 8-bit                | The platform's local codepage                               |

- Auto-detect is the default because it is right nearly always — this matters because log4cplus built for `wchar_t` writes UTF-16 on Windows, and users should not have to know that.
- When auto-detect is active, Preferences shows **which** encoding it settled on, so a wrong guess is visible rather than silent — but only under the entry for **the log it read**, which is the one that was open when Preferences was reached. A pattern and the defaults cover a class of logs and get no such line: a reading taken from whichever file happened to be open is a fact about that file, and each log is examined on its own as it is opened. Nor does another log's entry get it, and nor does anything when there is no sample at all.
- The choice is explicit and forceable because no heuristic is reliable on short files, on files whose first records are pure ASCII, or on legacy 8-bit logs — auto-detect is a convenience, not a guarantee.
- The setting travels with the rest of an entry's settings, at whichever level that entry sits.

### Timestamps and time zones

Timestamps are parsed into real points in time, not treated as opaque text — this is what makes time-range filtering (§6) and jump-to-time possible.

Because a log file records no zone information, **how timestamps are read** and **how they are shown** are configured separately. Both belong to an entry in the settings tree, and both can be set there; the display mode additionally has a shortcut of its own.

- **Source time zone** — how to read the timestamps in the file: **Infer from pattern** *(default)*, Local time, UTC, or a fixed offset. Inference uses the date specifier in the configured pattern, since log4cplus distinguishes local-time and UTC forms. Explicit selection exists because the producing application may have been configured in ways the pattern does not reveal, and because logs are routinely read on a different machine than they were written on.

- **Timestamp display** — how to show them. **Right-clicking the timestamp column's header** offers five mutually exclusive modes, and applies the choice to the *file*, so every view of it agrees. The same five are in Preferences, where they can be set for a pattern or for the defaults rather than for one log.

  | Mode | Shows |
  | --- | --- |
  | **As written in the file** *(default)* | The file's own date format, unconverted — what a text editor shows, which is the least surprising thing when cross-checking against raw log text |
  | **Local time** | The same format, converted to the reader's zone |
  | **UTC** | The same format, converted to UTC |
  | **Seconds** | Seconds since the epoch — `1784644325`, or `1784644325.123` when the file's own format carries milliseconds |
  | **Seconds from run start** | Seconds since the first record of *this record's* run (§3a) — `0.000`, `1.250`, … With no run-start pattern configured the whole file counts as one run, so the mode always works |

  The two seconds modes render a plain number and involve no time zone at all. They exist for reading elapsed time straight out of the log, without arithmetic. Milliseconds appear only when the file's format actually carries them: a log written without sub-second precision shows whole seconds rather than a fabricated `.000` on every row.

Time-range filter bounds (§6) are always entered as wall clock in the display time zone, so what you type matches what you see in the first three modes; the seconds modes do not change how bounds are entered.

**Format autodetection.** Opening a file whose settings do not parse it guesses the `ConversionPattern` from the file itself and **pre-fills the dialog with the guess for confirmation** — never applying it silently, since a wrong pattern quietly mis-splits every record. A **Detect** button re-runs the guess into the pattern field at any time. Detection failure leaves the dialog on the fallback default, so manual entry is always the authority and always available.

## 5. Main view

- Records are displayed as a table, one row per record, with a column per field in the configured format (timestamp, priority, subsystem, message, and any others the pattern defines).

- Columns can be resized, reordered, and individually hidden. Column layout is remembered per view (§5a). Right-clicking any column header offers that list; right-clicking the **timestamp** column also offers its display mode (§4), which applies to the whole file rather than to the view.

- **A column opens at a width that fits what is in it.** Its width is measured from the table's own fixed-width font — the column's heading and a typical value of its field — rather than being a pixel count chosen in advance: a *Priority* column too narrow to fit the word *Priority* cannot say what it is. Once the whole file has been read, the **Subsystem** and **Thread** columns take the width of the longest name the log actually contains, which is known exactly by then and not before.

- **A width the reader has set is theirs.** A divider they dragged, a column they fitted, and a layout restored from the last session are never widened or narrowed by loftail afterwards. The header menu offers the way back and the way further:

  - **Fit to Contents** widens or narrows every column to the widest value it holds, so a column of short subsystem names stops spending a third of the window. On a log too large to measure in full it reads a sample rather than every record, and no fit opens a column wider than a window's worth.
  - **Reset Widths** forgets every width anyone has set — dragged, fitted or restored — and puts the columns back where they opened.
  - **Double-clicking a divider** fits the column to its left, which is what a double-click on a divider does everywhere else and previously did nothing here.

- **The table renders in a fixed-width font — every column, and the header.** Logs are column-aligned text written by machines; a proportional font destroys the alignment inside a message and makes timestamps and levels ragged from row to row. The font is the one the desktop designates as fixed-width, at the usual UI text size.

- **A value too wide for its column ends in an ellipsis, and hovering it shows it in full.** A thread named `http-nio-8080-exec-3` does not fit a thread column at any sensible width, and a name cut off mid-character is indistinguishable from a name that genuinely ends there — so a value that does not fit is drawn with a trailing `…`, and its whole text is available as a tooltip. Column **headings** behave the same way, so a column headed *Priorit* names itself on hover rather than leaving the reader to guess.

  - **A value that already fits offers no tooltip.** The tooltip says "there is more here" and nothing else; one that repeated what is already on screen would train the reader to ignore the ones that do not.
  - A **message** shown wrapped is on screen in full, in as many lines as it takes, so it offers nothing to hover. With wrapping off it is clipped at its column like every other field, and elides and answers exactly like one — line by line, since a multi-line record's lines are clipped one by one.

- **Multi-line records are shown in full, in place.** A record whose message spans several lines occupies a correspondingly taller row in the table — the entire text is visible without expanding, selecting, or opening a detail pane. Row heights therefore vary throughout the table.

- **Every other record is shaded.** Records alternate between the view's background and a faintly tinted band, and the band covers the whole **record** rather than a line of it — so in *always on* wrapping, where one record occupies three or four lines, the shading is what says where one record ends and the next begins instead of the reader tracking it by the timestamp column. The tint is derived from the theme, so it is equally present on a light and on a dark one. A record a highlight rule has coloured (§7) keeps its own colour, unshaded: the band is what a record wears when nothing else has claimed it.

- **Oversized records are capped.** A record longer than 100 lines (a large stack dump, a serialized payload) displays truncated with an indicator and expands on request, so no single record can fill the viewport. Copying always yields the full text regardless of display truncation.

- **Line wrapping is a user setting with three modes:**
  
  - **Off** — long lines extend horizontally; the view scrolls sideways.
  - **Selected record only** — the focused record wraps so it can be read in full; all others stay unwrapped.
  - **Always on** — every record wraps to the viewport width.

  The mode a newly opened view starts in is remembered for the log (§4); changing it afterwards changes the view in front of you and is remembered for that view alone, so two views of one file can differ. Note that in *always on* mode the vertical scrollbar is an approximation that refines as you scroll, since exact total height cannot be known without measuring every record (see `ARCHITECTURE.md` §7.1). Scroll position and navigation stay accurate; only the thumb size and position are estimates.

- **A selection is built with the pointer the way a table's is.** Clicking a record selects it; **dragging** from it extends the selection as the pointer moves, and a drag held past the top or bottom edge of the view scrolls and goes on extending, so a selection is not limited to what is on screen. **Shift+click** extends to the record clicked, and the arrow keys extend the same way while Shift is held. **Ctrl+click** (Cmd on macOS) takes a single record in or out of the selection without disturbing the rest, so a selection need not be contiguous. Right-clicking *inside* a selection leaves it alone — the menu below acts on it — and right-clicking outside one moves it to the record under the cursor.

- **Edit ▸ Select All** (`Ctrl+A`) selects every record **currently in view** — the filtered subset while a filter is active (§6), not every record in the file — so it composes with the filters, and so does the copy that usually follows it. It acts on the view in front of you and does not move it: the reader keeps their place, and a click, a drag or a Shift+click takes the selection back down to what was clicked.

- The selection is copied to the clipboard. Copying yields the original raw text by default, with **Copy as Columns** available as a separate action for pasting into a spreadsheet.

- **Right-clicking a record filters and highlights by what that record contains.** The menu is built from the record under the cursor and offers, for each field it carries: *Show Only Subsystem "net.io"* / *Hide Subsystem "net.io"*, the same pair for its thread, *Show WARN and Above* at the record's own level, and *Start Time Range Here* / *End Time Range Here*. With two or more timestamped records selected it also offers *Filter to Selected Time Range*. A **Highlight** group takes the same values and colors instead of hiding — the rule is added to the end of the list (§7) with a color picked from the palette, so rules already there keep their precedence. Copy and Copy as Columns are on the menu too.
  
  - Every one of these is an ordinary filter or highlight rule: it appears in the Filters or Highlighters pane exactly as if it had been set there by hand, and can be adjusted, undone or saved as a preset from there. Nothing the menu does is expressible only through the menu.
  - **An item the record cannot answer for is not shown** rather than shown greyed. An unparsed plain-text line has no subsystem, thread, level or timestamp (§4), and a format without `%t` or `%d` has none for any record (§6).
  - *Show only* **replaces** that axis's selection; *hide* narrows it and leaves the rest, so repeated hides accumulate. Filtering is per file (§5a), so the effect is visible in every view of that log.
  - Which column was right-clicked decides which items come **first**; it never changes which items are there.

- **Double-clicking a Subsystem or Thread cell shows only that value.** It is the menu's own *Show Only Subsystem "net.io"* / *Show Only Thread "worker"*, reached without the menu, on the two columns where a cell names a value the filters hold — so it lands in the Filters pane like any other edit and is taken back there. A double-click anywhere else does nothing: the Time, Priority and Message columns have no one obvious meaning to give it, and a cell the record cannot answer for — the subsystem of an unparsed line (§4), or any thread cell under a format with no `%t` (§6) — does nothing either. It is not a toggle; double-clicking the same cell again re-applies the same filter. (Double-clicking a **divider in the column header** is the separate gesture above: it fits that column.)

- **Find / Find Next.** Text search over record content, with next/previous navigation, case-sensitivity and regular-expression options, and wrap-around at the end. Distinct from filtering: find moves the cursor and leaves every record visible; filtering removes non-matching records. Find operates on what is currently visible — if a filter is active, find searches the filtered subset.

  - **Enter searches forward and Shift+Enter searches backwards**, from the query box, without reaching for the ▲/▼ buttons or for F3/Shift+F3 — which go on doing the same two things from anywhere in the window. Both of a keyboard's Enter keys count, the main one and the keypad's. Escape still closes the bar.
  - **What matched is marked where it is.** Finding a record is only half of finding something: on a long message the reader still has to hunt for the words. So the run the query matched is marked in the records on screen — every match in view, not only the one in the record the search landed on, since a record can hold several and the neighbours are worth seeing too. A mark is that run in the record's **own two colours swapped**, so it is exactly as readable as the record already was: a record a highlight rule coloured keeps its colour and wears the mark inside it, and a selected record wears it in the selection's. Under **Line Wrap ▸ Always On** a match running across a wrapped line is marked on both lines; with wrap off, a match past the point where a column elides is not on screen and is not marked. The marks follow the query — changing it re-marks, emptying it marks nothing, a query that matches nothing marks nothing, and closing the bar takes them away — and they stay put across scrolling, a resize, a filter change, a tab switch and new records arriving. The digest strip is left unmarked: Find walks the table, and a mark down there would say it had landed in the strip.
  - **The bar says what the search did**, in its own status area rather than the window's status bar — which reports the file and is rewritten as records arrive. Landing on a match reads `2 of 7`: the match's place among the matches in the current view, and how many there are. Nothing matching reads `no match`; a view with no records at all reads `no records`; a query the regex option cannot compile reads `bad regex`; emptying the box clears the report, as does reopening the bar for a new search.
  - **A wrap says so.** Find Next at the last match still goes back round to the first, and now reads `1 of 7, wrapped to the top`; Find Previous at the first match reads `wrapped to the bottom`. The next ordinary step drops the note again.
  - **The total is bounded, and says when it is a floor.** Counting matches means reading every visible record, which on a very large log is not something to do between keystrokes — so counting gives up after a fixed effort. When it does, the total is shown with a trailing `+` (`2 of 7+` — seven counted before counting gave up, and there may be more), and when the match landed on lies past the point counting reached, there is no place to report and the bar says only `match`. What is on the left of `of` is always exact; what is on the right is exact only without the `+`.
  - The report describes the search that was run. It is not kept up to date as new records arrive — the next Find Next re-counts.

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

- **By subsystem.** The subsystem list is discovered automatically from the file as it is scanned, so the user picks from what is actually present rather than typing from memory. Subsystems can also be entered manually — useful when following a file that has not yet emitted a given subsystem — by typing the name into the same box that narrows the list. Every discovered subsystem starts selected, including ones that first appear later in the scan.
  
  - **And whether that stays true is a control, not a rule — and it is the first entry in the list.** **Others**, on top of the discovered names and ticked like them, stands for every subsystem the scan has not turned up yet: ticked, a new one arrives selected; unticked, the list is a *restriction* and only what is ticked is ever shown. It matters most on a log still being written, where "everything" and "everything so far" are different filters. It is an entry rather than a checkbox beside the list because it is the same kind of answer as the entries around it — these subsystems, and the others — and it is on top rather than at the bottom because the bottom of the list is wherever the scan has got to. It is not narrowed away by the box above the list, since it is not one of the names being searched.
  - **The list's own buttons carry it** (below), and *Show Only* (§5) unticks it: restricting to one named subsystem is a statement about the axis, not about the file, so a subsystem that turns up afterwards stays unselected — otherwise a filter meant as "only this one" would quietly grow. Nothing else moves it, so ticking one more subsystem by hand is not read as taking the restriction back. The setting travels with a saved preset or session.
- **By priority.** A single **minimum level** is chosen (DEBUG…FATAL); records below it are hidden. Selecting WARN, for example, shows WARN, ERROR, and FATAL. The default is **INFO**, the level most logs are read at. **TRACE is not offered**, although it is a level a record can carry: it is the lowest there is, so a TRACE floor hides nothing, and an entry that cannot narrow anything is the one thing a list of levels should not open with. What it meant is still available and from the control that means it — leaving the axis switched off.
- **The subsystem axis is enabled by default**, selecting everything, so unticking a subsystem takes effect on the first click. Every other axis starts switched off, priority included — a log opens showing every record it holds. Priority was enabled by default while TRACE was its default level, which hid nothing; now that its lowest level is DEBUG there is no setting that is both switched on and harmless, and defaulting it on would hide every DEBUG line of every log on open. Nothing is lost by the change: a switched-off axis greys its own controls, so the minimum level cannot be moved without the tick that puts it in force, and "I changed the level and nothing happened" is unreachable rather than merely unlikely.
- **A record that lacks a field is never hidden by a filter on that field.** An unparsed plain-text line has no subsystem, no thread, and no priority; filtering by subsystem must not make it disappear, since §4 promises those lines stay visible.
- **By thread.** Like subsystems, the thread list is discovered from the file as it is scanned. Available only when the log format includes a thread field.
- **By message text.** Substring or regular-expression match against the message, with a case-sensitivity option. Unlike the other axes this cannot offer a pick-list, so it is a text box, with the three options — regular expression, case sensitivity, negation — as toggles on the row beneath it. A negation option (*hide* matching records) is included, since excluding known noise is as common as isolating a signal. **A pattern that is not a valid regular expression says so** in words under the field: it matches nothing, which is otherwise indistinguishable from a pattern that is merely too narrow.
- **By time range.** A start and/or end bound; records outside the range are hidden. Available only when the log format includes a timestamp field. **The bounds are asked for in the same terms the timestamp column is currently showing** (§4): a date and time in the display zone for the three wall-clock modes, and a number of seconds — from the epoch, or from the selected run's start — for the two that render the column as seconds, at the same precision the column uses. Reading "12.480" off a row and then being asked for a calendar date is a question about a quantity the log never displayed, and answering it means converting by hand from a baseline only loftail knows. **Changing how the column reads never moves a bound**: the digits are rewritten, the instant they name is kept. In "seconds from run start" that instant is counted from the *selected* run, since one bound cannot be relative to each record's own run — so selecting another run rewrites the bounds too, and again does not move them. **The two bounds open on the span the file actually covers** and keep following it while the file is scanned or grows, so switching the axis on hides nothing and the editors start somewhere useful. A bound the user sets is theirs from then on and is never moved back.
- **An axis the log's format cannot support is not shown at all.** Without a thread field or a timestamp field, the corresponding axis is simply not in the pane — exactly as in the Highlighters pane (§7). A switched-off axis keeps its controls because switching it on is a decision the user can make; a missing field is not, so there is nothing to read and decide about, and a section that only ever says "not available here" is read once and occupies the pane for the rest of the session. Such an axis is never in force either: a preset or a restored session that arrives with it switched on has it silently ignored, and nothing on screen offers to switch it back on.
- Filters can be **enabled and disabled individually** without being deleted, so a user can toggle a view on and off while keeping it configured. **An axis that is switched off still shows its controls**, greyed: what an axis offers is most worth knowing before deciding to use it, and a pane whose controls appear only once their axis is ticked can be explored but not read. The pane scrolls where it has to.
- **The axes read top to bottom as Priority, Subsystem, Message text, Thread, Time range**, which is the order they are reached for: the level floor and the subsystem list say which records are being read, and the free-text search comes under them because a stream is picked and *then* searched. The context spinners (below) travel with the message search rather than with the pane, so they move with it.
- **The two value lists grow with the pane; nothing else does.** A subsystem or thread list is as long as the log has subsystems or threads — not known when the pane is drawn, and still growing while the file is scanned — where every other axis is a fixed handful of rows. So a taller dock is a taller pair of lists, sharing the extra height evenly, rather than the same two short lists with a gap underneath. Below the height that fits both, the pane scrolls instead of shrinking them. **The same is true of a highlight rule's axes** (§7), for the same reason and by the same means: it is one set of axes, and which of them grows is not something the two panes are allowed to differ on.
- **Every axis is a tick, a name and a line across the pane** — the switch that turns it on is the row it is named in, and the line under that name is where the axis begins. It reads the same in the Highlighters pane (§7), because it is the same five axes: what an axis looks like is not something the two panes are allowed to differ on.
- **Combination semantics:** within one axis, selected values are OR-ed (any of these subsystems); across axes, AND (matching subsystem **and** matching priority).
- **Ctrl+clicking an entry selects it and deselects every other one** — *Show Only* (§5) reached from the list itself, and the edit these lists are most often wanted for: picking one subsystem out of thirty is otherwise *None* followed by hunting the name down again, or a trip to a record that happens to carry it. It works anywhere on the entry, not only on its tick, and it means the same thing there as it does on the record menu — including *Others*, so a subsystem the scan turns up afterwards stays deselected. Ctrl+clicking ***Others*** itself is the same rule with the other entry as its target: only what has not been found yet, which deselects every listed name and puts the discovery rule back on. A plain click is untouched and still ticks one entry. The chord works identically in the thread list and in a highlight rule's own subsystem and thread lists (§7) — they are the same lists.
- The subsystem list supports select-all / select-none / invert, and a text box to narrow long lists. **The three buttons act on what the narrowing currently shows**, and say how many entries it is hiding from them — a hidden entry keeps its tick, and so keeps letting its records through. **They also set *Others*:** *All* and *None* are claims about the axis, not about the handful of names listed a third of the way through a scan, so *None* really does mean nothing — including whatever turns up next — and *Invert* flips that answer too. The same box is where a subsystem is entered by hand: typing a name the list does not hold offers to add it, ticked, whatever *Others* says — asking for a name is asking to see it.
- **The pane's own tab says whether anything is being filtered out.** Because it is one of four panes tabbed together, filters are usually in force while out of sight, so the tab is marked while they are — which is the case that needs answering, and the reason the answer is not written inside the pane where it could only be read once you had already switched to it. **View ▸ Clear Filters** returns every axis to its default, ticks every discovered value again and switches context off, which is otherwise five axes' worth of clicking; it sits with the rest of the view's commands rather than on a row of the pane, which is a row the axes get instead.
- On a very large log, edits made in quick succession are applied together rather than one at a time. This is measured, not assumed: a filter pass that completes quickly is applied as it is made, so on an ordinary log the view narrows as the user types.
- **Changing a filter does not move the reader.** Narrowing or widening the view remaps every row on screen, so without this the selected record was lost and the view landed wherever the shortened scrollbar happened to clamp — usually the bottom of the file, several thousand records from what was being read. Instead:
  - **The selected record keeps its place.** If it survives the new filter it stays selected, and if it was on screen it stays at the same height in the window: the rows around it change, the row being read does not move.
  - **If the new filter hides the selected record, the view still shows the same part of the file** — the nearest surviving record at or after what was at the top comes to the top, so what has already been read scrolls off rather than back into view.
  - **A hidden selection is remembered, never moved.** Nothing is selected while the record is filtered out — the selection is not slid onto a neighbour the user did not pick — but widening the filter again brings it back, which is what makes typing a search and backspacing over it a round trip rather than a one-way door. Anything else that replaces the records themselves (the file rotating, reloading, or switching runs) forgets it, and so does selecting another record.
  - **Following the tail outranks all of it**, and a filter change never attaches or detaches follow by itself. A view pinned to the newest record stays pinned; a view the user had scrolled away from stays where it is, even when the new filter leaves fewer records than fill the window.
  - A multi-record selection collapses to the one record that was focused, since the rows between its ends need not have survived.
  - Each view of a file keeps **its own** place: two views of one log, scrolled differently, both stay where they were (§9).

### Context

Searching the messages for one string also hides everything that led up to it, which is usually the thing worth reading. **Context** brings the neighbours back: two spinners, *Before* and *After*, ask for that many records either side of every match — `grep -B` and `-A`.

- **Context belongs to the message-text search, and it sits in that section of the pane** — on the same row as the search's own options. It is `grep`'s option, and `grep` is what the message axis is: "show me two records either side of every *mention of this*". The other axes select a *class* of record rather than an event to read around, so "two records either side of every WARN" is not a question context answers — with no message search set up, the spinners do nothing and the pane greys them out to say so.
- **The two spinners are marked `↑` and `↓`, not with words**, because they share one row with the search's own `.*` / `Aa` / `≠` toggles and there is no room for two more captions. The arrow is the direction the records lie in: the log is read top to bottom, so *before* a match is above it and *after* is below. Hovering either spinner names it in full, and so does a screen reader.
- **A neighbour still has to pass every other filter.** Context relaxes the message search alone: filtering to WARN-and-above and searching for `timeout` with *Before* at 2 shows the two preceding **WARN-or-worse** records, stepping over the INFOs between them. The filters say what stream is being read; context says how much of that stream to keep around each hit.
- Context records are **shown dimmed**, so a match is still distinguishable at a glance from what was pulled in around it. A dimmed record that also carries a highlight color (§7) keeps that color, softened.
- They are ordinary rows in every other respect: selectable, copyable (§5), reachable by Find (§5), and the record menu on one acts on *that* record.
- Overlapping windows do not repeat a record, and a record that is itself a match is shown as a match even when it also falls inside a neighbour's window.
- Context never reaches **outside the selected run** (§3a): the lead-up to the first error of a run is what that run logged, not the tail of the run before it.
- Both values are **per file**, like the filters themselves, and travel with a saved preset and a restored session (§9, §10).
- With context on, the record count in the status bar says how much of what is shown is context rather than match.

## 7. Highlighting

Highlighting colors matching records without removing anything, for spotting events in context.

- **A log arrives with three rules already in it: FATAL, ERROR and WARN.** They are what a reader opens a log to find, and without them a FATAL renders exactly like a TRACE. FATAL is the loudest — a vivid red fill; ERROR is a deep red one; WARN is a quiet amber tint. Each sets a background and the text colour that reads on it, so all three are legible on either theme, and each does nothing but colour: none of them marks a tab or raises a notification, because loftail is not going to decide on your behalf, before you have opened the pane, that every ERROR in every log is worth interrupting you for. Nothing below WARN is coloured — colouring the records you were skipping spends attention rather than saving it.
- **They are a starting point, not furniture.** They are ordinary rules: recolour them, retarget them, add actions to them, reorder them, delete them one at a time or all at once. A log whose rules you have changed keeps your version, and a log whose rules you have deleted stays empty across quitting and relaunching — **a deleted rule does not come back**. What is seeded is the rule list of a log nothing has been remembered about yet, and rules are remembered for exactly as long as they always have been (§10): they belong to the open file, so closing a tab and opening the log again starts from the three, in the same way it starts over from rules you added yourself.
- **A highlight rule matches on the same criteria a filter does** (§6): subsystem, thread, priority, time range, and message text. Anything worth hiding is worth being able to color instead — so a rule can pick out every record whose message matches a regex, or every record from one thread inside a time window, without removing anything from the view. Combination is the same as for filters: OR within an axis, AND across them.
- Priority is matched as a **minimum level** (`≥`), the same way the priority filter works, so a rule for "ERROR and above" also colors FATAL; order the list high-severity-first for a per-level look.
- **A record that lacks a field is never matched by a rule on that field** — the deliberate mirror image of §6's promise for filters. An unparsed plain-text line has no subsystem, thread, priority or timestamp: filtering must not *hide* it, and highlighting must not *color* it, or a subsystem rule would paint every such line. The message-text axis has no such case, since an unparsed line's whole text is its message.
- Each axis is **opt-in**, and a rule with no axis set colors nothing. (This is the one place the two panes differ by design: the Filters pane ships its subsystem and priority axes enabled, because there they select everything and so hide nothing, while an all-inclusive highlight rule would color everything.)
- Rules are an **ordered list**; when several match a record, the first match wins. Order is user-adjustable.
- **The rule list is a table, and what a rule DOES is set in the rule's own row.** A rule is two questions — what it matches, and what it does about it — and the second one is a handful of small answers that belong beside the rule they answer for: a row carries the rule's on/off tick, one line saying what it matches, its pair of colour pickers, and a button each for the digest, a notification and the tab marker. So every rule's effects are on screen at once, comparable down the list, and settable without selecting that rule first. What is left below the table is the **condition** — the five axes — and nothing captions it: there is no second half to tell it apart from, and the row highlighted above already says which rule is being edited.
- **The table has no header row, and everything in it says what it is where it is.** A header naming five columns with small pictures is a legend, and a legend is worth its space only while the things below it cannot speak for themselves. Each action is a **button wearing its own icon** — a list, a bell, a bookmark — so what a column means is repeated in every row rather than stated once above them. **An action that is switched on is drawn solid, and one that is off is a faint outline**: a button that is merely pressed in is a shade of grey a shade away from the one beside it, which at this size answers "is this rule digesting?" only for someone willing to compare two cells — and in the row being edited, which is tinted as a whole, not even then. Hovering anything names it in full, and so does a screen reader.
- **A colour swatch shows the rule, not the colour** — every entry in either picker is drawn as a letter in the rule's text colour on a tile of its background, so what an entry shows is what a matching record will look like *if that entry is chosen*. The text picker runs the letter through the palette over the background the rule already has; the background picker runs the tile through it under the text colour it already has. Choosing in one repaints the other's whole list. This is the question these two controls exist to answer — dark red on dark blue is a choice nobody makes deliberately, and it used to take an *Apply* and a look at the log to find out. The one entry not previewed is *default*, drawn as a struck-through empty tile: what it has to say is that it names no colour at all, and previewed it would be indistinguishable from Ink or Paper. Since both pickers then show the same pair when closed, the **text** picker's letter carries a bar under it in the letter's own colour — the usual mark for a text-colour control, and the only one that survives at this size.
- **The colour pickers share one cell, text before background.** They are one answer — how a matching record is drawn — and a column each spent that answer's width twice over in a list that also has to hold a line of prose.
- **An axis that is switched off still shows its controls**, greyed, and **an axis this log's format cannot fill is left out altogether** — both exactly as in the Filters pane (§6), for the reasons given there. The two panes show the same five axes and do not differ on this; the Filters pane kept an unfillable axis and explained it in its title until it stopped.
- Rules can be enabled and disabled individually, like filters.
- **A rule's effect is a set of actions, and colouring is one of them.** Four ship — colour the record, list it in the digest strip, raise a desktop notification, mark the tab — each set independently in the rule's row, so a rule may match without colouring. Three are buttons that stay pressed. The fourth is the pair of colour pickers itself: **a rule colours when it names a colour**, on either role, and a rule left at *default* on both does not colour, which is one click from either picker and needs no separate switch to fall out of step with them. **Text colour comes before background**, in the order a record is read. A rule with **no** colour and **no** action pressed still matches and does nothing, which is how a rule is parked rather than deleted. Rules saved before actions existed colour, exactly as they always did.
- **First match wins per action.** A digest-only rule placed above a colouring rule does not stop the record being coloured: each action is decided by the first enabled rule that carries *that* action. So adding a rule for one effect can never silently switch off another rule's.
- **The digest strip** sits under the record table and above the Find bar — inside the view, not a pane, since panes attach left or right only (§8, §5a). It shows one row per digest-enabled rule that has a match: the **last** record that rule matched, rendered exactly as it is in the log — same columns, aligned with the table above, and **in the colours of the rule that put it there, whether or not that rule also colours the log**. That is what says which rule a row belongs to. It answers "what is the newest of each thing I care about" while the view is somewhere else, so:
  - it is **captioned *Digest*** — a second table under the first, holding rows that also appear in the log above it, explains nothing on its own and reads as a rendering fault until it is named;
  - its rows are **in timestamp order**, not in the order the file happens to hold them. It is the one place in loftail where records from different points in the log are stacked, and it is read as *what happened, and when*; a writer whose threads reach the appender out of the order they stamped their records must not make it read as though the later thing happened first. A record with no timestamp cannot be placed in time and keeps its position rather than being pushed to either end;
  - it is **sized to fit its rows and is not scrolled** — three rules is three lines, no rule is no strip at all — up to a limit of about a third of the view, past which it does scroll rather than grow;
  - it **ignores the filters**, because it is a question about the file and not about the current view — but it never reaches outside the selected run (§3a);
  - it **looks back over recent history rather than the whole file**, so a rule whose last match is far behind the tail shows nothing until it matches again;
  - it is for **reading and copying**, not for navigating: a row may name a record the filtered view is not showing.
- **The tab marker.** A rule can mark its tab when a matching record arrives while that log is not the one on screen, or while loftail is not the window in front. The mark clears when that tab is brought forward with the window in front.
- **The notification.** The same trigger, out to the desktop's own notification service. It is **rate-limited by construction**: at most one per log every ten seconds, and matches suppressed in between are reported together as a count rather than dropped — so a rule matching ten thousand records in one go is one notification saying so. Clicking it raises that log. Where the desktop offers no notification service — and some do not — the pane says so and the action falls back to marking the tab. loftail shows a tray icon only while some rule is asking for notifications, and removes it when none is.
- **Each rule is listed in the colors it paints**, so the list previews the rules rather than describing them: the cell saying what a rule matches is drawn with that rule's own background and text color. A rule left at *default* on either role simply shows the normal appearance for it. Only that cell is painted — a tick drawn on a deep fill stops being a tick, and a colour picker sitting on the colour it is offering says nothing at all.
- ***New* starts from the selected rule.** A second rule is nearly always a variant of the one in front of you — the same axes with one value changed — so New adds a copy of the selected rule, criteria and colors alike, ready to be adjusted; the copy arrives enabled even if the rule it came from was switched off. With nothing selected it adds an empty rule, every axis off and so coloring nothing, in a color no other rule is using.
- ***Clear* removes every rule**, which was otherwise *Remove* pressed once per rule; and because the pane is one of four tabbed together, the pane's own tab is marked while it holds any rules, so highlighting is never in force with no sign of it (§8).
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

Filters, highlighters, and runs (§3) are each presented in a side pane — and presets (§9) where the build has them — so they are visible and toggleable without opening dialogs.

- Panes can be shown/hidden, resized, moved to either side, or tabbed together. They arrange themselves *around* the log area and never inside it (§5a): a pane cannot be tabbed next to a log, and dragging one can never displace the file being read. A closed pane is brought back from the View menu.
- **Dragging a pane moves that pane**, not the group it is tabbed with, and a pane can be dropped on the left or right side only — not as a strip above or below the log.
- **Panes can also be floated as separate windows, where the platform supports it.** Under Wayland they stay docked: the compositor does not let an application follow the pointer outside its window or place a window under the cursor, so a torn-off pane could not be dragged or positioned — it would strand mid-drag rather than float. The same machine under XWayland does allow it.
- **There is one of each pane, and it follows the active view.** With several logs open, the panes always show and edit whichever one is being read; moving to another log rebinds them to that log's filters and highlighters, and moving between two views of the *same* log changes nothing, because those views share them.
- Pane layout is part of the remembered session (§10).
- Enabling and disabling an individual filter or highlighter is a single click within its pane — no dialog.

## 9. Presets

**Presets are a build option and are OFF by default.** A build configured with `-DLOFTAIL_WITH_PRESETS=ON` has the pane described below; a default build has no Presets pane and never reads or writes preset files. Everything else in this section describes a build that has them.

- **Filter presets** store a complete set of filters; **highlighter presets** store a complete set of highlight rules. The two are independent and separately recallable.
- Presets are created from the current state, and can be renamed and deleted.
- Applying a preset replaces the current set on that axis, rather than merging — which would make the result hard to predict.
- Presets are listed in a side pane and applied in one click.
- Presets persist across sessions and are independent of any particular log file.
- Presets can be exported to and imported from a JSON file, for sharing with colleagues.
- Moving between a build that has presets and one that does not costs nothing either way. The preset files are left where they are, and a remembered pane arrangement (§10) still restores: the pane is simply absent, and it returns alongside the others when a presets build next reads that arrangement.

## 10. Session persistence

On relaunch, loftail restores:

- Every file that was open — each reopened at its end and following, like any open (§3), so follow state is never a remembered choice
- Which run of each file was being viewed (§3)
- Active filters and highlighters per file, including which were enabled
- Every view: how many views each file had, each one's column layout and wrap mode, and which view was active
- Saved presets, where the build has them (§9)
- Window geometry, the order of the tabs, and the arrangement of the side panes (§5a, §8)

**Scoping:** active filters, active highlighters and the run selection are remembered **per file**, so returning to a given log restores how you were reading *that* log. Column layout and wrap mode are remembered **per view**, so a second view showing wide messages and one showing just timestamps each keep their shape. Presets and the window/pane layout are global.

The log format, the encoding, the source time zone, the timestamp display mode and the run-start pattern are **not** part of the session at all: they are settings (§4), resolved from the file's own entry, its file pattern or the defaults every time it is opened. That is what makes a change in Preferences reach a tab that was restored from a previous session rather than only a freshly opened one.

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
