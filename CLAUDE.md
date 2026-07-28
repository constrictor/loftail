# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project status

**M0–M8 complete, 2026-07-28.** The application is functional on Linux: it opens and indexes a log, filters and highlights it, tails it live, splits it into runs, restores its session, and autodetects a format. Directory layout is `src/core/` (UI-free, links QtCore only), `src/ui/` (Qt Widgets), and `tests/`; the commands below work and all CTest cases pass.

**In progress: M9 — multiple documents, tabs and splits.** Several logs open at once as draggable, splittable, floatable tabs. See `PLAN.md` M9.

**Outstanding regardless of milestone:** Windows and macOS builds and runtime behavior are still unverified (`PLAN.md` M6/M7), including the stubbed Windows `BufferedLogSource` share-mode open and `pathIdentity()`.

**Windows testing has no fonts.** The offscreen QPA plugin on Windows uses Qt's own font database, which looks in `$QTDIR/lib/fonts`; Qt no longer ships fonts, so `QFontDatabase::families()` comes back **empty** and nothing resolves — not even to a wrong font. (Offscreen on Linux has fontconfig, so this never shows up locally.) Any test that asserts on resolved font properties must guard on an empty family list and `QSKIP`; `tst_logview::everyColumnRendersFixedPitch` does. This is an environment limit, not a `monospaceFont()` bug.

**When a Windows test fails, read the CI diagnostic, not the ctest output.** Test output does not reach ctest on Windows — a failure shows as `***Failed` with an empty block, which reads like a crash and is not. The Windows job re-runs each failed binary one test function at a time, prints per-function exit codes, and dumps QtTest's report via `-o file,txt` for the failing one. That is where the actual assertion appears.

## What loftail is

A cross-platform (Windows/macOS/Linux) desktop GUI viewer for logs produced by **log4cplus**. Every file is opened watched for new content, so it serves finished logs and still-being-written logs with no mode switch (`SPEC.md` §3). It filters and highlights by subsystem (the logger name passed to `Logger::getInstance()`) and priority, with saved presets and full session restore.

## Document map

| File              | Contents                                                                  |
| ----------------- | ------------------------------------------------------------------------- |
| `SPEC.md`         | User-visible behavior of what has shipped. The product definition.        |
| `FUTURE.md`       | User-visible features planned for **later** releases, and the P1 accommodations that keep them cheap to add. |
| `ARCHITECTURE.md` | Internal technical decisions and rationale. Not user-visible.             |
| `PLAN.md`         | Milestone-by-milestone implementation plan.                               |

When a change alters user-visible behavior, update `SPEC.md`. When it alters an internal contract or invariant, update `ARCHITECTURE.md`. Keep product decisions out of `ARCHITECTURE.md` and implementation detail out of `SPEC.md`.

## Stack

