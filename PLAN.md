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
- [ ] CI is optional at this stage but the build must be verified on Windows and macOS before M7 — a CI workflow (`.github/workflows/packaging.yml`) that builds + tests + packages on `ubuntu-24.04`, `windows-latest`, and `macos-latest` was authored in M7 and is the vehicle for this verification, but it has **not been executed** (no runner available on this Linux-only dev machine), so the Windows/macOS build confirmation remains genuinely outstanding until the workflow runs green.

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
- [x] **M2c — estimated geometry.** Wrap *always on*: character-count-based height, per-block measurement cache keyed by viewport width, debounced resize, refining scrollbar (`ARCHITECTURE.md` §7.1.1).

M2c is separable and lands last on purpose — the other two wrap modes are fully usable without it, so if estimated mode proves troublesome it can slip without blocking anything downstream.

## M3 — Log format UI

Makes M1 reachable by the user. `SPEC.md` §4.

- [x] Log Format dialog: pattern entry, live preview over sample lines from the current file, per-field breakdown
- [x] Encoding selector (Auto-detect default; forced UTF-8 / UTF-16LE / UTF-16BE / system 8-bit), showing what auto-detect resolved to; changing it triggers a full rescan
- [x] Source and display time-zone selectors (`SPEC.md` §4); changing the source zone reparses timestamps only, not the whole index
- [x] Compile errors shown inline against the offending position
- [x] Warn when `%p` or `%c` is missing (filtering degrades)
- [x] `IFormatProvider` + `ManualFormatProvider`; per-file format cache (no directory fallback)
- [x] Bad pattern → file still opens with unparsed lines as plain text

**Done when:** a user can open an arbitrary log4cplus file, type its pattern, see the preview resolve, and get correct columns — with the choice remembered on reopen.

## M4 — Filtering

`SPEC.md` §6.

