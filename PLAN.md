# loftail — Implementation Plan

**Status:** Draft, 2026-07-20.
See `SPEC.md` for user-visible behavior and `ARCHITECTURE.md` for the technical decisions these milestones implement.

---

## Sequencing rationale

Three principles drive the ordering:

1. **Riskiest contract first.** `PatternCompiler` (M1) determines the shape of everything downstream and has zero UI dependencies, so it is built and tested before any window exists.
2. **Prove the performance path early.** The lazy-index-plus-virtualized-view pipeline (M2) is validated against a real large log before any feature work sits on top of it. If the design is wrong, that must surface in week one, not week six.
3. **Read-existing before watch-for-more.** Every file is conceptually live (`SPEC.md` §3), but the two halves stage cleanly: M2 reads and indexes the content present at open; M6 adds the watch-and-append loop, which is where the concurrency, rotation handling, and platform divergence live. During M2–M5 the app shows a snapshot that does not yet auto-update — an incomplete feature, not a design contradiction. The product is not complete until M6. Crucially, the `LogSource` built in M2a must be append-safe from the start (no immutability assumption, non-blocking file handles, rotation detection) even though append ingestion arrives in M6 — retrofitting those assumptions later would touch the whole read path.

Packaging (M7) is late but not last-minute — cross-platform packaging surprises are real, and M7 sits before the optional work rather than after it.

---

## M0 — Scaffold

Project skeleton that builds and runs an empty window on all three platforms.

- [x] CMake project (`cmake_minimum_required 3.21`, C++20), Ninja, `qt_add_executable`
- [x] Directory layout: `src/core/` (no UI), `src/ui/`, `tests/`
- [x] `QApplication` + empty `QMainWindow`; org/app name set so `QSettings` works
- [x] CTest wired up with one trivial passing test
- [x] `.gitignore`, and **initialize the git repository** — it is not one yet
- [ ] CI is optional at this stage but the build must be verified on Windows and macOS before M7

**Done when:** `cmake --build build && ctest --test-dir build` succeeds and the app opens a window on Linux, and the build is confirmed on Windows and macOS.

**Also:** update the "Project status" and "Commands" sections of `CLAUDE.md`, which currently say nothing is scaffolded.

## M1 — Pattern compiler

`ConversionPattern` → `LogFormat`. Pure logic, no UI. `ARCHITECTURE.md` §3.

- [x] Tokenizer for the pattern string: literals, `%%`, specifiers with optional modifiers
- [x] Modifiers: left/right padding (`%-5p`), truncation (`%.30c`), width+precision (`%20.30m`)
- [x] Specifiers: `%d{...}` (strftime-style → sub-regex), `%p`, `%c`, `%m`, `%t`, `%F`, `%L`, `%M`, `%n`; unknown specifiers produce a structured error, not a silent mismatch
- [x] Both the local-time and UTC date specifiers, reporting which zone each implies via `LogFormat::impliedZone` (`ARCHITECTURE.md` §5.1) — this feeds the *Infer from pattern* default
- [x] Emit `recordRe`, `recordStartRe` (prefix up to the message field), field list, role indices
- [x] Structured `CompileError` carrying an offset into the pattern, so the UI can point at the mistake

**Done when:** table-driven tests cover every modifier and specifier, malformed patterns, and patterns lacking `%p` or `%c`. This milestone is disproportionately test-heavy by design.

**Risk:** `%d{...}` inner-format translation is the fiddliest part. Handle the common strftime subset and reject the rest with a clear error rather than half-supporting it.

## M2 — Index, model, view

The performance spine. `ARCHITECTURE.md` §4–§7.

- [x] `Document` type owning all per-file state, held in a one-element vector (`ARCHITECTURE.md` §12)
- [x] `LogSource` interface + the platform read strategy (`MappedLogSource` on POSIX, `BufferedLogSource` on Windows), **built append-safe from the start**: no immutability assumption, non-blocking shared file handles, rotation/truncation detection wired up even though append *ingestion* lands in M6 (`ARCHITECTURE.md` §6) — POSIX `MappedLogSource` implemented and measured; `BufferedLogSource` is a portable QFile fallback with the rotation/truncation seam wired, its Windows-only non-blocking share-mode `CreateFile` open deferred to the M6 Windows work
- [x] Indexer: scan → `QVector<Record>`, applying the record-start rule for multi-line records, counting `lineCount`, and retaining unparsed lines
- [x] Logger-name interning; subsystem set falls out of the scan (thread names interned too)
- [x] Two-level block prefix sums over `lineCount`
- [x] `LogModel : QAbstractTableModel`, lazy `data()`, columns from `LogFormat::fields`
- [x] **`LogView : QAbstractScrollArea`** — line-unit scrolling, variable row heights, visible-only painting, selection via `QItemSelectionModel`, copy-to-clipboard *(M2b; M2a proved the scheme with a throwaway exact-geometry prototype, now retired)*
- [x] Indexing on a worker thread with **batched** model updates, progress reporting, and cancellation *(M2b; `IndexController` drives the `Indexer` off the GUI thread and streams `IndexBatch`es into `LogModel` via begin/endInsertRows)*
- [x] Open-file UI: dialog, drag-and-drop, recent files *(M2b)*