- **C++20**, **Qt 6** (6.10 is installed on the dev machine; target 6.4 minimum — the version in Ubuntu 24.04's repos)
- **Qt Widgets**, not QML — see `ARCHITECTURE.md` for the rationale; the decision is load-bearing and should not be revisited casually
- **CMake ≥ 3.21** + **Ninja**
- **Qt Test** for unit tests
- **Reference build environment is Ubuntu 24.04 LTS**: the project must build with the stock toolchain (GCC 13, CMake 3.28, Ninja, Qt 6.4.2) using no separately-installed Qt. This is why the Qt minimum is 6.4, not 6.5 LTS — see `ARCHITECTURE.md` §1.

loftail does **not** link log4cplus. It reads log files as text; it has no compile-time relationship to the library that produced them.

## Commands

```bash
# Configure (Debug)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug

# Build
cmake --build build

# Run
./build/src/loftail

# All tests
ctest --test-dir build --output-on-failure

# A single test binary
./build/tests/tst_patterncompiler

# A single test case within a binary (Qt Test convention)
./build/tests/tst_sessiongui sessionRoundTrip
```

On Windows and macOS, `CMAKE_PREFIX_PATH` must point at the Qt installation.

## Architecture invariants

These ten constraints are cheap to honor from the start and expensive to retrofit. Do not violate them without explicitly reopening the decision.

1. **The model stores byte offsets, never parsed text.** A record entry is `{offset, timestamp, length, loggerId, threadId, lineCount, priority}` — exactly 32 bytes. Message text is parsed lazily inside `data()`. Holding parsed strings for every record makes large files unusable.

2. **A record is not a line.** log4cplus messages can contain embedded newlines, so one record may span several physical lines. Indexing rule: a line matching `recordStartRe` begins a record; non-matching lines are continuations of the preceding record. Code that assumes one-line-per-row is wrong.

3. **Nothing downstream of the parser knows about the pattern string.** `PatternCompiler` turns a log4cplus `ConversionPattern` into a `LogFormat` (regex + field map). Views, filters, and highlighters consume only the field map. This indirection is what makes format autodetection (a later-release feature, `FUTURE.md`) a drop-in rather than a rewrite.

4. **Filtering compares integers, not strings.** Logger and thread names are interned to `quint32` ids at index time; priority is an enum **declared in severity order** so filtering by minimum level is one `>=` test (`ARCHITECTURE.md` §7.2); timestamps are `qint64`. Filter predicates operate on those. The intern tables double as the subsystem and thread lists shown in the filter pane. Message-text filtering is the one axis with no integer fast path, so it runs last in the predicate chain.

5. **Every file is opened append-aware; there is no post-mortem vs live mode.** loftail cannot know whether a file is still being written, so all files are watched and a finished log is just one that never grows (`SPEC.md` §3). No `LogSource` may assume the file is immutable, and none may hold it in a way that blocks the writer from appending, rotating, or truncating — observing a log must not disturb the process producing it. File access goes through the `LogSource` interface; the model cannot tell which implementation it has. The mmap (POSIX) vs buffered (Windows) split is now platform-driven, not mode-driven — see `ARCHITECTURE.md` §6.

6. **The record table is a custom `LogView : QAbstractScrollArea`, not a `QTableView`.** Multi-line records render at full height, and `QTableView` cannot do variable row heights lazily — it needs an O(n) `resizeRowsToContents()` pass or per-row entries in `QHeaderView`, either of which defeats the lazy index. `LogView` scrolls in *line* units over two-level prefix sums of `Record::lineCount`. It has two geometry modes: **exact** (wrap off / selected-record-only) and **estimated** (wrap always on, where height depends on viewport width). Keep the estimation machinery unreachable from the exact path. See `ARCHITECTURE.md` §7.1–7.1.1; this is the project's highest-risk component.

7. **All per-file state lives in `Document`; all per-view state lives in `DocumentView`; nothing reaches for "the current file" globally.** Several files are open at once and one file may have several views, so the two scopes must not blur: filters, highlighters, format and run selection belong to the *file*; scroll, selection, wrap mode, column layout, follow and find belong to the *view*. No singletons holding file state; panes bind to an `activeDocumentChanged(Document*)` signal rather than a fixed reference, and that signal fires only when the **file** changes, never when switching between two views of one file. The settings schema stores a `documents` array and a `views` array. See `ARCHITECTURE.md` §12.

8. **Never scan for `\n` in raw bytes.** Encoding is user-selectable, defaults to auto-detect, and may be UTF-16, where a newline is `0A 00` or `00 0A`. All line-boundary and text work goes through the `Decoder` layer; only `Record::offset`/`length` stay in byte terms. See `ARCHITECTURE.md` §6.1 — this is the easiest invariant to violate by accident.

9. **The indexer is a single forward pass.** No backward passes, no seek-and-re-read. Compressed and SSH-backed sources are planned (`FUTURE.md`) and neither supports random access during indexing. Random access is fine in `data()` on the paint path, which only touches already-indexed records. See `ARCHITECTURE.md` §6.2.

10. **`Record::timestamp` is always UTC epoch milliseconds.** The source time zone and the timestamp display mode are both user-configurable, but conversion happens exactly twice: source zone applied at index time, display zone applied when formatting or interpreting typed filter bounds — or not at all, in the two *seconds* display modes, which render the stored ms directly. Nothing in between is zone-aware. Storing local wall-clock time would make comparisons zone-dependent and break across DST transitions, where the same local time occurs twice. See `ARCHITECTURE.md` §5.1.

## Conventions

- Qt naming throughout (`camelCase` members, `PascalCase` types) so the codebase reads consistently with the framework it sits on.
- Prefer `QStringView`/`QByteArrayView` on parse paths; avoid allocating per record.
- No hardcoded paths — `QStandardPaths::AppConfigLocation` for settings and presets.
- Open log files in binary mode and handle CRLF explicitly rather than relying on platform text-mode translation.
- Parsing and indexing logic must be unit-testable without a `QApplication` instance; keep it free of UI dependencies.

## Working notes

- The user refers to the logging library as "cplus4log"; the actual library is **log4cplus**. Confirmed 2026-07-20.
- Format configuration was manual first (user supplies the `ConversionPattern`); autodetection was deliberately built after it, in M8, behind the `IFormatProvider` seam. Both paths still end at the same `PatternCompiler`, and a detected pattern is never applied without confirmation — it pre-fills the Log Format dialog (`SPEC.md` §4).