- [x] Filter over `LogModel`; priority as a minimum-level `>=` test, subsystems and threads as sets of interned ids — implemented as a compact **`FilteredIndex`** (visible-record ordinals + their own two-level block prefix sums) that `LogModel` and the custom `LogView` consume, rather than a literal `QSortFilterProxyModel`: the line-unit `LogView` (invariant #6) needs prefix sums over the *visible* subset, which a row-only proxy does not provide. `FilteredIndex::geometry()` hands the view the compact index when filtering and the source index (identity) otherwise, so both the exact statics and the AlwaysOn `EstimatedGeometry` work unchanged. The predicate lives in `core/Filter.h` (`FilterSet`), UI-free and unit-tested.
- [x] Subsystem and thread filter UI: auto-discovered lists (from the intern tables), manual entry, select-all/none/invert (over the narrowed view), narrowing text box — `FilterPane`
- [x] Priority filter: a single minimum-level selector; predicate is one `>=` test against the severity-ordered `Priority` enum (`ARCHITECTURE.md` §7.2), with `Unknown` exempt so unparsed lines are never hidden
- [x] Message-text filter: substring and regex, case-sensitivity, negation; ordered **last** in the predicate chain (`FilterSet::accepts` decodes only after the integer axes pass — invariant #4). The message is pulled through the `Decoder` path (no raw-byte scans, invariant #8)
- [x] Time-range filter: start/end bounds against `Record::timestamp`, entered in the display zone and converted to UTC ms once (§5.1); available only when the format has a date field
- [x] **Find / Find Next**: `FindBar` + Ctrl+F / F3 / Shift+F3, sharing `TextMatcher` with the message filter via `Find::search`; walks the visible rows from the cursor with wrap-around and moves only the selection — no filter state changes
- [x] Individual enable/disable per filter — a per-axis checkbox in the pane, no dialog (the pane is a `QDockWidget`)
- [x] Filtered/total counts in the status area ("N of M records shown")

**Done when:** filters apply to 1M records within the §11 repaint budget — measure with a message-text filter active, since it is the only axis without an integer fast path — and toggling one is a single click. **Met** for the repaint budget: on a 1M-record / 73 MB log (Release, warm), the post-filter prefix-sum rebuild is 0.43 ms and a filtered repaint frame is 0.04 ms, both well under §11. Toggling an integer axis recomputes the whole visible set in ~5 ms (single click). The message-text axis — the one with no integer fast path — is decode-bound: recomputing the visible set over all 1M records is ~270 ms (it must decode each surviving record through the `Decoder`, per invariant #1/#8), so it runs last in the chain and integer filters shrink its input (text + `priority≥WARN` ≈ 100 ms; text + one subsystem ≈ 45 ms). The recompute is synchronous; running it on a worker thread (as the indexer already does) is the natural next step but was left out here because the index is mutated on the GUI thread during the live scan (`IndexController::onBatch`), so a concurrent read belongs with the M6 live-append work.

## M5 — Highlighting, panes, presets, persistence

Delivers the side-pane workflow that motivates the product. `SPEC.md` §7–§10.

- [x] Curated 12-entry dual-theme palette; each rule persists a **background** and a **foreground** palette index (or *default*), never an RGB value
- [x] Highlight rules: ordered list, first-match-wins evaluated in `data()`; the matched rule supplies both roles, *default* falling back to the theme color
- [x] Rule editor: match on subsystem and/or priority, pick background and text color (each palette-or-default), reorder, enable/disable
- [x] Atomic (temp-file + rename) writes for settings and presets, for the multi-instance case (`ARCHITECTURE.md` §8.1)
- [x] Three `QDockWidget` panes: filters, highlighters, presets
- [x] Filter and highlighter presets: create from current state, apply, rename, delete; JSON under `AppConfigLocation` with a schema version
- [x] Preset **export/import** to a user-chosen JSON file, schema-versioned; portable across themes since rules carry palette indices, not colors (`ARCHITECTURE.md` §8)
- [x] Session restore: last file, format, filters, highlighters, window geometry, pane and column layout
- [x] Settings schema with a `documents` **array** and per-file scoping from the start (`ARCHITECTURE.md` §12.4) — writing it as a single-document schema now means a migration later
- [x] Panes bind to the active document by signal, not by construction (`ARCHITECTURE.md` §12.3)
- [x] Missing last-file handled gracefully (empty view + inline notice, not an error dialog every launch)

**Done when:** quitting and relaunching restores the previous working state completely.

## M6 — Live updates

Completes the always-watched model from `SPEC.md` §3. The `LogSource` is already append-safe from M2 (§6); this milestone activates the watch-and-append loop on top of it.

- [x] `QFileSystemWatcher` + low-frequency size poll (watcher alone is unreliable on network mounts) — `LiveWatcher` watches the file AND its directory (to catch a rename/replace) plus a 750 ms size poll; both funnel into one `maybeChanged()` signal.
- [x] Incremental indexing of appended bytes; partial trailing record held until complete; block prefix sums extended in place — `Indexer::scanAppendedTail()` continues the single forward pass from the last confirmed record boundary (invariant #9); the trailing record is provisional and re-read each tick so it resolves correctly when it grows with continuation lines or re-splits (invariant #2); `RecordIndex::extendBlockSums()` extends the two-level sums in place (invariant #1). Driven by `LiveController` on the GUI thread.
- [x] Rotation/truncation detection via size and file identity → silent rescan, no user notice — `LiveController::checkNow()` compares `pathIdentity()` against the open source's identity (rename+recreate) and catches shrink/`wasTruncated()` (copytruncate); either triggers `Document::rescan()` behind a model reset with no dialog. **Linux-verified; Windows file-identity (GetFileInformationByHandle) + non-blocking share-mode open still to be exercised there — see below.**
- [x] Follow is on at every open (view starts at the file's end); scroll-away detaches, a return-to-bottom control re-attaches — `LogView` follow state: appended rows keep the view pinned; a vertical scroll away detaches, scrolling back or the overlay "Follow tail" button / View ▸ Follow Tail (Ctrl+End) re-attaches.
- [x] Incoming records pass through the active filters and highlighters unchanged — appended records run the same `FilterSet::accepts()` predicate chain (integers first, text last) and the `FilteredIndex` visible subset + compact sums are extended in place; `resolveHighlighters()` re-binds late-discovered subsystems so highlight rules apply to appended records.

**Done when:** every file visibly auto-updates as it grows with no user action, and the tail harness (append / truncate / rotate against a temp file) converges correctly — verified on all three platforms, since Windows file-sharing behavior differs and must be exercised there specifically. **Implemented and fully verified on Linux** (tail harness `tst_tail`: append incl. a multi-line record split across writes → byte-exact convergence with a one-shot scan, truncate, rotate, and filtered/highlighted append; async watch `tst_livegui`: the real `QFileSystemWatcher`+poll grows the model under a live event loop with no manual poke; `tst_logview` follow detach/re-attach; plus an offscreen GUI-binary smoke run under a background appender). **Windows and macOS runtime verification remains OUTSTANDING** — not runnable on this machine. The `BufferedLogSource` Windows non-blocking share-mode `CreateFile` open and `pathIdentity()`/`GetFileInformationByHandle` are stubbed for POSIX and must be completed + exercised on Windows.

**Risk:** the platform-divergent milestone. Budget time for Windows.

## M7 — Packaging

- [x] Command-line argument handling (`loftail <file>`, `--pattern <p>`); files always open at end, following, so there is no `--follow` — finalized in `src/main.cpp` with `QCommandLineParser` (positional `[file]`, `--pattern`, `--help`, `--version`, app description). No `--follow` by design (SPEC.md §3). Degrades gracefully: no file → empty window; bad `--pattern` → opens as plain text (M3); missing/unknown option → Qt usage message. Verified: `--version`, `--help`, unknown-option exit 1, and headless (`QT_QPA_PLATFORM=offscreen`) opens of a good pattern, a bad pattern, and a nonexistent file all behave correctly.
- [x] File association is a **capability declaration only, never a default claim** — the app advertises it can open `.log` (Linux `.desktop` `MimeType=text/x-log;`; macOS `Info.plist` `CFBundleDocumentTypes` for the `log` extension with `LSHandlerRank=Alternate`, `Role=Viewer`), so it appears in the file manager's "Open With" list, but it never registers itself as the **default** handler — that stays an `xdg-mime`/`duti`/installer/user action. Windows registers nothing. The Linux entry only takes effect once XDG desktop integration installs it (`appimaged`/AppImageLauncher/distro package). Soft-vs-default split documented in `packaging/README.md`.
- [x] Linux: AppImage (built on Ubuntu 24.04, the reference environment — `ARCHITECTURE.md` §1), via `linuxdeploy` + `linuxdeploy-plugin-qt` to bundle Qt so it runs without a system Qt — mechanism is `install()` rules in `src/CMakeLists.txt` (binary → `usr/bin`, `.desktop` → `usr/share/applications`, SVG icon → `usr/share/icons/...`) plus `packaging/linux/build-appimage.sh`. **Actually built and verified here:** produced a 55 MB `loftail-Release-x86_64.AppImage`; a headless run under a stripped `env -i` (`QT_QPA_PLATFORM=offscreen`, offscreen QPA plugin bundled alongside xcb) prints the version and opens a file, and `/proc/<pid>/maps` confirms it loads `libQt6Core.so.6` + `libqoffscreen.so` from the AppImage's own tree, not system `/usr/lib` — i.e. it runs without a system Qt.
- [x] Windows: `windeployqt` + installer or portable zip — script authored: `packaging/windows/build-portable.ps1` (Release build → `cmake --install` → `windeployqt` → zip); CMake install rules are platform-aware for the Windows path; an MSI/NSIS installer is noted as an option. **Authored but NOT run/verified from this Linux machine** — the CI workflow builds+smoke-tests it on `windows-latest`.
- [x] macOS: `.app` bundle via `macdeployqt`; note that distribution outside a signed/notarized flow will warn users — script authored: `packaging/macos/build-appbundle.sh` (`MACOSX_BUNDLE` + `Info.plist.in` in CMake → `macdeployqt -dmg`); the unsigned-Gatekeeper-warning and the `-codesign`/notarization path are documented. **Authored but NOT run/verified from this Linux machine** — the CI workflow builds it on `macos-latest`.
- [ ] Verify a clean-machine launch on each platform (no Qt installed) — **Linux: done** (see above). **Windows/macOS: outstanding** — cannot be produced/verified from this Linux dev machine; `.github/workflows/packaging.yml` is the vehicle that builds and smoke-tests those two artifacts on their native runners.

**Done when:** each platform has an artifact that runs on a machine without a Qt development environment.

**Status:** Linux AppImage is built and clean-run-verified here. Windows and macOS packaging mechanisms are authored and correct but remain unbuilt/unverified on this Linux-only machine; the added CI workflow is where they get built and smoke-tested (consistent with M0's still-open cross-platform verification).

## M8 — Format autodetection (post-1.0)

`ARCHITECTURE.md` §9, `FUTURE.md`. The one later-release feature with a scheduled milestone; deliberately after a shipping product.

- [x] Candidate pattern library + match-rate scoring over the first ~200 records — `FormatDetector` (`src/core`) compiles a curated library of common log4cplus patterns (all date-led, so the numeric date shape anchors against false matches) and scores each by match rate over the first 200 records via the existing `FormatPreview` (matched records / total). The best candidate at or above a 0.6 confidence threshold wins; ties break toward the richer pattern. Pure/UI-free, `QApplication`-less.
- [x] Structural inference fallback, anchored on the closed priority vocabulary — when no library candidate clears the bar, `FormatDetector` synthesizes candidates from the sample lines anchored on the closed priority vocabulary (built from the `Priority` enum via `priorityName()`, not a duplicated list): a token from `TRACE|DEBUG|INFO|WARN|ERROR|FATAL` pivots the line, a leading date-shaped run becomes `%d{...}`, a token after it `%c`, the remainder `%m`, and separators (`" - "`, `" | "`, `[%t]`, …) are reconstructed verbatim. Each synthesized pattern is scored the same way and accepted only if it too clears the threshold.
- [x] `DetectingFormatProvider` behind the existing `IFormatProvider` seam — `DetectingFormatProvider : IFormatProvider` inspects the sample and returns a `LogFormat` from the same `PatternCompiler`, so nothing downstream of the parser can tell a detected format from a typed one (invariant #3). It exposes `detected()`/`detectedPattern()`; on no-detection it returns a `CompileError` (drop-in for `ManualFormatProvider`).
- [x] Pre-fills the existing M3 dialog for confirmation — no new UI, never applied silently — `MainWindow::openWithSettings` runs detection on the uncached-open path (a cached format still short-circuits it, M3 unchanged) and seeds the existing `LogFormatDialog` pattern field with the detected pattern; the user still confirms via OK. Detection failure leaves the dialog seeded with the fallback default, i.e. it opens as it did before M8. The dialog also gains a "Detect" button that re-runs detection into the pattern field. Verified headless (`-platform offscreen`): a library-detected non-default log, an inference-recovered non-library log, and a garbage file all behave as specified.

**Done when:** common log4cplus patterns are detected without user input, and detection failure falls back cleanly to manual entry.

---

## M9 — Multiple documents and tabs (post-1.0)

`SPEC.md` §5a, `ARCHITECTURE.md` §12, formerly `FUTURE.md`'s "Multiple open files". The feature §12's four accommodations were built for; delivered in seven stages, each building and keeping the suite green.

- [x] **Per-file machinery out of the window.** `DocumentContext` (`src/ui`) takes the `LogModel`, `IndexController`, `LiveController`, `FormatSettings` and pending run restore that used to be `MainWindow` members; `DocumentView` takes the `LogView` plus its own `FindBar`. The window keeps a vector of contexts and a pointer to the active *view*. Also removed the dead `Document::following()` — follow was already per-view in `LogView`.
- [x] **The dock shell.** No visible central widget; open files and side panes are all `QDockWidget`s under `AllowNestedDocks | AllowTabbedDocks | GroupedDragging`, so Qt's own dock dragging supplies drag-to-split, tab groups and floating windows. Added View ▸ Panes (a closed pane was previously unrecoverable). **Superseded by the document well below** — the shared arrangement was the problem, not the implementation.
- [x] **N documents.** An open adds a tab instead of tearing the previous file down; reopening an open file raises it. Active view follows focus, and `activeDocumentChanged` fires only on a *file* change. Close Tab / Close All, a Window menu, multi-file drop, per-file indexing progress in each tab's own label. The Filters pane's per-file widget state is stashed and restored across the switch (§12.2).
- [x] **A second view onto one file.** Window ▸ New View, sharing the `Document` and its `LogModel`; per-view scroll, selection, wrap, columns and follow. Tabs of one file are numbered.
- [x] **Session schema v2.** A `views` array beside `documents`, `activeView`, per-view column state and wrap mode, UUID dock names, and the restore ordering of §12.3. v1 sessions migrate, minus their `windowState`. Dropped the duplicated global `view/columnState` key.
- [x] **Tests and docs.** `tst_multidoc` (an open adds a tab; pane rebinding on a file switch but not a view switch; a file closing with its last view) and `tst_tabsession` (the tab order and per-view state round-trip through quit-and-relaunch; a missing file is skipped). `tst_session` covers the arrays and the migrations; the existing GUI tests gained session isolation, since an open now accumulates.
- [x] **The document well, replacing the dock shell (schema v3).** Sharing one dock arrangement between panes and logs was unusable in practice: an ordinary pane drag could tab the Filters pane over the log being read. Open files moved into a central `QTabWidget` — reorderable, closable tabs, no tearing off — leaving the panes as the only docks, structurally unable to enter the document area or be entered by it (§12.2). The session lost its per-view `dockName` (tab order replaced it) and `windowState` shrank to the pane layout, so v2 stores migrate minus that blob. Logs no longer split or float; `FUTURE.md` records what it would take to bring side-by-side back inside the well.
- [x] **Pane dragging made predictable.** Dropped `GroupedDragging` — with the panes tabbed together by default it turned every single-pane drag into a whole-group drag — restricted panes to the left and right areas (now that dropping the flag makes `setAllowedAreas()` safe), and disabled pane floating under Wayland, where a client can neither follow the pointer out of its window nor place the resulting one (§12.2). `tst_multidoc::aPaneDragMovesThatPaneAlone` pins all three.

**Done when:** several logs open as tabs in a document area the panes cannot invade; a second view onto one file scrolls independently; and the tabs come back in order through quit-and-relaunch.

---

## M10 — Highlight rules gain the full filter axis set

`SPEC.md` §7, `ARCHITECTURE.md` §7.2/§8. Highlighting shipped with two of the five axes filtering has, so there was no way to color by message text at all — the gap a user hits immediately, since the Highlighters pane offers nowhere to type a pattern.

- [x] **`MatchCriteria`** (`src/core`) — the portable form of the five axes (names, levels, wall clock, a pattern) as against `FilterSet`'s resolved form (ids, UTC ms, a compiled regex), with `resolve()` between them. One type, two consumers: the Filters pane resolves it into the Document's `FilterSet`, and a `HighlightRule` embeds it. Its JSON keeps `FilterPane`'s original key names verbatim, so existing filter presets and sessions load unchanged.
- [x] **`FilterSet::absentFieldMatches`** — the one genuine semantic inversion, as a flag rather than a second predicate chain: a record lacking the field an axis tests must not be *hidden* by a filter and must not be *colored* by a highlight rule.
- [x] **`HighlightRule` embeds `MatchCriteria`**; `HighlighterSet::resolve()` builds one `FilterSet` per rule (regexes compiled here, off the paint path). `fromJson()` still reads the original flat two-axis keys, which is what makes a preset/session schema bump unnecessary — and both stores reject an unknown version outright, so a bump would have discarded every existing preset.
- [x] **The paint path.** `HighlighterSet::match()` takes the decode as a lazy callable and memoizes it across rules (`FilterSet::accepts`'s shape); `LogModel::rowColors()` resolves both roles in one pass, since `LogView` asking for the two roles separately ran the rule list — and the decode — twice per record. Bound: one decode per *visible* record per repaint.
- [x] **`AxisEditor`** (`src/ui`) — the five axis group boxes lifted out of `FilterPane` and shared with the per-rule editor in `HighlighterPane`, which wraps it in a scroll area and collapses each axis until it is enabled. Two behavior changes ride along: `setCriteria()` applies a stored selection *exactly* (the "newly discovered arrives checked" rule is right for a whole-file filter and wrong for one rule among several), and an invalid regex is now flagged inline — `FilterPane` never surfaced `TextMatcher::isValid()`, so a broken filter regex used to silently empty the view.
- [x] **Tests.** `tst_matchcriteria` (JSON keys, the zone conversion, both policy arguments), `tst_highlighterpane` (a typed regex reaches the rule; switching rules shows that rule's selection; an invalid regex is flagged), and `tst_highlight` extended with the three new axes, the absent-field inversion, decode laziness, and the legacy flat-rule read.

**Done when:** a highlight rule can match on anything a filter can, and the message-text axis costs one decode per visible record rather than one per cell.

---

## M11 — Remote log sources over SSH (post-1.0)

`SPEC.md` §3, `ARCHITECTURE.md` §6.3, formerly `FUTURE.md`'s "Remote log sources (SSH)". Reading a server's log meant scp-ing it down and losing the tail — the one place loftail's "every file is opened live" premise stopped short. §6.2 pre-specified the shape (a local cache behind the same `LogSource`) and this milestone spends that accommodation. It brings in the project's first non-Qt dependency, and makes it optional.

- [x] **`RemoteLocation`** (`src/core`) — the `ssh://user@host:port/path` value type, and the normal form every entry point reduces to before the string becomes a `Document::path()`. That one rule is what makes `viewOfPath()`, the recent-files dedupe, the format-cache key and the session all agree that one remote log is one log. Fixed a latent bug on the way: `FormatCache::canonicalKey()` fell through to `absoluteFilePath()` for a URL, producing a working-directory-dependent key that lost the file's remembered format.
- [x] **`SpooledLogSource` + `RemoteSpool`** — a remote log is fetched forward into a local spool and read back through an ordinary local source, so the paint path is unchanged and zero-copy. The spool is shared per file and reference-counted, which is what makes `Document::rescan()` during a tail a pointer swap rather than a reconnect on the GUI thread. `refreshSize()` clamps to a fetcher-published committed size; that ordering is the only synchronisation between the two threads, and there is no mutex.
- [x] **`LogSource::wasReplaced()`** — the rotate-by-replace check moves out of `LiveController` and behind a non-pure virtual, because what has to be re-resolved differs per source (a path re-stat locally, a generation comparison remotely). Non-pure so `MemoryLogSource` and other fakes are untouched. `tst_tail` passing unaltered is the guard that local behavior is byte-identical.
- [x] **`SshSession` / `SshFetcher`** — the only libssh2-touching files. Host-key verification before any credential is sent (unknown offers accept-once/accept-and-remember; **changed refuses outright**, with no override); agent, then key files, then password or keyboard-interactive. Rotation is detected by comparing `fstat(handle)` with `stat(path)` — the inode substitute — probed once at connect, with a head-compare fallback fired only on suspicion.
- [x] **Credentials and host keys** — `HostBookmarkStore` (a file, not `QSettings`, because a password may be in it and a file can be made owner-only), and `GuiSshPrompter`. A remembered password is plain text, off by default, and the warning names the file it goes to rather than gesturing at "your configuration". *(M14 moved a remembered password to the OS keychain wherever one will answer, and kept this file as the fallback; it also connected two wires this milestone left dangling — see below.)*
- [x] **Entry points** — `ssh://` accepted by the Open dialog, the command line, drag-and-drop (`sftp://` too, which is what a file manager's SSH mount produces), recent files and session restore, plus `File ▸ Open Remote…` and a `File ▸ Remote Hosts` submenu. Saved hosts live in the dialog and that submenu, not a dock pane: every pane binds to the active Document (invariant #7) and a global list has none to bind to.
- [x] **Build gating** — `option(LOFTAIL_WITH_SSH)`, auto-detected and never `REQUIRED`, because §1 promises the reference build installs nothing extra. Without libssh2 everything still compiles and a remote open says why it cannot proceed. `LOFTAIL_SSH_FETCH=ON` builds libssh2 from source, statically against WinCNG, for the Windows CI job — which has no package manager, and which then needs no new DLL in the portable zip.
- [x] **Tests.** `tst_remotelocation`, `tst_spooledsource`, `tst_remotetail` (tst_tail's whole matrix over a fake fetcher, plus the two cases with no local counterpart: uncommitted bytes staying invisible, and a rescan not reconnecting), `tst_hostbookmarks`, `tst_remoteopen` (the real MainWindow, offscreen); all network-free. `tst_sshlive` is gated on `LOFTAIL_TEST_SSH_URL` and run by hand.

**Done when:** `loftail ssh://user@host/var/log/app.log` opens, tails, and survives a remote logrotate as silently as a local file does, and a build without libssh2 still builds, tests, and says so.

**Risk:** the CI dependency and the untested transport, not the design. **Nothing in CI exercises a single libssh2 call beyond linking** — the handshake, host-key, agent and keyboard-interactive paths, and whether a real `sftp-server`'s FSTAT tracks the handle, are covered only by the manual `tst_sshlive` harness. SFTP throughput on a large log is unmeasured; if priming proves slow, the opt-in "fetch only the end" becomes the default rather than an option. The Windows source build of libssh2 and the AppImage's bundling of it are both first exercised by CI, not locally.

---

## M12 — Compressed and archived logs (post-1.0)

`SPEC.md` §3, `ARCHITECTURE.md` §6.4, formerly `FUTURE.md`'s "Compressed logs". Rotated logs arrive compressed, and loftail refused every one of them — the last routine way of receiving a log it could not open. §6.2 pre-specified the shape (a local cache the paint path reads from) and M11 had already built that cache, so this milestone spends the accommodation rather than inventing one.

- [x] **The spool seam generalized off the transport.** `RemoteFetcher`/`RemoteSpool`/`RemoteSpoolRegistry` → `SourceFetcher`/`SourceSpool`/`SourceSpoolRegistry`, keyed on a plain normalized path string rather than a parsed `RemoteLocation` — the registry only ever used `toString()` and never needed to understand what it held. Naming, because the distinction is what the milestone rests on: SSH is a *transport* and an archive is a *file type*, and they compose. `FetchStatus::remoteSize` → `totalSize`. Zero behavior change; the guard was the three M11 tests passing with only type names edited.
- [x] **`ArchiveLocation` and the path layer** — the nested-path value type (`/logs/bundle.tar.gz/var/log/app.log`), always compiled like `RemoteLocation` so both build configurations agree about what a settings file means. The resolution rule (rule 0 keeps a real directory named `bundle.zip` working) and the collapse rule (a bare compressed stream keeps its plain path, so one log never gets two spellings). `normalizeLogPath()`/`logPathIsSpooled()` join the path-helper family; `FormatCache::canonicalKey()` and `LiveWatcher` take the archive branch beside the remote one. **New ungated test:** `tst_archivelocation`.
- [x] **`ArchiveFetcher` + `ArchiveReader`, and the build gate.** libarchive optional and auto-detected, gated as libssh2 is but with **no build-from-source fallback** — see the risk note below. The fetcher's input is an **ordinary `LogSource`**, which is what makes a remote archive free — two fetchers chain, SSH downloading while the archive expands. A 128 KiB synchronous prime in `start()`, or `Document::prepare` samples an empty file and autodetects nothing. **New gated tests:** `tst_archivefetcher`, `tst_archivemembers`.
- [x] **Completion on the live seam.** `FetchStatus::State::Complete` published after the final `committedSize`; non-pure `LogSource::isComplete()` beside `wasReplaced()`; `LiveController` reads it before refreshing and acts after ingesting, then stops watching for good. No mode follows — the follow control is untouched. Also built the `FetchStatus` → status-bar seam that had no reader at all, which delivers two of `SPEC.md` §3's existing remote-status promises as a side effect. **New tests:** `tst_complete` (ungated — a contract of the live seam, not of libarchive), `tst_archivetail`.
- [x] **Entry points and the member picker.** `OpenArchiveDialog`, shown only when there is a genuine choice; resolution happens exactly once, at the top of `MainWindow::openFile`, so `prepare()`, `rescan()` and session restore never open a dialog behind the user's back. Several members open as several tabs. Drag-and-drop needed no change at all. **New gated UI test:** `tst_archiveopen`.
- [x] **Space, cancellation and error paths.** A free-space refusal before anything is written; a distinct out-of-space message mid-expansion, keeping what expanded readable; a cancelled scan now stops the fetcher, which previously went on expanding into a spool nobody would read.
- [x] **Docs and CI.** `SPEC.md` §3, `ARCHITECTURE.md` §6.4 (plus the §6.2 and §6.3 amendments), `FUTURE.md` struck through, this checklist, `CLAUDE.md`, and the packaging workflow.

**Done when:** `loftail app.log.gz`, `loftail bundle.tar.gz/var/log/app.log` and `loftail ssh://host/var/log/app.log.1.gz` all open, fill in while they expand, and stop cleanly with no mode to choose — and a build without libarchive still builds, tests, and says why it cannot.

**Two bugs worth remembering, both found by tests rather than review.** A container must be opened as **bytes**, never as a log: `openLogSource("app.log.gz")` *means* "expand it", so a fetcher using it to read its own input recursed into expanding itself. And because a single-stream container collapses to its own path, the expanded log and the raw container are the same address string — sharing a spool key between them recursed the same way, one level up. Both are structural, not typos; `openContainerSource()` and `expandedSpoolKey()` exist to make them impossible.

**What the Windows work actually cost, since the plan guessed wrong about it.** The plan named building libarchive and its codecs from source as the top risk, with vcpkg as a fallback "if it does not go green within about two CI iterations". It took three, and the useful part is *why* each failed:

1. **Include order.** libarchive's header reaches `<windows.h>`, whose `min`/`max` macros break `std::numeric_limits<qint64>::min()` in any loftail header included after it. Fixed with `NOMINMAX` on the build rather than a rule about include order.
2. **`dllimport`.** libarchive declares its API imported unless `LIBARCHIVE_STATIC` is defined, and sets that only privately for its own build.
3. **The chain itself, and this is the one that mattered.** libarchive locates zlib and liblzma through `find_package` in **MODULE** mode, which `CMAKE_FIND_PACKAGE_REDIRECTS_DIR` does not reach — so the FetchContent-built codecs were silently ignored, libarchive compiled with *no compression at all*, and the configure line still said `ENABLED`. It reached CI reporting success while every `.gz` fixture failed to write and zip quietly fell back to "store". A local Linux run could not have caught it: the system zlib satisfied MODULE mode there and masked the whole problem.

Only the third is a fault of the approach rather than of the code, and it is the one that killed it: a dependency that reports itself found while being useless is worse than one that fails. The from-source path was **removed**, not fixed — Windows uses vcpkg, which supplies the codecs as a matter of course and also gets bzip2 and zstd, so the codec set is now the same on every platform. The lesson worth keeping: `LOFTAIL_SSH_FETCH` works because libssh2 has no dependencies of its own to satisfy.

**Remaining risk.** Disk exhaustion on a large expansion is guarded by a free-space check that a deliberate zip bomb defeats; an expansion cap is deferred by decision, not oversight. **Unlike M11, the dependency is genuinely exercised in CI** — no network, no credentials, fixtures built at runtime by libarchive's write side — so the "a green pipeline means nothing about it" caveat does not carry over.

---

## M13 — Logs that are not there (post-1.0)

`SPEC.md` §3 "Logs that are not there", `ARCHITECTURE.md` §6.5. A path that did not exist was a hard failure at every entry point — status bar message, no tab — and session restore dropped a missing file, which (because `saveSession()` writes only what is open) forgot it permanently at the next quit. This milestone makes "not there" a state rather than an error, for local files, archives and remote hosts alike.

- [x] **`LogSource::originVanished()`** — non-pure and false by default, the third arrival by the route `wasReplaced()` (M11) and `isComplete()` (M12) took. Distinct from `wasReplaced()` and checked after it: replaced means something *else* is at the origin and rescans, vanished means *nothing* is and waits. `BufferedLogSource` deliberately does NOT route through `pathIdentity()`, which is stubbed to 0 on Windows and would report every open file as vanished.
- [x] **A `Document` that can wait.** `prepare()` classifies waitable vs fatal from two questions — `logSourceAvailable()` ("is it there") and a new `logPathIsWellFormed()` ("does it name a log at all"). Both are needed: without the second, `ssh://` opens a tab that waits forever for something that cannot exist. `enterWaiting()`/`resume(provider)`/`formatSettled()`, and `rescan()` enters waiting rather than sitting on an error nobody reads.
- [x] **The state machine on the live seam**, not a fourth `LogSource`. `LiveController` enters and leaves waiting on the tick that was already there, with a 2 s grace period measured in **elapsed time, not checks** — the watcher fires checks too, and a rotation produces a burst of them at exactly the moment the path is briefly empty. `resumeRequested()` hands the resume to the owner because the pattern lives there (invariant #3). `LiveWatcher` needed no change at all: it already watched the directory and re-added the file when it appeared.
- [x] **The transport half.** `FetchStatus::State::Waiting`, distinct from `Error`; `SshFetcher::start()` succeeds into it for an unreachable host and spawns the worker anyway; a reconnect step in `tailLoop()`, which nothing ever had. Retries are non-interactive without exception — a prompt marshalled off the worker would be a modal dialog for a log opened hours ago — and **File ▸ Reconnect** is what asks again, finally giving `SourceSpool::poke()` a caller.
- [x] **UI.** A waiting tab is marked `◦`, `LogView::setPlaceholderText()` says why in the view itself, and the status bar carries the fetcher's own words. No format dialog on arrival: it happens on a watch tick, possibly for a tab that is not on screen. Session restore brings a missing file back as a waiting tab, which also fixed the asymmetry where a *local* `prepareContext()` failure vanished without appearing in the "could not reopen" list at all.
- [x] **Tests.** New `tst_waiting` (local, POSIX, beside `tst_tail`) and `tst_waitingremote` (ungated, over the fake fetcher); `FakeRemote` gains `setInitiallyUnavailable()`/`becomeAvailable()`/`becomeUnavailable()`, with `setStartFailure()` kept as the *refusal* case. New end-to-end cases in `tst_openflow` and `tst_remoteopen` that drive the real watcher. Three existing cases changed on purpose (`tst_spooledsource`, `tst_sessiongui`, `tst_tabsession`) — each asserted that an unavailable log produced no tab, which is the behaviour being replaced. `tst_tail`, `tst_remotetail`, `tst_complete` and `tst_archivetail` pass **unaltered**, which is the guard that rotation, truncation, completion and ordinary tailing are untouched.

**Done when:** `loftail /tmp/notyet.log` and `loftail ssh://downhost/var/log/app.log` both open, wait, and start reading when the log turns up; deleting an open log waits for it; and a rotation is still silent.

**The bug worth remembering, and it was nearly shipped.** `ArchiveFetcher::start()` runs on the GUI thread and feeds libarchive through `awaitInput()`, which waits while the transport underneath is *healthy* — and the whole point of `State::Waiting` is that a retrying transport IS healthy. So `ssh://downhost/bundle.tar.gz/app.log` would have spun there forever, on the GUI thread, with no error and no way out. It was found by reasoning about the composition rather than by a test, which is the argument for §6.4's "an archive composes with a transport" being written down: the two features are orthogonal, so a change to one has to be checked against the other. The fix defers `beginExpansion()` to the worker when the container's origin has vanished.

**Remaining risk.** The reconnect path is in the untested half of the codebase — `CLAUDE.md`'s caveat that nothing in CI exercises a single libssh2 call beyond linking applies to it in full. Everything above `SourceFetcher` is covered by `tst_waitingremote` with no network; the handshake on a *re*connect, and whether a server that has just come back behaves like one that never went away, are covered only by running `tst_sshlive` by hand.

---

## M14 — OS keychain for remembered passwords

A remembered SSH password stops being clear text on disk wherever the machine has somewhere better to put it. Behaviour in `SPEC.md` §3; design in `ARCHITECTURE.md` §6.3.2.

- [x] **`SecretStore`** (`src/core`) — abstract, free of QtKeychain types, **always compiled**, with a global setter in `SshPrompter`'s shape. Unlike `sshPrompter()` it is never null: "no keychain" is not a policy, only which backend was found, and `available() == false` already says it. `NullSecretStore` is what a build without the dependency gets.
- [x] **`KeychainSecretStore`** — the only QtKeychain-touching translation unit, gated on `LOFTAIL_HAVE_KEYCHAIN`. QtKeychain's jobs are asynchronous and `authenticate()` needs an answer now, so a nested `QEventLoop` bridges them — safe for the reason `SocketDetach` already establishes, plus `ExcludeUserInputEvents` for the modality a bare loop lacks and a timer bound for a wedged daemon. `available()` **round-trips** rather than trusting `QKeychain::isAvailable()`, and a thread guard makes the "GUI thread only" rule a runtime fact.
- [x] **The auth chain** — one new rung, after the agent (so a key login never triggers an unlock dialog) and after the `!prompter` bail (so the fetcher thread can never reach it). A stored password the server rejects is erased, because sshd counts failures against `MaxAuthTries`.
- [x] **Two dangling wires from M11.** `SshSession` filled in `askPassword()`'s `bool *remember` and never read it — ticking the box had never persisted anything. And `HostBookmarkStore::find()` had **zero call sites in `src/`**, so a bookmark's saved password was written and never read at connect. `SshPrompter::passwordAccepted()` (non-pure, the `wasReplaced()` shape) fixes the first; `MainWindow::primeRemoteCredentials()` at the top of `openFile()` fixes the second, priming the credential cache rather than inventing a route into core.
- [x] **UI** — three destinations, decided *before* the box can be ticked, keeping M11's "the warning informs the decision, it does not confirm it" rule: a keychain, named on the checkbox with no ⚠; the plain-text file, worded exactly as before; or neither, in which case the box is **disabled** and says why. That third state is the honest rendering of what already happened silently.
- [x] **Build gating** — `option(LOFTAIL_WITH_KEYCHAIN)`, the same three-tier probe as the other two. `LOFTAIL_KEYCHAIN_FETCH=ON` builds from source, which is right here where it was wrong for libarchive: no codecs to silently lose, and vcpkg's port would drag a second Qt into the Windows job.
- [x] **Tests.** `tst_secretstore`, `tst_sshcredentials` and three new cases in `tst_hostbookmarks` — all **ungated**, driven through `tests/FakeSecretStore.h`, because what they pin is a rule about consent rather than about a dependency. `tst_keychainlive` is gated twice like `tst_sshlive`. A fourth CI configuration covers SSH-on/keychain-off, the one most users on a server actually get.

**Done when:** ticking *remember* on a desktop with a keyring writes nothing to `hosts.json` and the password survives a restart; a build or a machine with no keychain behaves exactly as before and says so; and a keychain that refuses reports it rather than writing a file.

**Risk, and it is M11's shape again.** **Nothing in CI exercises a real keychain, and nothing can** — the runners are headless with no session bus, so no backend would answer and a green pipeline says nothing about KWallet, GNOME Keyring, the Credential Manager or the macOS Keychain. `tst_keychainlive` is manual. Two further gaps: the AppImage cannot bundle libsecret, because QtKeychain `dlopen()`s it and `ldd` never sees it, so inside the AppImage a host without it silently gets the plain-text path (told to the user, but only in the dialog); and the FetchContent source build is first exercised by the Windows job, not locally.

**That second gap bit immediately, and is now closed.** The fetch path shipped in M14 **broken in two ways at once and neither was reachable by any local build**: the tag was written `v0.15.0`, but upstream dropped the `v` prefix at 0.14.1, so the clone died with `invalid reference` — a configure failure *before* any of the options below it were read, which is how the second defect stayed hidden. That second one is `BUILD_WITH_QT6`, which QtKeychain still defaults to **OFF**, its else branch being a `find_package(Qt5 COMPONENTS Core REQUIRED)` — a hard failure on a Qt6-only runner rather than a fallback. Both are fixed, and the path is now exercisable on Linux too:

```bash
cmake -S . -B build-fetch -G Ninja -DLOFTAIL_KEYCHAIN_FETCH=ON \
      -DCMAKE_DISABLE_FIND_PACKAGE_Qt6Keychain=ON -DLIBSECRET_SUPPORT=OFF
```

`LIBSECRET_SUPPORT=OFF` is what makes it a fair stand-in for Windows: QtKeychain's Unix backend hard-requires `libsecret-1`, while on Windows it is a dependency-free wrapper over the Credential Store. The lesson generalizes past this bug — **a dependency path only one CI job can reach is a path nobody can debug**, so give it a local invocation even when the local build does not need it.

---

## M15 — filter with context (`grep -B`/`-A`)

A filter that hides everything but the ERRORs also hides what led to them. Behaviour in `SPEC.md` §6 ("Context"); design in `ARCHITECTURE.md` §7.2.1. Promoted from `ideas.md` Tier 1 #1, whose claim — "the architecture has already paid for this" — held: nothing below the emitter changed.

- [x] **`ContextEmitter.h`** (`src/core`, header-only) — the whole rule, as one forward pass over three callables (`inBound`, `matches`, `sink`). One template rather than two loops, so `Document::applyFilters()` and `LiveController`'s append branch cannot drift — the move `acceptsInView()` already made for the run bound.
- [x] **`FilteredIndex`** — one parallel `QVector<quint8>` of match/context flags plus a maintained count, threaded through `setVisible()`/`appendVisible()`/`popLastVisible()`/`clear()`, and two live-path queries (`lastMatchSource()`, `trailingCountFrom()`). The compact index, the prefix sums and every geometry path are **untouched**: a context record is just one more ordinal.
- [x] **`Document`** — `setContext(before, after)` (per file, invariant #7, clamped by `kMaxContext`) and `acceptsInView()` split into `inRunBound()` + `matchesFilters()`, because a context record is one the *filters* rejected and the *run* still admits. `applyFilters()` keeps its identity early-out: context never activates the view on its own.
- [x] **`LiveController`** — the pop point and the re-scan start are the same row, widened to `base − before` **only when the provisional record's class flips**. Both halves are load-bearing and neither is obvious; the second is the difference between one row of tail churn per tick and `before + 1`.
- [x] **UI** — two spinners in a Context box on the Filters pane (not in the shared `AxisEditor`, which a highlight rule also uses); `contextTextColor()`/`contextFillColor()` in `UiColors`, so a rule-coloured context row is softened rather than left shouting; the status bar says how much of what is shown is context.
- [x] **Persistence** — two additive keys in `FilterPane::saveState()`'s object, written only when non-zero. They ride into both filter presets and the session with **no schema bump** in either store, following `loggerRestrictive`'s precedent exactly.
- [x] **Tests.** `tst_filtercontext` — the emitter table-driven plus a randomized comparison against a naive `O(n·C)` reference, `Document` over real bytes including the run composition, and eight live cases. **Ungated**, unlike the `tst_tail` cases it would otherwise have joined: nothing here needs mmap-sees-appends, an unlink or a rename, and the rule must hold on Windows. Plus cases in `tst_filterpane`, `tst_logmodel`, `tst_uicolors`, `tst_recordmenu` and `tst_sessiongui`.

**Done when:** filtering to ERROR with *Before 3* shows the three records ahead of each error, dimmed; the same view tailed live matches a one-shot scan of the same bytes exactly; and setting both spinners to zero leaves the view byte-identical to what it was before the feature existed.

**The one thing to be careful of later.** The live path is a tail append only because of the **suffix invariant** (`ARCHITECTURE.md` §7.2.1), and that invariant needs the in-view bound to accept a *contiguous* range of ordinals. Run selection satisfies it today by construction. A future view restriction that admitted a scattered set — anything bookmark-shaped, say — would silently make leading context need mid-list insertion, which `FilteredIndex` cannot do. `tst_filtercontext::emitterSuffixInvariantHolds` and `liveLeadingContextIsAlwaysATailAppend` are what would catch it.

---

## Deliberately deferred

Later-release features are catalogued in `FUTURE.md` (side-by-side views, bookmarks; multi-file views shipped in M9, format autodetection in M8, SSH sources in M11, and compressed/archived sources in M12); each names the P1 accommodation that keeps it additive. Recorded here only so they are not silently dropped from the plan.

An **expansion cap** — "expand only the first N bytes of a very large member", the analogue of SSH's `tailStartBytes` — is deferred by decision rather than oversight (M12). The free-space check turns the honest case into a clear refusal; the cap is the only real defence against a deliberate zip bomb, and it needs a dialog control, a per-location options store and its own tests. Build it if someone actually hits it.

Column reorder/hide with remembered layout (`SPEC.md` §5) is additive to the M2 spine and lands in M2b. **Done (M2b):** `LogView` drives its columns through a `QHeaderView` (reorder, resize, hide via the header context menu); the layout round-trips through the header's own `saveState()`/`restoreState()`, persisted to `QSettings` — full session restore is folded in at M5. (Preset export/import, once deferred, is now in M5.)

Note that M11's spool is a **bytes** cache, not an index cache, and so does not reopen the ruling below: it holds a copy of the remote file's contents, is discarded when the log is closed, and needs no invalidation or versioning because it is never reused across runs.

Explicitly ruled out for now: caching the index to disk. It needs invalidation, versioning, and a cache location — real complexity to solve a problem that may not exist. Revisit only if the M2a measurements miss the §11 indexing target.