**Done when:** a multi-hundred-MB real log opens, scrolls smoothly, and shows correct fields — including multi-line records rendered at full height. **Measure against `ARCHITECTURE.md` §11 here** and correct the design now if the targets are missed.

**Risk: this is the highest-risk milestone in the project, and the custom view is the highest-risk part of it.** Build `LogView` against a real log *first*, before the surrounding UI — a scrolling prototype over a synthetic index is enough to prove or disprove the approach in `ARCHITECTURE.md` §7.1. Everything downstream assumes it works.

**M2 is now split**, since wrapping and encoding both landed inside it:

- [x] **M2a — the spine.** Encoding detection and forced-encoding paths + `Decoder`; indexer producing the 32-byte `Record` (timestamps normalized to UTC epoch ms per `ARCHITECTURE.md` §5.1, threads interned); `Document`; `LogModel`; block prefix sums; and a throwaway scrolling prototype of `LogView` in **exact** geometry mode. Proves the performance targets against a real log. *(Measured on a 200 MB / 1.9M-record synthetic log4cplus log, Release build, warm file: indexing 214 MB/s single-threaded vs the ≥100 target; block-sum rebuild 0.45 ms per 1M records vs <20; paint-frame model/geometry cost well under the 16.6 ms/frame 60 fps budget.)*
- [x] **M2b — the production view.** `LogView` proper: selection, keyboard navigation, clipboard (raw + copy-as-columns), column headers, wrap modes off and selected-record-only.
- [ ] **M2c — estimated geometry.** Wrap *always on*: character-count-based height, per-block measurement cache keyed by viewport width, debounced resize, refining scrollbar (`ARCHITECTURE.md` §7.1.1).

M2c is separable and lands last on purpose — the other two wrap modes are fully usable without it, so if estimated mode proves troublesome it can slip without blocking anything downstream.

## M3 — Log format UI

Makes M1 reachable by the user. `SPEC.md` §4.

- [ ] Log Format dialog: pattern entry, live preview over sample lines from the current file, per-field breakdown
- [ ] Encoding selector (Auto-detect default; forced UTF-8 / UTF-16LE / UTF-16BE / system 8-bit), showing what auto-detect resolved to; changing it triggers a full rescan
- [ ] Source and display time-zone selectors (`SPEC.md` §4); changing the source zone reparses timestamps only, not the whole index
- [ ] Compile errors shown inline against the offending position
- [ ] Warn when `%p` or `%c` is missing (filtering degrades)
- [ ] `IFormatProvider` + `ManualFormatProvider`; per-file format cache (no directory fallback)
- [ ] Bad pattern → file still opens with unparsed lines as plain text

**Done when:** a user can open an arbitrary log4cplus file, type its pattern, see the preview resolve, and get correct columns — with the choice remembered on reopen.

## M4 — Filtering

`SPEC.md` §6.

- [ ] Filter proxy over `LogModel`; priority as a minimum-level `>=` test, subsystems and threads as sets of interned ids
- [ ] Subsystem and thread filter UI: auto-discovered lists, manual entry, select-all/none/invert, narrowing text box
- [ ] Priority filter: a single minimum-level selector; predicate is one `>=` test, which requires the `Priority` enum in severity order (`ARCHITECTURE.md` §7.2)
- [ ] Message-text filter: substring and regex, case-sensitivity, negation; ordered last in the predicate chain so integer tests run first
- [ ] Time-range filter: start/end bounds against `Record::timestamp`
- [ ] **Find / Find Next**: shares the matching code, walks visible rows from the cursor, changes no filter state
- [ ] Individual enable/disable per filter, no dialog
- [ ] Filtered/total counts in the status area

**Done when:** filters apply to 1M records within the §11 repaint budget — measure with a message-text filter active, since it is the only axis without an integer fast path — and toggling one is a single click.

## M5 — Highlighting, panes, presets, persistence

Delivers the side-pane workflow that motivates the product. `SPEC.md` §7–§10.

