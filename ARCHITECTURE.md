# loftail — Architecture & Technical Decisions

**Status:** Draft, 2026-07-20.
**Scope:** Internal decisions and their rationale. Nothing here is user-visible; product behavior lives in `SPEC.md`.

---

## 1. Stack

| Decision   | Choice                     | Rationale                                                                   |
| ---------- | -------------------------- | --------------------------------------------------------------------------- |
| Language   | C++20                      | Matches the log4cplus ecosystem; needed for the performance profile in §7.  |
| Toolkit    | Qt 6 Widgets               | See §2.                                                                     |
| Minimum Qt | 6.4 (dev machine has 6.10) | The version in Ubuntu 24.04's repos (6.4.2); nothing needed requires newer. |
| Build      | CMake ≥ 3.21 + Ninja       | `qt_add_executable`/`qt_add_test`; Ninja for build speed.                   |
| Tests      | Qt Test                    | In-tree, no extra dependency, integrates with CTest.                        |

**Reference build environment: Ubuntu 24.04 LTS.** The project must build with the toolchain 24.04 ships — GCC 13, CMake 3.28, Ninja, and Qt 6.4.2 from the system repos — using no separately-installed Qt. This is why the minimum Qt is 6.4 rather than the 6.5 LTS: pinning to 6.5 would force a non-apt Qt on the reference distro for no functional gain. Nothing in the design uses a 6.5+ API; if that ever changes, this constraint must be revisited, not silently broken.

**loftail does not link log4cplus.** It parses log files as text. There is no compile-time or runtime dependency on the producing library, which keeps the build trivial on all three platforms. (If loftail ever wants its own diagnostic logging, use `QLoggingCategory`, not log4cplus.)

## 2. Qt Widgets over QML

Both can render a virtualized table over a lazy model; `QTableView` and Qt Quick `TableView` are comparable there. The decision rests on three things Widgets provides that QML would require building:

1. **Cross-row text selection and copy.** `QTableView` gives rubber-band and shift-click range selection natively. QML has no cross-delegate text selection — it would mean hand-rolling hit-testing, selection anchors, and clipboard serialization. For a log viewer, selecting a span of records and copying them is a primary interaction, not a nicety.
2. **Dockable panes.** `SPEC.md` §8 describes `QDockWidget` almost exactly: show/hide, move, float, tab — and a `QMainWindow` central widget keeps them out of the document area for free (§12.2). QML has no docking framework; `SplitView` gives fixed panes only.
3. **Layout persistence.** `QMainWindow::saveState()`/`restoreState()` covers most of `SPEC.md` §10 in a few lines. QML would need bespoke serialization of every pane property.

Secondary: Widgets renders cells via `QStyledItemDelegate::paint()` without instantiating a QObject per visible cell, which is lighter for a dense fast-scrolling table; and Widgets picks up native platform styling on all three targets.

QML would win for animation-heavy or touch/embedded targets. Neither applies. **Do not revisit this without a concrete reason** — the cost is concentrated in items 1 and 2, which are easy to underestimate.

**Caveat added 2026-07-20:** the full-height multi-line requirement means the record table is a custom `QAbstractScrollArea` rather than `QTableView` (§7.1), so argument 1 above is weakened — some selection behavior is hand-rolled either way. The decision still holds: arguments 2 and 3 are untouched, `QItemSelectionModel` and the delegate/clipboard machinery remain reusable, and a custom scroll area in Widgets is substantially less work than the QML equivalent.

## 3. Pattern compilation

The `ConversionPattern` is treated as source code, compiled at runtime to a regex plus a field map:

```
"%d{%Y-%m-%d %H:%M:%S} [%t] %-5p %c - %m%n"
              ↓ PatternCompiler
recordRe:  ^(?<ts>…) \[(?<thread>[^\]]*)\] (?<prio>\S+)\s+(?<logger>\S+) - (?<msg>.*)$
fields:    [ts, thread, prio, logger, msg] + role indices
```

```cpp
struct LogFormat {
    QRegularExpression recordRe;        // full record
    QRegularExpression recordStartRe;   // prefix up to the message field — see §4
    QVector<Field>     fields;          // ordered; drives column headers
    int prioGroup   = -1;               // -1 when the pattern omits the field
    int loggerGroup = -1;
    int threadGroup = -1;
    int dateGroup   = -1;
    int msgGroup    = -1;
    DateFormat impliedDateFormat;       // how to parse the %d text
    Qt::TimeSpec impliedZone;           // local or UTC, per the date specifier — §5.1
};

class PatternCompiler {
public:
    static Expected<LogFormat, CompileError> compile(QStringView pattern);
};
```

Must handle the modifiers that appear in real configs: left/right padding (`%-5p`), truncation (`%.30c`), combined width and precision (`%20.30m`), literal `%%`, and `%d{...}` with a strftime-style inner format translated to its own sub-regex.

**The specifier set is closed and matches log4cplus's `PatternLayout`** (`include/log4cplus/layout.h`): an unrecognized specifier is a `CompileError`, not a silently ignored column, so a pattern from a different logging library fails loudly. Two consequences of covering the whole set:

- **Not every specifier earns a `Record` field.** Only date, priority, logger, thread, and message are stored at index time; `%F %L %M %l %T %i %h %H %r %x %X %E %b` compile to ordinary capture groups and are re-extracted from the record's first line inside `data()` when their column is painted. This keeps `Record` at 32 bytes (invariant #1) — a pattern with ten specifiers costs no more index memory than one with five.
- **The context specifiers need a different sub-regex.** `%x` (NDC), `%X` (MDC), and `%E` (environment variable) carry arbitrary application text: it may contain spaces, and it is *empty* whenever no context was pushed, which collapses `[%x]` to `[]` in the output. `\S+` — correct for every other field — matches neither case, so these three compile to a lazy `.*?` bounded by the literal that follows them. The trade is ambiguity when two free-text fields sit adjacent with no separator; the lazy quantifier makes that degrade to the shortest match instead of swallowing the following fields.

Specifier arguments (`%c{2}`, `%X{key}`, `%E{VAR}`) are consumed even where they change nothing about the regex — `%c{2}` still matches one whitespace-free token — because leaving the braces in the literal stream would compile `{2}` into text the log line does not contain. `%X{key}` and `%E{VAR}` name their column after the key.

**`PatternCompiler` is pure:** string in, `LogFormat` or a structured error out. No I/O, no Qt GUI types. It is the most testable unit in the project and should be built and covered first.

**Key indirection:** no component downstream of the parser ever sees the pattern string. Views, filters, and highlighters consume only `LogFormat::fields` and the role indices. This is what makes autodetection (§9) a drop-in.

## 4. Records vs. lines

log4cplus messages may contain embedded newlines (its own documented examples do), so a record can span several physical lines.

**Indexing rule:** a line matching `recordStartRe` opens a record; subsequent non-matching lines are continuations appended to it.

`recordStartRe` is derived by compiling the pattern only up to the message field — the prefix is what reliably identifies a record boundary, since the message itself is unconstrained.

Consequences that must hold everywhere: row count ≠ line count; a record's byte range covers its continuations; filtering operates on records, so a filtered-out record hides its continuation lines too. Code assuming one line per row is incorrect.

Lines that match nothing and precede any record start (or appear when the pattern is wrong) are retained as `Unparsed` records so the view can display them as plain text — `SPEC.md` §4 requires that a bad pattern never yields an empty window.

## 5. Index

```cpp
struct Record {
    qint64  offset;    // byte offset into the source
    qint64  timestamp; // epoch ms; INT64_MIN when the pattern has no date field
    quint32 length;    // spans continuation lines
    quint32 loggerId;  // interned
    quint32 threadId;  // interned
    quint16 lineCount; // physical lines; drives row height (§7.1). Clamped to 65535.
    quint8  priority;  // Priority enum, declared in severity order (§7.2); Unknown < Trace, for unparsed
};                     // 32 bytes, exactly — no padding waste
```

Everything here is collected during the indexing scan, which is already reading every byte.

- `lineCount` costs nothing to count.
- `timestamp` is parsed eagerly because time-range filtering and jump-to-time need it, and adding it later would mean reindexing every file. The compiled `%d` sub-format tells the parser exactly how to read it. **It is always stored as UTC epoch milliseconds** — see §5.1.
- `threadId` is interned exactly like `loggerId`, giving thread filtering the same integer-compare fast path and the same free discovery of the value set.

**Budget:** ~32 MB of index per million records, up from 24. Worth the two filter axes it buys, but note it before anyone opens a 10 GB log.

### 5.1 Time zones

`SPEC.md` §4 makes both the source time zone and the timestamp display user-configurable. One rule keeps that from leaking everywhere:

> **`Record::timestamp` is always UTC epoch milliseconds.** Zone conversion happens exactly twice — once on the way in, once on the way out.

