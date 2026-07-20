# loftail — Architecture & Technical Decisions

**Status:** Draft, 2026-07-20.
**Scope:** Internal decisions and their rationale. Nothing here is user-visible; product behavior lives in `SPEC.md`.

---

## 1. Stack

| Decision | Choice | Rationale |
|---|---|---|
| Language | C++20 | Matches the log4cplus ecosystem; needed for the performance profile in §7. |
| Toolkit | Qt 6 Widgets | See §2. |
| Minimum Qt | 6.5 LTS (dev machine has 6.10) | LTS is packaged everywhere; nothing needed requires newer. |
| Build | CMake ≥ 3.21 + Ninja | `qt_add_executable`/`qt_add_test`; Ninja for build speed. |
| Tests | Qt Test | In-tree, no extra dependency, integrates with CTest. |

**loftail does not link log4cplus.** It parses log files as text. There is no compile-time or runtime dependency on the producing library, which keeps the build trivial on all three platforms. (If loftail ever wants its own diagnostic logging, use `QLoggingCategory`, not log4cplus.)

## 2. Qt Widgets over QML

Both can render a virtualized table over a lazy model; `QTableView` and Qt Quick `TableView` are comparable there. The decision rests on three things Widgets provides that QML would require building:

1. **Cross-row text selection and copy.** `QTableView` gives rubber-band and shift-click range selection natively. QML has no cross-delegate text selection — it would mean hand-rolling hit-testing, selection anchors, and clipboard serialization. For a log viewer, selecting a span of records and copying them is a primary interaction, not a nicety.
2. **Dockable panes.** `SPEC.md` §8 describes `QDockWidget` almost exactly: show/hide, move, float, tab. QML has no docking framework; `SplitView` gives fixed panes only.
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
    quint8  priority;  // Priority enum; Unknown for unparsed
};                     // 32 bytes, exactly — no padding waste
```

Everything here is collected during the indexing scan, which is already reading every byte.

- `lineCount` costs nothing to count.
- `timestamp` is parsed eagerly because time-range filtering and jump-to-time need it, and adding it later would mean reindexing every file. The compiled `%d` sub-format tells the parser exactly how to read it. **It is always stored as UTC epoch milliseconds** — see §5.1.
- `threadId` is interned exactly like `loggerId`, giving thread filtering the same integer-compare fast path and the same free discovery of the value set.

**Budget:** ~32 MB of index per million records, up from 24. Worth the two filter axes it buys, but note it before anyone opens a 10 GB log.

### 5.1 Time zones

`SPEC.md` §4 makes both the source and display time zone user-configurable. One rule keeps that from leaking everywhere:

> **`Record::timestamp` is always UTC epoch milliseconds.** Zone conversion happens exactly twice — once on the way in, once on the way out.

- **In:** the indexer applies the *source* zone (inferred from the pattern's date specifier, or explicitly set) to convert parsed wall-clock text to UTC.
- **Out:** the view applies the *display* zone when formatting a timestamp for a cell, and the filter UI applies it when interpreting typed range bounds.

Comparison, sorting, filtering, and any future multi-file merge therefore operate on a single monotonic integer scale with no zone awareness whatsoever. Storing local wall-clock time instead would make every comparison zone-dependent and would break outright across a DST transition, where the same local timestamp occurs twice.

**Changing the source zone requires reparsing timestamps** — but only timestamps, not a full reindex: record boundaries and byte offsets are unaffected, so it is a pass over the existing index. Changing the display zone is free, a repaint.

`PatternCompiler` reports which zone the date specifier implies, since log4cplus distinguishes local-time and UTC date specifiers. That is what the *Infer from pattern* default consumes. Treat the inference as a hint that the user can override, not ground truth — the pattern reveals the specifier, not how the producing application was actually configured.

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

Two implementations, selected by **platform**, not by mode:

- **`MappedLogSource`** (POSIX) — mmap of the currently-indexed extent, re-mapped as the file grows. Safe under rotation on POSIX: `rename`/`unlink` leave an existing mapping intact (it holds the inode), and copytruncate is caught by the size-shrink check below. No copying, fast random access on the paint path.
- **`BufferedLogSource`** (Windows, and the fallback everywhere) — incremental buffered reads. Preferred on Windows because a held file mapping can block the writer from rotating or truncating the file — exactly what a logging framework does — and under the always-watched model that risk would otherwise apply to *every* open file, not just ones a user chose to tail. Opened with full sharing (`FILE_SHARE_READ | WRITE | DELETE`) so loftail never locks the writer out.

The `bytes()`/`size()` interface hides which one is in use; the model and indexer are identical across both.

**Rotation/truncation detection is always active** (it is not gated on a tailing toggle any more): poll size and file identity (inode on POSIX, file index on Windows). If size shrinks or identity changes, the file was rotated — discard the index and rescan. `QFileSystemWatcher` is the primary change signal but is unreliable on some filesystems (notably network mounts), so pair it with a low-frequency size poll rather than trusting it alone. On POSIX, guard mmap reads against a concurrent copytruncate so a read past the new EOF cannot `SIGBUS`.

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

`SPEC.md` §11 defers `.gz` and SSH-retrieved logs but names them, because both violate an assumption that is otherwise easy to bake in: **that a log source is local and randomly seekable.** Neither is. Gzip has no random access without an index; a remote source adds latency to every read.

The `isRandomAccess()` flag exists so the indexer can branch now rather than being restructured later. Concretely, the constraint to honor today: **the indexer must be able to work as a forward, single-pass stream.** It already does — it scans start to finish. Do not add a second backward pass or a "seek to offset X and re-read" step. Random access is a legitimate optimization for `data()` on the paint path, which is only reachable for records already indexed and, for non-seekable sources, will be served from a local cache.

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

- Exploit the fixed-pitch font — wrapped height needs **no text shaping**, only a character count: `ceil(charsInLine / viewportCols)`. That reduces the problem from measuring text to counting characters.
- Measure exactly the blocks that have been visited; estimate the rest from a running average of characters-per-record observed so far.
- The scrollbar is therefore approximate and **refines as the user scrolls**, which `SPEC.md` §5 states plainly rather than hiding. Scroll position and navigation stay exact; only the thumb geometry is estimated.
- Cache measured block heights keyed by viewport column count; a resize invalidates the cache, and recomputation is debounced so a drag-resize measures once at the end rather than per frame.

The estimation machinery is only reachable in *always on* mode — the other two modes never construct it. This keeps the common path exact and simple, and confines the complexity to the mode that demands it.

**This is the highest-risk component in the project, and 7.1.1 is the highest-risk part of it.** Prototype exact mode first in M2a to validate the core scheme; estimated mode can follow in M2b once the foundation is proven.

### 7.2 Model and filtering

- `LogModel : QAbstractTableModel` — rows are records, columns come from `LogFormat::fields`. `data()` parses lazily; it is on the paint path and must not allocate more than necessary. Prefer `QStringView` into the mapped bytes. The model stays a `QAbstractTableModel` even though the view is custom: it keeps the proxy-filter machinery and the model/view separation intact.
- Filtering via a `QSortFilterProxyModel` subclass over `LogModel`. Predicates read `Record::priority` and `Record::loggerId` directly, not display strings.
- Priority filtering is a bitmask over the six levels — a single AND test per record.
- Subsystem and thread filtering are `QSet<quint32>` of interned ids.
- Time-range filtering is two `qint64` comparisons against `Record::timestamp`.
- **Message-text filtering is the one axis with no fast path.** It cannot use interned ids; it must decode and scan message bytes per record. Order the predicate chain so the cheap integer tests run first and text matching only sees what survives them — on a typical filter set that is a small fraction of records. For substring matching use a `QByteArray` search over the encoded form rather than constructing a `QString` per record; only regex matching needs decoded text.
- Find/Find Next (`SPEC.md` §5) shares the text-matching code with message filtering but not the mechanism: find walks the proxy's visible rows from the cursor and returns a row index, changing no filter state.
- Highlighting is **not** a proxy: it is applied in `data()` via `BackgroundRole`/`ForegroundRole`, evaluating the ordered rule list and returning on first match (`SPEC.md` §7).
- Sorting is deliberately not offered: records are inherently in chronological order, and sorting a lazy offset index would require a full materialization pass.
- **Filtering invalidates the §7.1 prefix sums**, since hidden records contribute no height. Rebuild the block sums over the visible subset whenever the filter changes — a single linear pass over the index with no parsing, comfortably inside the §11 repaint budget. Do not attempt incremental patching; the full rebuild is fast and much harder to get wrong.

**Threading:** indexing runs on a worker thread and reports progress incrementally so the view populates during the scan rather than after it. The model is updated on the GUI thread in batches (via queued signals) — batching matters, since per-record signals on a fast scan will drown the event loop. Indexing must be cancellable.

## 8. Persistence

- `QSettings` for window geometry, `QMainWindow::saveState()` output, column layout, active filters/highlighters, last file, and follow state (watching is always on, so it is not a stored choice).
- Presets as JSON under `QStandardPaths::AppConfigLocation` — a discrete file format, since `SPEC.md` §9 proposes export/import.
- Per-file (and per-directory fallback) format cache, keyed by canonical path, so a configured file reopens without prompting.
- Schema version field in both settings and preset files from day one; migrating unversioned user data later is unpleasant.

**Highlight colors store a palette index, never an RGB value** (`SPEC.md` §7). The palette maps each index to a light-theme and a dark-theme color, so switching themes remaps every existing rule automatically. Persisting raw colors would freeze rules to whichever theme was active when they were created — the exact problem the curated palette exists to prevent.

### 8.1 Concurrent instances

`SPEC.md` §3 allows multiple instances at once, which makes settings a shared mutable resource across processes. Three consequences:

- **Write atomically.** Preset and settings files are written to a temp file and renamed, so an instance crashing or two writing at once can never leave a truncated file. `QSettings` handles this for its own store; the JSON preset file is ours to get right.
- **Per-file state is keyed by file path**, so instances viewing different logs never contend. This is the main reason the per-file scoping in `SPEC.md` §10 is worth having beyond its UX merit.
- **Global state is last-writer-wins**, since instances have no coordination channel. Writing on change rather than only at exit narrows the window in which one instance's state is lost, and is what we should do. **[?]** — see `SPEC.md` open question 4.

Deliberately *not* doing: a lock file, a single-instance server, or inter-instance IPC. Each adds a failure mode (stale locks, port conflicts) far more annoying than the state loss it prevents.

## 9. Format autodetection (P2)

Deliberately deferred, but the seam exists in P1:

```cpp
class IFormatProvider {
    virtual Expected<LogFormat, Error> formatFor(QByteArrayView sample) = 0;
};
```

P1 ships `ManualFormatProvider` (reads the user's pattern from settings). P2 adds `DetectingFormatProvider`, which falls through three layers, cheapest first:

1. **Candidate scoring.** A library of known patterns — log4cplus defaults plus common house styles. Compile each, run over the first ~200 records, score by match rate. Resolves the common case in milliseconds with no inference.
2. **Structural inference.** Tokenize a sample; find positionally stable fields. The strongest anchor is priority: `TRACE|DEBUG|INFO|WARN|ERROR|FATAL` is a closed six-word vocabulary, so a token column drawn from it is near-certainly `%p`. A leading date-shaped run is `%d`; a dotted identifier adjacent to the priority is `%c`; the remainder is `%m`. Synthesize a pattern string and hand it to the same `PatternCompiler`.
3. **Give up** and fall back to the manual dialog.

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

## 12. Multi-file accommodation

`SPEC.md` §11 defers opening several files at once, but the architecture must not preclude it. Four constraints make later support additive rather than a rewrite. They cost almost nothing now and are expensive to retrofit.

**1. A `Document` owns all per-file state.**

```cpp
class Document {              // one open log file
    std::unique_ptr<LogSource> source;
    LogFormat                  format;
    RecordIndex                index;      // records + intern table + block sums
    FilterSet                  filters;
    HighlighterSet             highlighters;
    ColumnLayout               columns;
    bool                       following;  // auto-scroll to newest; watching is always on
};
```

The main window holds `std::vector<std::unique_ptr<Document>>` plus an *active document* pointer — a vector of length one today. Nothing outside `Document` may hold per-file state.

**2. No singletons or globals for file state.** No `currentFile()` accessor, no static index, no free function reaching for "the" log. This is the constraint most easily violated by accident and the most painful to unwind.

**3. Panes bind to the active document by signal, not by construction.** The filter, highlighter, and preset panes observe an `activeDocumentChanged(Document*)` signal and rebind. A pane built against a fixed `Document&` reference works fine with one file and has to be torn apart for two.

**4. Settings schema stores an array from day one.** Persist documents as a list even while it always has exactly one element:

```json
{ "schemaVersion": 1,
  "documents": [ { "path": "...", "format": "...", "filters": [], "following": true } ],
  "activeDocument": 0 }
```

Adding files later then requires no settings migration. Presets and window/pane layout stay global, matching the scoping in `SPEC.md` §10.

The work remaining when multi-file is actually implemented — a tab bar or split view, and per-document indexing threads — is then genuinely additive. That is the point of the four constraints above.
