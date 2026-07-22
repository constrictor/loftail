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

## Deliberately deferred

Later-release features are catalogued in `FUTURE.md` (multi-file views, compressed and SSH sources, bookmarks, and — with a milestone, M8 — format autodetection); each names the P1 accommodation that keeps it additive. Recorded here only so they are not silently dropped from the plan.

Column reorder/hide with remembered layout (`SPEC.md` §5) is additive to the M2 spine and lands in M2b. **Done (M2b):** `LogView` drives its columns through a `QHeaderView` (reorder, resize, hide via the header context menu); the layout round-trips through the header's own `saveState()`/`restoreState()`, persisted to `QSettings` — full session restore is folded in at M5. (Preset export/import, once deferred, is now in M5.)

Explicitly ruled out for now: caching the index to disk. It needs invalidation, versioning, and a cache location — real complexity to solve a problem that may not exist. Revisit only if the M2a measurements miss the §11 indexing target.