- **In:** the indexer applies the *source* zone (inferred from the pattern's date specifier, or explicitly set) to convert parsed wall-clock text to UTC.
- **Out:** the view applies the *display* zone when formatting a timestamp for a cell, and the filter UI applies it when interpreting typed range bounds. **The display zone is derived, not stored:** `Document` holds a `TimeDisplay` mode (`SPEC.md` §4) and `displayZone()` returns local, UTC, or the source zone accordingly. It is *cached* rather than computed per call, because `LogModel::cellText` reads it once per painted Date cell and `QTimeZone::systemTimeZone()` is not free.

The two *seconds* modes convert nothing at all: `Record::timestamp` is already UTC epoch ms, so seconds are a subtraction and a divide. The out-conversion is therefore **conditional**, not universal — which does not weaken the invariant, because it only ever removes a conversion, never adds one somewhere else.

Comparison, sorting, filtering, and any future multi-file merge therefore operate on a single monotonic integer scale with no zone awareness whatsoever. Storing local wall-clock time instead would make every comparison zone-dependent and would break outright across a DST transition, where the same local timestamp occurs twice.

**Changing the source zone requires reparsing timestamps** — but only timestamps, not a full reindex: record boundaries and byte offsets are unaffected, so it is a pass over the existing index. Changing the display mode is free, a repaint: `MainWindow::applySettings` can never route a `TimeDisplay` change to a rescan or a reparse.

**`RunSeconds` makes a timestamp cell depend on the run partition** — the first time a cell's rendering has depended on anything but its own `Record` plus a zone. The baseline is memoised per run behind `Document::runBaseTimestamp()` (O(log runs) plus a one-entry hint, since `LogView` paints contiguous row ranges), with a resume cursor so a run whose leading records are unparsed is walked once in total rather than once per cell. The memo is invalidated by `detectRuns()`, `reparseTimestamps()`, `rescan()` and `prepare()` — but deliberately **not** by `updateRunsAfterAppend()`, because appending only grows the run list and cannot un-resolve a baseline already found. Everything that repartitions the runs already ends in a model reset or a full repaint, so no `dataChanged` plumbing is needed.

A run's baseline falls *forward* to the first timestamped record inside it rather than using the stored `Run::startTimestamp`, which is `kNoTimestamp` whenever the marker record's own date failed to parse. Note the marker must still *start* a record to be visible to run detection at all: a wholly non-matching line is a continuation of the record above it (invariant #2), so the reachable case is a structurally well-formed line whose date is not a real one.

`PatternCompiler` reports which zone the date specifier implies, since log4cplus distinguishes local-time and UTC date specifiers: **`%d` is UTC and `%D` is local time**, per `log4cplus/layout.h`. The mapping reads backwards — the lowercase, more common specifier is the *UTC* one — and loftail had it inverted until 2026-07-28, silently shifting every `%d`/`%D` timestamp by the local UTC offset. `tst_patterncompiler`'s `impliedZone` rows pin it. That is what the *Infer from pattern* default consumes. Treat the inference as a hint that the user can override, not ground truth — the pattern reveals the specifier, not how the producing application was actually configured.

Parse eagerly **only** what filtering needs — priority and logger. Everything else (timestamp, thread, message text) is parsed lazily in `QAbstractTableModel::data()` from the mapped bytes. Storing parsed strings per record is the single most likely way to make this application unusable on large files.

**Interning:** logger names map to `quint32` via a `QHash<QString, quint32>` built during indexing. Filter predicates then compare integers rather than strings, which matters at millions of records. The intern table is also the authoritative subsystem list for the filter pane (`SPEC.md` §6) — discovery is a side effect of indexing, not a separate pass.

**Budget:** ~24 MB of index per million records. Note this before anyone opens a 10 GB log; if it becomes a problem the fallback is a chunked/paged index, but do not build that speculatively.

## 6. File access

```cpp
class LogSource {                       // the model cannot tell which impl it has
    virtual QByteArrayView bytes(qint64 offset, quint32 length) = 0;
    virtual qint64 size() const = 0;
    virtual bool   isRandomAccess() const = 0;  // false for gz/remote (§6.2)
};
```

**Every local file is opened append-aware.** `SPEC.md` §3 removes the post-mortem/live distinction: loftail cannot know whether a file is finished, so it treats all of them as potentially-growing. **No `LogSource` may assume the file is immutable**, and none may hold the file in a way that blocks the writing process from appending, rotating, or truncating it — observing a log must not disturb the process producing it.

Two **local** implementations, selected by **platform**, not by mode (a third, for remote logs, is §6.3):

- **`MappedLogSource`** (POSIX) — mmap of the currently-indexed extent, re-mapped as the file grows. Safe under rotation on POSIX: `rename`/`unlink` leave an existing mapping intact (it holds the inode), and copytruncate is caught by the size-shrink check below. No copying, fast random access on the paint path.
- **`BufferedLogSource`** (Windows, and the fallback everywhere) — incremental buffered reads. Preferred on Windows because a held file mapping can block the writer from rotating or truncating the file — exactly what a logging framework does — and under the always-watched model that risk would otherwise apply to *every* open file, not just ones a user chose to tail. Opened with full sharing (`FILE_SHARE_READ | WRITE | DELETE`) so loftail never locks the writer out.

The `bytes()`/`size()` interface hides which one is in use; the model and indexer are identical across both.

**Rotation/truncation detection is always active** (it is not gated on a tailing toggle any more): poll size and file identity (inode on POSIX, file index on Windows). If size shrinks or identity changes, the file was rotated — discard the index and rescan. The "was it replaced" half of that question lives behind `LogSource::wasReplaced()`, because *what has to be re-resolved* differs per source: a mapped fd follows the inode it mapped and sees nothing, so it re-stats its path, while a spooled remote source compares generations (§6.3). The controller asks the source and does not care which. `QFileSystemWatcher` is the primary change signal but is unreliable on some filesystems (notably network mounts), so pair it with a low-frequency size poll rather than trusting it alone. On POSIX, guard mmap reads against a concurrent copytruncate so a read past the new EOF cannot `SIGBUS`.

Files are opened in binary mode; CRLF is handled explicitly rather than via platform text-mode translation, so a Windows-authored log reads identically on Linux.

### 6.1 Encoding

`SPEC.md` §4 makes encoding an explicit per-file setting whose default is auto-detect. **The stored setting is the user's choice, including the sentinel `Auto`** — not the encoding that detection resolved to. Persisting the resolved value would silently freeze a guess made from one version of a file and apply it to a later one that may differ.

Detection runs on open, over the first ~64 KB:

1. **BOM** — decisive when present: UTF-8, UTF-16LE, UTF-16BE.
2. **No BOM** — a high frequency of NUL bytes at alternating positions indicates UTF-16, and which position identifies the byte order. Otherwise, validate as UTF-8; on failure fall back to the system 8-bit codepage.
3. The resolved encoding is surfaced in the Log Format dialog so a wrong guess is visible rather than silent.

Forcing an encoding skips detection entirely. Detection is a heuristic — unreliable on short files, on files whose leading records are pure ASCII, and on legacy 8-bit logs — so the explicit choice is a first-class path, not a fallback.

Changing the encoding invalidates the index and requires a full rescan, since line boundaries themselves depend on it.

The consequence that reaches furthest: **the record scanner cannot search for `\n` bytes.** In UTF-16 a newline is `0A 00` or `00 0A`, and a naive byte search both misses real terminators and fires inside unrelated code units. A `Decoder` sits between `LogSource` and the indexer, exposing line boundaries and decoded text; the indexer works in its terms and never touches raw bytes directly.

`Record::offset` and `length` remain **byte** offsets in all encodings — they index the source, not the decoded text. Only the decoder converts.

### 6.2 Designing for future sources

`FUTURE.md` plans `.gz` and SSH-retrieved logs, and both violate an assumption that is otherwise easy to bake in: **that a log source is local and randomly seekable.** Neither is. Gzip has no random access without an index; a remote source adds latency to every read.

The `isRandomAccess()` flag exists so the indexer can branch now rather than being restructured later. Concretely, the constraint to honor today: **the indexer must be able to work as a forward, single-pass stream.** It already does — it scans start to finish. Do not add a second backward pass or a "seek to offset X and re-read" step. Random access is a legitimate optimization for `data()` on the paint path, which is only reachable for records already indexed and, for non-seekable sources, will be served from a local cache.

**How that turned out (M11).** Half of this prediction was wrong, and the record is worth keeping straight rather than quietly amended. `isRandomAccess()` was expected to go *false* for SSH; it did not, and the flag remains unread by any branch in the codebase. The reason is the last clause above: once a non-seekable source is "served from a local cache", the cache is an ordinary local file and the source is randomly seekable again — the latency the flag was meant to warn about is absorbed by the cache, not exposed through the interface. What did pay off, decisively, is the **forward-single-pass constraint**. Because the indexer never seeks backwards, the spool can be filled and indexed *concurrently*, which is the whole difference between following a remote log and downloading one. Had the indexer needed a second pass, `SpooledLogSource` would have had to block until the fetch completed, and remote logs would not be live. Keep the constraint; treat `isRandomAccess()` as documentation of intent rather than as a working seam.

**And again for `.gz` (M12).** The other half of the prediction was wrong in exactly the same way, which is what makes it worth recording twice rather than once. Gzip really has no random access — and it did not matter, for the identical reason: the expansion lands in a spool, the spool is an ordinary local file, and `isRandomAccess()` stayed true. The forward-single-pass constraint paid a second time, and more visibly than the first: an archived log **fills in as it expands** rather than freezing until it is done, because the spool is being filled and indexed at once. Two features, one constraint, and the flag that was designed for the job is still unread. The lesson to carry forward is that the *cache* was the accommodation, not the flag.

### 6.3 The spool, and remote sources (SSH)

The spool machinery below is **shared by every source that is not already a readable local file** — SSH since M11, archives since M12 (§6.4). It is named for that rather than for the transport: `SourceFetcher`, `SourceSpool`, `SourceSpoolRegistry`. SSH is a *transport* and an archive is a *file type*; the two compose, so a seam serving both must not be named for one of them.

A log is fetched forward into a **local spool file** and read back through an ordinary local source. `SpooledLogSource::bytes()`/`size()` delegate to `openLogSource(spoolPath)` — the same mmap-on-POSIX / buffered-on-Windows source everything else uses. Nothing on the read or paint path knows where the bytes came from, and `isRandomAccess()` stays true (§6.2 explains why that is not the failure it looks like).

**The spool is shared per log and reference-counted**, not owned by a Document. `SourceSpoolRegistry` hands out `shared_ptr<SourceSpool>` keyed by a plain normalized path **string** — the registry holds spools for several kinds of source and understands none of them; only `defaultFetcher()`, which builds the fetcher, has to read the key. The last handle dropping tears down the fetcher, the connection, and the spool files. This is the load-bearing decision, and it is what makes three separate problems not arise:

- `Document::rescan()` runs on the GUI thread from the live watch tick. It reopens with `OpenPolicy::Reuse`, finds the live spool, and returns — a rotation mid-tail is a pointer swap, never a reconnect that could block the UI or prompt for a password behind the user's back.
- Two tabs on one remote file share one connection and one spool.
- Changing a remote file's log format reopens the Document and costs no network.

**Three threads, and the contract between them is the design:**

| Thread | Owns |
| --- | --- |
| GUI | `Document`, `LiveController`, the `SpooledLogSource`, and the inner local source over the spool |
| Index worker (existing) | `Indexer`, reading that same inner source via `bytes()` |
| Fetcher (one per spool) | `SourceFetcher`, the `LIBSSH2_SESSION` or the decompressor, and every write to the spool |

Two rules keep them apart, and there is no mutex between the GUI and the fetcher at all:

1. **`refreshSize()` is the only method that reopens or re-maps the inner source, and exactly one thread ever calls it on a given instance.** This already held for local files — `Indexer::index()` snapshots `size()` once and `LiveController` is constructed only after the worker joins — but it *matters* now, because the fetcher is appending during the initial scan. The rule is stated **per instance**, not as "the GUI thread": for a Document's source that thread is the GUI thread, but a fetcher may hold a private source of its own and drive it from its own thread, which is what §6.4 does and why the distinction is not pedantry.
2. **The fetcher publishes `committedSize` only after its write has landed, and readers clamp to it.** `refreshSize()` returns `min(spoolFileSize, committedSize)`. A half-written chunk is therefore not merely unlikely to be observed, it is unobservable. This ordering is the entire synchronisation.

**Rotation makes a new spool *generation*, never a rewrite.** The index worker may be mmapping the current spool and every `Record::offset` indexes it, so on a rotation or truncation the fetcher opens spool file *N+1* and bumps an atomic generation, publishing it last once the new file exists. `identity()` returns the generation and `wasReplaced()` compares it, so `LiveController`'s existing rotation path works unchanged. The old spool is unlinked when its last reader lets go.

**Detecting rotation without an inode.** SFTP attributes carry size and mtime but no inode. The primary check is `fstat(handle)` against `stat(path)`: OpenSSH's `sftp-server` implements FSTAT against the real descriptor, so an open handle follows the rotated-away file exactly as a POSIX mmap does locally, and a disagreement between the two means the *name* now points somewhere else — including the same-size rotate that a size check misses. Whether a given server behaves that way is probed once at connect; one that re-resolves by name instead falls back to comparing the head of the file, and only on suspicion (mtime advanced without the size growing), never on the ordinary poll.

**Reconnecting (M13).** The tail loop above polls but never re-established a session: `connectTo()` was only ever called from `start()`, so a dropped link stayed dropped until the log was reopened. It now reconnects from the worker, non-interactively, and a `start()` that merely could not reach the host succeeds into `State::Waiting` instead of failing the open. See §6.5.1.

**The socket is taken away from Qt, and it has to be** (`SocketDetach.h`). `QTcpSocket` resolves the name, applies the connect timeout and phrases the connection error — and then its descriptor is duplicated and Qt's own copy closed, so libssh2 is the only reader. Skipping that step does not degrade the transport, it breaks it: a `QTcpSocket` keeps a read notifier armed, so any turn of a Qt event loop drains the socket into a `QByteArray` libssh2 cannot see, and libssh2 waits out its timeout and reports "Timed out waiting on socket". Two ordinary event loops did it — the modal password prompt, which runs one in the middle of authentication (hence: agent and key logins fine, password logins failing the instant the password was accepted), and the main window's own, which runs for the whole life of a tail while the session belongs to a fetcher thread. The keychain's nested `QEventLoop` (§6.3.2) is a third such loop in the same window, and it is safe for the reason already given rather than a new one — the descriptor is gone from Qt before the handshake, let alone before `authenticate()`. It carries `QEventLoop::ExcludeUserInputEvents` to stand in for the modality a bare loop lacks, which is the only thing the dialog had that it does not. `tst_socketdetach` pins the Qt behaviour on a loopback pair, ungated, because the property is Qt's rather than libssh2's and a Qt that stopped doing it would make the workaround unnecessary.

**Connecting blocks the calling thread, deliberately.** The connect, host-key check and authentication happen on the thread that opened the document, and only the tail loop is handed to the fetcher thread — a handoff, so exactly one thread ever touches a `LIBSSH2_SESSION`. The alternative considered was connecting on a worker and spinning a nested `QEventLoop` in `Document::prepare()` so a progress dialog could offer Cancel. It was rejected: `prepare()` is reached from `openWithSettings()`, which already runs a modal dialog and can abort an open, and adding re-entrancy there is a poor trade for a progress bar. The cost is a frozen UI for the length of a connect, bounded by the SSH timeout. A "Connecting…" dialog with Cancel is a genuine follow-up, and it needs the connect moved off-thread to be worth having.

**One prompt per host** comes from a per-target credential cache, not from a shared connection. Sharing one `LIBSSH2_SESSION` across the per-file fetcher threads would need a mutex around every read and would serialize them; caching the accepted password per `user@host:port` gets the user-visible property with none of that. The cost is one TCP connection per open file, which is what `scp` does anyway.

**The full ladder `authenticate()` climbs**, in order: the agent and the `~/.ssh` default keys; the per-target credential cache above; the OS keychain (§6.3.2); and only then the prompt, up to three times. A **saved host's** remembered plain-text password enters at the *cache* rung rather than as a rung of its own — the UI primes the cache before the open (`MainWindow::primeRemoteCredentials`) — which is what keeps the agent ahead of it: a host that would have signed in with a key never has a stored password sent for it. Note what this costs: sshd counts failures against `MaxAuthTries` (6 by default), and the keychain rung moves the worst case one attempt closer to that ceiling, which is why a stored password the server rejects is *erased* rather than left to fail again on every future connect.

**The transport sits behind `SourceFetcher`**, and that seam is what makes the feature testable: `tst_spooledsource`, `tst_remotetail` and `tst_remoteopen` drive the whole application — opening, indexing, tailing, rotation, session restore — against a fake fetcher, with no network and no libssh2 linked. CI exercises no libssh2 call beyond linking; the handshake, host-key and authentication paths are covered by `tst_sshlive`, which runs by hand against a real server.

#### 6.3.1 The exec fallback, for a server that will not do SFTP

Some servers sign you in and then refuse SFTP — sshd with no `Subsystem sftp` line, or an account confined to a shell that cannot start one. Before this, that was where a remote log stopped. `SshSession` now has two modes, chosen at connect: SFTP, and failing that, `stat` and `tail` over a plain exec channel. `SshFetcher` and everything above it are unchanged, because the five operations they use (`openFile`, `statPath`, `statHandle`, `readAt`, `fstatTracksHandle`) are the same five either way.

**The fallback is chosen by probing, never by reading the error code**, and that is the correction that made it work on the machines it was written for. "No SFTP" arrives in two shapes and libssh2 reports them differently for reasons that have nothing to do with which one it is. A server that *answers* the subsystem request with a refusal gives a prompt channel failure. A server that **accepts** the channel and then has no `sftp-server` binary behind it — a generic sshd config on a stripped-down embedded image, which is the common case — gives libssh2 nothing at all to report: it waits for the SFTP version packet that is never coming and gives up with a plain `LIBSSH2_ERROR_TIMEOUT`, "Timed out waiting on socket", *after a successful login*. Classifying a timeout as "transient, retry as you are" therefore locked out exactly the servers the fallback exists for. So the code is not consulted for that decision at all: run the probe command, and if a command runs then the session is healthy and it is SFTP that is missing, whatever the error said. The code is consulted only afterwards, and only for the one question the caller actually asks — if the probe went unanswered too, a timeout means the link went quiet (retryable, `Unreachable`) and anything else means the server answered and said no (`Refused`).

Two details of that probe. It runs on a **shorter leash** when the SFTP attempt timed out — a session that has just been silent for the whole window has already cost the user that wait once, and connecting blocks the thread that opened the document, so a second full-length wait would double a freeze rather than diagnose anything. And its failure is reported in two ways, because `runCommand()` distinguishes them for free: a command that *ran* and printed nothing means the shell is fine and the two utilities are missing, which names something the user can go and install; a command that could not be started at all means the account has no shell.

**SCP was considered and is not usable**, which is worth recording because it is the obvious suggestion. The transport needs to read from an arbitrary offset, re-`stat` the path on every poll, and `fstat` an open handle to tell a same-size rotate from an append. SCP offers none of the three: it hands over a whole file from byte 0, with its size fixed at open and no handle to interrogate. An SCP-backed tail would re-download the entire log on every poll. `tail -c +N` gives genuine forward reads, which is the one property the single-forward-pass indexer actually needs (invariant #9).

The fallback is **automatic but not silent**. It is only reached once SFTP has already refused, so there is nothing to lose by trying it, and `FetchStatus::note` carries a standing "reading with shell commands — <host> does not offer SFTP" into the status bar. Three costs come with it, and none is hidden:

- **A process per read.** Every `readAt` is a channel plus a `tail | head` on the far end, where SFTP reuses one handle. Chunked at 256 KB, so a prime is round-trip-bound rather than pathological.
- **Weaker rotation detection.** There is no handle, so `statHandle()` deliberately reports *invalid* rather than quietly returning the path's attributes — which would make the two agree by construction and defeat the very check that compares them. `fstatTracksHandle()` is false, and `SshFetcher`'s existing mtime/head-compare fallback (§6.3) is what runs.
- **A shell, and therefore quoting.** The remote path reaches a command line, so `shellQuote()` is a security boundary rather than a formatting detail: a path containing `'; rm -rf ~; '` is otherwise remote code execution. Single quotes, with the one escape POSIX allows (`'\''`).

`SshExecCommands` — the quoting, the command construction and the parsing of what comes back — is **always compiled**, like `RemoteLocation` and `ArchiveLocation`, while the transport that uses it is gated. A security boundary compiled in one configuration is a security boundary tested in one configuration. `tst_sshexec` is ungated and runs the generated commands through a real `/bin/sh` against real files, with a canary file the injection cases must fail to create; comparing strings would only prove the quoting matches what the author expected, not that a shell agrees.

Parsing is defensive for the same reason the classification in §6.5 is: a wrong size here would truncate or over-read a log, so anything that is not exactly two non-negative integers reads as "no attributes". The answer is taken from the **last** non-empty line, because a login banner on stdout is common and is somebody else's noise.

#### 6.3.2 Where a remembered password lives (M14)

A password the user asks loftail to **keep** goes through one seam, `SecretStore` — abstract, in `loftail_core`, free of QtKeychain types and **always compiled**, with a global setter in the shape `SshPrompter` established. Only `KeychainSecretStore.cpp` includes QtKeychain, and only when `LOFTAIL_HAVE_KEYCHAIN` is set; `SecretStore.cpp` picks it or a `NullSecretStore` at the call site, exactly as `SourceSpool::defaultFetcher()` does for `makeSshFetcher`.

**`secretStore()` is never null, and `sshPrompter()` may be.** That is the one deliberate divergence between the two. For a prompter, null *is* a policy — "never prompt", and every question then answers itself in the safe direction. Here "no keychain" is not a policy, only which backend was compiled in or found at runtime, and it already has a first-class spelling the object gives about itself: `available() == false`. A nullable store would put a null check at every call site guarding a condition the store already states.

**`available()` is a round trip, not a link check.** QtKeychain's own `QKeychain::isAvailable()` answers "was a backend library found", which on Linux is true as soon as `libsecret-1` can be `dlopen`ed, running daemon or not — upstream says so in `keychain_unix.cpp`: *"In the future there should be a difference between 'API available' and 'keychain available'."* A headless CI runner and a KDE box with `kwalletd` not started are the same case and both pass that test. So it is used only as a cheap negative, and a real read of a probe key decides — where **`EntryNotFound` is the probe's success**, because only something that is there can say so. Two consequences worth stating rather than discovering: the answer is cached for the process, so a keyring started after loftail is not noticed until restart (the alternative is a D-Bus round trip on every connect and every dialog build); and inside the AppImage `available()` is false on a host with no `libsecret-1.so.0`, because QtKeychain reaches libsecret through `QLibrary("secret-1")` and `ldd` therefore cannot see it for `linuxdeploy` to bundle.

**The consent rule.** `rememberSshPassword()` returns `UseFileFallback` only when `available()` said no. It never returns it after `available()` said yes: a keychain that is present and refuses yields `Failed`, and the caller reports that and writes nothing. The rule exists because the checkbox names its destination *before* it can be ticked, so a silent downgrade would put a secret in a file the user was never told about, from the one dialog whose whole job is to say where the secret goes. It is an executable assertion in three places — `tst_secretstore`, `tst_hostbookmarks` and `tst_sshcredentials` — all ungated, because it is a rule about consent rather than about a dependency.

**`SshSession` does not decide where a password goes**, because it does not know what the user was shown. `SshPrompter::passwordAccepted(target, password, remember)` is called once, only after the server's yes, and the prompter routes it — the object that drew the checkbox is the only one that knows what was consented to. This is also what finally consumes the `remember` out-parameter, which the dialog had been filling in and `authenticate()` dropping since M11.

**The thread rule: a keychain is consulted only on the thread that has a prompter.** Three reasons, the first sufficient alone. (1) A keychain read can *prompt* — non-interactive under `CredRead`, but a locked KWallet raises `kwalletd`'s unlock dialog and macOS asks for an item whose ACL does not list the binary; `SshFetcher::reconnect()` already forbids exactly that, in those words. (2) The fetcher's worker runs no event loop — `Worker::run()` is `tailLoop()`, with no `exec()` — while QtKeychain's Unix path wants `QDBusConnection::sessionBus()`. (3) `reconnect()` needs none of it anyway: the password went into the credential cache when the document was opened, a rung earlier. Enforced twice — structurally, by putting the keychain rung *below* `authenticate()`'s existing "is there anybody to ask" test, and at runtime by a thread guard in `KeychainSecretStore`, so a later caller cannot quietly reintroduce a keychain read on the fetcher thread.

**Nothing in CI exercises a real keychain**, and nothing can: the runners are headless with no session bus, so no backend would answer. `tst_keychainlive` is gated twice like `tst_sshlive` — not built without the dependency, and skipped unless `LOFTAIL_TEST_KEYCHAIN` is set, because it writes to the developer's real keyring. Everything above the backend is covered with no keychain at all through `tests/FakeSecretStore.h`. A green pipeline says nothing about KWallet, GNOME Keyring, the Credential Manager or the macOS Keychain.

**The plain-text file is unchanged**, in shape, schema and containment rules, and remains the fallback where no keychain will answer — `HostBookmark::savePassword` and `password` in `hosts.json` (§8), off by default, owner-only, named in the dialog. `forgetSshPassword()` deliberately touches **only the keychain**: clearing the file from inside a network auth routine would mean `SshSession` reading `AppConfigLocation`, the hidden dependency the bookmark lookup was kept out of core to avoid. That asymmetry is the price of the placement, and a stale plain-text entry is cleared where it was set.

### 6.4 Archived sources (M12)

A compressed or archived log is a **second fetcher behind the same spool**, exactly as `FUTURE.md` predicted from the beginning. `ArchiveFetcher` decompresses one member forward into a spool file; `SpooledLogSource` reads it back. Nothing above the fetcher changed.

**The input is an ordinary `LogSource`, and that is the whole design.** `ArchiveFetcher` holds a private `std::unique_ptr<LogSource>` over the container and feeds libarchive from it through a read callback. For a local container that is a `MappedLogSource`; for one on another machine it is a `SpooledLogSource` over the SSH fetcher's own spool — and the archive fetcher cannot tell. So **two fetchers chain**: SSH downloads `app.log.1.gz` while the archive fetcher expands what has arrived. Neither knows the other exists. This is why an archive is spelled as a *nested path* with no scheme of its own (§6.4.1): an archive is a file type and SSH is a way of reaching a file, and inventing `archive+ssh://` would have been inventing a combinatorial problem that does not exist.

Two consequences that are easy to get wrong, and were:

- **A container is opened as bytes, never as a log.** `openContainerSource()` exists beside `openLogSource()` and deliberately skips the archive branch. Without it, `openLogSource("app.log.gz")` — which *means* "expand it" — is what a fetcher would call to read its own input, and it recurses into expanding itself.
- **Spool keys are namespaced by what fills them.** Because a single-stream container collapses to its own plain path (§6.4.1), the expanded log and the raw container are the *same address string*. Sharing a registry key between them is not merely ambiguous: the registry publishes its entry only after `start()` returns, so the inner lookup misses the outer spool and builds a second expansion, forever. `expandedSpoolKey()` prefixes the key, and the prefix never escapes the registry.

#### 6.4.1 Addressing

An archived log is spelled by continuing the path through the container: `/logs/bundle.tar.gz/var/log/app.log`, and `ssh://host/logs/bundle.tar.gz/app.log` for one on another machine. Like an `ssh://` URL it travels through the application as an ordinary path **string**, which is again why no session schema bump was needed.

Resolution is deterministic and costs at most one `stat`:

0. A **local path that already names a regular file is never split.** This is what keeps a real directory called `bundle.zip` working — the file that is actually there wins over the reading where the directory is an archive. It cannot apply remotely, where the answer would cost a round trip on a path that is normalized constantly.
1. Otherwise the cut falls at the **first component carrying an archive extension**, of either kind. Which table matched decides what the address *means*; it does not decide where it splits.
2. Otherwise this is an ordinary log.

Classification is **pure string work — no I/O, no content sniffing** — so it answers the same for a file that does not exist yet, which is what session restore needs. The cost is real and is stated in `SPEC.md`: a `.log` that is secretly gzip fails with a decompression error rather than being rescued by a sniff. That is the deliberate trade for determinism.

**The collapse rule.** A single-stream container (`.gz .xz .bz2 .zst`) keeps its **plain path** as the normal form and never grows a member. Without it one log would have two spellings, and therefore two tabs, two format-cache entries and two spools. A multi-member container always carries its member.

#### 6.4.2 Completion

An expansion genuinely ends, and loftail knows it does **because loftail produced the bytes**. That is the one thing invariant #5 does not forbid: the ban is on *guessing* that a file somebody else is writing has stopped.

- `FetchStatus::State::Complete` is published **after** the final `committedSize` — the same ordering rule `generation` follows, so a reader that observes Complete is guaranteed to observe the final size.
- `LogSource::isComplete()` is non-pure and false by default, arriving by exactly the route `wasReplaced()` did in M11: only a source that can prove it implements it.
- `LiveController` reads it **before** `refreshSize()` and acts on it **after** `ingestAppended()`, then stops the watcher for good. Reversing either half drops the last chunk silently — no error, just records that never appear. `tst_complete` pins it, ungated, because it is a contract of the live seam and not of libarchive.

**No user-facing mode follows.** The follow control is untouched; after completion the newest record simply stops moving, exactly as it does for a local file nobody is writing. Stopping the watch is an absence of work, not a setting.

An expansion whose container is remote **does** complete, once the container has been fetched whole. The cautious-looking alternative — never completing, because the container might grow — is not caution but a permanent wait: an SSH fetcher tails forever and never reaches a terminal state, while libarchive always reads past a gzip member looking for a concatenated one. A rewritten container is not re-expanded either way (reopening does that, and rotation is the transport's business one level down), so there is genuinely nothing left to wait for.

#### 6.4.3 Gating and testing

libarchive is optional and auto-detected, gated as libssh2 is (§1): `LOFTAIL_WITH_ARCHIVE`, a three-tier probe, and a configure-time `Archived log sources: ENABLED/DISABLED` line. **One deliberate difference from libssh2: there is no build-from-source fallback.** libarchive is only useful with its codecs, and it finds zlib and liblzma through `find_package` in MODULE mode, which `CMAKE_FIND_PACKAGE_REDIRECTS_DIR` cannot satisfy — a FetchContent-built codec is ignored, libarchive builds with no compression, and reports itself found regardless. That was tried, and it reached CI reporting `ENABLED` while every `.gz` fixture failed to *write*, with zip silently falling back to "store". A package manager supplies the codecs as a matter of course, so the Windows job uses vcpkg; a static libarchive also needs `LOFTAIL_ARCHIVE_STATIC=ON` for the `dllimport` declaration. `ArchiveLocation`, `ArchiveFetcher.h` and `ArchiveReader.h` are always compiled so both builds agree about what a settings file means; only `ArchiveReader.cpp` and `ArchiveFetcher.cpp` touch the dependency.

**Unlike M11, this really is exercised in CI.** There is no network, no credential and no disposable remote path to arrange: the fixtures are built at runtime by libarchive's own write side. `tst_archivelocation` and `tst_complete` run in every configuration; `tst_archivemembers`, `tst_archivefetcher`, `tst_archivetail` and `tst_archiveopen` run wherever the dependency is present. The M11 caveat — that a green pipeline says nothing about the transport — does not carry over to this milestone.

**Spool files** live under `QStandardPaths::CacheLocation` (never config — they can be gigabytes), in a per-process directory holding a `QLockFile`. Startup sweeps sibling directories and removes only those whose lock can be taken, since a lock that cannot be taken means a live owner — several instances may run at once (`SPEC.md` §3), so "old" would not be a safe test.

**libssh2 is optional.** §1 promises the reference build works with the stock Ubuntu toolchain and nothing separately installed, so it is detected and never required: without it, `RemoteLocation`, the spool, the source and the bookmark store still compile, remote paths are still recognised, persisted and displayed identically, and opening one reports that support is not built in. Only `SshSession.cpp` and `SshFetcher.cpp` are conditional.

### 6.5 A log that is not there (M13)

`SPEC.md` §3 makes an address that is not currently openable a **state** rather than an error: the tab opens, says it is waiting, and starts reading when the log turns up. The same state covers a log deleted while open. It is the third consequence of the same refusal that produced the always-watched model — loftail cannot know whether a log is finished, and it equally cannot know whether one is *late*.

**Waiting lives on the live seam, not in a fourth `LogSource`.** The tempting shape was an `AbsentLogSource` that reports size 0 and turns into a real source when the path appears. It was rejected because the thing that has to change is not how bytes are read — it is *whether there are any yet*, which is the live controller's existing question. A document therefore has three states, and the transitions all happen on the 750 ms tick that was already there:

| State | Source | Index |
| --- | --- | --- |
| Open | yes | the log |
| Waiting (local) | **none** | empty |
| Waiting (spooled) | **kept** | empty |

**That asymmetry is load-bearing.** A local wait releases the source: there is nothing at the path, holding an unlinked inode open pins bytes nobody will read, and invariant #5 says observing a log must not disturb the writer. A **spooled** wait keeps it, because `SpooledLogSource` owns the `shared_ptr<SourceSpool>`, the spool owns the fetcher, and the fetcher is the thing retrying — releasing the source tears down the very machinery doing the waiting, and the log never comes back. `logPathIsSpooled()` is the existing discriminator and needed no change.

**`LogSource::originVanished()`** answers "is the thing I was opened from still there", non-pure and false by default — the third arrival by the route `wasReplaced()` took in M11 and `isComplete()` in M12. Two of those three now make the same point: the non-pure-virtual seam is the right shape for "only some sources can answer this". The implementations differ exactly as the interface predicts: a mapped source re-stats its path, a buffered one asks `QFileInfo::exists` (**not** `pathIdentity()`, which is stubbed to 0 on Windows and would report every file as vanished), and a spooled one reads its fetcher's state.

It is an **observation, not the guess invariant #5 forbids**. "The file is gone" is what a stat answers; "the file is finished" is not, which is why `isComplete()` needs loftail to have produced the bytes and this does not. It is also distinct from `wasReplaced()`, and the two are checked in that order: replaced means something *else* is at the origin and rescans, vanished means *nothing* is and waits.

**The grace period is what keeps a rotation silent.** `logrotate` renames and then creates, and a check landing in that gap sees a path with nothing at it — indistinguishable, in that instant, from a deletion. So vanishing must hold for 2 s before the view is cleared. It is measured as **elapsed time, not a count of checks**: the filesystem watcher fires checks too, and a rotation produces a burst of them at exactly the moment the path is empty, so counting them would shorten the grace period precisely when it is needed.

**Resuming needs the owner, because the pattern lives there.** A document that opened waiting has never sampled a byte, so its format and encoding are unsettled — `formatSettled()` means *settled against real bytes*, which is why a spooled source that opens empty does not count. Settling them needs an `IFormatProvider`, and invariant #3 keeps the pattern string out of core. `LiveController` therefore emits `resumeRequested()` and the window calls `Document::resume(provider)`. The cost is stated in the header: a document whose owner never connects that signal waits forever, by design rather than by accident.

**No dialog on arrival.** The log turns up on a watch tick, for a tab that may not be on screen, so raising the Log Format dialog there is the "behind the user's back" case `MainWindow::openFile()` already refuses for the archive member picker. The remembered pattern is applied; if it does not fit, the log reads as plain text and the status bar points at the dialog. Nothing is persisted in that case, so reopening still offers the dialog properly.

**Waitable vs fatal** is decided in `Document::prepare()` by two questions, and both are needed. `logSourceAvailable()` is the "is it there" half; `logPathIsWellFormed()` is the "does it name a log at all" half. Without the second, `ssh://` — which parses into no host and never will — would open a tab that waits forever for something that cannot exist. Refusals (a changed host key, a rejected password, an archive naming no member, a dependency not built in) stay refusals with no tab.

#### 6.5.1 The transport half

A remote log's absence cannot be answered from a path: there is none to stat, and asking the far end costs a round trip on every tick. So the fetcher answers, through `FetchStatus::State::Waiting` — "the input is not there; still trying", distinct from `Error`, which is trouble with a source that *exists*. Both retry on the same backoff; the difference is what the user is told.

- `SshFetcher::start()` splits its failures. Unreachable or no-such-file **succeeds** into `Waiting` and spawns the worker anyway; the spool exists and is empty, which `SpooledLogSource::open()` already treated as legal. A refusal still returns false.
- `tailLoop()` gained a reconnect step — before M13 nothing ever re-established a dropped session, so a link that blipped stayed broken until the log was reopened.
- **Retries are non-interactive, without exception.** `connectTo()` is called from the worker with a **null prompter**, so it gets the agent, the usual keys, or a cached password — the common case, entirely automatic. Marshalling a prompt out of the worker would put a modal dialog on screen for a log opened hours ago. A retry that genuinely needs a person publishes that and latches `m_reconnectRefused`, and **File ▸ Reconnect** — which runs on the GUI thread and does have a prompter — is the only thing that clears it. That action also gives `SourceSpool::poke()` its first caller; it had been written for a button that did not exist yet.
- A retry out of `Waiting` does **not** announce `Connecting` on each attempt. It would flap the state several times a minute, and because `originVanished()` reads it, the document above would bounce out of waiting and straight back into it.
- A retry connects with a **shorter timeout** than the first attempt (5 s vs 20 s), and that is about *closing*, not about patience: `stop()` joins the fetcher thread and is reached from the GUI thread when the last tab on a log closes, so a connect in progress is exactly how long closing a tab on a dead host freezes the window. A retry has nothing to lose by giving up early — the next one is seconds away.

**An archive whose container is not there** is where this nearly went wrong. `ArchiveFetcher::start()` runs on the GUI thread and feeds libarchive through `awaitInput()`, which waits while the transport underneath is healthy — and a transport that is *retrying* is healthy, so `ssh://downhost/bundle.tar.gz/app.log` would have spun there forever. The opening of the member is therefore deferred to the worker when the container's source reports its origin vanished (`beginExpansion()` is called from `start()` or from the worker, never both). A *local* container never reaches the fetcher at all: `logSourceAvailable()` asks about the container, so `Document::prepare()` classifies it as waitable first.

## 7. Model, view, and filtering

### 7.1 Variable row heights — why not `QTableView`

`SPEC.md` §5 requires multi-line records to render at full height in the table. That single requirement rules out `QTableView`, and it is worth being explicit about why, because the reason is not obvious and the workaround is not viable.

`QTableView` delegates vertical geometry to `QHeaderView`. Uniform row heights give O(1) scroll math. Non-uniform heights require either `resizeRowsToContents()` — which calls the delegate's `sizeHint()` for **every row in the model**, an O(n) pass that is unusable at millions of records — or per-section sizes stored in `QHeaderView`, which materializes a per-row entry and defeats the entire lazy-index design from §5. There is no lazy per-row height API. Retrofitting this later would mean replacing the view, the scroll model, and the selection plumbing at once.

**Therefore: a custom `LogView : QAbstractScrollArea`**, painting visible rows directly.

The design that makes this cheap:

- **Scroll in line units, not pixels or records.** The vertical scrollbar's range is the total physical line count. Every line is the same height (fixed-pitch font), so line units map to pixels by a single multiply.
- **Two-level prefix sums over `Record::lineCount`.** Store a cumulative line count every 4096 records; resolving a scroll position to a record is a binary search over the block array plus a short linear scan within one block. That is O(log n) with a cache-friendly inner loop, and appending during live tail only touches the last block.
- **Overhead:** one `quint64` per 4096 records — negligible next to the 24-byte index itself.
- Painting walks forward from the first visible record until the viewport is full. Only visible records are ever touched, preserving the §5 laziness guarantee.

Reused rather than rebuilt: `QItemSelectionModel` for selection state, the `LogFormat` field map for columns, and a `QStyledItemDelegate`-style paint helper per cell. Hand-rolled: hit-testing, rubber-band and shift-click range selection, keyboard navigation, and clipboard serialization.

Displayed height is capped at 100 lines per record (`SPEC.md` §5). `lineCount` is clamped for display only — the prefix sums use the clamped value so geometry stays consistent, while copy operations use the true byte range.

### 7.1.1 Line wrapping and the two geometry modes

`SPEC.md` §5 makes wrapping a three-mode setting, and *always on* is the mode that breaks the scheme above: a wrapped record's height depends on the viewport width, so every resize invalidates every height. Measuring all records on resize is an O(n) text-shaping pass — worse than the `QTableView` problem this design exists to avoid.

Hence two geometry modes behind one interface:

**Exact mode** (wrap off, or selected-record-only). Height is a property of the record alone. Prefix sums are exact, the scrollbar is exact, and the *selected-record* case is a bounded special case: one record's height changes, so patch that single block's sum rather than rebuilding.

**Estimated mode** (wrap always on):

- Exploit the fixed-pitch font — wrapped height needs **no text shaping**, only a character count: `ceil(charsInLine / viewportCols)`. That reduces the problem from measuring text to counting characters. The font is not incidental to styling: `monospaceFont()` (`src/ui/Fonts.h`) takes the family the platform designates as fixed-width and applies it to the whole view, header included, so every column shares one uniform advance. Anything that gives a column a proportional font invalidates this estimate.
- Measure exactly the blocks that have been visited; estimate the rest from a running average of characters-per-record observed so far.
- The scrollbar is therefore approximate and **refines as the user scrolls**, which `SPEC.md` §5 states plainly rather than hiding. Scroll position and navigation stay exact; only the thumb geometry is estimated.
- Cache measured block heights keyed by viewport column count; a resize invalidates the cache, and recomputation is debounced so a drag-resize measures once at the end rather than per frame.

The estimation machinery is only reachable in *always on* mode — the other two modes never construct it. This keeps the common path exact and simple, and confines the complexity to the mode that demands it.

**This is the highest-risk component in the project, and 7.1.1 is the highest-risk part of it.** Prototype exact mode first in M2a to validate the core scheme; estimated mode can follow in M2b once the foundation is proven.

### 7.2 Model and filtering

- `LogModel : QAbstractTableModel` — rows are records, columns come from `LogFormat::fields`. `data()` parses lazily; it is on the paint path and must not allocate more than necessary. Prefer `QStringView` into the mapped bytes. The model stays a `QAbstractTableModel` even though the view is custom: it keeps the proxy-filter machinery and the model/view separation intact.
- Filtering via a `QSortFilterProxyModel` subclass over `LogModel`. Predicates read `Record::priority` and `Record::loggerId` directly, not display strings.
- Priority filtering is a single `>=` test against a minimum level (`SPEC.md` §6). This requires the `Priority` enum to be **declared in severity order** (`Trace < Debug < Info < Warn < Error < Fatal`) so the integer comparison is the severity comparison. `Unknown` (unparsed records) sorts below `Trace` so a minimum level never hides unparsed lines — they carry no priority to filter on and must stay visible.
- Subsystem and thread filtering are `QSet<quint32>` of interned ids. Interned id **0 is the "field absent" sentinel** and is exempt from its own axis, for the same reason `Unknown` is exempt from the priority minimum: a record that never carried the field cannot be judged by a filter on it.
- **An axis that excludes nothing is written as inactive.** The subsystem and priority axes ship enabled (`SPEC.md` §6) so their controls are live on the first click, but "enabled at TRACE" and "enabled with every subsystem ticked" narrow nothing. `FilterPane` collapses those states to `enabled = false` in the `FilterSet`, keeping `FilteredIndex` on its identity path instead of materializing a compact copy of every record for a filter that hides none of them. The checkbox stays ticked; only the model-side flag is dropped. Both collapses are exact, not heuristic.
- **Newly discovered values arrive selected.** The subsystem/thread lists are repopulated repeatedly as the scan turns up names, so `FilterPane` tracks every name it has ever listed: a name absent from that set is new and starts checked, while one the user unticked stays unticked. Without it, an enabled-by-default axis would start dropping records mid-scan. The set is per-binding and is cleared in `setDocument()` — carried across a rebind it would leave the next file wholly filtered out.
- **...unless the selection is a restriction, which is a different thing that looks identical.** `MatchCriteria::loggerRestrictive` / `threadRestrictive` mark a selection made by *pointing at one value* (`AxisEditor::showOnlyValue()`, the record menu's edit — §7.4) rather than by ticking a list. The name list alone cannot tell the two apart, and the discovery rule above is exactly wrong for the second: "show only `net.http`" would silently acquire every subsystem the scan found afterwards, and on a tailing log it would keep acquiring them for as long as the file is open. Restrictive turns the discovery rule off for that axis without turning it into an exact load — a name the user *types* still arrives checked, because typing it is the request to see it. It is cleared by any hand edit to the list (one tick, All/None/Invert, a manual add), which is the user taking the axis back; it is **persisted**, unlike `coversAll`, because it is part of what the selection means and a restored session or applied preset must not widen where the original did not. It is written to JSON only when true, so every state that predates it serializes byte-identically and neither store's schema version moves.
- Time-range filtering is two `qint64` comparisons against `Record::timestamp`.
- **Message-text filtering is the one axis with no fast path.** It cannot use interned ids; it must decode and scan message bytes per record. Order the predicate chain so the cheap integer tests run first and text matching only sees what survives them — on a typical filter set that is a small fraction of records. For substring matching use a `QByteArray` search over the encoded form rather than constructing a `QString` per record; only regex matching needs decoded text.
- Find/Find Next (`SPEC.md` §5) shares the text-matching code with message filtering but not the mechanism: find walks the proxy's visible rows from the cursor and returns a row index, changing no filter state.
- Highlighting is **not** a proxy: it is applied in `data()` via `BackgroundRole`/`ForegroundRole`, evaluating the ordered rule list and returning on first match (`SPEC.md` §7). The matched rule carries an independent background and foreground choice, each either a palette index or *default*; a *default* returns an invalid `QVariant` for that role so the view falls back to the normal theme color. First-match-wins is per-rule, not per-role — a rule that sets only the background does not let a lower rule supply the foreground.
- **Highlight rules match on the same five axes as filters, through the same predicate.** A `HighlightRule` embeds a `MatchCriteria` — the *portable* form of the axes (subsystem/thread names, a priority level, display-zone wall clock, a pattern string) as against `FilterSet`'s *resolved* form (interned ids, UTC ms, a compiled regex). `HighlighterSet::resolve()` turns each rule's criteria into one `FilterSet`, so there is exactly one predicate chain in the codebase and the two features cannot drift into half-overlapping axis sets. Two arguments to that resolve carry the whole difference:
  - `AbsentField::DoesNotMatch` vs `::Matches`. A record lacking the field an axis tests must not be *hidden* by a filter (`SPEC.md` §4) and must not be *colored* by a highlight rule (`SPEC.md` §7). Same axis, opposite exemption, one flag on `FilterSet` rather than a second copy of the chain.
  - `NoOpAxes::Keep` vs `::Collapse`. Filtering collapses an axis that excludes nothing so `FilteredIndex` keeps its identity path; highlighting has no compact index to protect, and "≥TRACE" is a legitimate *color everything parsed* rule.
- **The text axis reaches the paint path, so its cost is bounded twice over.** `HighlighterSet::match()` takes the decode as a *callable* (the same lazy-`MessageFn` shape as `FilterSet::accepts`) and invokes it only once a rule's integer axes have passed and that rule has an active text axis, memoizing the result across the rule list — N text rules cost at most one decode. The decode is `Document::messageText()` (capture-free `recordStartRe`, whole-line fallback), not `cellText()`'s full-capture `recordRe`. And `LogModel::rowColors()` resolves both roles in one first-match-wins pass, because `LogView` would otherwise ask for `BackgroundRole` and `ForegroundRole` separately and run the list — and the decode — twice per record. The bound is one decode per *visible* record per repaint: tens of records, against the ~270 ms a text *filter* costs over 1M (`PLAN.md` M4). No cache is warranted; one would need invalidating on filter reset, live append, rule edit and theme change.
- **`resolve()` is where everything expensive happens**, and it runs per rule edit, per rescan and per display-zone change — never per record. It is also load-bearing: an unresolved `HighlighterSet` matches nothing, so every path that installs rules (session restore included) must call `Document::resolveHighlighters()`.
- Sorting is deliberately not offered: records are inherently in chronological order, and sorting a lazy offset index would require a full materialization pass.
- **Filtering invalidates the §7.1 prefix sums**, since hidden records contribute no height. Rebuild the block sums over the visible subset whenever the filter changes — a single linear pass over the index with no parsing, comfortably inside the §11 repaint budget. Do not attempt incremental patching; the full rebuild is fast and much harder to get wrong.

#### 7.2.1 Filter context (`grep -B`/`-A`)

Context (`SPEC.md` §6) shows N records either side of each match, dimmed. It is the one thing the filter chain cannot express, because it is not a property of a record: whether row *r* is shown depends on rows *r±N*. So it is not an axis and not a `FilterSet` field — it is two ints on the `Document`, beside the filters, and one **forward pass** (`ContextEmitter.h`) that turns "which records match" into "which records are visible, and which of those are only neighbours".

- **Everything below it is unchanged**, which is why the feature is small: `FilteredIndex::setVisible()` already took any ascending list of source ordinals and rebuilt its own prefix sums, so a context record is just one more ordinal, and every piece of tested geometry — the exact line↔record statics and the `EstimatedGeometry` estimator alike — runs over it without knowing. The only new state is one byte per visible row saying "context, not match", which `LogModel::rowIsContext()` exposes and `LogView` renders as a dimmed row. Core has no palette, so core does not decide what dimmed *looks* like.
- **Context composes with the run bound rather than ignoring it**, which is why `acceptsInView()` is now also available as its two halves: `inRunBound()` (integer comparisons) and `matchesFilters()` (may decode). A neighbour is a record the *filters* rejected but the run still admits — pulling the lead-up in from the previous run would be exactly wrong (`SPEC.md` §3a).
- **Context never activates the view on its own.** `applyFilters()` keeps its early-out: with no filter and no run there is nothing to be context to, and the identity path must stay allocation-free (§7.2 above).
- **The suffix invariant — the load-bearing claim.** After any emission step, the emitted set contains every in-bound row in `[lastMatch − before, lastEmitted]`. A new match at `r > lastEmitted` therefore needs only rows *above* `lastEmitted`: the rest of its leading window is already there. So **leading context is a tail append**, never a mid-list insert, and the live path extends the visible subset in place exactly as it did before context existed (§6, invariant #9). This holds only because `inRunBound()` accepts a *contiguous* range of ordinals — true today, since the run bound is a byte interval and `Record::offset` is monotone in ordinal. A non-contiguous view restriction would break it and force real insertions; that is now a constraint on §7.3, not an incidental property of it.
- **The live edge case is the provisional trailing record**, the one whose bytes can still change. Without context, at most its own view row can be wrong, so the tail is popped one row and re-evaluated. With `-B`, a provisional that *was* a match also dragged up to `before` neighbours in with it, so the pop widens to `base − before` — and the re-scan must restart at **the same row it popped back to**, not at the provisional. Restarting at the provisional would drop any match inside the popped window from the view permanently and silently (`tst_filtercontext::liveProvisionalFlipDoesNotDropAnEarlierMatch`). The pop widens only when the provisional's class actually **flips**: if it was and still is a match, the invariant leaves the emitter at `lastEmitted == base − 1` and the re-emit reproduces it exactly, so paying `before + 1` row removals per tick would be pure churn — visible to a detached reader as the view jumping on every append.
- **No emission state is cached.** `ContextState`'s two fields are read back off the `FilteredIndex` at the start of each tick, which is why `rescan()`, `enterWaiting()` and rotation need no bookkeeping for context at all: they clear the subset, and the state clears with it.
- The whole `-B`/`-A` rule is a template over three callables, so it is testable as a string-in/string-out function and the one-shot and incremental callers cannot drift — the same move `acceptsInView()` made for the run bound.

**Threading:** indexing runs on a worker thread and reports progress incrementally so the view populates during the scan rather than after it. The model is updated on the GUI thread in batches (via queued signals) — batching matters, since per-record signals on a fast scan will drown the event loop. Indexing must be cancellable.

### 7.3 Run selection

A log file often concatenates several application runs (`SPEC.md` §3, Runs). Rather than a second view layer, a selected run is modelled as **one more bound composed into the existing filtered view**:

- A run is a contiguous record range expressed as a half-open **byte-offset interval `[startOffset, endOffset)` over `Record::offset`**. Offsets never shift under append (records keep their offsets), so the last run's `endOffset` is `INT64_MAX` and appended records fall into it automatically — the "watching the last run" case needs no special code. **That contiguity is now load-bearing rather than incidental:** filter context (§7.2.1) relies on the in-view predicate accepting an unbroken range of ordinals, and a view restriction that admitted a scattered set would make its tail-append guarantee false.
- **Detection reuses the text matcher** (the one behind message filtering and Find) against each record's **whole first decoded line** — the same shape as `recordStartRe`, and going through the `Decoder` so it is encoding-correct (invariant #8). Runs are stored on the `Document` as start markers; a run's `endOffset` and record count are derived from the next marker, so a new marker appended live *retroactively* bounds the previously last run with no mutation of existing entries. Records before the first marker form a leading "preamble" run so nothing is unreachable.
- **`Document::acceptsInView()`** combines the run bound (checked first, cheapest) with `FilterSet::accepts()`. Both `applyFilters()` and the live-append path call it, so the run restriction is applied identically on the initial pass and on tail — the run range is *not* a `FilterSet` axis (its match target differs, and keeping it on the `Document` avoids the two panes clobbering one shared struct). The visible subset it produces is the ordinary `FilteredIndex` (§7.1), so scrolling/geometry/highlight/Find are unchanged.
- **Live:** the appended tail is scanned for new markers *before* candidate admission, so a new run's records are rejected by the (now-bounded) selected run rather than admitted and later removed — the view freezes at the boundary and the new run appears in the pane to switch to (`SPEC.md` §3). Rotation/truncation re-detects over the fresh index and defaults to the newest run.
- **Persistence:** the run-start pattern lives in `FormatSettings` (per-file, like the format); the session records *which* run by its start offset/timestamp (a stable key, re-resolved to an ordinal after re-indexing), never the ordinal.

### 7.4 The record context menu

Right-clicking a record turns that record's own field values into filter and highlight criteria (`SPEC.md` §5). It is an **input method for `MatchCriteria`, not a second filtering system** — nothing in it can express a criterion the panes cannot. That is what makes it additive: presets, session restore, the absent-field exemptions and portability across a re-index all apply to it without a line of new code, because what it produces is what the panes produce.

Three seams, each placed where it already had everything it needs:

- **`LogView` answers only "where".** It emits `recordMenuRequested(viewRow, column, globalPos)` and builds nothing: it has the hit test and no access to the panes. The position arrives in **viewport** coordinates — `QAbstractScrollArea` forwards a context-menu event from the viewport exactly as it forwards a mouse press — and the record is resolved against the total display-line count rather than through `recordAtViewportY()`, which clamps a click below the last record to that record. Clamping is right for a click, which selects the nearest row, and wrong for a menu, which would then act on a record the cursor is demonstrably not on.
- **`MainWindow` assembles it**, because it is the only place that reaches both the record's fields (through the `Document`'s intern tables) and the panes the items edit. `buildRecordMenu()` is split from the popup and public so tests inspect and trigger items without `exec()` blocking on a real menu. Items are **omitted, not disabled**, when the record or the format cannot answer for that axis; the clicked column reorders the groups and never changes their contents.
- **The edits land on the panes' own controls** (`FilterPane::showOnlyValue()` and friends → `AxisEditor`), never on `Document::filters()` directly. The pane's widget state is not derivable from the resolved `FilterSet` — which is why the window stashes it per file (§12.1) — so a direct write would leave the pane showing something else and the next hand edit would silently undo the menu's. It is also the undo story: there is no undo stack, and the ticks visibly moving *is* the feedback.

The one thing that is genuinely new rather than borrowed is the restriction bit `showOnlyValue()` sets; see §7.2, "unless the selection is a restriction". Two smaller decisions worth keeping: a highlight item **appends** its rule, since rules are first-match-wins (`SPEC.md` §7) and a rule the user placed deliberately must outrank one added by a menu click; and setting one time bound has to answer for the other, because the editors always hold *some* wall clock and an unseeded end bound sits in the year 2000 — so the far end is pushed out to the file's observed span whenever it would otherwise make the range empty.

## 8. Persistence

- `QSettings` for window geometry, `QMainWindow::saveState()` output (the pane layout), the open files and the views onto them in tab order, per-view column layout and wrap mode, and per-file filters/highlighters. Follow state is **not** persisted: every file opens at its end, following (`SPEC.md` §3), so there is nothing to restore. The schema is at version 3; see §12.3 for its shape, the two migrations, and the restore ordering.
- Presets as JSON under `QStandardPaths::AppConfigLocation` — a discrete file format, since `SPEC.md` §9 proposes export/import.
- Per-file format cache, keyed by canonical path, so a configured file reopens without prompting. Per file only — no directory-level fallback; a new file is never assumed to share a sibling's format.
- Schema version field in both settings and preset files from day one; migrating unversioned user data later is unpleasant.

**Highlight rules store two palette references — background and foreground — never RGB values** (`SPEC.md` §7). Each reference is a palette index into a 12-entry table, or a *default* sentinel meaning "leave this role at the theme's normal color." The palette maps each index to a light-theme and a dark-theme color, so switching themes remaps every existing rule automatically. Persisting raw colors would freeze rules to whichever theme was active when they were created — the exact problem the curated palette exists to prevent.

Preset export/import (`SPEC.md` §9) is JSON. Because rules carry palette *indices* rather than colors, an exported preset is portable across themes by construction — the importing user's palette supplies the actual colors. Include the schema version (§8) in exported files so a preset shared today still imports after the format evolves.

**Growing the highlight axis set did not bump either schema version, deliberately.** A rule's axes now persist as a nested `match` object, and `HighlightRule::fromJson()` falls back to reading the original flat `matchLogger`/`minPriority` keys when that object is absent, so every preset and session written before the change keeps loading. That backward read is what makes the bump unnecessary — and the bump would have been destructive, because both `PresetStore` and `SessionStore` gate on *exact* version equality with no migration path: an unrecognised version yields an empty collection and a refused import, so bumping would silently discard every preset a user already had. For the same reason `MatchCriteria::toJson()` keeps `FilterPane`'s original key names verbatim (`minPriorityIndex`, `loggerChecked`, …) rather than tidying them.

**A saved host is keyed by its display name, not by its connection** (`SPEC.md` §3). `hosts.json` was originally keyed by `(user, host, port)` — the tuple that actually identifies a connection — which is defensible and was wrong for this list, because the thing the user picks from is a list of *names*. Two entries could differ only in a field the list does not show, and then neither could be told from the other nor removed on purpose: `remove()` took the connection tuple and deleted whichever row matched first. Keying on `displayName()` (the label, or the host when there is none) makes the visible identity the real one, so `save()` overwrites in place and `remove()` takes a name. The comparison is trimmed and case-insensitive for exactly the same reason — `Prod` and `prod ` are one row as far as anyone reading the list is concerned.

Two consequences worth keeping straight. `all()` drops later duplicates rather than trusting the file, because a file written before this rule — or edited by hand — can still hold them, and the whole point is that such rows must not reach the list. And `find()`, which maps an `ssh://` address to a bookmark for its auth choice and poll cadence, still matches on the connection tuple and now takes the *first* hit: an address carries no name, one connection may legitimately be saved under several, and every one of them describes the same machine.

**No schema bump for it.** The stored shape is unchanged — the same keys, the same version — and only the meaning of the key changes on the read side, so an existing `hosts.json` keeps loading. This is the same reasoning as the highlight-axis change above: the exact-version gate makes a bump destructive, and there is nothing here a backward read cannot handle.

**And `find()` finally has a caller (M14).** Until then it had none anywhere in `src/` — only tests — so the `password`, `auth` and `keyFile` it was written to deliver were persisted and never read at connect time, and a bookmark's remembered password did nothing at all. `MainWindow::primeRemoteCredentials()` calls it from the top of `openFile()`, the single funnel every entry point goes through, and hands the password to the transport the only way core will take one: through the per-target credential cache (§6.3.2). `indexOfTarget()` is its inverse for `passwordAccepted()`, which is handed a target and nothing else; it compares *forwards* — `locationFor({}).target() == target` — rather than parsing `user@host:port` apart, because `target()` emits `host:port` with no `@` when there is no user, and an IPv6 literal carries colons of its own. Still unread: `pollMs` and `tailStartBytes` reach the transport only from the Open Remote dialog and the Remote Hosts menu, so the same URL arriving from the command line, recent files or session restore ignores them — deliberately left, since priming `tailStartBytes` would change what a restored tab shows.

### 8.1 Concurrent instances

`SPEC.md` §3 allows multiple instances at once, which makes settings a shared mutable resource across processes. Three consequences:

- **Write atomically.** Preset and settings files are written to a temp file and renamed, so an instance crashing or two writing at once can never leave a truncated file. `QSettings` handles this for its own store; the JSON preset file is ours to get right.
- **Per-file state is keyed by file path**, so instances viewing different logs never contend. This is the main reason the per-file scoping in `SPEC.md` §10 is worth having beyond its UX merit.
- **Global state is last-writer-wins** (`SPEC.md` §10), since instances have no coordination channel. Write on change rather than only at exit, to narrow the window in which one instance's state is lost.

Deliberately *not* doing: a lock file, a single-instance server, or inter-instance IPC. Each adds a failure mode (stale locks, port conflicts) far more annoying than the state loss it prevents.

### 8.2 Chrome colours, and the placeholder Qt forgets

A log's own colours have been theme-aware since M5: highlight rules reference a `HighlightPalette` slot by index, and each slot carries a light and a dark variant (§8). The **chrome** around them was not. An invalid pattern's red, a caution's amber, a muted aside and the grey of placeholder text were each a hex literal chosen against a light theme — `#c0392b`, `#b9770e`, `#b04a00`, `color: gray` — and on a dark palette they ranged from dim to unreadable. `UiColors` gives those four the same treatment, as plain functions of a `QPalette` so they need no `QApplication` and track whatever theme the widget is actually in. It is deliberately *not* part of `HighlightPalette`: those twelve slots are a user-facing palette that rules reference by index and presets round-trip through, while these four are internal and nothing persists them.

**The placeholder is a Qt gap rather than a loftail one, and it is worth naming.** `QPalette::PlaceholderText` arrived in Qt 5.12. A platform theme that predates it — or simply does not fill it in, which is common — leaves the role at Qt's built-in default of **black at 50% alpha**, no matter how dark `Base` is. Every placeholder in the application then renders black on a dark field: present, occupying space, unreadable. It was reported from a real desktop, and the diagnosis is repeatable from a screenshot alone — the *placeholders* were black while the typed values beside them (`22`, `1000 ms`) were light, which no theme would do on purpose.

`ensureReadablePlaceholder()` repairs it, and two properties of how are load-bearing:

- **Conditional, not unconditional.** A theme that set the role sensibly is left completely alone. Overriding it everywhere would replace a deliberate choice with a computed one and make loftail look wrong on the themes that were already right.
- **Measured, not inferred.** The test is the WCAG contrast of the *composited* colour against the field, not "did the theme set this role". That also catches a theme that sets the role badly rather than not at all, and compositing matters because the failing value carries 50% alpha — comparing it uncomposited measures a colour that is never drawn.

The repair writes a widget palette, which does not follow a later live theme switch; the colours are recomputed the next time the widget is built. Worth knowing, not worth a palette-tracking mechanism for a hint colour.

## 9. Format autodetection

Built after the manual path (M8), behind a seam that existed from the start:

```cpp
class IFormatProvider {
    virtual Expected<LogFormat, Error> formatFor(QByteArrayView sample) = 0;
};
```

`ManualFormatProvider` (reads the user's pattern from settings) came first; `DetectingFormatProvider` was added in M8 behind the same seam, and falls through three layers, cheapest first:

1. **Candidate scoring.** A library of known patterns — log4cplus defaults plus common house styles. Compile each, run over the first ~200 records, score by match rate. Resolves the common case in milliseconds with no inference.
2. **Structural inference.** Tokenize a sample; find positionally stable fields. The strongest anchor is priority: `TRACE|DEBUG|INFO|WARN|ERROR|FATAL` is a closed six-word vocabulary, so a token column drawn from it is near-certainly `%p`. A leading date-shaped run is `%d`; a dotted identifier adjacent to the priority is `%c`; a bracketed run between the logger and the message is `%x`; the remainder is `%m`. Synthesize a pattern string and hand it to the same `PatternCompiler`.
3. **Give up** and fall back to the manual dialog.

Two constraints on layer 2, both learned the hard way:

- **A synthesized pattern may never carry a multi-digit literal.** If the date shape is not recognized, the sample's own digits get copied through as literal text, producing a pattern that matches only the records sharing that timestamp. Match rate cannot detect this: scoring runs over the head of the file, where a startup burst routinely puts every sampled record in the same second, so the memorized pattern scores 1.0 and then indexes a handful of records out of a million. Refuse to synthesize it and let layer 3 ask the user.
- **Slash dates need an order decision.** `03/12/26` is 12 March or 3 December and nothing in the text says which. The order is inferred once over the sample — a component above 12 can only be a day — and defaults to month-first, which is what log4cplus's `%D{%m/%d/%y}` produces. Getting this wrong is silent: both orders compile to the same regex and score identically, and only the parsed `Record::timestamp` differs (§5.1).

Detection produces a *pattern string*, never a bespoke parser — it reuses the entire P1 path. It also requires no new UI: it pre-fills the existing Log Format dialog for confirmation, which is the second reason to build the manual path first.

## 10. Testing

- **`PatternCompiler`** — the bulk of unit testing. Table-driven: pattern in, expected regex behavior and field map out. Cover every modifier, malformed patterns, and patterns missing `%p` or `%c`.
- **Indexing** — small fixture logs covering multi-line records, unparsed leading lines, CRLF vs LF, empty files, and a file ending mid-record.
- **Encoding** — the same fixture log stored as UTF-8, UTF-8 with BOM, UTF-16LE, and UTF-16BE must produce byte-for-byte identical indexes apart from offsets. This is the cheapest possible guard against the §6.1 line-terminator trap.
- **Geometry** — prefix-sum lookups against a synthetic index with known line counts, in both exact and estimated modes; assert that estimated mode converges to the exact total once every block has been visited.
- **Tailing** — a test harness that appends to, truncates, and rotates a temp file, asserting the model converges to the right state.
- **Model/filter** — assert filter predicates against a synthetic index without any UI.
- Everything except the view layer must be testable without a `QApplication`; keep parsing and indexing free of UI dependencies.

## 11. Performance targets

Provisional, to be validated against a real log early rather than assumed:

- Index ≥ 100 MB/s single-threaded on a warm file.
- Scrolling stays at 60 fps on a 1 GB file.
- Toggling a filter on 1M records repaints in < 100 ms.
- Live tail appends visible within ~200 ms of a write.
- Rebuilding block prefix sums after a filter change: < 20 ms per million records.

**Filter context (§7.2.1) is bounded by the same numbers with one caveat worth stating: it widens the *visible* set, and the compact index holds a 32-byte `Record` copy per visible row.** A sparse filter at `-C 50` can approach a hundred records shown per match, and in the limit a second copy of the whole record index. The pass itself does not get more expensive — still one `matchesFilters()` per record, plus at most `before` extra integer bound checks per match, and never a decode for a context row. The memory is the reason `Document::kMaxContext` exists and the spinners are capped rather than open-ended.

## 12. Multiple documents: contexts, views, and the window shell

Several logs are open at once as tabs in a central document well, and one log may be open in several views (`SPEC.md` §5a). This section was previously a list of accommodations for a deferred feature; it now describes the implementation those accommodations bought — the four constraints below (§12.1–12.4) held, and the work was additive.

### 12.1 Three scopes, and what belongs in each

The load-bearing distinction is that a *file* and a *view onto it* are different things.

| Scope | Type | Holds |
| ----- | ---- | ----- |
| Per file, below the UI | `Document` (`src/core`) | source, format, decoder, `RecordIndex`, `FilterSet`/`FilteredIndex`, `HighlighterSet`, zones and the timestamp display mode, runs and the run selection |
| Per file, in the UI | `DocumentContext` (`src/ui`) | the `Document`, its `LogModel`, `IndexController`, `LiveController`, `FormatSettings`, indexing progress, the Filters pane's per-file widget state, and the list of views |
| Per view | `DocumentView` (`src/ui`) | a `LogView` and its own `FindBar`; and inside the `LogView`, scroll position, selection, wrap mode, `QHeaderView` column layout, and follow state |

**Filters, highlighters and the run selection are document-scoped, not view-scoped.** Making them per view would need a `FilteredIndex` per view, which cascades into a per-view row space in `LogModel`, a per-view branch in `LiveController`'s in-place append admission (§7.3), and a Runs pane that could no longer bind by `activeDocumentChanged(Document*)`. Two views of one file therefore show the *same records* and differ only in how they are being read — which is also what a single global set of panes can coherently express.

**Follow state is per view** and lives only in `LogView`. `Document` deliberately has no `following` flag: pinning one view to a point in the history while another tails is the main reason to split a file at all.

**One `LogModel` backs all of a file's views.** The model carries no view state (only the light/dark cue), and each `LogView` constructs its own `QHeaderView` and `QItemSelectionModel` — the ordinary Qt multi-view case. A second model would double the append and reset traffic and buy nothing.

**The timestamp display mode is per file, despite being chosen from a per-view widget.** Its control is the timestamp column header's context menu (`SPEC.md` §4) and the header is per view, but the mode itself sits in `FormatSettings` beside the source zone it replaced. Per view would mean the shared `LogModel` could no longer format the Date column — it carries no view state — pushing that formatting into `LogView` for one setting's sake. The menu is built once, owned by the window, and its checkmark is refreshed from the active context by `updateActionStates()`, the same shape as the per-view `m_followAction`.

### 12.2 The window shell: a document well, and docks around it

The window is a **central `QTabWidget` holding the open files**, with the four side panes as the only `QDockWidget`s. The separation is the point (`SPEC.md` §5a): Qt's dock areas cannot encroach on a main window's central widget, so a pane can never be dropped into the document area and a log can never be dragged out into the panes' — no bespoke drag policing, just the one structural fact.

- `setDockOptions(AnimatedDocks | AllowNestedDocks | AllowTabbedDocks)` must be set **before any dock widget is added**. It governs the panes alone.
- **No `GroupedDragging`.** It makes a drag on any dock's title bar move that dock's entire tab group — and the panes ship tabbed together, so pulling Filters out took the other three with it. Dropping it also lifts the old ban on `setAllowedAreas()` (which `GroupedDragging` mishandled), so panes are now restricted to the left and right areas, matching `SPEC.md` §8 and removing the accidental full-width strip above or below the log.
- **Panes do not float under Wayland** (`panesMayFloat()`, keyed on the QPA platform name, not the OS). Tearing a dock off needs two things Wayland withholds from clients: pointer motion after the pointer has left the widget, and the ability to place a top-level window under the cursor. Qt says the first part out loud mid-drag — *"This plugin supports grabbing the mouse only for popup windows"*. What Wayland does provide is an **implicit** grab for the duration of a button press, delivered to the surface that received the press; a drag that stays inside the main window therefore works, while a tear-off moves the dock to a new surface mid-gesture and loses the rest of it, stranding the pane. `restoreSession()` also un-floats any pane a saved layout brings back floating, so a session written elsewhere (or before a wedged drag) cannot resurrect an unplaceable window.
- **The pane title bars are repainted, not removed** (`PaneTitleStyle`). Because the panes ship tabbed, Qt's dock title bar prints each pane's name a second time directly under the tab that already carries it, above two hairline buttons that are hard to see on a dark palette and hard to hit. The obvious fix — hide the bar — is **not available**, and all three routes were measured rather than assumed: a dock can be dragged only by Qt's *own* title bar; a custom title bar widget never receives the drag and Qt exposes no public way to start one; and dragging a tab in the dock tab bar only reorders it. Hiding the bar would therefore make §8's "dragging a pane moves that pane" unreachable in the default layout. So a `QProxyStyle` installed **on the pane docks only** blanks the title *text* while the dock is tabbed and draws larger, higher-contrast button glyphs. Two details are load-bearing: the text is suppressed at paint time rather than by clearing `windowTitle()`, which is also where the tab bar gets its label; and "am I tabbed" is asked on every paint rather than cached, because Qt has no tabification-changed signal and, when a tab group changes, **only the dock that moved emits anything at all**. `tst_panechrome` pins both the loftail behaviour and the three Qt constraints — if a future Qt lets a tab drag undock, that test fails and the cheaper design becomes available.
- The central widget is a `QStackedWidget` over the tabs and the "no file open" notice, so an empty window shows the notice rather than an empty tab frame.
- A file name is `&`-escaped before it becomes tab text: `QTabBar` reads `&` as a mnemonic.
- Two views of one file are numbered by **tab position**, not creation order, so a dragged tab never ends up labelled `[2]` to the left of `[1]`. `QTabBar::tabMoved` renumbers.

**This was originally the opposite decision**, and the reversal is the interesting part. Open files were `QDockWidget`s too, which bought drag-to-split, tab groups and floating logs for free from Qt's dock dragging. It was rejected in use: with one arrangement shared by panes and logs, ordinary pane dragging could tab a Filters pane on top of the log being read, or wedge a log into the strip along the edge. The flexibility was real, and worth less than knowing where the log is. What the earlier notes recorded as an accepted trade-off versus a third-party docking framework (KDDockWidgets, Qt-ADS) — "nothing structurally prevents a pane from being tabbed next to a log, as Visual Studio's separate document well would" — turned out to be the whole problem, and a plain central `QTabWidget` buys the document well without vendoring either library into a three-platform packaging story.

The cost is deliberate: **logs no longer split, tear off, or float.** Two views of one file still scroll independently, but side by side is not available; if it is wanted back, it belongs in a splitter *inside* the document area, never by returning the logs to the dock layout.

**Which view is active is which tab is current.** `QTabWidget::currentChanged` is the single signal for it — the earlier `QApplication::focusChanged` walk existed because a raised dock did not necessarily take focus, and a non-current tab page cannot be focused at all. `activeDocumentChanged` is emitted only when the underlying **`Document`** changes, so moving between two views of one file does not rebind the panes — which would otherwise reset the Filters pane's discovered-value state for no reason.

**The Filters pane needs an explicit hand-off.** `HighlighterPane` hydrates from the `Document` it binds to (and syncs rules back on every edit), so it needs nothing. `FilterPane`'s widget state is *not* derivable from a `FilterSet`, so the window stashes it into the outgoing `DocumentContext` and restores it into the pane on the way in; an empty stash means "the defaults", which is what a newly opened file gets.

### 12.3 Session schema v3, and the restore ordering

```json
{ "schemaVersion": 3,
  "geometry": "...", "windowState": "...",
  "documents": [ { "path": "...", "format": "...", "timeDisplay": "utc", "filters": {}, "highlighters": {}, "runAll": false } ],
  "views":     [ { "document": 0, "columnState": "...", "wrapMode": 0 } ],
  "activeView": 0 }
```

Two arrays, matching the two scopes: N files, and N views pointing back at them. **The `views` array is in tab order**, which is all the layout an open file has now; `windowState` is `QMainWindow::saveState()` and carries the pane arrangement alone.

**Restore order:**

1. `restoreGeometry()`, then `restoreState()`. Every dock — i.e. every pane — already exists, so nothing is left to place afterwards.
2. For each saved view, in order: build its file's `DocumentContext` if this is its first view (`Document::prepare()` — the fast, synchronous half of an open), then create the view, which appends its tab. Appending in saved order *is* restoring the layout.
3. Activate the saved view, then start every indexing worker. Indexing goes last so worker batches never race the layout settling.

`activeView` is an **index into the saved array**, resolved during step 2 rather than afterwards: a skipped file shifts every index after it, so the view it names has to be recognised as it is created. A file that has gone missing is skipped with an inline notice and no dialog (`SPEC.md` §10); its tab simply never appears, and if it held the active view the first surviving tab takes over.

**v1 and v2 are migrated, not discarded** — v1 as one document with one synthesized view carrying the column state that used to live on the document; v2 as-is, minus the per-view `dockName` that tab order replaced. Both `windowState` blobs are deliberately **dropped**: v1's describes a window laid out nothing like this one, and v2's records the *collapsed* central widget of the all-docks shell, which would restore the document well at zero width — a silent failure that reads as the tabs having vanished.

**`timeDisplay` was added *within* v2, not as a version of its own.** It is one additive key in the existing shape, read with the legacy `displayZone` key as a fallback (the `"local"`/`"utc"` spellings are shared deliberately), so such a store round-trips through either build. A bump is earned by structural change — a new array, a field moved between scopes, a renamed key, a dropped blob — as v1→v2 was and v2→v3 was; and it costs every session whose version `load()` does not list.

### 12.4 The constraints that made this additive

For the record, since they still bind:

1. **A `Document` owns all per-file state.** Nothing outside it may hold per-file state; `DocumentContext` holds the *machinery* around one file, not the file's state.
2. **No singletons or globals for file state.** No `currentFile()` accessor, no static index. This is the constraint most easily violated by accident and the most painful to unwind.
3. **Panes bind to the active document by signal, not by construction.** A pane built against a fixed `Document&` works fine with one file and has to be torn apart for two.
4. **The settings schema stores arrays.** It held a one-element `documents` array from day one, which is why v2 could add `views` beside it instead of restructuring — and why v3 could drop the window-layout coupling from `views` without touching either scope.
