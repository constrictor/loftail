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

`FUTURE.md` plans `.gz` and SSH-retrieved logs, and both violate an assumption that is otherwise easy to bake in: **that a log source is local and randomly seekable.** Neither is. Gzip has no random access without an index; a remote source adds latency to every read.

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

**Threading:** indexing runs on a worker thread and reports progress incrementally so the view populates during the scan rather than after it. The model is updated on the GUI thread in batches (via queued signals) — batching matters, since per-record signals on a fast scan will drown the event loop. Indexing must be cancellable.

### 7.3 Run selection

A log file often concatenates several application runs (`SPEC.md` §3, Runs). Rather than a second view layer, a selected run is modelled as **one more bound composed into the existing filtered view**:

- A run is a contiguous record range expressed as a half-open **byte-offset interval `[startOffset, endOffset)` over `Record::offset`**. Offsets never shift under append (records keep their offsets), so the last run's `endOffset` is `INT64_MAX` and appended records fall into it automatically — the "watching the last run" case needs no special code.
- **Detection reuses the text matcher** (the one behind message filtering and Find) against each record's **whole first decoded line** — the same shape as `recordStartRe`, and going through the `Decoder` so it is encoding-correct (invariant #8). Runs are stored on the `Document` as start markers; a run's `endOffset` and record count are derived from the next marker, so a new marker appended live *retroactively* bounds the previously last run with no mutation of existing entries. Records before the first marker form a leading "preamble" run so nothing is unreachable.
- **`Document::acceptsInView()`** combines the run bound (checked first, cheapest) with `FilterSet::accepts()`. Both `applyFilters()` and the live-append path call it, so the run restriction is applied identically on the initial pass and on tail — the run range is *not* a `FilterSet` axis (its match target differs, and keeping it on the `Document` avoids the two panes clobbering one shared struct). The visible subset it produces is the ordinary `FilteredIndex` (§7.1), so scrolling/geometry/highlight/Find are unchanged.
- **Live:** the appended tail is scanned for new markers *before* candidate admission, so a new run's records are rejected by the (now-bounded) selected run rather than admitted and later removed — the view freezes at the boundary and the new run appears in the pane to switch to (`SPEC.md` §3). Rotation/truncation re-detects over the fresh index and defaults to the newest run.
- **Persistence:** the run-start pattern lives in `FormatSettings` (per-file, like the format); the session records *which* run by its start offset/timestamp (a stable key, re-resolved to an ordinal after re-indexing), never the ordinal.

## 8. Persistence

- `QSettings` for window geometry, `QMainWindow::saveState()` output (the pane layout), the open files and the views onto them in tab order, per-view column layout and wrap mode, and per-file filters/highlighters. Follow state is **not** persisted: every file opens at its end, following (`SPEC.md` §3), so there is nothing to restore. The schema is at version 3; see §12.3 for its shape, the two migrations, and the restore ordering.
- Presets as JSON under `QStandardPaths::AppConfigLocation` — a discrete file format, since `SPEC.md` §9 proposes export/import.
- Per-file format cache, keyed by canonical path, so a configured file reopens without prompting. Per file only — no directory-level fallback; a new file is never assumed to share a sibling's format.
- Schema version field in both settings and preset files from day one; migrating unversioned user data later is unpleasant.

**Highlight rules store two palette references — background and foreground — never RGB values** (`SPEC.md` §7). Each reference is a palette index into a 12-entry table, or a *default* sentinel meaning "leave this role at the theme's normal color." The palette maps each index to a light-theme and a dark-theme color, so switching themes remaps every existing rule automatically. Persisting raw colors would freeze rules to whichever theme was active when they were created — the exact problem the curated palette exists to prevent.

Preset export/import (`SPEC.md` §9) is JSON. Because rules carry palette *indices* rather than colors, an exported preset is portable across themes by construction — the importing user's palette supplies the actual colors. Include the schema version (§8) in exported files so a preset shared today still imports after the format evolves.

**Growing the highlight axis set did not bump either schema version, deliberately.** A rule's axes now persist as a nested `match` object, and `HighlightRule::fromJson()` falls back to reading the original flat `matchLogger`/`minPriority` keys when that object is absent, so every preset and session written before the change keeps loading. That backward read is what makes the bump unnecessary — and the bump would have been destructive, because both `PresetStore` and `SessionStore` gate on *exact* version equality with no migration path: an unrecognised version yields an empty collection and a refused import, so bumping would silently discard every preset a user already had. For the same reason `MatchCriteria::toJson()` keeps `FilterPane`'s original key names verbatim (`minPriorityIndex`, `loggerChecked`, …) rather than tidying them.

### 8.1 Concurrent instances

`SPEC.md` §3 allows multiple instances at once, which makes settings a shared mutable resource across processes. Three consequences:

- **Write atomically.** Preset and settings files are written to a temp file and renamed, so an instance crashing or two writing at once can never leave a truncated file. `QSettings` handles this for its own store; the JSON preset file is ours to get right.
- **Per-file state is keyed by file path**, so instances viewing different logs never contend. This is the main reason the per-file scoping in `SPEC.md` §10 is worth having beyond its UX merit.
- **Global state is last-writer-wins** (`SPEC.md` §10), since instances have no coordination channel. Write on change rather than only at exit, to narrow the window in which one instance's state is lost.

Deliberately *not* doing: a lock file, a single-instance server, or inter-instance IPC. Each adds a failure mode (stale locks, port conflicts) far more annoying than the state loss it prevents.

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
