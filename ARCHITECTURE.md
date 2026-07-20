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
    int msgGroup    = -1;
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
    quint32 length;    // spans continuation lines
    quint32 loggerId;  // interned
    quint16 lineCount; // physical lines; drives row height (§7.1). Clamped to 65535.
    quint8  priority;  // Priority enum; Unknown for unparsed
};                     // still 24 bytes with padding
```

`lineCount` is counted during the indexing scan, which is already reading every byte — it costs nothing to collect and it fits in the existing padding, so the index stays at 24 bytes per record.

Parse eagerly **only** what filtering needs — priority and logger. Everything else (timestamp, thread, message text) is parsed lazily in `QAbstractTableModel::data()` from the mapped bytes. Storing parsed strings per record is the single most likely way to make this application unusable on large files.

**Interning:** logger names map to `quint32` via a `QHash<QString, quint32>` built during indexing. Filter predicates then compare integers rather than strings, which matters at millions of records. The intern table is also the authoritative subsystem list for the filter pane (`SPEC.md` §6) — discovery is a side effect of indexing, not a separate pass.

**Budget:** ~24 MB of index per million records. Note this before anyone opens a 10 GB log; if it becomes a problem the fallback is a chunked/paged index, but do not build that speculatively.

## 6. File access

```cpp
class LogSource {                       // the model cannot tell which impl it has
    virtual QByteArrayView bytes(qint64 offset, quint32 length) = 0;
    virtual qint64 size() const = 0;
};
```

- **`MappedLogSource`** — mmap, for post-mortem. Immutable file, no copying, fast random access.
- **`BufferedLogSource`** — incremental buffered reads, for live tailing.

The split exists because **mmap and live tailing interact badly on Windows**: a file mapping's size is fixed at creation, so it must be recreated as the file grows, and holding a mapping can block the writing process from rotating or truncating the file — precisely what a logging framework does. Using mmap only for static files sidesteps this entirely.

**Rotation/truncation detection:** poll size and file identity (inode on POSIX, file index on Windows). If size shrinks or identity changes, the file was rotated — discard the index and rescan. `QFileSystemWatcher` is the primary change signal but is unreliable on some filesystems (notably network mounts), so pair it with a low-frequency size poll rather than trusting it alone.

Files are opened in binary mode; CRLF is handled explicitly rather than via platform text-mode translation, so a Windows-authored log reads identically on Linux.

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

**[?]** `SPEC.md` §5 proposes capping displayed height at 100 lines. If accepted, `lineCount` is clamped for display purposes only — the prefix sums use the clamped value so geometry stays consistent, while copy operations use the true byte range.

**This is the highest-risk component in the project.** Prototype it against a real log early in M2, before feature work depends on it.

### 7.2 Model and filtering

- `LogModel : QAbstractTableModel` — rows are records, columns come from `LogFormat::fields`. `data()` parses lazily; it is on the paint path and must not allocate more than necessary. Prefer `QStringView` into the mapped bytes. The model stays a `QAbstractTableModel` even though the view is custom: it keeps the proxy-filter machinery and the model/view separation intact.
- Filtering via a `QSortFilterProxyModel` subclass over `LogModel`. Predicates read `Record::priority` and `Record::loggerId` directly, not display strings.
- Priority filtering is a bitmask over the six levels — a single AND test per record.
- Subsystem filtering is a `QSet<quint32>` of interned ids.
- Highlighting is **not** a proxy: it is applied in `data()` via `BackgroundRole`/`ForegroundRole`, evaluating the ordered rule list and returning on first match (`SPEC.md` §7).
- Sorting is deliberately not offered: records are inherently in chronological order, and sorting a lazy offset index would require a full materialization pass.
- **Filtering invalidates the §7.1 prefix sums**, since hidden records contribute no height. Rebuild the block sums over the visible subset whenever the filter changes — a single linear pass over the index with no parsing, comfortably inside the §11 repaint budget. Do not attempt incremental patching; the full rebuild is fast and much harder to get wrong.

**Threading:** indexing runs on a worker thread and reports progress incrementally so the view populates during the scan rather than after it. The model is updated on the GUI thread in batches (via queued signals) — batching matters, since per-record signals on a fast scan will drown the event loop. Indexing must be cancellable.

## 8. Persistence

- `QSettings` for window geometry, `QMainWindow::saveState()` output, column layout, active filters/highlighters, last file, and live-tail state.
- Presets as JSON under `QStandardPaths::AppConfigLocation` — a discrete file format, since `SPEC.md` §9 proposes export/import.
- Per-file (and per-directory fallback) format cache, keyed by canonical path, so a configured file reopens without prompting.
- Schema version field in both settings and preset files from day one; migrating unversioned user data later is unpleasant.

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
- **Indexing** — small fixture logs covering multi-line records, unparsed leading lines, CRLF vs LF, empty files, a file ending mid-record.
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
    bool                       tailing;
};
```

The main window holds `std::vector<std::unique_ptr<Document>>` plus an *active document* pointer — a vector of length one today. Nothing outside `Document` may hold per-file state.

**2. No singletons or globals for file state.** No `currentFile()` accessor, no static index, no free function reaching for "the" log. This is the constraint most easily violated by accident and the most painful to unwind.

**3. Panes bind to the active document by signal, not by construction.** The filter, highlighter, and preset panes observe an `activeDocumentChanged(Document*)` signal and rebind. A pane built against a fixed `Document&` reference works fine with one file and has to be torn apart for two.

**4. Settings schema stores an array from day one.** Persist documents as a list even while it always has exactly one element:

```json
{ "schemaVersion": 1,
  "documents": [ { "path": "...", "format": "...", "filters": [], "tailing": true } ],
  "activeDocument": 0 }
```

Adding files later then requires no settings migration. Presets and window/pane layout stay global, matching the scoping in `SPEC.md` §10.

The work remaining when multi-file is actually implemented — a tab bar or split view, and per-document indexing threads — is then genuinely additive. That is the point of the four constraints above.