- [ ] Curated 12-entry dual-theme palette; each rule persists a **background** and a **foreground** palette index (or *default*), never an RGB value
- [ ] Highlight rules: ordered list, first-match-wins evaluated in `data()`; the matched rule supplies both roles, *default* falling back to the theme color
- [ ] Rule editor: match on subsystem and/or priority, pick background and text color (each palette-or-default), reorder, enable/disable
- [ ] Atomic (temp-file + rename) writes for settings and presets, for the multi-instance case (`ARCHITECTURE.md` §8.1)
- [ ] Three `QDockWidget` panes: filters, highlighters, presets
- [ ] Filter and highlighter presets: create from current state, apply, rename, delete; JSON under `AppConfigLocation` with a schema version
- [ ] Preset **export/import** to a user-chosen JSON file, schema-versioned; portable across themes since rules carry palette indices, not colors (`ARCHITECTURE.md` §8)
- [ ] Session restore: last file, format, filters, highlighters, window geometry, pane and column layout
- [ ] Settings schema with a `documents` **array** and per-file scoping from the start (`ARCHITECTURE.md` §12.4) — writing it as a single-document schema now means a migration later
- [ ] Panes bind to the active document by signal, not by construction (`ARCHITECTURE.md` §12.3)
- [ ] Missing last-file handled gracefully (empty view + inline notice, not an error dialog every launch)

**Done when:** quitting and relaunching restores the previous working state completely.

## M6 — Live updates

Completes the always-watched model from `SPEC.md` §3. The `LogSource` is already append-safe from M2 (§6); this milestone activates the watch-and-append loop on top of it.

- [ ] `QFileSystemWatcher` + low-frequency size poll (watcher alone is unreliable on network mounts)
- [ ] Incremental indexing of appended bytes; partial trailing record held until complete; block prefix sums extended in place
- [ ] Rotation/truncation detection via size and file identity → silent rescan, no user notice
- [ ] Follow is on at every open (view starts at the file's end); scroll-away detaches, a return-to-bottom control re-attaches
- [ ] Incoming records pass through the active filters and highlighters unchanged

**Done when:** every file visibly auto-updates as it grows with no user action, and the tail harness (append / truncate / rotate against a temp file) converges correctly — verified on all three platforms, since Windows file-sharing behavior differs and must be exercised there specifically.

**Risk:** the platform-divergent milestone. Budget time for Windows.

## M7 — Packaging

- [ ] Command-line argument handling (`loftail <file>`, `--pattern <p>`); files always open at end, following, so there is no `--follow`
- [ ] File association is explicitly **not** handled by the application — if wanted, it belongs to the installer
- [ ] Linux: AppImage (built on Ubuntu 24.04, the reference environment — `ARCHITECTURE.md` §1), via `linuxdeploy` + `linuxdeploy-plugin-qt` to bundle Qt so it runs without a system Qt
- [ ] Windows: `windeployqt` + installer or portable zip
- [ ] macOS: `.app` bundle via `macdeployqt`; note that distribution outside a signed/notarized flow will warn users
- [ ] Verify a clean-machine launch on each platform (no Qt installed)

**Done when:** each platform has an artifact that runs on a machine without a Qt development environment.

## M8 — Format autodetection (post-1.0)

`ARCHITECTURE.md` §9, `FUTURE.md`. The one later-release feature with a scheduled milestone; deliberately after a shipping product.

- [ ] Candidate pattern library + match-rate scoring over the first ~200 records
- [ ] Structural inference fallback, anchored on the closed priority vocabulary
- [ ] `DetectingFormatProvider` behind the existing `IFormatProvider` seam
- [ ] Pre-fills the existing M3 dialog for confirmation — no new UI, never applied silently

**Done when:** common log4cplus patterns are detected without user input, and detection failure falls back cleanly to manual entry.

---

## Deliberately deferred

Later-release features are catalogued in `FUTURE.md` (multi-file views, compressed and SSH sources, bookmarks, and — with a milestone, M8 — format autodetection); each names the P1 accommodation that keeps it additive. Recorded here only so they are not silently dropped from the plan.

Column reorder/hide with remembered layout (`SPEC.md` §5) is additive to the M2 spine and lands in M2b. **Done (M2b):** `LogView` drives its columns through a `QHeaderView` (reorder, resize, hide via the header context menu); the layout round-trips through the header's own `saveState()`/`restoreState()`, persisted to `QSettings` — full session restore is folded in at M5. (Preset export/import, once deferred, is now in M5.)

Explicitly ruled out for now: caching the index to disk. It needs invalidation, versioning, and a cache location — real complexity to solve a problem that may not exist. Revisit only if the M2a measurements miss the §11 indexing target.
